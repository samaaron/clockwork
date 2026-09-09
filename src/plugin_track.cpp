// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_track.cpp — see plugin_track.h for what a track is and where it sits.
 *
 * THE PUBLICATION SCHEME, because it is the only subtle thing here.
 *
 * The audio thread walks every track every block, and delivers scheduled
 * notes into their instruments from inside the block. The control thread
 * edits tracks and chains while it does. There is no ordering between two
 * atomics that stops a reader seeing a new length against an old array, so
 * nothing here is published piecemeal: the whole studio is an IMMUTABLE
 * Snapshot. An edit builds a complete new one, swaps one pointer, and the
 * audio thread's next read sees all of it or none of it.
 *
 * RETIRING THE OLD ONE is the part with a real hazard. The audio thread may
 * be inside the old snapshot at the instant of the swap, so freeing it there
 * would pull the array out from under a live walk. The control thread instead
 * waits for the audio thread to publish a block epoch two past the swap
 * before freeing — and gives up after a bounded time, because an engine that
 * is not running audio at all never advances that epoch and an edit must not
 * hang on it. See retire().
 *
 * Tracks and nodes are heap objects with stable addresses, owned by the
 * control thread and referenced from snapshots. A track's gain and mute are
 * atomics ON the track, so a fader move is a store and not a republish; a
 * node's parameter-name table is built once at load and pointed at from every
 * snapshot that includes the node. Both are freed only after the last
 * snapshot that referenced them has been retired, which the order of
 * operations in remove() guarantees.
 */
#include "plugin_track.h"

#include "clock/timeline_mirror.h"
#include "plugin_discovery.h"
#include "plugin_editor_window.h"
#include "plugin_host.h"
#include "clockwork_path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Where the lanes start in the staging the process walk is handed, and the
// number the model publishes as a track's channels. Whoever owns the staging
// says: the bridge (src/native/PluginBridgeMain.cpp) lays its own out from
// zero and tells the model where the engine's lanes are so the channel
// numbers it reports are the engine's. Atomic because the engine's lane base
// changes on a cold swap while the audio thread is walking.
std::atomic<uint32_t> g_lane_base{0};

ClockworkTrackLoadFn g_load_fn  = nullptr;
void*          g_load_ctx = nullptr;

// The largest block a track will ever be handed. Plugins are opened against
// this, because both formats want the maximum up front so they can allocate
// before the audio thread exists.
constexpr uint32_t kMaxBlock = 2048;
// A track is stereo. Its lanes are, its plugins are run as, and a mono plugin
// is given both channels and asked for both back.
constexpr uint32_t kChans = 2;

struct ParamName {
    uint32_t    id;
    std::string name;
    double      min;     // the plugin's range, for the echo of a set by name
    double      max;
};

// One plugin in a chain. Control thread owns every field; the audio thread
// reads only through a Snapshot. Heap-allocated so its address is stable:
// the plugin's param-edit listener points at `handle`.
struct Node {
    ClockworkTrackHandle handle = CLOCKWORK_TRACK_NO_HANDLE;
    ClockworkTrackId     track  = CLOCKWORK_TRACK_NONE;
    HostedPlugin*  plugin = nullptr;
    bool           instrument = false;
    bool           bypass = false;
    // The MIDI channel an instrument listens on, 0 for all. See the header.
    int16_t        channel = 0;
    std::string    name, vendor, id, format, path;
    uint32_t       index = 0;
    std::vector<ParamName> params;   // built at load, never changed
};

struct Track {
    ClockworkTrackId  id   = CLOCKWORK_TRACK_NONE;
    uint32_t    slot = 0;
    std::string name;
    std::atomic<float> gain{1.0f};
    std::atomic<int>   mute{0};
    float gainSmoothed = 1.0f;       // audio thread only
    // The timeline the chain renders against: 0 is the session clock. Read
    // through the snapshot (a bind republishes), so no atomic.
    int32_t     timeline = 0;
    std::string timelineName = "link";
    // The last snapshot read cleanly from the mirror, so a torn read keeps
    // the block on the grid it was on. Audio thread only.
    ClockworkTimeline boundSnapshot {};
    bool        boundValid = false;
    std::vector<std::unique_ptr<Node>> nodes;
};

struct SnapNode {
    HostedPlugin*    plugin;
    ClockworkTrackHandle   handle;
    bool             instrument;
    bool             bypass;
    int16_t          channel;
    const ParamName* params;
    uint32_t         nparams;
};

struct SnapTrack {
    ClockworkTrackId id;
    uint32_t   slot;
    Track*     track;
    char       name[CLOCKWORK_TRACK_NAME_MAX];
    int32_t    timeline_id;
    uint32_t   count;
    SnapNode   node[CLOCKWORK_TRACK_MAX_NODES];
};

// Immutable once published.
struct Snapshot {
    uint32_t   count = 0;
    SnapTrack  track[CLOCKWORK_TRACK_MAX] {};
    SnapTrack* bySlot[CLOCKWORK_TRACK_MAX] {};
};

std::atomic<Snapshot*> g_snap{nullptr};   // what the audio thread reads
std::atomic<uint64_t>  g_epoch{0};        // bumped by the audio thread per block

std::mutex g_mu;                          // control-thread mutations only
std::vector<std::unique_ptr<Track>> g_tracks;   // display order
ClockworkTrackId     g_nextTrack  = 1;          // 0 is "nothing"; never reused
ClockworkTrackHandle g_nextHandle = 1;
bool           g_slotUsed[CLOCKWORK_TRACK_MAX] {};

const ClockworkClockState* g_clock = nullptr;

// The engine's mirror of the follower timelines (clockwork_track_set_timelines):
// slot k is timeline id k + 1. Read on the audio thread; set once at boot.
std::atomic<const ClockworkTimelineMirrorSlot*> g_timelines{nullptr};
std::atomic<uint32_t>                     g_timeline_slots{0};

// Scratch, static because this is the audio thread. `work` holds the track
// being processed; `gen` whichever instrument is generating right now — one
// buffer reused down the chain, since instruments are summed one at a time.
alignas(16) float g_work[kChans * kMaxBlock];
alignas(16) float g_gen[kChans * kMaxBlock];
alignas(16) float g_silence[kMaxBlock];

void put(char* dst, size_t cap, const std::string& src) {
    std::snprintf(dst, cap, "%s", src.c_str());
}

// ── The block ────────────────────────────────────────────────────────────────

