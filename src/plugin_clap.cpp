// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_clap.cpp — the CLAP adapter behind src/plugin_host.h.
 *
 * It exports clockwork_plugin_clap::* (see plugin_clap.h) rather than plugin_host.h's
 * C symbols: those are defined once, in plugin_host.cpp, which dispatches to
 * whichever formats were compiled in. Two adapters both defining plugin_scan
 * would be a duplicate symbol at link.
 *
 * plugin_host.h is modelled on CLAP, so this adapter is mostly plumbing: open
 * the DSO, read the `clap_entry` symbol, walk the plugin factory, instantiate,
 * and drive one `clap_process` per block. What follows is the part that is NOT
 * plumbing — the places where CLAP's contract and clockwork's do not line up
 * and a decision had to be made.
 *
 * THREADS. CLAP states, per call, which thread may make it. This file honours
 * that: only `clap_plugin::process` is called from plugin_process(). Everything
 * else — init, activate, params.get_info/get_value, state.save/load,
 * latency.get — is main-thread and is called only from the open/close/query
 * entry points, which clockwork drives from its control thread. The one
 * knowing exception is start_processing(), which CLAP marks [audio-thread] and
 * which plugin_open() calls: opening runs on the control thread and the instance
 * is not published to the audio thread until open returns, so no audio thread
 * can be inside this plugin yet, and this is what hosts do in practice.
 *
 * LATENCY IS CACHED, NOT POLLED. plugin_latency() is documented as asked per
 * block, but CLAP marks `clap_plugin_latency::get` [main-thread], so calling it
 * per block would break the contract this adapter is meant to keep. Instead the
 * value is read once at open and refreshed inside the host's
 * `clap_host_latency::changed` callback — which CLAP marks [main-thread], so
 * the refresh is legal exactly where it happens — and published through an
 * atomic that plugin_latency() reads. Same observable behaviour, no violation.
 *
 * EVENTS ARE QUEUED, NOT APPLIED. plugin_param_set() pushes a
 * clap_event_param_value — and a note or controller its own event — onto one
 * fixed-size single-producer ring; the events are sorted by frame offset and
 * handed to the plugin as the block's input event list. Nothing reaches the plugin until the next plugin_process(), which is
 * how CLAP works and is what makes the frame offset mean anything. A caller
 * that sets a parameter and immediately reads it back with plugin_param_info()
 * will see the old value until a block has run.
 *
 * BUFFERS ARE COPIED. Clockwork hands in `const float* const*`; CLAP wants
 * `float**` and permits a plugin to write through its input pointers when the
 * host says the ports are in-place. Rather than cast the const away, the block
 * is staged through adapter-owned buffers allocated at open: inputs are copied
 * in, outputs are copied back out. Missing input channels are fed silence, and
 * clockwork output channels the plugin does not cover are zeroed.
 *
 * PORT LAYOUT COMES FROM THE PLUGIN. Channels are drawn from clockwork's flat
 * arrays and dealt out across the plugin's declared clap.audio-ports in order.
 * A plugin with no clap.audio-ports has no audio ports — that is what the
 * extension says — so it gets no audio and its outputs are silence.
 *
 * ALLOCATION. Everything plugin_process() needs is allocated at open. It still
 * calls into third-party code that may allocate, so it suspends clockwork's
 * rt_alloc guard across exactly that call and nothing else — see plugin_host.h.
 */
#include "plugin_clap.h"
#include "plugin_transport.h"   // the block's musical position, shared with VST3
#include "shared_memory.h"      // ClockworkClockState
#include "clockwork_product.h"

#include "rt_alloc.h"
#include "clockwork_path.h"