// The grid this track's chain renders against this block: null for the
// session clock (what plugin_set_clock gave every plugin), else the bound
// timeline's snapshot out of the engine's mirror. A slot nothing holds, an
// id past the mirror, or no mirror at all is the 60 BPM placeholder — the
// answer every unheld timeline gives, so the binding is honoured rather than
// silently swapped for the clock. A torn read keeps the last clean one.
const ClockworkTimeline* track_timeline(const SnapTrack& st, ClockworkTimeline& out) {
    if (st.timeline_id <= 0) return nullptr;
    Track* t = st.track;
    const ClockworkTimelineMirrorSlot* slots = g_timelines.load(std::memory_order_acquire);
    const uint32_t k = static_cast<uint32_t>(st.timeline_id - 1);
    if (slots && k < g_timeline_slots.load(std::memory_order_relaxed)) {
        if (clockwork::timelineMirrorRead(slots[k], out)) {
            t->boundValid = out.id > 0;
            if (t->boundValid) t->boundSnapshot = out;
        }
        if (t->boundValid) { out = t->boundSnapshot; return &out; }
    }
    out = clockwork_timeline_placeholder(-1);
    return &out;
}

void run_chain(const SnapTrack& st, float* const* work, uint32_t frames, int64_t block_time) {
    const float* ins[kChans];
    float*       gen[kChans] = { g_gen, g_gen + kMaxBlock };

    ClockworkTimeline bound;
    const ClockworkTimeline* timeline = track_timeline(st, bound);

    for (uint32_t i = 0; i < st.count; ++i) {
        const SnapNode& n = st.node[i];
        if (!n.plugin || n.bypass) continue;
        plugin_set_timeline(n.plugin, timeline);

        if (n.instrument) {
            // Generate into scratch, then MIX IN: an instrument's output is an
            // ADDITION to what is on the track, not a replacement for it. Fed
            // silence rather than the track, because an instrument that
            // happens to have an audio input would otherwise become an
            // unintended effect.
            for (uint32_t ch = 0; ch < kChans; ++ch) {
                std::memset(gen[ch], 0, sizeof(float) * frames);
                ins[ch] = g_silence;
            }
            plugin_process(n.plugin, ins, kChans, gen, kChans, frames, block_time);
            for (uint32_t ch = 0; ch < kChans; ++ch) {
                float* dst = work[ch];
                const float* src = gen[ch];
                for (uint32_t f = 0; f < frames; ++f) dst[f] += src[f];
            }
        } else {
            for (uint32_t ch = 0; ch < kChans; ++ch) ins[ch] = work[ch];
            // Same buffers in and out. Both formats permit it, and the
            // alternative is a second copy of the block for no benefit.
            plugin_process(n.plugin, ins, kChans, work, kChans, frames, block_time);
        }
    }
}

void track_process_at(float* const* in, uint32_t n_in, float* const* out, uint32_t n_out,
                      uint32_t frames, int64_t block_time, uint32_t base) {
    // Published before the walk, so a control thread that swaps mid-block can
    // tell that this block has begun and must be waited out.
    g_epoch.fetch_add(1, std::memory_order_acq_rel);

    if (!in || frames == 0) return;
    if (frames > kMaxBlock) frames = kMaxBlock;

    const Snapshot* s = g_snap.load(std::memory_order_acquire);
    float* work[kChans] = { g_work, g_work + kMaxBlock };

    for (uint32_t slot = 0; slot < CLOCKWORK_TRACK_MAX; ++slot) {
        const uint32_t ch = base + 2 * slot;
        if (ch + 1 >= n_in) break;                 // no return lane for this slot
        float* ret[kChans] = { in[ch], in[ch + 1] };
        if (!ret[0] || !ret[1]) continue;

        const SnapTrack* st = s ? s->bySlot[slot] : nullptr;
        if (!st) {
            // Nothing lives here. Its return lane must read as silence rather
            // than as whatever the last track to hold the slot left behind.
            std::memset(ret[0], 0, sizeof(float) * frames);
            std::memset(ret[1], 0, sizeof(float) * frames);
            continue;
        }

        // What the graph sent LAST block. The output staging is not cleared
        // between blocks, so it is still there.
        for (uint32_t c = 0; c < kChans; ++c) {
            const float* src = (ch + c < n_out && out) ? out[ch + c] : nullptr;
            if (src) std::memcpy(work[c], src, sizeof(float) * frames);
            else     std::memset(work[c], 0, sizeof(float) * frames);
        }

        run_chain(*st, work, frames, block_time);

        // Post-chain gain, ramped across the block so a fader move does not
        // step. Mute is a gain of zero with the same ramp: no click either way.
        Track* t = st->track;
        const float target = t->mute.load(std::memory_order_relaxed)
                           ? 0.0f : t->gain.load(std::memory_order_relaxed);
        float g = t->gainSmoothed;
        const float step = (target - g) / static_cast<float>(frames);
        for (uint32_t f = 0; f < frames; ++f) {
            g += step;
            ret[0][f] = work[0][f] * g;
            ret[1][f] = work[1][f] * g;
        }
        t->gainSmoothed = target;
    }
}

// ── Lookups on the snapshot — audio-thread safe ──────────────────────────────

const SnapTrack* snap_track(const Snapshot* s, ClockworkTrackId t) {
    if (!s || t == CLOCKWORK_TRACK_NONE) return nullptr;
    for (uint32_t i = 0; i < s->count; ++i)
        if (s->track[i].id == t) return &s->track[i];
    return nullptr;
}

const SnapNode* snap_node(const Snapshot* s, ClockworkTrackHandle h, const SnapTrack** owner = nullptr) {
    if (!s || h == CLOCKWORK_TRACK_NO_HANDLE) return nullptr;
    for (uint32_t i = 0; i < s->count; ++i)
        for (uint32_t j = 0; j < s->track[i].count; ++j)
            if (s->track[i].node[j].handle == h) {
                if (owner) *owner = &s->track[i];
                return &s->track[i].node[j];
            }
    return nullptr;
}

bool ieq(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        const int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

// Exact match first across the whole table, then case-insensitive: a plugin
// with both "Cutoff" and "cutoff" (it happens) gets the one that was typed.
const ParamName* node_param_named(const SnapNode& n, const char* name) {
    for (uint32_t i = 0; i < n.nparams; ++i)
        if (std::strcmp(n.params[i].name.c_str(), name) == 0) return &n.params[i];
    for (uint32_t i = 0; i < n.nparams; ++i)
        if (ieq(n.params[i].name.c_str(), name)) return &n.params[i];
    return nullptr;
}

// ── Parameter edits coming BACK from a plugin ────────────────────────────────

std::mutex          g_editMu;     // guards the pair below, never held while calling
ClockworkTrackParamEditFn g_editFn  = nullptr;
void*               g_editCtx = nullptr;

void node_param_edit(void* ctx, uint32_t id, double normalized) {
    if (!ctx) return;
    const ClockworkTrackHandle h = *static_cast<const ClockworkTrackHandle*>(ctx);
    // Copy under the lock and release it BEFORE calling out: the listener is
    // someone else's code and may detach itself from inside the call.
    ClockworkTrackParamEditFn fn;
    void* c;
    {
        std::lock_guard<std::mutex> lk(g_editMu);
        fn = g_editFn;
        c  = g_editCtx;
    }
    if (fn) fn(c, h, id, normalized, 1);
}

// A set by name, echoed as the host's (own = 0). Audio thread: the lock is
// only ever contended by attach/detach, so a failed try is a dropped echo,
// never a wait.
void node_param_set_by_name(const SnapNode& n, const ParamName& p, double value,
                            uint32_t frame_offset) {
    plugin_param_set(n.plugin, p.id, value, frame_offset);
    ClockworkTrackParamEditFn fn = nullptr;
    void* c = nullptr;
    if (g_editMu.try_lock()) {
        fn = g_editFn;
        c  = g_editCtx;
        g_editMu.unlock();
    }
    if (!fn) return;
    const double span = p.max - p.min;
    const double norm = span > 0.0 ? (value - p.min) / span : 0.0;
    fn(c, n.handle, p.id, norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm), 0);
}

// ── Publication ──────────────────────────────────────────────────────────────

// Free a snapshot the audio thread may still be reading. Two epochs, because
// the first observed increment may be the very block that loaded the old
// pointer. The timeout is for an engine with no audio thread running — every
// test process, every application before its device opens — which never
// advances the epoch; there the old snapshot is provably unreachable and
// freeing is right. An epoch of zero means no block has ever run: free at
// once rather than pay the timeout on every edit before the device opens.
void retire(Snapshot* old) {
    if (!old) return;
    const uint64_t start = g_epoch.load(std::memory_order_acquire);
    if (start == 0) { delete old; return; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (g_epoch.load(std::memory_order_acquire) < start + 2) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    delete old;
}

// Build and publish from g_tracks. Caller holds g_mu.
void republish() {
    Snapshot* next = new Snapshot();
    for (const auto& t : g_tracks) {
        if (next->count >= CLOCKWORK_TRACK_MAX) break;
        SnapTrack& st = next->track[next->count++];
        st.id    = t->id;
        st.slot  = t->slot;
        st.track = t.get();
        put(st.name, sizeof st.name, t->name);
        st.timeline_id = t->timeline;
        st.count = 0;
        for (const auto& n : t->nodes) {
            if (st.count >= CLOCKWORK_TRACK_MAX_NODES) break;
            if (!n->plugin) continue;
            SnapNode& sn = st.node[st.count++];
            sn.plugin     = n->plugin;
            sn.handle     = n->handle;
            sn.instrument = n->instrument;
            sn.bypass     = n->bypass;
            sn.channel    = n->channel;
            sn.params     = n->params.data();
            sn.nparams    = static_cast<uint32_t>(n->params.size());
        }
        next->bySlot[t->slot] = &st;
    }
    Snapshot* old = g_snap.exchange(next, std::memory_order_acq_rel);
    retire(old);
}

// Caller holds g_mu.
Track* find_track(ClockworkTrackId t) {
    if (t == CLOCKWORK_TRACK_NONE) return nullptr;
    for (auto& tr : g_tracks) if (tr->id == t) return tr.get();
    return nullptr;
}

Track* find_track_by_name(const char* name) {
    if (!name || !*name) return nullptr;
    for (auto& tr : g_tracks) if (tr->name == name) return tr.get();
    return nullptr;
}

Node* find_node(ClockworkTrackHandle h, Track** owner = nullptr) {
    if (h == CLOCKWORK_TRACK_NO_HANDLE) return nullptr;
    for (auto& tr : g_tracks)
        for (auto& n : tr->nodes)
            if (n->handle == h) {
                if (owner) *owner = tr.get();
                return n.get();
            }
    return nullptr;
}

const char* check_name(const char* name, const Track* self) {
    if (!name || !*name) return "a track needs a name";
    if (std::strlen(name) >= CLOCKWORK_TRACK_NAME_MAX) return "track name too long";
    for (auto& tr : g_tracks)
        if (tr.get() != self && tr->name == name) return "a track with that name already exists";
    return nullptr;
}

void fill_info(const Track& t, ClockworkTrackInfo* out) {
    ClockworkTrackInfo e {};
    e.id   = t.id;
    e.slot = t.slot;
    put(e.name, sizeof e.name, t.name);
    const uint32_t base = g_lane_base.load(std::memory_order_relaxed);
    e.send_channel   = base + 2 * t.slot;
    e.return_channel = base + 2 * t.slot;
    e.gain = t.gain.load(std::memory_order_relaxed);
    e.mute = t.mute.load(std::memory_order_relaxed);
    e.node_count = static_cast<uint32_t>(t.nodes.size());
    e.timeline_id = t.timeline;
    put(e.timeline, sizeof e.timeline, t.timelineName);
    *out = e;
}

void fill_node(const Node& n, ClockworkTrackNode* out) {
    ClockworkTrackNode e {};
    e.handle        = n.handle;
    e.track         = n.track;
    e.is_instrument = n.instrument;
    e.bypass        = n.bypass;
    e.channel       = n.channel;
    e.latency       = n.plugin ? plugin_latency(n.plugin) : 0;
    put(e.name,   sizeof e.name,   n.name);
    put(e.vendor, sizeof e.vendor, n.vendor);
    put(e.id,     sizeof e.id,     n.id);
    put(e.format, sizeof e.format, n.format);
    put(e.path,   sizeof e.path,   n.path);
    e.index = n.index;
    *out = e;
}

// Close plugins AFTER the snapshot that referenced them has been retired —
// closing first would pull a plugin's code out from under a block still
// inside it. Callers collect the dead, republish, then hand them here.
void close_all(std::vector<std::unique_ptr<Node>>& dead) {
    for (auto& n : dead) {
        if (!n->plugin) continue;
        clockwork_editor_window_forget(n->plugin);
        plugin_close(n->plugin);
        n->plugin = nullptr;
    }
    dead.clear();
}

// The audio thread's or the control thread's view — see the header: safe from
// the audio thread and from the (single) thread that mutates, not from
// arbitrary others.
const Snapshot* current() { return g_snap.load(std::memory_order_acquire); }

// ── Rig files ────────────────────────────────────────────────────────────────
//
// Hand-rolled JSON in both directions. The rig's shape is small and fixed,
// and a dependency on a JSON library for a few hundred bytes of structure is
// not worth what it costs a build that has to carry it to three platforms.

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const uint8_t* d, size_t n) {
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(d[i]) << 16)
                         | (i + 1 < n ? static_cast<uint32_t>(d[i + 1]) << 8 : 0)
                         | (i + 2 < n ? static_cast<uint32_t>(d[i + 2]) : 0);
        o += kB64[(v >> 18) & 63];
        o += kB64[(v >> 12) & 63];
        o += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
        o += i + 2 < n ? kB64[v & 63] : '=';
    }
    return o;
}