#include <clap/clap.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <functional>   // std::hash — CLAP names groups, so the id is a hash of the name
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace clockwork_plugin_clap {
namespace {

/* ── The one deliberate hole in the no-allocation rule ────────────────────── */
/*
 * rt_alloc::Guard sets the thread-local flag; the test binary's operator
 * new/delete overrides count anything allocated while it is set. A plugin will
 * allocate, and that is not clockwork's failure to report — so the flag is
 * cleared for the duration of the plugin call and restored immediately after.
 * Suspending it here, at the one call that cannot honour it, is the whole
 * point: the guarantee stays enforced everywhere else.
 */
struct SuspendRtGuard {
    bool prev;
    SuspendRtGuard()  : prev(rt_alloc::g_in_rt) { rt_alloc::g_in_rt = false; }
    ~SuspendRtGuard() { rt_alloc::g_in_rt = prev; }
};

/* ── DSO handling ─────────────────────────────────────────────────────────── */

void* dso_open(const char* path) {
#if defined(_WIN32)
    // The path is UTF-8 off the wire, so the wide entry point. ALTERED_SEARCH_PATH
    // makes the plugin's own folder the first place its dependent DLLs are
    // looked for — which is where a vendor puts them, and which the default
    // search (the engine's folder, then the system) never reaches.
    return (void*)::LoadLibraryExW(clockwork_path::to_wide(path).c_str(), nullptr,
                                   LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    return ::dlopen(path, RTLD_LOCAL | RTLD_NOW);
#endif
}

void* dso_sym(void* h, const char* name) {
#if defined(_WIN32)
    return (void*)::GetProcAddress((HMODULE)h, name);
#else
    return ::dlsym(h, name);
#endif
}

void dso_close(void* h) {
    if (!h) return;
#if defined(_WIN32)
    ::FreeLibrary((HMODULE)h);
#else
    ::dlclose(h);
#endif
}

const char* dso_error() {
#if defined(_WIN32)
    // Per-thread for the same reason the open error is: the text is handed
    // back by pointer and must outlive the call.
    static thread_local std::string text;
    text = "LoadLibrary failed: " + clockwork_path::last_error_text(::GetLastError());
    return text.c_str();
#else
    const char* e = ::dlerror();
    return e ? e : "dlopen failed";
#endif
}

/* The error string plugin_open hands back. Owned by the adapter, per-thread so
   two threads failing to open at once do not overwrite each other. */
thread_local std::string g_err;

const char* set_err(std::string msg) {
    g_err = std::move(msg);
    return g_err.c_str();
}

/* ── Scan cache ───────────────────────────────────────────────────────────── */
/*
 * plugin_host.h promises the strings in a PluginDesc stay valid until the next
 * scan of the same path. CLAP's descriptors live in the DSO and die with
 * entry->deinit(), and a scan must not leave every file it looked at loaded —
 * so the strings are copied here, keyed by path, and replaced on rescan.
 */
struct ScanEntry {
    std::vector<std::string> ids, names, vendors;
    /* Whether each plugin GENERATES. CLAP states it in the descriptor's
       feature list, which is NULL-terminated and unordered. */
    std::vector<char> instruments;
};

std::mutex                        g_scan_mu;
std::map<std::string, ScanEntry>  g_scan_cache;

} // namespace

/* ── The instance ─────────────────────────────────────────────────────────── */

struct Instance {
    void*                             dso     = nullptr;
    const clap_plugin_entry_t*        entry   = nullptr;
    const clap_plugin_t*              plugin  = nullptr;
    clap_host_t                       host{};

    const clap_plugin_params_t*       params  = nullptr;
    const clap_plugin_state_t*        state   = nullptr;
    const clap_plugin_latency_t*      latency = nullptr;
    const clap_plugin_audio_ports_t*  ports   = nullptr;

    bool     entry_inited = false;
    bool     activated  = false;
    bool     processing = false;

    double   sample_rate = 0.0;
    uint32_t max_block   = 0;

    std::atomic<uint32_t> latency_frames{0};

    /* Host-callback observations. Nothing acts on them yet — nothing here
       re-activates the plugin or calls its on_main_thread — but they are recorded
       rather than dropped so the next version has something to act on. */
    std::atomic<bool> want_restart{false};
    std::atomic<bool> want_process{false};
    std::atomic<bool> want_callback{false};

    /* Port plan, fixed at open. */
    std::vector<clap_audio_buffer_t> in_bufs, out_bufs;
    std::vector<float*>              in_chans, out_chans;   /* flattened, per port */
    std::vector<float>               in_store, out_store;   /* max_block per channel */
    uint32_t                         total_in_ch  = 0;
    uint32_t                         total_out_ch = 0;

    /* Parameter events queued since the last block. Single producer (whoever
       calls plugin_param_set), single consumer (the audio thread). Overflow
       drops the newest and is silent: the alternative on the audio thread is
       to block, which is worse. */
    static constexpr uint32_t kEventCap = 1024;
    /* One slot holds any event the adapter sends. CLAP's input list is a list
       of headers, so notes, controllers and parameter values share one ring
       and one time-ordered block list rather than three that would have to be
       merged. The header is the first member of every variant. */
    union Event {
        clap_event_header_t      header;
        clap_event_param_value_t param;
        clap_event_note_t        note;
        clap_event_midi_t        midi;
    };
    Event                     evq[kEventCap]{};
    std::atomic<uint32_t>     ev_head{0};
    std::atomic<uint32_t>     ev_tail{0};
    std::vector<Event>        block_events;   /* reserved at open */

    /* Note port 0's dialects, from CLAP_EXT_NOTE_PORTS. A plugin with no note
       port takes no notes; one that speaks only MIDI is sent MIDI bytes; one
       that speaks CLAP gets clap_event_note, which carries a float velocity
       the way plugin_host.h does. */
    bool     has_note_port = false;
    uint32_t note_dialects = 0;

    /* The session clock, or null: free running. Read per block as one
       clockwork::Timeline snapshot into `transport`, which the process struct then
       points at — audio thread only, so a plain field. */
    const ClockworkClockState*  clock = nullptr;
    clap_event_transport_t transport{};
    /* The timeline bound for the coming blocks, when a track is following
       one that is not the clock (set_timeline). Audio thread only. */
    ClockworkTimeline timeline{};
    bool        has_timeline = false;

    clap_input_events_t  in_events{};
    clap_output_events_t out_events{};

    /* Parameter names, copied at open so PluginParam::name outlives the call. */
    std::vector<std::string> param_names;
    /* CLAP states a parameter.s group as a module PATH — "Oscillator/Filter"
       — rather than an id. Cached with the names and for the same reason: the
       pointer handed out must outlive the call. The id in PluginParam::group
       is a hash of this string, so parameters in the same module compare
       equal without the caller ever seeing the text unless it wants it. */
    std::vector<std::string> param_modules;
    std::vector<uint32_t>    param_ids;

    std::vector<uint8_t> state_blob;   /* scratch for save/load */

    int64_t steady = 0;                /* frames since activate, for steady_time */
};

namespace {

/* ── Host callbacks ───────────────────────────────────────────────────────── */

Instance* self(const clap_host_t* h) {
    return static_cast<Instance*>(h->host_data);
}

void CLAP_ABI host_latency_changed(const clap_host_t* h) {
    /* [main-thread] per clap/ext/latency.h, so asking the plugin here is legal
       — as it also is at open, and nowhere else. */
    Instance* p = self(h);
    if (p->latency && p->plugin && p->activated)
        p->latency_frames.store(p->latency->get(p->plugin), std::memory_order_relaxed);
}
const clap_host_latency_t s_host_latency = { host_latency_changed };

void CLAP_ABI host_params_rescan(const clap_host_t* h, clap_param_rescan_flags flags);
void CLAP_ABI host_params_clear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void CLAP_ABI host_params_request_flush(const clap_host_t* h) {
    self(h)->want_process.store(true, std::memory_order_relaxed);
}
const clap_host_params_t s_host_params = {
    host_params_rescan, host_params_clear, host_params_request_flush
};

void CLAP_ABI host_state_mark_dirty(const clap_host_t*) {}
const clap_host_state_t s_host_state = { host_state_mark_dirty };

const void* CLAP_ABI host_get_extension(const clap_host_t* h, const char* id) {
    (void)h;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &s_host_latency;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))  return &s_host_params;
    if (!std::strcmp(id, CLAP_EXT_STATE))   return &s_host_state;
    return nullptr;   /* notably: no clap.gui — that extension is not implemented here */
}

void CLAP_ABI host_request_restart(const clap_host_t* h) {
    self(h)->want_restart.store(true, std::memory_order_relaxed);
}
void CLAP_ABI host_request_process(const clap_host_t* h) {
    self(h)->want_process.store(true, std::memory_order_relaxed);
}
void CLAP_ABI host_request_callback(const clap_host_t* h) {
    self(h)->want_callback.store(true, std::memory_order_relaxed);
}

/* ── Input/output event lists ─────────────────────────────────────────────── */

uint32_t CLAP_ABI in_events_size(const clap_input_events_t* l) {
    return static_cast<uint32_t>(static_cast<Instance*>(l->ctx)->block_events.size());
}
const clap_event_header_t* CLAP_ABI in_events_get(const clap_input_events_t* l, uint32_t i) {
    Instance* p = static_cast<Instance*>(l->ctx);
    if (i >= p->block_events.size()) return nullptr;
    return &p->block_events[i].header;
}
/* The plugin may push parameter values back (a plugin moving its own knob).
   Accepting and discarding them is honest for an adapter with no CLAP editor to
   reflect them in and no param listener to forward them to; refusing would make
   well-behaved plugins retry. */
bool CLAP_ABI out_events_try_push(const clap_output_events_t*, const clap_event_header_t*) {
    return true;
}

/* ── Parameter name cache ─────────────────────────────────────────────────── */

void refresh_params(Instance* p) {
    p->param_names.clear();
    p->param_modules.clear();
    p->param_ids.clear();
    if (!p->params) return;
    const uint32_t n = p->params->count(p->plugin);
    p->param_names.reserve(n);
    p->param_modules.reserve(n);
    p->param_ids.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info{};
        if (!p->params->get_info(p->plugin, i, &info)) break;
        p->param_names.emplace_back(info.name);
        p->param_modules.emplace_back(info.module);
        p->param_ids.push_back(info.id);
    }
}