std::vector<uint8_t> b64_decode(const std::string& s) {
    std::vector<uint8_t> o;
    uint32_t acc = 0; int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else continue;   // '=' and whitespace
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF)); }
    }
    return o;
}

void json_str(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);
        }
    }
    o += '"';
}

// Caller holds g_mu.
std::string rig_to_json() {
    std::string o = "{\n  \"clockwork_rig\": 1,\n  \"plugin_folders\": [";
    const uint32_t nf = plugin_extra_search_paths(nullptr, 0);
    std::vector<const char*> folders(nf);
    plugin_extra_search_paths(folders.data(), nf);
    for (uint32_t i = 0; i < nf; ++i) { if (i) o += ", "; json_str(o, folders[i]); }
    o += "],\n  \"tracks\": [";
    bool firstT = true;
    std::vector<uint8_t> blob;
    for (const auto& t : g_tracks) {
        o += firstT ? "\n    {" : ",\n    {";
        firstT = false;
        o += "\"name\": "; json_str(o, t->name);
        char num[64];
        std::snprintf(num, sizeof num, ", \"gain\": %.6g, \"mute\": %s, \"chain\": [",
                      static_cast<double>(t->gain.load()), t->mute.load() ? "true" : "false");
        o += num;
        bool firstN = true;
        for (const auto& n : t->nodes) {
            o += firstN ? "\n      {" : ",\n      {";
            firstN = false;
            o += "\"format\": "; json_str(o, n->format);
            o += ", \"id\": ";     json_str(o, n->id);
            o += ", \"name\": ";   json_str(o, n->name);
            o += ", \"vendor\": "; json_str(o, n->vendor);
            o += ", \"path\": ";   json_str(o, n->path);
            std::snprintf(num, sizeof num, ", \"index\": %u, \"bypass\": %s, \"channel\": %d",
                          n->index, n->bypass ? "true" : "false", static_cast<int>(n->channel));
            o += num;
            if (n->plugin) {
                const uint32_t need = plugin_state_save(n->plugin, nullptr, 0);
                if (need > 0) {
                    blob.resize(need);
                    const uint32_t got = plugin_state_save(n->plugin, blob.data(), need);
                    o += ", \"state\": \"";
                    o += b64_encode(blob.data(), got);
                    o += '"';
                }
            }
            o += '}';
        }
        o += firstN ? "]}" : "\n    ]}";
    }
    o += firstT ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

// A minimal JSON reader: enough for a rig, and strict about it.
struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    bool        b = false;
    double      n = 0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const char* k) const {
        if (kind != Obj) return nullptr;
        for (const auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const char* k, const std::string& d = {}) const {
        const JVal* v = get(k); return (v && v->kind == Str) ? v->s : d;
    }
    double num(const char* k, double d = 0) const {
        const JVal* v = get(k); return (v && v->kind == Num) ? v->n : d;
    }
    bool boolean(const char* k, bool d = false) const {
        const JVal* v = get(k); return (v && v->kind == Bool) ? v->b : d;
    }
};

struct JParser {
    const char* p;
    const char* end;
    bool ok = true;

    void ws() { while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    bool take(char c) { ws(); if (p < end && *p == c) { ++p; return true; } return false; }

    bool string(std::string& out) {
        ws();
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) return false;
                switch (*p) {
                    case '"': out += '"'; break;   case '\\': out += '\\'; break;
                    case '/': out += '/'; break;   case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;  case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;  case 'f': out += '\f'; break;
                    case 'u': {
                        if (end - p < 5) return false;
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; ++i) {
                            const char c = p[i]; cp <<= 4;
                            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
                            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
                            else return false;
                        }
                        p += 4;
                        // Rig strings are names and paths; anything outside
                        // the BMP that a plugin put in a name survives a
                        // round trip only as its escaped form, which is fine.
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                        else { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: return false;
                }
                ++p;
            } else {
                out += *p++;
            }
        }
        if (p >= end) return false;
        ++p;
        return true;
    }

    bool value(JVal& v) {
        ws();
        if (p >= end) return false;
        if (*p == '{') {
            ++p; v.kind = JVal::Obj;
            if (take('}')) return true;
            for (;;) {
                std::string k;
                if (!string(k) || !take(':')) return false;
                JVal child;
                if (!value(child)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                if (take(',')) continue;
                return take('}');
            }
        }
        if (*p == '[') {
            ++p; v.kind = JVal::Arr;
            if (take(']')) return true;
            for (;;) {
                JVal child;
                if (!value(child)) return false;
                v.arr.push_back(std::move(child));
                if (take(',')) continue;
                return take(']');
            }
        }
        if (*p == '"') { v.kind = JVal::Str; return string(v.s); }
        if (end - p >= 4 && std::strncmp(p, "true", 4) == 0)  { p += 4; v.kind = JVal::Bool; v.b = true;  return true; }
        if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; v.kind = JVal::Bool; v.b = false; return true; }
        if (end - p >= 4 && std::strncmp(p, "null", 4) == 0)  { p += 4; v.kind = JVal::Null; return true; }
        char* q = nullptr;
        const double d = std::strtod(p, &q);
        if (q == p) return false;
        p = q; v.kind = JVal::Num; v.n = d;
        return true;
    }
};

// Where a plugin recorded by format and id lives on THIS machine, when the
// recorded path does not. Scans every folder, so this is the slow path a
// moved rig takes once per missing plugin.
bool locate(const std::string& format, const std::string& id, std::string& path, uint32_t& index) {
    if (id.empty()) return false;
    const uint32_t n = plugin_scan_all(nullptr, 0);
    if (n == 0) return false;
    std::vector<PluginEntry> all(n);
    plugin_scan_all(all.data(), n);
    const PluginFormat f = format == "vst3" ? kPluginFormatVst3
                         : format == "clap" ? kPluginFormatClap : kPluginFormatUnknown;
    for (const PluginEntry& e : all) {
        if (f != kPluginFormatUnknown && e.format != f) continue;
        if (id == e.id) { path = e.path; index = e.index; return true; }
    }
    return false;
}

}  // namespace