void CLAP_ABI host_params_rescan(const clap_host_t* h, clap_param_rescan_flags flags) {
    /* [main-thread] */
    if (flags & (CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL))
        refresh_params(self(h));
}

/* ── Port plan ────────────────────────────────────────────────────────────── */

void build_ports(Instance* p) {
    std::vector<uint32_t> in_counts, out_counts;
    if (p->ports) {
        const uint32_t n_in  = p->ports->count(p->plugin, true);
        const uint32_t n_out = p->ports->count(p->plugin, false);
        for (uint32_t i = 0; i < n_in; ++i) {
            clap_audio_port_info_t info{};
            if (!p->ports->get(p->plugin, i, true, &info)) break;
            in_counts.push_back(info.channel_count);
        }
        for (uint32_t i = 0; i < n_out; ++i) {
            clap_audio_port_info_t info{};
            if (!p->ports->get(p->plugin, i, false, &info)) break;
            out_counts.push_back(info.channel_count);
        }
    }
    for (uint32_t c : in_counts)  p->total_in_ch  += c;
    for (uint32_t c : out_counts) p->total_out_ch += c;

    const size_t bl = p->max_block ? p->max_block : 1;
    p->in_store.assign(static_cast<size_t>(p->total_in_ch)  * bl, 0.0f);
    p->out_store.assign(static_cast<size_t>(p->total_out_ch) * bl, 0.0f);
    p->in_chans.resize(p->total_in_ch);
    p->out_chans.resize(p->total_out_ch);
    for (uint32_t c = 0; c < p->total_in_ch;  ++c) p->in_chans[c]  = p->in_store.data()  + c * bl;
    for (uint32_t c = 0; c < p->total_out_ch; ++c) p->out_chans[c] = p->out_store.data() + c * bl;

    uint32_t base = 0;
    for (uint32_t c : in_counts) {
        clap_audio_buffer_t b{};
        b.data32        = c ? &p->in_chans[base] : nullptr;
        b.data64        = nullptr;
        b.channel_count = c;
        b.latency       = 0;
        b.constant_mask = 0;
        p->in_bufs.push_back(b);
        base += c;
    }
    base = 0;
    for (uint32_t c : out_counts) {
        clap_audio_buffer_t b{};
        b.data32        = c ? &p->out_chans[base] : nullptr;
        b.data64        = nullptr;
        b.channel_count = c;
        b.latency       = 0;
        b.constant_mask = 0;
        p->out_bufs.push_back(b);
        base += c;
    }
}

/* ── State streams ────────────────────────────────────────────────────────── */

struct OStreamCtx { std::vector<uint8_t>* buf; };
int64_t CLAP_ABI ostream_write(const clap_ostream_t* s, const void* data, uint64_t size) {
    auto* c = static_cast<OStreamCtx*>(s->ctx);
    const uint8_t* b = static_cast<const uint8_t*>(data);
    c->buf->insert(c->buf->end(), b, b + size);
    return static_cast<int64_t>(size);
}

struct IStreamCtx { const uint8_t* data; uint64_t len; uint64_t pos; };
int64_t CLAP_ABI istream_read(const clap_istream_t* s, void* out, uint64_t size) {
    auto* c = static_cast<IStreamCtx*>(s->ctx);
    const uint64_t left = c->len - c->pos;
    const uint64_t n    = size < left ? size : left;
    if (n) std::memcpy(out, c->data + c->pos, static_cast<size_t>(n));
    c->pos += n;
    return static_cast<int64_t>(n);   /* 0 at EOF, which CLAP reads as end */
}

/* Sorted, stable, and allocation-free: insertion sort over a vector whose
   capacity was reserved at open. std::stable_sort would allocate. */
void sort_block_events(std::vector<Instance::Event>& v) {
    for (size_t i = 1; i < v.size(); ++i) {
        Instance::Event key = v[i];
        size_t j = i;
        while (j > 0 && v[j - 1].header.time > key.header.time) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = key;
    }
}

} // namespace

/* ── Discovery ────────────────────────────────────────────────────────────── */

uint32_t scan(const char* path, PluginDesc* out, uint32_t cap) {
    if (!path || !*path) return 0;

    void* dso = dso_open(path);
    if (!dso) return 0;               /* not a loadable object: a refusal, not an error */

    auto* entry = static_cast<const clap_plugin_entry_t*>(dso_sym(dso, "clap_entry"));
    if (!entry || !clap_version_is_compatible(entry->clap_version) ||
        !entry->init || !entry->deinit || !entry->get_factory) {
        dso_close(dso);
        return 0;                    /* a shared library, but not a CLAP one */
    }
    if (!entry->init(path)) { dso_close(dso); return 0; }

    auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || !factory->get_plugin_count || !factory->get_plugin_descriptor) {
        entry->deinit();
        dso_close(dso);
        return 0;
    }

    const uint32_t n = factory->get_plugin_count(factory);

    ScanEntry se;
    se.ids.reserve(n); se.names.reserve(n); se.vendors.reserve(n);
    se.instruments.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_plugin_descriptor_t* d = factory->get_plugin_descriptor(factory, i);
        se.ids.emplace_back(d && d->id     ? d->id     : "");
        se.names.emplace_back(d && d->name ? d->name   : "");
        se.vendors.emplace_back(d && d->vendor ? d->vendor : "");

        /* The feature list is a NULL-terminated array of strings in no
           particular order, so this is a scan rather than a lookup at [0]. */
        bool inst = false;
        if (d && d->features) {
            for (const char* const* f = d->features; *f; ++f) {
                if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) { inst = true; break; }
            }
        }
        se.instruments.push_back(inst ? 1 : 0);
    }

    entry->deinit();
    dso_close(dso);

    std::lock_guard<std::mutex> lk(g_scan_mu);
    ScanEntry& cached = (g_scan_cache[path] = std::move(se));
    if (out) {
        const uint32_t m = n < cap ? n : cap;
        for (uint32_t i = 0; i < m; ++i) {
            out[i].format = kPluginFormatClap;
            out[i].id     = cached.ids[i].c_str();
            out[i].name   = cached.names[i].c_str();
            out[i].vendor = cached.vendors[i].c_str();
            out[i].is_instrument = (i < cached.instruments.size() && cached.instruments[i]) ? 1 : 0;
            out[i].index  = i;
        }
    }
    return n;
}

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