extern "C" {

// ── Tracks ─────────────────────────────────────────────────────────────────

ClockworkTrackId clockwork_track_create(const char* name, const char** err) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (const char* e = check_name(name, nullptr)) { if (err) *err = e; return CLOCKWORK_TRACK_NONE; }
    uint32_t slot = CLOCKWORK_TRACK_MAX;
    for (uint32_t s = 0; s < CLOCKWORK_TRACK_MAX; ++s) if (!g_slotUsed[s]) { slot = s; break; }
    if (slot == CLOCKWORK_TRACK_MAX) { if (err) *err = "no free track slots"; return CLOCKWORK_TRACK_NONE; }

    auto t = std::make_unique<Track>();
    t->id   = g_nextTrack++;
    t->slot = slot;
    t->name = name;
    g_slotUsed[slot] = true;
    const ClockworkTrackId id = t->id;
    g_tracks.push_back(std::move(t));
    republish();
    return id;
}

int clockwork_track_remove(ClockworkTrackId id) {
    std::vector<std::unique_ptr<Node>> dead;
    std::unique_ptr<Track> gone;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = std::find_if(g_tracks.begin(), g_tracks.end(),
                               [id](const std::unique_ptr<Track>& t) { return t->id == id; });
        if (it == g_tracks.end()) return 0;
        gone = std::move(*it);
        g_tracks.erase(it);
        g_slotUsed[gone->slot] = false;
        dead = std::move(gone->nodes);
        republish();   // waits the audio thread out of any snapshot naming the track
    }
    close_all(dead);
    return 1;
}

void clockwork_track_clear(void) {
    std::vector<std::unique_ptr<Node>> dead;
    std::vector<std::unique_ptr<Track>> gone;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& t : g_tracks)
            for (auto& n : t->nodes) dead.push_back(std::move(n));
        gone = std::move(g_tracks);
        g_tracks.clear();
        std::memset(g_slotUsed, 0, sizeof g_slotUsed);
        republish();
    }
    close_all(dead);
}

int clockwork_track_rename(ClockworkTrackId id, const char* name, const char** err) {
    std::lock_guard<std::mutex> lk(g_mu);
    Track* t = find_track(id);
    if (!t) { if (err) *err = "no such track"; return 0; }
    if (const char* e = check_name(name, t)) { if (err) *err = e; return 0; }
    t->name = name;
    republish();   // the snapshot carries names, for resolution on the audio thread
    return 1;
}

int clockwork_track_move(ClockworkTrackId id, uint32_t to_index) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = std::find_if(g_tracks.begin(), g_tracks.end(),
                           [id](const std::unique_ptr<Track>& t) { return t->id == id; });
    if (it == g_tracks.end()) return 0;
    const size_t from = static_cast<size_t>(it - g_tracks.begin());
    size_t to = to_index;
    if (to >= g_tracks.size()) to = g_tracks.size() - 1;
    if (to == from) return 1;
    std::unique_ptr<Track> moved = std::move(*it);
    g_tracks.erase(it);
    g_tracks.insert(g_tracks.begin() + static_cast<long>(to), std::move(moved));
    republish();
    return 1;
}

ClockworkTrackId clockwork_track_find(const char* name) {
    std::lock_guard<std::mutex> lk(g_mu);
    Track* t = find_track_by_name(name);
    return t ? t->id : CLOCKWORK_TRACK_NONE;
}

ClockworkTrackId clockwork_track_resolve(const char* name) {
    if (!name || !*name) return CLOCKWORK_TRACK_NONE;
    const Snapshot* s = current();
    if (!s) return CLOCKWORK_TRACK_NONE;
    for (uint32_t i = 0; i < s->count; ++i)
        if (std::strcmp(s->track[i].name, name) == 0) return s->track[i].id;
    return CLOCKWORK_TRACK_NONE;
}

uint32_t clockwork_track_count(void) {
    std::lock_guard<std::mutex> lk(g_mu);
    return static_cast<uint32_t>(g_tracks.size());
}

uint32_t clockwork_track_slot_mask(void) {
    const Snapshot* s = current();
    if (!s) return 0;
    uint32_t mask = 0;
    for (uint32_t slot = 0; slot < CLOCKWORK_TRACK_MAX; ++slot)
        if (s->bySlot[slot]) mask |= 1u << slot;
    return mask;
}

uint32_t clockwork_track_list(ClockworkTrackInfo* out, uint32_t cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    const uint32_t total = static_cast<uint32_t>(g_tracks.size());
    if (out) {
        const uint32_t k = cap < total ? cap : total;
        for (uint32_t i = 0; i < k; ++i) fill_info(*g_tracks[i], &out[i]);
    }
    return total;
}

int clockwork_track_info(ClockworkTrackId id, ClockworkTrackInfo* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    Track* t = find_track(id);
    if (!t) return 0;
    if (out) fill_info(*t, out);
    return 1;
}

int clockwork_track_set_gain(ClockworkTrackId id, float gain) {
    std::lock_guard<std::mutex> lk(g_mu);
    Track* t = find_track(id);
    if (!t) return 0;
    if (!(gain >= 0.0f)) gain = 0.0f;   // also catches NaN
    t->gain.store(gain, std::memory_order_relaxed);
    return 1;
}

int clockwork_track_set_mute(ClockworkTrackId id, int mute) {
    std::lock_guard<std::mutex> lk(g_mu);
    Track* t = find_track(id);
    if (!t) return 0;
    t->mute.store(mute ? 1 : 0, std::memory_order_relaxed);
    return 1;
}

uint32_t clockwork_track_lane_base(void) { return g_lane_base.load(std::memory_order_relaxed); }
void clockwork_track_set_lane_base(uint32_t base) { g_lane_base.store(base, std::memory_order_relaxed); }

// ── Chains ─────────────────────────────────────────────────────────────────