Instance* open(const char* path, uint32_t index,
               double sample_rate, uint32_t max_block,
               const char** err) {
    auto fail = [&](std::string msg, Instance* p) -> Instance* {
        if (err) *err = set_err(std::move(msg));
        if (p) clockwork_plugin_clap::close(p);
        return nullptr;
    };

    if (!path || !*path)              return fail("plugin_open: empty path", nullptr);
    if (max_block == 0)               return fail("plugin_open: max_block must be non-zero", nullptr);
    if (!(sample_rate > 0.0))         return fail("plugin_open: sample_rate must be positive", nullptr);

    void* dso = dso_open(path);
    if (!dso)
        return fail(std::string("plugin_open: cannot load '") + path + "': " + dso_error(), nullptr);

    auto* entry = static_cast<const clap_plugin_entry_t*>(dso_sym(dso, "clap_entry"));
    if (!entry || !clap_version_is_compatible(entry->clap_version)) {
        dso_close(dso);
        return fail(std::string("plugin_open: '") + path + "' is not a compatible CLAP plugin",
                    nullptr);
    }

    auto* p = new Instance();
    p->dso         = dso;
    p->entry       = entry;
    p->sample_rate = sample_rate;
    p->max_block   = max_block;

    if (!entry->init(path)) {
        p->entry = nullptr;   /* deinit must not be called when init failed */
        return fail(std::string("plugin_open: clap_entry.init failed for '") + path + "'", p);
    }
    p->entry_inited = true;

    auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory)
        return fail(std::string("plugin_open: '") + path + "' has no clap.plugin-factory", p);

    const uint32_t count = factory->get_plugin_count(factory);
    if (index >= count)
        return fail("plugin_open: plugin index out of range", p);

    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, index);
    if (!desc || !desc->id)
        return fail("plugin_open: no descriptor at that index", p);

    p->host.clap_version      = CLAP_VERSION;
    p->host.host_data         = p;
    p->host.name              = CLOCKWORK_PRODUCT_NAME;
    p->host.vendor            = "Clockwork";
    p->host.url               = "";
    p->host.version           = "1.0.0";
    p->host.get_extension     = host_get_extension;
    p->host.request_restart   = host_request_restart;
    p->host.request_process   = host_request_process;
    p->host.request_callback  = host_request_callback;

    p->plugin = factory->create_plugin(factory, &p->host, desc->id);
    if (!p->plugin)
        return fail(std::string("plugin_open: create_plugin failed for '") + desc->id + "'", p);

    if (!p->plugin->init(p->plugin)) {
        /* Failed init: destroy is still the way to release it. */
        return fail(std::string("plugin_open: plugin init failed for '") + desc->id + "'", p);
    }

    p->params  = static_cast<const clap_plugin_params_t*>(
        p->plugin->get_extension(p->plugin, CLAP_EXT_PARAMS));
    p->state   = static_cast<const clap_plugin_state_t*>(
        p->plugin->get_extension(p->plugin, CLAP_EXT_STATE));
    p->latency = static_cast<const clap_plugin_latency_t*>(
        p->plugin->get_extension(p->plugin, CLAP_EXT_LATENCY));
    p->ports   = static_cast<const clap_plugin_audio_ports_t*>(
        p->plugin->get_extension(p->plugin, CLAP_EXT_AUDIO_PORTS));

    /* Note ports: asked once, because which dialect to speak is decided per
       event on the audio thread and must not be a call into the plugin. */
    if (const auto* np = static_cast<const clap_plugin_note_ports_t*>(
            p->plugin->get_extension(p->plugin, CLAP_EXT_NOTE_PORTS))) {
        if (np->count(p->plugin, true) > 0) {
            clap_note_port_info_t info{};
            if (np->get(p->plugin, 0, true, &info)) {
                p->has_note_port = true;
                p->note_dialects = info.supported_dialects;
            }
        }
    }

    build_ports(p);
    refresh_params(p);

    p->block_events.reserve(Instance::kEventCap);
    p->in_events.ctx  = p;
    p->in_events.size = in_events_size;
    p->in_events.get  = in_events_get;
    p->out_events.ctx = p;
    p->out_events.try_push = out_events_try_push;

    if (!p->plugin->activate(p->plugin, sample_rate, 1, max_block))
        return fail(std::string("plugin_open: activate failed for '") + desc->id + "'", p);
    p->activated = true;

    if (p->latency)   /* [main-thread & active] — both true right here */
        p->latency_frames.store(p->latency->get(p->plugin), std::memory_order_relaxed);

    if (!p->plugin->start_processing(p->plugin))
        return fail(std::string("plugin_open: start_processing failed for '") + desc->id + "'", p);
    p->processing = true;

    if (err) *err = nullptr;
    return p;
}