ClockworkTrackHandle clockwork_track_add_plugin(ClockworkTrackId id, const char* path, uint32_t index,
                                    uint32_t at_index, double sample_rate,
                                    uint32_t max_block, const char** err) {
    if (!path || !*path) { if (err) *err = "no path"; return CLOCKWORK_TRACK_NO_HANDLE; }
    if (max_block == 0 || max_block > kMaxBlock) max_block = kMaxBlock;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Track* t = find_track(id);
        if (!t) { if (err) *err = "no such track"; return CLOCKWORK_TRACK_NO_HANDLE; }
        if (t->nodes.size() >= CLOCKWORK_TRACK_MAX_NODES) { if (err) *err = "the chain is full"; return CLOCKWORK_TRACK_NO_HANDLE; }
    }

    // Identify it from the scan rather than the path, so the node says
    // "Surge XT" and not a filename, and knows whether it generates. Before
    // the lock: scanning opens the module and is far too slow to hold a mutex
    // across. Before the open, so a file that is not a plugin fails here.
    // The scan and the open both run the plugin's own code, so the load
    // listener brackets both: a module that crashes in its factory is as
    // much a culprit as one that crashes in its constructor.
    struct Loading {
        const char* path;
        explicit Loading(const char* p) : path(p) { if (g_load_fn) g_load_fn(g_load_ctx, path); }
        ~Loading() { if (g_load_fn) g_load_fn(g_load_ctx, nullptr); }
    } loading(path);
    PluginDesc d[16] {};
    const uint32_t nd = plugin_scan(path, d, 16);
    if (nd == 0) { if (err) *err = "not a plugin this build can load"; return CLOCKWORK_TRACK_NO_HANDLE; }
    const PluginDesc* desc = (index < nd && index < 16) ? &d[index] : nullptr;
    if (!desc) { if (err) *err = "no plugin at that index in the bundle"; return CLOCKWORK_TRACK_NO_HANDLE; }

    auto n = std::make_unique<Node>();
    n->instrument = desc->is_instrument != 0;
    n->name   = desc->name   ? desc->name   : path;
    n->vendor = desc->vendor ? desc->vendor : "";
    n->id     = desc->id     ? desc->id     : "";
    n->format = desc->format == kPluginFormatVst3 ? "vst3"
              : desc->format == kPluginFormatClap ? "clap" : "";
    n->path   = path;
    n->index  = index;

    const char* openErr = nullptr;
    HostedPlugin* p = plugin_open(path, index, sample_rate, max_block, &openErr);
    if (!p) { if (err) *err = openErr ? openErr : "open failed"; return CLOCKWORK_TRACK_NO_HANDLE; }
    n->plugin = p;

    // Before it is published, so its first block already knows the tempo.
    plugin_set_clock(p, g_clock);

    // The name table, once. What clockwork_track_param_by_name reads on the audio
    // thread, so it is complete before the node is published and never
    // touched again.
    const uint32_t np = plugin_param_count(p);
    n->params.reserve(np);
    for (uint32_t i = 0; i < np; ++i) {
        PluginParam pp {};
        if (!plugin_param_info(p, i, &pp) || !pp.name) continue;
        n->params.push_back({ pp.id, pp.name, pp.min, pp.max });
    }

    ClockworkTrackHandle h;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Track* t = find_track(id);
        if (!t || t->nodes.size() >= CLOCKWORK_TRACK_MAX_NODES) {
            // The track went away while the plugin was loading. Unwind.
            n->plugin = nullptr;
            plugin_close(p);
            if (err) *err = t ? "the chain is full" : "no such track";
            return CLOCKWORK_TRACK_NO_HANDLE;
        }
        n->handle = h = g_nextHandle++;
        n->track  = id;
        // The node's own handle field is the listener ctx: stable for the
        // plugin's whole life because the node is heap-allocated and never
        // moves, whichever vector holds its pointer.
        plugin_set_param_listener(p, node_param_edit, &n->handle);
        size_t at = at_index;
        if (at > t->nodes.size()) at = t->nodes.size();
        t->nodes.insert(t->nodes.begin() + static_cast<long>(at), std::move(n));
        republish();
    }
    return h;
}

int clockwork_track_remove_plugin(ClockworkTrackHandle h) {
    std::vector<std::unique_ptr<Node>> dead;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Track* t = nullptr;
        Node* n = find_node(h, &t);
        if (!n) return 0;
        auto it = std::find_if(t->nodes.begin(), t->nodes.end(),
                               [n](const std::unique_ptr<Node>& x) { return x.get() == n; });
        dead.push_back(std::move(*it));
        t->nodes.erase(it);
        republish();
    }
    // AFTER the swap and after retire() has waited the audio thread out.
    close_all(dead);
    return 1;
}

int clockwork_track_move_plugin(ClockworkTrackHandle h, uint32_t to_index) {
    std::lock_guard<std::mutex> lk(g_mu);
    Track* t = nullptr;
    Node* n = find_node(h, &t);
    if (!n) return 0;
    auto it = std::find_if(t->nodes.begin(), t->nodes.end(),
                           [n](const std::unique_ptr<Node>& x) { return x.get() == n; });
    const size_t from = static_cast<size_t>(it - t->nodes.begin());
    size_t to = to_index;
    if (to >= t->nodes.size()) to = t->nodes.size() - 1;
    if (to == from) return 1;
    std::unique_ptr<Node> moved = std::move(*it);
    t->nodes.erase(it);
    t->nodes.insert(t->nodes.begin() + static_cast<long>(to), std::move(moved));
    republish();
    return 1;
}

int clockwork_track_node_bypass(ClockworkTrackHandle h, int bypass) {
    std::lock_guard<std::mutex> lk(g_mu);
    Node* n = find_node(h);
    if (!n) return 0;
    if (n->bypass == (bypass != 0)) return 1;
    n->bypass = bypass != 0;
    republish();
    // A bypassed instrument keeps receiving notes and keeps its voices; what
    // stops is its output. Held notes would otherwise ring when it comes back.
    return 1;
}

void clockwork_track_set_load_listener(ClockworkTrackLoadFn fn, void* ctx) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_load_fn  = fn;
    g_load_ctx = ctx;
}

int clockwork_track_node_set_channel(ClockworkTrackHandle h, int16_t channel) {
    if (channel < 0 || channel > 16) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    Node* n = find_node(h);
    if (!n) return 0;
    if (n->channel == channel) return 1;
    n->channel = channel;
    republish();
    // Notes held on the old channel would otherwise never see their offs.
    if (n->plugin) plugin_all_notes_off(n->plugin, 0);
    return 1;
}

uint32_t clockwork_track_nodes(ClockworkTrackId id, ClockworkTrackNode* out, uint32_t cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    Track* t = find_track(id);
    if (!t) return 0;
    const uint32_t total = static_cast<uint32_t>(t->nodes.size());
    if (out) {
        const uint32_t k = cap < total ? cap : total;
        for (uint32_t i = 0; i < k; ++i) fill_node(*t->nodes[i], &out[i]);
    }
    return total;
}

int clockwork_track_node_info(ClockworkTrackHandle h, ClockworkTrackNode* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    Node* n = find_node(h);
    if (!n) return 0;
    if (out) fill_node(*n, out);
    return 1;
}

ClockworkTrackId clockwork_track_node_track(ClockworkTrackHandle h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Node* n = find_node(h);
    return n ? n->track : CLOCKWORK_TRACK_NONE;
}

// ── Playing — audio-thread safe ────────────────────────────────────────────

// Whether a node takes an event on `channel` (1-based). A node on channel 0
// takes everything; a plugin that is itself multitimbral still sees the
// channel and can split on it. An event with no channel (< 1) goes to all.
static inline bool listens(const SnapNode& n, int16_t channel) {
    return n.channel == 0 || channel < 1 || n.channel == channel;
}

void clockwork_track_note(ClockworkTrackId t, int on, int16_t channel, int16_t pitch,
                    float velocity, uint32_t frame_offset) {
    const SnapTrack* st = snap_track(current(), t);
    if (!st) return;
    for (uint32_t i = 0; i < st->count; ++i) {
        const SnapNode& n = st->node[i];
        if (!n.instrument || !n.plugin || !listens(n, channel)) continue;
        if (on) plugin_note_on(n.plugin, channel, pitch, velocity, frame_offset);
        else    plugin_note_off(n.plugin, channel, pitch, velocity, frame_offset);
    }
}

void clockwork_track_cc(ClockworkTrackId t, int16_t channel, uint8_t number, float value,
                  uint32_t frame_offset) {
    const SnapTrack* st = snap_track(current(), t);
    if (!st) return;
    for (uint32_t i = 0; i < st->count; ++i) {
        const SnapNode& n = st->node[i];
        if (!n.instrument || !n.plugin || !listens(n, channel)) continue;
        plugin_cc(n.plugin, channel, number, value, frame_offset);
    }
}

void clockwork_track_pitch_bend(ClockworkTrackId t, int16_t channel, float bend, uint32_t frame_offset) {
    const SnapTrack* st = snap_track(current(), t);
    if (!st) return;
    for (uint32_t i = 0; i < st->count; ++i) {
        const SnapNode& n = st->node[i];
        if (!n.instrument || !n.plugin || !listens(n, channel)) continue;
        plugin_pitch_bend(n.plugin, channel, bend, frame_offset);
    }
}

static void snap_track_notes_off(const SnapTrack& st, uint32_t frame_offset) {
    for (uint32_t i = 0; i < st.count; ++i) {
        const SnapNode& n = st.node[i];
        if (!n.instrument || !n.plugin) continue;
        plugin_all_notes_off(n.plugin, frame_offset);
    }
}

void clockwork_track_all_notes_off(ClockworkTrackId t, uint32_t frame_offset) {
    const Snapshot* s = current();
    if (!s) return;
    if (t == CLOCKWORK_TRACK_NONE) {
        // Every track: what a "stop" means, when the schedule that held the
        // note-offs has just been flushed.
        for (uint32_t i = 0; i < s->count; ++i) snap_track_notes_off(s->track[i], frame_offset);
        return;
    }
    if (const SnapTrack* st = snap_track(s, t)) snap_track_notes_off(*st, frame_offset);
}

void clockwork_track_node_param(ClockworkTrackHandle h, uint32_t id, double value, uint32_t frame_offset) {
    const SnapNode* n = snap_node(current(), h);
    if (n && n->plugin) plugin_param_set(n->plugin, id, value, frame_offset);
}

int clockwork_track_param_by_name(ClockworkTrackId t, ClockworkTrackHandle h, const char* name,
                            double value, uint32_t frame_offset) {
    if (!name || !*name) return 0;
    const Snapshot* s = current();
    if (h != CLOCKWORK_TRACK_NO_HANDLE) {
        const SnapNode* n = snap_node(s, h);
        if (!n || !n->plugin) return 0;
        const ParamName* p = node_param_named(*n, name);
        if (!p) return 0;
        node_param_set_by_name(*n, *p, value, frame_offset);
        return 1;
    }
    const SnapTrack* st = snap_track(s, t);
    if (!st) return 0;
    for (uint32_t i = 0; i < st->count; ++i) {
        const SnapNode& n = st->node[i];
        if (!n.plugin) continue;
        if (const ParamName* p = node_param_named(n, name)) {
            node_param_set_by_name(n, *p, value, frame_offset);
            return 1;
        }
    }
    return 0;
}

// ── Parameters, for a control surface ──────────────────────────────────────

uint32_t clockwork_track_node_param_count(ClockworkTrackHandle h) {
    std::lock_guard<std::mutex> lk(g_mu);
    Node* n = find_node(h);
    return (n && n->plugin) ? plugin_param_count(n->plugin) : 0;
}

int clockwork_track_node_param_info(ClockworkTrackHandle h, uint32_t index, struct PluginParam* out) {
    std::lock_guard<std::mutex> lk(g_mu);
    Node* n = find_node(h);
    return (n && n->plugin) ? plugin_param_info(n->plugin, index, out) : 0;
}

void clockwork_track_set_param_edit_listener(ClockworkTrackParamEditFn fn, void* ctx) {
    std::lock_guard<std::mutex> lk(g_editMu);
    g_editFn  = fn;
    g_editCtx = ctx;
}

// ── Editors ────────────────────────────────────────────────────────────────

int clockwork_track_editor_show(ClockworkTrackHandle h) {
    HostedPlugin* p = nullptr;
    std::string title;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Track* t = nullptr;
        Node* n = find_node(h, &t);
        if (!n || !n->plugin) return 0;
        // Answered here rather than discovered by a window that never
        // appears: a plugin without an editor (CLAP for now, and any VST3
        // whose createView returns null) is a fact the client can show.
        if (!plugin_editor_has(n->plugin)) return 0;
        p = n->plugin;
        title = n->name + " — " + t->name;
    }
    // The hop to the main thread lives inside the window module, so this file
    // stays plain C++.
    clockwork_editor_window_show_async(p, title.c_str());
    return 1;
}

void clockwork_track_editor_hide(ClockworkTrackHandle h) {
    HostedPlugin* p = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        Node* n = find_node(h);
        if (!n || !n->plugin) return;
        p = n->plugin;
    }
    clockwork_editor_window_hide_async(p);
}

// ── State ──────────────────────────────────────────────────────────────────

uint32_t clockwork_track_node_state_save(ClockworkTrackHandle h, uint8_t* out, uint32_t cap) {
    std::lock_guard<std::mutex> lk(g_mu);
    Node* n = find_node(h);
    return (n && n->plugin) ? plugin_state_save(n->plugin, out, cap) : 0;
}