void close(Instance* p) {
    if (!p) return;
    if (p->plugin) {
        if (p->processing) p->plugin->stop_processing(p->plugin);
        if (p->activated)  p->plugin->deactivate(p->plugin);
        p->plugin->destroy(p->plugin);
    }
    if (p->entry && p->entry_inited) p->entry->deinit();
    dso_close(p->dso);
    delete p;
}

/* ── Audio ────────────────────────────────────────────────────────────────── */

namespace {

clap_beattime to_beattime(double beats) {
    return static_cast<clap_beattime>(std::llround(beats * static_cast<double>(CLAP_BEATTIME_FACTOR)));
}
clap_sectime to_sectime(double seconds) {
    return static_cast<clap_sectime>(std::llround(seconds * static_cast<double>(CLAP_SECTIME_FACTOR)));
}

/* Fill the block's transport event from the grid the block renders against
   (plugin_transport.h blockTimeline). Returns false when there is none.
   Seconds are the beats at the tempo — the position on a constant-tempo
   grid, which is the only grid a timeline snapshot describes. No loop, no
   pre-roll: clockwork has neither. */
bool pr_transport(Instance* p, int64_t block_time) {
    clockwork::Timeline grid;
    if (!clockwork_plugin::blockTimeline(p->has_timeline ? &p->timeline : nullptr, p->clock, grid))
        return false;
    const clockwork_plugin::BlockTransport bt = clockwork_plugin::blockTransport(grid, block_time);
    clap_event_transport_t& t = p->transport;
    t = clap_event_transport_t{};
    t.header.size     = sizeof(clap_event_transport_t);
    t.header.time     = 0;
    t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    t.header.type     = CLAP_EVENT_TRANSPORT;
    t.header.flags    = 0;
    t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
            | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE
            | (bt.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
    t.song_pos_beats   = to_beattime(bt.beat);
    t.song_pos_seconds = to_sectime(bt.beat * 60.0 / bt.bpm);
    t.tempo            = bt.bpm;
    t.tempo_inc        = 0.0;
    t.bar_start        = to_beattime(bt.barStartBeat);
    t.bar_number       = static_cast<int32_t>(bt.bar);
    t.tsig_num         = static_cast<uint16_t>(bt.meterNum);
    t.tsig_denom       = static_cast<uint16_t>(bt.meterDen);
    return true;
}

}  // namespace

void set_clock(Instance* p, const ClockworkClockState* clock) {
    if (p) p->clock = clock;
}

void set_timeline(Instance* p, const ClockworkTimeline* timeline) {
    if (!p) return;
    p->has_timeline = timeline != nullptr;
    if (timeline) p->timeline = *timeline;
}

void process(Instance* p,
             const float* const* in, uint32_t n_in,
             float* const* out, uint32_t n_out,
             uint32_t frames, int64_t block_time) {
    if (!p || !p->plugin || !p->processing) {
        for (uint32_t c = 0; c < n_out; ++c)
            if (out && out[c]) std::memset(out[c], 0, frames * sizeof(float));
        return;
    }
    if (frames > p->max_block) frames = p->max_block;

    /* Drain the parameter ring into the block's event list, then sort it:
       CLAP requires the input event list be ordered by time, and a caller may
       queue frame 300 before frame 10. */
    p->block_events.clear();
    uint32_t tail = p->ev_tail.load(std::memory_order_relaxed);
    const uint32_t head = p->ev_head.load(std::memory_order_acquire);
    for (; tail != head; ++tail) {
        Instance::Event ev = p->evq[tail % Instance::kEventCap];
        /* CLAP requires time < frames_count. An offset past the end of the
           block is clamped to its last frame rather than dropped or deferred:
           plugin_host.h scopes the offset to "the next block". */
        if (ev.header.time >= frames) ev.header.time = frames ? frames - 1 : 0;
        if (p->block_events.size() < Instance::kEventCap)
            p->block_events.push_back(ev);
    }
    p->ev_tail.store(tail, std::memory_order_release);
    sort_block_events(p->block_events);

    /* Stage the inputs; anything clockwork did not supply is silence. */
    for (uint32_t c = 0; c < p->total_in_ch; ++c) {
        float* dst = p->in_chans[c];
        if (in && c < n_in && in[c]) std::memcpy(dst, in[c], frames * sizeof(float));
        else                         std::memset(dst, 0, frames * sizeof(float));
    }
    /* Clear the outputs so a plugin that writes nothing yields silence rather
       than the last block. */
    for (uint32_t c = 0; c < p->total_out_ch; ++c)
        std::memset(p->out_chans[c], 0, frames * sizeof(float));

    /* MUSICAL TIME, from the session clock or the bound timeline: the same
       position VST3 gets in its ProcessContext (plugin_transport.h), in
       CLAP's fixed point. With neither the plugin runs free, which CLAP
       spells as no transport. */
    const bool has_transport = pr_transport(p, block_time);

    clap_process_t pr{};
    pr.steady_time         = p->steady;
    pr.frames_count        = frames;
    pr.transport           = has_transport ? &p->transport : nullptr;
    pr.audio_inputs        = p->in_bufs.empty()  ? nullptr : p->in_bufs.data();
    pr.audio_outputs       = p->out_bufs.empty() ? nullptr : p->out_bufs.data();
    pr.audio_inputs_count  = static_cast<uint32_t>(p->in_bufs.size());
    pr.audio_outputs_count = static_cast<uint32_t>(p->out_bufs.size());
    pr.in_events           = &p->in_events;
    pr.out_events          = &p->out_events;

    clap_process_status status;
    {
        SuspendRtGuard hole;   /* the one place clockwork stops asserting */
        status = p->plugin->process(p->plugin, &pr);
    }

    const bool ok = (status != CLAP_PROCESS_ERROR);
    for (uint32_t c = 0; c < n_out; ++c) {
        if (!out || !out[c]) continue;
        if (ok && c < p->total_out_ch) std::memcpy(out[c], p->out_chans[c], frames * sizeof(float));
        else                           std::memset(out[c], 0, frames * sizeof(float));
    }

    p->steady += frames;
}

uint32_t latency(const Instance* p) {
    /* Cached, not polled — see the file header. */
    return p ? p->latency_frames.load(std::memory_order_relaxed) : 0;
}

/* ── Parameters ───────────────────────────────────────────────────────────── */

uint32_t param_count(const Instance* p) {
    if (!p || !p->params) return 0;
    return p->params->count(p->plugin);
}

/* Static storage, so out->group_name is valid after this returns even when
   the module cache is short. A local temporary would dangle. */
static const std::string kEmptyModule;

int param_info(const Instance* p, uint32_t index, PluginParam* out) {
    if (!p || !p->params || !out) return 0;
    clap_param_info_t info{};
    if (!p->params->get_info(p->plugin, index, &info)) return 0;

    out->id   = info.id;
    out->min  = info.min_value;
    out->max  = info.max_value;
    /* The name must outlive this call, so it comes from the cache built at
       open. If a rescan raced us the cache may be short, in which case the name
       comes back empty rather than dangling. */
    out->name = (index < p->param_names.size()) ? p->param_names[index].c_str() : "";

    /* GROUPING AND AUTOMATABILITY WERE NEVER SET HERE, and both defaults are
       actively wrong: a caller zero-inits PluginParam, so every CLAP parameter
       came back automatable = 0, and a host that filters on it — as any host
       showing a panel must, since that flag is the only thing separating
       controls from internals — rendered a CLAP plugin with no parameters at
       all. It looked like a plugin with nothing to offer rather than a field
       nobody filled in. */
    out->automatable = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;

    /* CLAP names the group directly, where VST3 gives a number. Hashing the
       module path gives the same shape as VST3's unit id — an opaque integer
       that compares equal within a group — so a caller groups identically for
       both formats and never has to know which one it is holding. */
    const std::string& mod =
        (index < p->param_modules.size()) ? p->param_modules[index] : kEmptyModule;
    out->group      = static_cast<int32_t>(std::hash<std::string>{}(mod));
    out->group_name = mod.c_str();

    double v = info.default_value;
    if (p->params->get_value) p->params->get_value(p->plugin, info.id, &v);
    out->value = v;
    return 1;
}

/* Claim the next ring slot, or NULL when full. Every event producer goes
   through here so the single-producer discipline is in one place. */
static Instance::Event* claim(Instance* p, uint32_t& head) {
    head = p->ev_head.load(std::memory_order_relaxed);
    const uint32_t tail = p->ev_tail.load(std::memory_order_acquire);
    if (head - tail >= Instance::kEventCap) return nullptr;   /* full: drop */
    Instance::Event* ev = &p->evq[head % Instance::kEventCap];
    std::memset(ev, 0, sizeof *ev);
    return ev;
}

void param_set(Instance* p, uint32_t id, double value,
               uint32_t frame_offset) {
    if (!p) return;
    uint32_t head;
    Instance::Event* slot = claim(p, head);
    if (!slot) return;
    clap_event_param_value_t& ev = slot->param;
    ev.header.size     = sizeof(clap_event_param_value_t);
    ev.header.time     = frame_offset;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type     = CLAP_EVENT_PARAM_VALUE;
    ev.header.flags    = 0;
    ev.param_id        = id;
    ev.cookie          = nullptr;
    ev.note_id         = -1;
    ev.port_index      = -1;
    ev.channel         = -1;
    ev.key             = -1;
    ev.value           = value;
    p->ev_head.store(head + 1, std::memory_order_release);
}

/* Three raw MIDI bytes on note port 0, for a plugin that speaks that dialect. */
static void midi_bytes(Instance* p, uint8_t b0, uint8_t b1, uint8_t b2, uint32_t frame_offset) {
    uint32_t head;
    Instance::Event* slot = claim(p, head);
    if (!slot) return;
    clap_event_midi_t& ev = slot->midi;
    ev.header.size     = sizeof(clap_event_midi_t);
    ev.header.time     = frame_offset;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type     = CLAP_EVENT_MIDI;
    ev.port_index      = 0;
    ev.data[0] = b0; ev.data[1] = b1; ev.data[2] = b2;
    p->ev_head.store(head + 1, std::memory_order_release);
}

void note(Instance* p, bool on, int16_t channel, int16_t pitch,
          float velocity, uint32_t frame_offset) {
    if (!p || !p->has_note_port) return;
    if (channel < 0 || channel > 15 || pitch < 0 || pitch > 127) return;
    if (p->note_dialects & CLAP_NOTE_DIALECT_CLAP) {
        uint32_t head;
        Instance::Event* slot = claim(p, head);
        if (!slot) return;
        clap_event_note_t& ev = slot->note;
        ev.header.size     = sizeof(clap_event_note_t);
        ev.header.time     = frame_offset;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type     = on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
        ev.note_id         = -1;
        ev.port_index      = 0;
        ev.channel         = channel;
        ev.key             = pitch;
        ev.velocity        = velocity;
        p->ev_head.store(head + 1, std::memory_order_release);
    } else if (p->note_dialects & CLAP_NOTE_DIALECT_MIDI) {
        const uint8_t v = static_cast<uint8_t>(velocity <= 0.f ? 0 : velocity >= 1.f ? 127 : velocity * 127.f + 0.5f);
        midi_bytes(p, static_cast<uint8_t>((on ? 0x90 : 0x80) | channel),
                   static_cast<uint8_t>(pitch), v, frame_offset);
    }
}

/* Controllers and bend are MIDI's, not CLAP's: the CLAP dialect has no CC
   event (a plugin maps them to parameters itself), so both go as MIDI bytes
   to a plugin that accepts them and are dropped, stated here, for one that
   does not. */
void cc(Instance* p, int16_t channel, uint8_t number, float value, uint32_t frame_offset) {
    if (!p || !p->has_note_port || !(p->note_dialects & CLAP_NOTE_DIALECT_MIDI)) return;
    if (channel < 0 || channel > 15) return;
    const uint8_t v = static_cast<uint8_t>(value <= 0.f ? 0 : value >= 1.f ? 127 : value * 127.f + 0.5f);
    midi_bytes(p, static_cast<uint8_t>(0xB0 | channel), number & 0x7F, v, frame_offset);
}

void pitch_bend(Instance* p, int16_t channel, float bend, uint32_t frame_offset) {
    if (!p || !p->has_note_port || !(p->note_dialects & CLAP_NOTE_DIALECT_MIDI)) return;
    if (channel < 0 || channel > 15) return;
    if (bend < -1.f) bend = -1.f;
    if (bend >  1.f) bend =  1.f;
    const uint32_t v = static_cast<uint32_t>((bend + 1.f) * 0.5f * 16383.f + 0.5f);
    midi_bytes(p, static_cast<uint8_t>(0xE0 | channel), v & 0x7F, (v >> 7) & 0x7F, frame_offset);
}

void all_notes_off(Instance* p, uint32_t frame_offset) {
    if (!p || !p->has_note_port) return;
    if (p->note_dialects & CLAP_NOTE_DIALECT_MIDI) {
        for (uint8_t ch = 0; ch < 16; ++ch)
            midi_bytes(p, static_cast<uint8_t>(0xB0 | ch), 123, 0, frame_offset);
    }
    if (p->note_dialects & CLAP_NOTE_DIALECT_CLAP) {
        /* CLAP_EVENT_NOTE_CHOKE with key -1 / channel -1 addresses every
           voice at once — one event, not two thousand. */
        uint32_t head;
        Instance::Event* slot = claim(p, head);
        if (!slot) return;
        clap_event_note_t& ev = slot->note;
        ev.header.size     = sizeof(clap_event_note_t);
        ev.header.time     = frame_offset;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type     = CLAP_EVENT_NOTE_CHOKE;
        ev.note_id         = -1;
        ev.port_index      = 0;
        ev.channel         = -1;
        ev.key             = -1;
        p->ev_head.store(head + 1, std::memory_order_release);
    }
}

/* ── State ────────────────────────────────────────────────────────────────── */

uint32_t state_save(Instance* p, uint8_t* out, uint32_t cap) {
    if (!p || !p->state || !p->state->save) return 0;

    p->state_blob.clear();
    OStreamCtx ctx{ &p->state_blob };
    clap_ostream_t os{};
    os.ctx   = &ctx;
    os.write = ostream_write;
    if (!p->state->save(p->plugin, &os)) return 0;

    const uint32_t n = static_cast<uint32_t>(p->state_blob.size());
    if (out && cap >= n && n) std::memcpy(out, p->state_blob.data(), n);
    return n;
}

int state_load(Instance* p, const uint8_t* bytes, uint32_t len) {
    if (!p || !p->state || !p->state->load || !bytes) return 0;
    IStreamCtx ctx{ bytes, len, 0 };
    clap_istream_t is{};
    is.ctx  = &ctx;
    is.read = istream_read;
    return p->state->load(p->plugin, &is) ? 1 : 0;
}

}  // namespace clockwork_plugin_clap