int clockwork_track_node_state_load(ClockworkTrackHandle h, const uint8_t* bytes, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_mu);
    Node* n = find_node(h);
    return (n && n->plugin) ? plugin_state_load(n->plugin, bytes, len) : 0;
}

uint32_t clockwork_track_rig_to_json(char* out, uint32_t cap) {
    std::string j;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        j = rig_to_json();
    }
    if (out && cap > 0) {
        const size_t k = std::min<size_t>(cap - 1, j.size());
        std::memcpy(out, j.data(), k);
        out[k] = '\0';
    }
    return static_cast<uint32_t>(j.size());
}

int clockwork_track_rig_from_json(const char* json, double sample_rate, uint32_t max_block,
                            uint32_t* missing, const char** err) {
    if (missing) *missing = 0;
    if (!json) { if (err) *err = "no rig"; return 0; }
    JVal root;
    JParser jp { json, json + std::strlen(json) };
    if (!jp.value(root) || root.kind != JVal::Obj) { if (err) *err = "rig is not valid JSON"; return 0; }
    if (root.num("clockwork_rig", 0) != 1) { if (err) *err = "not a clockwork rig file"; return 0; }

    // Folders first, so a plugin recorded only by id can be found in one.
    if (const JVal* f = root.get("plugin_folders"); f && f->kind == JVal::Arr)
        for (const JVal& v : f->arr) if (v.kind == JVal::Str) plugin_add_search_path(v.s.c_str());

    clockwork_track_clear();

    uint32_t lost = 0;
    const JVal* tracks = root.get("tracks");
    if (tracks && tracks->kind == JVal::Arr) {
        for (const JVal& tv : tracks->arr) {
            if (tv.kind != JVal::Obj) continue;
            std::string name = tv.str("name");
            if (name.empty()) continue;
            const char* e = nullptr;
            const ClockworkTrackId t = clockwork_track_create(name.c_str(), &e);
            if (t == CLOCKWORK_TRACK_NONE) continue;   // a duplicate name, or out of slots
            clockwork_track_set_gain(t, static_cast<float>(tv.num("gain", 1.0)));
            clockwork_track_set_mute(t, tv.boolean("mute", false) ? 1 : 0);

            const JVal* chain = tv.get("chain");
            if (!chain || chain->kind != JVal::Arr) continue;
            for (const JVal& nv : chain->arr) {
                if (nv.kind != JVal::Obj) continue;
                std::string path = nv.str("path");
                uint32_t index = static_cast<uint32_t>(nv.num("index", 0));
                const std::string format = nv.str("format");
                const std::string id = nv.str("id");

                // The recorded path, if it still names this plugin; else the
                // id, wherever it now lives.
                bool have = false;
                if (!path.empty()) {
                    PluginDesc d[16] {};
                    const uint32_t nd = plugin_scan(path.c_str(), d, 16);
                    if (index < nd && index < 16 && (id.empty() || (d[index].id && id == d[index].id)))
                        have = true;
                }
                if (!have) have = locate(format, id, path, index);
                if (!have) { ++lost; continue; }

                const char* ae = nullptr;
                const ClockworkTrackHandle h = clockwork_track_add_plugin(t, path.c_str(), index,
                                                              UINT32_MAX, sample_rate,
                                                              max_block, &ae);
                if (h == CLOCKWORK_TRACK_NO_HANDLE) { ++lost; continue; }
                if (nv.boolean("bypass", false)) clockwork_track_node_bypass(h, 1);
                const double ch = nv.num("channel", 0);
                if (ch >= 1 && ch <= 16) clockwork_track_node_set_channel(h, static_cast<int16_t>(ch));
                const std::string state = nv.str("state");
                if (!state.empty()) {
                    const std::vector<uint8_t> blob = b64_decode(state);
                    if (!blob.empty())
                        clockwork_track_node_state_load(h, blob.data(), static_cast<uint32_t>(blob.size()));
                }
            }
        }
    }
    if (missing) *missing = lost;
    return 1;
}

int clockwork_track_rig_save(const char* path, const char** err) {
    if (!path || !*path) { if (err) *err = "no path"; return 0; }
    std::string j;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        j = rig_to_json();
    }
    // The path came over the wire as UTF-8; the stream is opened through a
    // filesystem::path so it is read as such on every platform.
    std::ofstream f(clockwork_path::from_utf8(path), std::ios::binary | std::ios::trunc);
    if (!f) { if (err) *err = "cannot write rig file"; return 0; }
    f.write(j.data(), static_cast<std::streamsize>(j.size()));
    if (!f) { if (err) *err = "write failed"; return 0; }
    return 1;
}

int clockwork_track_rig_load(const char* path, double sample_rate, uint32_t max_block,
                       uint32_t* missing, const char** err) {
    if (missing) *missing = 0;
    if (!path || !*path) { if (err) *err = "no path"; return 0; }
    std::ifstream f(clockwork_path::from_utf8(path), std::ios::binary);
    if (!f) { if (err) *err = "cannot read rig file"; return 0; }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string j = ss.str();
    return clockwork_track_rig_from_json(j.c_str(), sample_rate, max_block, missing, err);
}

// ── Wiring ─────────────────────────────────────────────────────────────────

void clockwork_track_set_clock(const struct ClockworkClockState* clock) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_clock = clock;
    for (auto& t : g_tracks)
        for (auto& n : t->nodes)
            if (n->plugin) plugin_set_clock(n->plugin, clock);
}

void clockwork_track_set_timelines(const struct ClockworkTimelineMirrorSlot* slots, uint32_t count) {
    // Count first, then the pointer: a reader that sees the pointer sees a
    // count it may index by.
    g_timeline_slots.store(slots ? count : 0, std::memory_order_relaxed);
    g_timelines.store(slots, std::memory_order_release);
}

int clockwork_track_set_timeline(ClockworkTrackId id, int32_t timeline_id, const char* name) {
    if (timeline_id < 0 || !name || !*name || std::strlen(name) >= CLOCKWORK_TRACK_NAME_MAX) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    Track* t = find_track(id);
    if (!t) return 0;
    t->timeline     = timeline_id;
    t->timelineName = name;
    republish();
    return 1;
}

void clockwork_track_process(float* const* in, uint32_t n_in, float* const* out, uint32_t n_out,
                       uint32_t frames, int64_t block_time, uint32_t lane_base) {
    track_process_at(in, n_in, out, n_out, frames, block_time, lane_base);
}

}  // extern "C"
