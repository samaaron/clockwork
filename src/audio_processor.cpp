// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

#include "audio_processor.h"
#include "clockwork_product.h"

#include "audio_config.h"
#include "lanes/lanes.h"    // clockwork_reserve_lanes / clockwork_lane_base, defined below

#include <array>
#include <atomic>

namespace {
// ─── Reserved lanes ──────────────────────────────────────────────────────────
// Channels clockwork keeps ABOVE the device, for its own routing. A hosted plugin
// track needs a channel the DSP can write a send to and one it can read a return
// from, and neither may reach a speaker or arrive from a microphone. So the DSP
// is built wider than the device, the backend copies only the channels it has,
// and the block above is clockwork's alone. Both directions widen by the same
// amount so a lane index means the same thing either way. A Link peer's audio
// needs a channel of the same kind, so Link Audio has lanes of its own, directly
// above the host's.
//
// The base is stable: 32, or the first multiple of 8 above a wider device. Stable
// matters — a lane's index is published to clients as the channel they route to,
// and a base that moved with every device switch would invalidate every route
// across a cold swap.
uint32_t lane_base_for(uint32_t device_in, uint32_t device_out) {
    const uint32_t widest = device_in > device_out ? device_in : device_out;
    uint32_t base = 32;
    if (widest > base) base = (widest + 7u) & ~7u;
    return base;
}
} // namespace


#include "clock/clock_math.h"
#include "osc_debug.h"
#include "osc/OscOutboundPacketStream.h"
#include "clock/ClockworkClock.h"
#include "clock/EngineClock.h"
#include "clock/host_transport.h"
#include "OscIngress.h"
#include "IngressCallCtx.h"
#include "clockwork_sys.h"        // the /clockwork/ prefix rule: clockwork here, DSP everywhere else
#include "ReplyChannel.h"
#include "lanes/lanes_internal.h"   // clockwork_egress_nrt_write — off-thread debug egress
#include "core_state.h"             // a core's Egress
// Platform macros (CLOCKWORK_COLD_BSS, tiered-memory attributes). Header-only and
// free of any DSP dependency, so it is included in both builds — the core still
// places the ring arena + scheduler pool in bulk RAM on tiered targets.
#include "platform.h"
#include "dsp_api.h"  // the contract with the DSP
#include "restart_plan.h"   // the least audio coming back does (come_back)
#include "guest_malloc.h"       // every call into the DSP is marked, so its mallocs are counted as its own
#include "guest_fence.h"        // and, under AddressSanitizer in a harness that arms it, the arena it may not touch is poisoned
#include "shm_tap_stream.hpp"   // the tap slots DspHost::scope_* hand a DSP
#include "clockwork_ports.h"    // frames crossing the boundary, on a stable slot
#include "clockwork_port_bus.h" // ...and which DSP channels they occupy
#include "clockwork_event_sink.h" // events leaving clockwork, on a stable slot
#include "os_pages.h"             // a core's memory from the system, natively


// A host with no NRT thread has two possible ends to clockwork's chain:
// itself, when it has a front that answers the control verbs on a thread of
// its own (the web client's main thread, which owns Web MIDI and the Gamepad
// API), or this audio thread, when it has not. Which, is its core's
// (Routing::host_forward).

// The sinks' host emitter (clockwork_event_sink.h): a guest's MIDI send goes
// out over the egress the same way a forwarded verb does — a bundle carrying
// the send's time, its one element "/clockwork/midi/sink/send ,sb <port>
// <bytes>" — for the host's front to hand to the port with that time. Safe
// on the audio thread: a ring write and nothing else. Installed and removed
// with the forward switch, because they are one fact: the host is the far
// end. Defined below with the forward route.
static int clockwork_sink_host_emit(void* ctx, uint32_t kind, const char* target,
                                    const uint8_t* bytes, uint32_t len, ClockworkTimetag when);
static bool forward_to_host(clockwork::CoreState& st, uint32_t token, clockwork::Timetag when,
                            const RingPart* element, uint32_t n_parts);

// The core's emitter installed, or none.
static void install_host_emitter(ClockworkCore* core, clockwork::Routing& r, bool on) {
    r.host_emitter = ClockworkSinkHostEmitter{ &clockwork_sink_host_emit, core };
    clockwork_sink_set_host_emitter(clockwork_core_sinks(core), on ? &r.host_emitter : nullptr);
    r.host_emitter_installed = on;
}

void clockwork_host_forward(ClockworkCore* core, int on) {
    clockwork::Routing* r = clockwork::routing(core);
    if (!r) return;
    r->host_forward.store(on != 0, std::memory_order_release);
    install_host_emitter(core, *r, on != 0);
}
int clockwork_host_forwards(ClockworkCore* core) {
    const clockwork::Routing* r = clockwork::routing(core);
    return r && r->host_forward.load(std::memory_order_acquire) ? 1 : 0;
}

namespace {
// A block, open (CoreClock::block_open) until the scope ends.
struct BlockOpenScope {
    explicit BlockOpenScope(clockwork::CoreClock& c) : cc(c) { cc.block_open.store(true, std::memory_order_relaxed); }
    ~BlockOpenScope() { cc.block_open.store(false, std::memory_order_release); }
    BlockOpenScope(const BlockOpenScope&) = delete;
    BlockOpenScope& operator=(const BlockOpenScope&) = delete;
    clockwork::CoreClock& cc;
};
} // namespace

#include <emscripten/webaudio.h>
extern "C" size_t emscripten_get_heap_size(void);   // the end of the memory, for the assets area that grows to it
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <string>
#include <mutex>
#include <vector>

#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

// Scheduler includes. The queue is the Clockwork's: timed MIDI out and timed
// OSC forwarding are its own features and exist with no DSP attached.
//
// engine_schedule.h is included unconditionally because it is the file that
// STATES how CLOCKWORK_SCHEDULER composes with DspInfo::holds_schedule, and
// it defines nothing when the option is off. schedule_parse.h is not gated
// either, and for a sharper reason: a timestamped bundle is the DSP's, so even
// a build with no store has to read its timetag to forward it honestly.
#include "scheduler/engine_schedule.h"  // clockwork's own timed queue
#include "host/osc_reader.h"            // sched/flush's tag, read without throwing
#include "params/params.h"           // the parameter table: its region's reset, its verbs, the guest's calls
#include "params/clockwork_params.h"
#include "scheduler/schedule_parse.h"   // shared bundle / "/clockwork/schedule" parsing
#if CLOCKWORK_SCHEDULER
#include "scheduler/fire_due.h"         // shared scheduler fire loop
#endif
// MIDI clock out is ClockworkClock's, not the scheduler's: every tick carries its
// own time to its sink. Present whatever this build decided about the queue.
#include "clock/MidiClockOut.h"

// Lanes drain-state reset (init_memory resets ring sequences; the lanes
// consumer state must restart with them) and the shared ring walker the
// IN-ring drain below runs on.
#include "lanes/lanes_internal.h"
#include "lanes/ring_drain.h"
#include "lanes/ring_marks.h"       // where a generation's messages end in the in ring
#include "metrics_history.h"        // what every counter and gauge did over the last hour
#ifndef __EMSCRIPTEN__
#include "native/LinkAudioHost.h"   // the block's stamp in Link's clock (CoreClock::link_audio)
#endif
#include "workers/RingBufferWriter.h"   // the in ring's writer lock, which a teardown's mark takes

// Pre-allocated heap for RT-safe allocations
#include "mem_region.h"
#include "clockwork_event_sink.h"
#include "sink_profile.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/console.h>
#endif
#include "clockwork_config.h"

#if CLOCKWORK_SCHEDULER
/*
 * Defined at the bottom, beside the scheduler instance itself. Declared here
 * because init_memory must call it: it forces the queue to be built on the
 * control thread at boot, rather than lazily on the audio thread.
 *
 * DECLARED UNDER THE SAME GUARD AS THE CALL — CLOCKWORK_SCHEDULER alone. Guard
 * it any more narrowly and a build with a scheduler but no worklet clock (every
 * NATIVE build) loses the declaration and keeps the call.
 */
void clockwork_sched_warm(ClockworkCore* core);
#endif

// Thread-local RT guard for allocation detection (read by test binary only)
#include "rt_alloc.h"

#include "clockwork_taps.h"

// The audio thread's /clock handler, for a core with no control thread: the
// worklet, an attached core. Answers the clock's own verbs inline and replies
// on the OUT ring. /clockwork/clock is clockwork's own namespace and is
// always consumed here — it never reaches the DSP. (The full engine registers
// a clock route of its own, which forwards to its control thread.)
//
// A client's set of the session's timeline is taken as a host's transport is
// (clock/host_transport.h): on the clock state, and on Link's session where
// there is one, with no lock; and the clients hear what it moved at the next
// block, as they hear a host's or a peer's.
static void clock_set_follow(void* ctx, const clockwork::HostWord& said, double atNtp) {
    ClockworkCore* const core = static_cast<ClockworkCore*>(ctx);
    clockwork::CoreState* const st = clockwork::state(core);
    ClockworkClock* const clk = clockwork::clock_of(core);
    if (!st || !clk) return;
    clockwork::CoreClock& cc = st->clock;
    const double at = atNtp > 0.0 ? atNtp
                    : static_cast<double>(cc.block_ntp.load(std::memory_order_relaxed)) / clockwork::kNtpUnitsPerSecond;
    double rate = 0.0;
    if (uint8_t* const base = st->arena.laid_out())
        rate = static_cast<double>(clockwork::metrics_in(base)->audio_sample_rate.load(std::memory_order_relaxed));
    cc.host_moved |= clk->followHost(said, at, rate);
}
static void clock_set_tempo(void* ctx, double bpm, double atNtp) {
    clockwork::HostWord said;
    said.kind = clockwork::HostWord::Tempo;
    said.value = bpm;
    clock_set_follow(ctx, said, atNtp);
}
static void clock_set_playing(void* ctx, bool playing) {
    clockwork::HostWord said;
    said.kind = clockwork::HostWord::Playing;
    said.playing = playing;
    clock_set_follow(ctx, said, 0.0);
}
static void clock_set_meter(void* ctx, int32_t num, int32_t den) {
    clockwork::HostWord said;
    said.kind = clockwork::HostWord::Meter;
    said.num = num;
    said.den = den;
    clock_set_follow(ctx, said, 0.0);
}
// What a client that has just asked for the timeline's notices is sent.
static void clock_subscribed(const ClockworkClock& clk, ReplyChannel& chan, uint32_t token,
                             const uint8_t* data, uint32_t len) {
    try {
        const osc::ReceivedMessage msg(osc::ReceivedPacket(reinterpret_cast<const char*>(data),
                                                           static_cast<osc::osc_bundle_element_size_t>(len)));
        bool tokened = false;
        int32_t echo = 0;
        for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it)
            if (it->IsInt32()) { tokened = true; echo = it->AsInt32Unchecked(); }
        std::array<char, 96> buf{};
        if (tokened) {
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/subscribe.reply")) << echo << osc::EndMessage;
            chan.reply(token, reinterpret_cast<const uint8_t*>(s.Data()), static_cast<uint32_t>(s.Size()));
        }
        const ClockworkClockSnapshot now = readClockworkClock(clk.state());
        {
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/tempo")) << now.bpm << osc::EndMessage;
            chan.reply(token, reinterpret_cast<const uint8_t*>(s.Data()), static_cast<uint32_t>(s.Size()));
        }
        {
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/transport"))
              << static_cast<int32_t>(now.is_playing ? 1 : 0)
              << static_cast<osc::int64>(std::llround(now.is_playing_at_ntp * 1e6))
              << osc::EndMessage;
            chan.reply(token, reinterpret_cast<const uint8_t*>(s.Data()), static_cast<uint32_t>(s.Size()));
        }
    } catch (...) {
        // malformed: asked for all the same, and told nothing
    }
}

static bool clockCoreRoute(void* routeCtx, const void* callCtx,
                           const uint8_t* data, size_t len) {
    auto* chan = static_cast<ReplyChannel*>(routeCtx);   // RT egress, bound at registration; its context is the core
    ClockworkCore* const core = chan ? static_cast<ClockworkCore*>(chan->ctx) : nullptr;
    ClockworkClock* clk = clockwork::clock_of(core);
    if (!clk) return true;   // clock route owns /clockwork/clock/; drop during startup, don't error
    auto* cc  = static_cast<const DrainCallCtx*>(callCtx);
    uint32_t token = cc ? cc->sourceId : 0;              // reply metadata, threaded in
    // The client asks for the timeline's notices, or to be told no more
    // (lanes.h). Asking is answered with where the timeline is now, as each
    // notice will say it, and acked when it came with a token, as every
    // subscribe is: a client asks again until it hears.
    {
        const char* const addr = reinterpret_cast<const char*>(data);
        const bool ask = std::strcmp(addr, CLOCKWORK_SYS("clock/notify/subscribe")) == 0;
        if (ask || std::strcmp(addr, CLOCKWORK_SYS("clock/notify/unsubscribe")) == 0) {
            clockwork_client_wants(core, CLOCKWORK_WANTS_CLOCK, ask);
            if (ask && chan) clock_subscribed(*clk, *chan, token, data, static_cast<uint32_t>(len));
            return true;
        }
    }
    const ClockSets sets{ core, &clock_set_tempo, &clock_set_playing, &clock_set_meter };
    const bool handled = handleClockCoreOsc(*clk, data, static_cast<uint32_t>(len),
        [chan, token](const uint8_t* d, uint32_t n) {
            if (chan) chan->reply(token, d, n);
        }, &sets);
    if (!handled && chan) {
        // Not a clock-core verb. Before refusing, offer it to clockwork's own
        // system surface: /clockwork/clock/state/get lives there
        // (clockwork_sys.h), because it answers against the published clock
        // MIRROR rather than the clock object, and this route claimed the
        // whole /clockwork/clock/ namespace ahead of it — so without this
        // hand-off the verb was implemented and unreachable, and every web
        // client asking for the snapshot got "unsupported" instead.
        uint8_t* const base = clockwork::arena_base(core);
        const auto* clockState = base
            ? reinterpret_cast<const ClockworkClockState*>(base + CLOCK_STATE_START)
            : nullptr;
        const auto rt = handle_clockwork_sys_rt(data, static_cast<uint32_t>(len),
            [chan, token](const uint8_t* d, uint32_t n) { chan->reply(token, d, n); },
            clockState, arenaInstanceGeneration(base));
        if (rt == ClockworkSysRt::Answered) return true;

        // A /clock verb this build doesn't answer (native-only Link-session
        // surface, or a typo): refuse explicitly instead of dropping silently,
        // so clients can tell "unsupported here" from "lost datagram". Pair
        // with /clockwork/clock/capabilities/get for feature detection.
        replyClockUnsupported(core, data, static_cast<uint32_t>(len),
            [chan, token](const uint8_t* d, uint32_t n) {
                chan->reply(token, d, n);
            });
    }
    return true;
}
#endif  // CLOCKWORK_WORKLET_CLOCK

// Defined with the rest of the channel-map writer, down beside the port bus
// that mostly drives it. Declared here because init_memory publishes the
// device's own channels, and that runs first.
namespace { void channel_map_set_device(clockwork::CoreState& st, uint32_t in_width, uint32_t out_width); }

// The RT-out ring's writer (defined after the namespace), for a payload in
// parts and in one piece. Dependencies (sequence counter, status-flags word,
// metrics) are passed in rather than read from engine globals; default-null
// args here keep callers terse and let the function be unit-tested
// directly. Defaults live on these declarations only — never repeated on
// the definitions.
bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t route,
    uint32_t source_id,
    const RingPart* parts,
    uint32_t n_parts,
    std::atomic<uint32_t>* status_flags = nullptr,
    PerformanceMetrics* pm = nullptr
);
bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t route,
    uint32_t source_id,
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags = nullptr,
    PerformanceMetrics* pm = nullptr
);

// Custom errno implementation for single-threaded AudioWorklet
// This bypasses libc's __errno_location which isn't compiled with atomics support
// AudioWorklet is single-threaded so a simple global is sufficient
#ifdef __EMSCRIPTEN__
extern "C" {
    static int global_errno = 0;

    int* __errno_location() {
        return &global_errno;
    }
}
#endif

// Clockwork reports its OWN version and nothing else — a guest's protocol
// version is a claim about the guest's command set, so a guest that wants to
// advertise one answers /version itself. Defined in clockwork_config.h.

extern "C" {
    // The arena's size is its layout's (shared_memory.h, clockwork::Layout),
    // deliberately not restated as a number here.
    //
    // 16-byte aligned for shm_audio_buffer slots at SCOPE_START; JS Float64Array
    // needs only 8. FAST RAM, on a tiered device too: the arena's cursors,
    // rings, scope and metrics are touched every block (the table says which,
    // per region: ClockworkArenaEntry::tier, clockwork_arena.h), and a hot
    // region in PSRAM distorts the audio. The bulk-tier regions in it — the
    // taps, the window, the guest's persistent store — are kept small by the
    // tiered profiles (memory_profile.h) so the whole span fits beside the
    // heap and the audio task's stack. Not CLOCKWORK_COLD_BSS: that would put
    // all of it in PSRAM.
#if !defined(__EMSCRIPTEN__) && CLOCKWORK_HAS_TIERED_MEMORY
    // On a device the process holds one core, and its arena is static
    // storage, placed in the fast RAM, laid out by the target's profile.
    alignas(16) uint8_t arena_storage[clockwork::kDefaultLayout.arena_bytes];   // default-layout-ok: static storage, sized when built

    uint8_t* clockwork_local_arena(ClockworkCore*) { return arena_storage; }
#elif !defined(__EMSCRIPTEN__)
    // Natively each core has its own arena when its host gives it no segment:
    // zeroed, made at its first ask and freed when the core goes
    // (clockwork::Memory).
    uint8_t* clockwork_local_arena(ClockworkCore* core) {
        if (!clockwork::make_state(core)) return nullptr;
        clockwork::Memory& m = clockwork::state(core)->memory;
        if (!m.arena) m.arena = static_cast<uint8_t*>(std::calloc(1, clockwork::state(core)->arena.layout.arena_bytes));   // 16-aligned, as the arena needs
        return m.arena;
    }

    void clockwork_free_local_arena(ClockworkCore* core) {
        if (clockwork::CoreState* st = clockwork::state(core)) {
            std::free(st->memory.arena);
            st->memory.arena = nullptr;
        }
    }
#endif

    // ── A core's own sizes (lanes.h) ──────────────────────────────────────
    // A target whose one core's arena exists before the core does — static
    // storage on a device, the engine's memory on the web, whose table the
    // worklet reads before it boots — takes only its own sizes.
#if defined(__EMSCRIPTEN__) || CLOCKWORK_HAS_TIERED_MEMORY
    static constexpr bool kArenaBeforeCore = true;
#else
    static constexpr bool kArenaBeforeCore = false;
#endif

    static void fill_profile(ClockworkProfile* out, const clockwork::Profile& p) {
        if (!out || out->struct_bytes < sizeof(ClockworkProfile)) return;
        out->struct_bytes       = sizeof(ClockworkProfile);
        out->in_ring_bytes      = p.in_ring_bytes;
        out->rt_out_ring_bytes  = p.rt_out_ring_bytes;
        out->nrt_out_ring_bytes = p.nrt_out_ring_bytes;
        out->scope_frames       = p.scope_frames;
        out->scope_channels     = p.scope_channels;
        out->tap_slots          = p.tap_slots;
        out->track_tap_slots    = p.track_tap_slots;
        out->tap_ring_frames    = p.tap_ring_frames;
        out->window_bytes       = p.window_bytes;
        out->persist_bytes      = p.persist_bytes;
    }

    void clockwork_profile_default(ClockworkProfile* out) {
        fill_profile(out, clockwork::kDefaultProfile);   // default-layout-ok: what the call is for
    }
    void clockwork_profile_plugin(ClockworkProfile* out) { fill_profile(out, clockwork::kPluginProfile); }

    // A host's profile as the layout reads it, or why it is not one.
    static const char* read_profile(const ClockworkProfile* profile, clockwork::Profile& p) {
        if (!profile || profile->struct_bytes < sizeof(ClockworkProfile))
            return "no profile, or one from another version of Clockwork";
        p = clockwork::Profile{
            profile->in_ring_bytes, profile->rt_out_ring_bytes, profile->nrt_out_ring_bytes,
            profile->scope_frames, profile->scope_channels,
            profile->tap_slots, profile->track_tap_slots, profile->tap_ring_frames,
            profile->window_bytes, profile->persist_bytes,
        };
        return nullptr;
    }

    const char* clockwork_profile_check(const ClockworkProfile* profile) {
        clockwork::Profile p {};
        if (const char* why = read_profile(profile, p)) return why;
        return clockwork::layOut(p).refused;
    }

    const char* clockwork_set_profile(ClockworkCore* core, const ClockworkProfile* profile) {
        clockwork::Profile p {};
        if (const char* why = read_profile(profile, p)) return why;
        if (!clockwork::make_state(core)) return "no such core";
        clockwork::CoreState& st = *clockwork::state(core);
        if (st.arena.at() || st.memory.arena)
            return "the core is laid out already: set its profile before its first init";
        const clockwork::Layout layout = clockwork::layOut(p);
        if (layout.refused) return layout.refused;
        if (kArenaBeforeCore && p != st.arena.layout.profile)
            return "this target lays its one core's arena out before the core exists, and takes only its own sizes";
        st.arena.layout = layout;
        return nullptr;
    }

#ifdef __EMSCRIPTEN__
    // THE ENGINE'S MEMORY, on the web: one block for the life of the process
    // (CLOCKWORK_AREA_ENGINE_MEMORY). The arena, laid out by the target's
    // profile, is its first bytes; the placement arena — the RT router's block, the guest's
    // working memory, the heap — is the rest. Claimed once,
    // through clockwork_memory_claim, before anything else is allocated, so
    // it is the first thing after the linker's heap base and the same on
    // every boot. A reload instantiates a new module over the same memory:
    // these two survive it (nothing re-initialises the zeroed data), so the
    // new engine finds the block where it was and starts the placement
    // arena over inside it.
    uint8_t* g_engine_block       = nullptr;
    size_t   g_engine_block_bytes = 0;

    uint8_t* clockwork_local_arena(ClockworkCore*) { return g_engine_block; }
#endif

#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
    // THE ENGINE'S MEMORY, NATIVELY: one block per core that the guest's two
    // spans are carved from, as on the web, so a desktop engine's memory is
    // one thing to see and one allocation to make (clockwork::Memory).
    // Unlike the web's block it does not hold the arena: natively the arena
    // is the core's own, or a shared-memory segment other processes map, and
    // the guest's memory must not be in a segment any process can reach. Nor
    // the core's own pool, which lives as long as the core while this block
    // is a build's: a build whose guest wants more than the block holds takes
    // a new one. Everything carved from the block is given back before it is
    // replaced (the pool and the spans are all per-build), so replacing it
    // frees nothing live. THE BLOCK IS CUT IN TWO BY HAND — the fast span,
    // then the bulk span — and clockwork::mem is not involved: natively it
    // stays the system allocator. The block is never installed as the
    // placement arena: a fixture that boots one engine for the whole process
    // would then have every later test's "system" allocation come out of the
    // engine's block and be freed as the system's. The block goes with the
    // engine (teardown_memory). A tiered device has real regions to place
    // into and no block.
    //
    // Natively a process may hold many cores, so the memory a core's pools
    // run over comes in whole chunks of the address map's on a chunk
    // (clockwork_heap.h), straight from the system (os_pages.h): no two
    // cores' memory shares one, and a span zeroed at a build gives its pages
    // back rather than having them written.

    // Have a block that holds `bytes`: the one there is when it does, else a
    // new one with everything carved from the old one forgotten first.
    // False when the system has no such block, with no spans, so the boot
    // refuses with the figures.
    static bool engine_block_holding(clockwork::CoreState& st, size_t bytes, uint32_t verbosity);
#endif

    // The guest's arenas when CLOCKWORK reserved them rather than the host
    // (clockwork::Memory's fast and bulk): a host that passed no region
    // (headless native, an embedded board) and a guest that declared wants
    // (DspInfo::memory_bytes_wanted) meet there, and on a tiered device the
    // bulk arena is always clockwork's to place, since only clockwork::mem
    // knows which RAM is which. Taken at the first build that needs them,
    // reused across rebuilds exactly as a host-owned span is, and given back
    // at teardown_memory — never in destroy_dsp, which a rebuild goes
    // through.

    // Defined beside clockwork_log; see the note there for why boot failures
    // cannot use it.
    static void clockwork_boot_fail(clockwork::CoreState& st, const char* fmt, ...);

    // Why this build has no guest (Guest::boot_error). Written on the control
    // thread while the build runs, before the audio thread is let at it.
    static void clear_boot_error(clockwork::CoreState& st) {
        st.guest.boot_error[0] = '\0';
        st.guest.boot_failed.store(false, std::memory_order_release);
    }

    static void record_boot_error(clockwork::CoreState& st, const char* fmt, ...) {
        clockwork::Guest& g = st.guest;
        if (g.boot_failed.load(std::memory_order_relaxed)) return;
        va_list args;
        va_start(args, fmt);
        vsnprintf(g.boot_error.data(), g.boot_error.size(), fmt, args);
        va_end(args);
        g.boot_failed.store(true, std::memory_order_release);
    }

    const char* clockwork_boot_error(ClockworkCore* core) {
        const clockwork::Guest* g = clockwork::guest(core);
        return g && g->boot_failed.load(std::memory_order_acquire) ? g->boot_error.data() : nullptr;
    }

    // The block a build is laid out in, for the block a host asked for: no
    // more than the profile's largest (kMaxBlockSize), and a bad size falls
    // back to the default.
    static uint32_t build_block(uint32_t asked) {
        return (asked == 0 || asked > clockwork::kMaxBlockSize) ? clockwork::kDefaultBlockSize : asked;
    }

    // The core's staging laid out for `block` frames of `out` and `in`
    // channels, and zeroed: silence for a host that reads before the first
    // tick, and for an input channel nothing fills. A span that holds it is
    // kept; one too small is given back and a larger taken, from the fast
    // tier (a hot buffer in slow RAM distorts the audio). False when there
    // is none to take, with the staging as it was. With the audio thread
    // stopped. These are the buffers the DSP is handed: it writes the
    // output, and Clockwork names the channels and never learns where the
    // DSP keeps anything.
    static bool fit_staging(clockwork::Staging& sg, uint32_t block, uint32_t out, uint32_t in) {
        const size_t channels = static_cast<size_t>(out) + in;
        const size_t ptr_bytes = (channels * sizeof(float*) + 15u) & ~size_t{15};   // the samples start 16-aligned
        const size_t bytes = ptr_bytes + channels * block * sizeof(float);
        if (sg.span_bytes < bytes) {
            void* const span = clockwork::mem::alloc(clockwork::mem::Tier::Fast, bytes, /*allow_spill=*/false);
            if (!span) return false;
            sg.span.reset(span);
            sg.span_bytes = bytes;
        }
        auto* const base = static_cast<uint8_t*>(sg.span.get());
        auto* const samples = reinterpret_cast<float*>(base + ptr_bytes);
        std::memset(samples, 0, channels * block * sizeof(float));
        sg.out_ptrs = reinterpret_cast<float**>(base);
        sg.in_ptrs  = reinterpret_cast<const float**>(base) + out;
        sg.out = out ? samples : nullptr;
        sg.in  = in ? samples + static_cast<size_t>(out) * block : nullptr;
        for (uint32_t c = 0; c < out; ++c) sg.out_ptrs[c] = sg.out + static_cast<size_t>(c) * block;
        for (uint32_t c = 0; c < in; ++c)  sg.in_ptrs[c]  = sg.in + static_cast<size_t>(c) * block;
        return true;
    }

    // Sources into the input bus. Called immediately before dsp_process, after
    // whatever the device backend wrote there: a bound source OWNS its range
    // for the block.
    static void pull_port_sources(const clockwork::CoreState& st, uint32_t frames) {
        const clockwork::Build& build = st.build;
        for (const auto& b : st.port_bus.bindings) {
            const ClockworkPort p = b.port.load(std::memory_order_acquire);
            if (p == CLOCKWORK_PORT_NONE) continue;
            if (clockwork_port_direction(st.ports, p) != kClockworkPortSource) continue;
            const uint32_t first = b.first_channel;
            if (first >= build.in_channels) continue;
            uint32_t n = clockwork_port_channels(st.ports, p);
            if (n > build.in_channels - first) n = build.in_channels - first;
            if (n == 0) continue;
            std::array<float*, clockwork::kMaxChannels> ch{};
            for (uint32_t c = 0; c < n; ++c)
                ch[c] = st.staging.in + static_cast<size_t>(first + c) * build.block;
            // The return value is deliberately ignored: the shortfall is
            // already silence in the buffer and already counted on the port,
            // and there is nothing useful the audio thread could do with it.
            (void)clockwork_port_read(st.ports, p, ch.data(), n, frames);
        }
    }

    // The DSP's output into sinks. Called immediately after dsp_process, so a
    // sink records what actually left clockwork.
    static void push_port_sinks(const clockwork::CoreState& st, uint32_t frames) {
        const clockwork::Build& build = st.build;
        for (const auto& b : st.port_bus.bindings) {
            const ClockworkPort p = b.port.load(std::memory_order_acquire);
            if (p == CLOCKWORK_PORT_NONE) continue;
            if (clockwork_port_direction(st.ports, p) != kClockworkPortSink) continue;
            const uint32_t first = b.first_channel;
            if (first >= build.out_channels) continue;
            uint32_t n = clockwork_port_channels(st.ports, p);
            if (n > build.out_channels - first) n = build.out_channels - first;
            if (n == 0) continue;
            std::array<const float*, clockwork::kMaxChannels> ch{};
            for (uint32_t c = 0; c < n; ++c)
                ch[c] = st.staging.out + static_cast<size_t>(first + c) * build.block;
            (void)clockwork_port_write(st.ports, p, ch.data(), n, frames);
        }
    }

    // What the linked guest declares about itself (DspInfo): dsp_describe is
    // static, answering before any instance exists, so it is read once, the
    // first time anything asks. Null for a guest that declares nothing.
    static const DspInfo* guest_info() {
        static const DspInfo* const info = [] {
            GuestCallScope guest(GuestCall::Boot);   // what it takes from malloc is counted as its own
            return dsp_describe();
        }();
        return info;
    }
    // Whether the guest holds its own schedule. The whole schedule branch in
    // this file turns on it (ingest_frame): a guest that does receives a
    // timed message the moment it arrives, carrying its timetag; one that
    // does not has clockwork hold it until due.
    static bool guest_holds_schedule() {
        const DspInfo* info = guest_info();
        return info && info->holds_schedule != 0;
    }
    // Whether an inbound event (clockwork_event_route) is also the guest's.
    static bool guest_wants_events() {
        const DspInfo* info = guest_info();
        return info && info->wants_events != 0;
    }

    // The core whose instance's ring (RT-out) this thread is the one writer
    // of, or null: the audio thread inside process_audio(), and the restart
    // thread inside destroy_dsp() and rebuild_dsp(), which run with the audio
    // thread stopped. What an instance says goes out on that ring, in order
    // (emit_osc_parts), and so do this thread's log lines (emit_debug_osc);
    // any other thread (watchdog, recovery, boot on native), and this one
    // for any other core, writes the locked NRT-out ring, so no instance's
    // ring ever has a second concurrent writer.
    static thread_local const clockwork::CoreState* t_instance_ring_of = nullptr;
    struct InstanceRingScope {
        explicit InstanceRingScope(const clockwork::CoreState& st) : prev(t_instance_ring_of) { t_instance_ring_of = &st; }
        ~InstanceRingScope() { t_instance_ring_of = prev; }
        InstanceRingScope(const InstanceRingScope&) = delete;
        InstanceRingScope& operator=(const InstanceRingScope&) = delete;
        const clockwork::CoreState* const prev;
    };


    // A cold boot is a new engine, and starts over what the last one on this
    // core gathered (a web reload boots cold on the core it holds): its ring
    // peaks would be published as this one's, and a limit it used up would
    // keep this one's first error out of the log.
    static void start_engine_diagnostics_over(clockwork::CoreState& st) {
        st.metrics.clear_peaks();
        st.ingress.gap_log.reset();
        st.ingress.corrupt_log.reset();
        st.routing.unclaimed_log.reset();
        st.schedule.refused_log.reset();
        st.schedule.late_dropped_log.reset();
        st.schedule.late_log.reset();
    }


    // Return the base address of the ring buffer; JS derives every buffer position
    // from it.
#ifdef __EMSCRIPTEN__
    // An arena describes itself from the moment anyone can see it: the
    // worklet reads the table before it boots the engine (it sizes its views
    // from it), so the static arena carries its header from the first time
    // its base is handed out. init_memory writes the same bytes again.
    // Claim the engine's memory (g_engine_block above): the arena, then
    // `placement_bytes` for the placement arena. The worklet calls this
    // first of all, with the memArenaSize it will hand clockwork_init, so
    // the block is sized to the layout; a caller that skips it gets the
    // default size through get_arena_base. Once per process: a second call
    // answers with the block there is. 0 when the module's allocator could
    // not give the bytes.
    EMSCRIPTEN_KEEPALIVE
    int clockwork_memory_claim(uint32_t placement_bytes) {
        if (g_engine_block) return reinterpret_cast<int>(g_engine_block);
        // 0 is the build-time default, as it is for clockwork_init's arena_bytes
        if (placement_bytes == 0) placement_bytes = (uint32_t)CLOCKWORK_MEM_ARENA_SIZE;
        const size_t arena_bytes = clockwork::kDefaultLayout.arena_bytes;   // default-layout-ok: claimed before the core exists
        const size_t bytes = arena_bytes + (size_t)placement_bytes;
        void* block = std::malloc(bytes);   // 16-aligned, as the arena needs
        if (!block) return 0;
        std::memset(block, 0, arena_bytes);  // the arena starts zeroed, as static storage did
        g_engine_block       = static_cast<uint8_t*>(block);
        g_engine_block_bytes = bytes;
        return reinterpret_cast<int>(g_engine_block);
    }

    EMSCRIPTEN_KEEPALIVE
    int get_arena_base() {
        if (!g_engine_block && !clockwork_memory_claim((uint32_t)CLOCKWORK_MEM_ARENA_SIZE)) return 0;
        if (arenaHeader(g_engine_block)->magic != CLOCKWORK_ARENA_MAGIC)
            writeArenaHeader(g_engine_block, clockwork::kDefaultLayout);   // default-layout-ok: read before the core exists
        return reinterpret_cast<int>(g_engine_block);
    }

    // The product's version as the build says it (clockwork_product.h): what
    // the worklet hands its client, as getInfo().version.
    EMSCRIPTEN_KEEPALIVE
    const char* clockwork_product_version() {
        return CLOCKWORK_PRODUCT_VERSION;
    }

    // The worklet's scratch: its client handle, the poll array, the tap it
    // watches the ingress ring with. Exported under clockwork's own names,
    // not as malloc and free: with the allocator wrapped (guest_malloc.h,
    // CLOCKWORK_WEB_COUNT_GUEST_MALLOC) the linker exports those under the
    // wrappers' names, and the worklet should not have to know either way.
    // No guest call is open when the worklet calls these, so nothing here
    // is counted as the guest's; and they are the host's own scratch, made
    // once after the engine is ready, so they are left out of the count of
    // what the program mallocs after boot (guest_malloc.h), which is meant
    // to read 0.
    EMSCRIPTEN_KEEPALIVE
    void* clockwork_worklet_alloc(size_t bytes) {
        clockwork_malloc_host_scratch_begin();
        void* p = malloc(bytes);
        clockwork_malloc_host_scratch_end();
        return p;
    }
    EMSCRIPTEN_KEEPALIVE
    void clockwork_worklet_free(void* p) {
        clockwork_malloc_host_scratch_begin();
        free(p);
        clockwork_malloc_host_scratch_end();
    }
#endif

    // ── The one way out ────────────────────────────────────────────────────
    //
    // Every reply and notification clockwork emits leaves here: the answers to
    // /clockwork/, the worklet's clock replies, and — through DspHost —
    // everything the DSP says. An `origin` token goes in and DspHost::emit_osc comes
    // out, so no reply TYPE crosses the boundary at all.
    //
    // Routing is the token's: non-zero replies to that client, 0 goes to the
    // notify audience — the clients that sent /clockwork/notify, one
    // unicast each, and none at all until someone subscribes. Lane choice
    // keeps RT-out single-writer —
    // the audio thread owns it; any other thread takes the locked NRT-out ring.
    // Returns whether the frame was QUEUED, not whether anyone got it, so
    // DspHost::emit_osc can pass a refusal on.
    static uint32_t reply_route(uint32_t origin) { return origin ? EGRESS_REPLY : EGRESS_BROADCAST_NOTIFY; }

    // To a named audience (EgressRoute) — what an inbound event takes to
    // reach the clients subscribed to its subsystem — one OSC payload laid
    // down from `parts`, with nothing gathered first.
    static bool emit_osc_parts(clockwork::CoreState& st, uint32_t route, uint32_t origin,
                               const RingPart* parts, uint32_t n_parts) {
        uint8_t* const base = st.arena.laid_out();
        if (!base || n_parts == 0)
            return false;
        const clockwork::Layout& L = st.arena.layout;
        RingCursors* const cursors = clockwork::cursors_in(base, L);
        const bool useRtOut =
            t_instance_ring_of == &st || !st.egress.nrt_drained.load(std::memory_order_relaxed);
        if (useRtOut) {
            return ::ring_buffer_write(
                base + L.rt_out_ring_start, L.profile.rt_out_ring_bytes,
                &cursors->out_head, &cursors->out_tail, &cursors->out_sequence,
                route, origin, parts, n_parts, &cursors->status_flags, clockwork::metrics_in(base));
        }
        return clockwork_egress_nrt_write_parts(st.core, route, origin, parts, n_parts);
    }
    static bool emit_osc_route(clockwork::CoreState& st, uint32_t route, uint32_t origin,
                               const uint8_t* osc, uint32_t len) {
        if (!osc || len == 0) return false;
        const RingPart part{ osc, len };
        return emit_osc_parts(st, route, origin, &part, 1);
    }
    static bool emit_osc_out(clockwork::CoreState& st, uint32_t origin, const uint8_t* osc, uint32_t len) {
        return emit_osc_route(st, reply_route(origin), origin, osc, len);
    }

    // The core's state behind a host callback's context, which is the core.
    static clockwork::CoreState& state_of(void* ctx) {
        return *clockwork::state(static_cast<ClockworkCore*>(ctx));
    }

    // ── What the DSP may call back into (dsp_api.h, DspHost) ───────────────
    //
    // Five function pointers, defined below in order: emit_osc is the egress above,
    // so a DSP's reply takes the route every other reply takes; log is clockwork_log,
    // so its diagnostics land in the same debug stream; open_sink/send_sink are its
    // own output path; free_bytes gives back what clockwork allocated. A DSP needs no
    // client registry held on its behalf — it answers the origin it was given.
    // Every host callback is the engine's own work inside the guest's call:
    // HostCallScope lifts the guest fence for its body (guest_fence.h).
    static int dsp_host_emit_osc(void* ctx, uint32_t origin,
                           const uint8_t* bytes, uint32_t len) {
        HostCallScope host;
        return emit_osc_out(state_of(ctx), origin, bytes, len) ? 1 : 0;
    }

    static void log_to(clockwork::CoreState& st, const char* text);
    static void dsp_host_log(void* ctx, int /*level*/, const char* text) {
        HostCallScope host;
        if (text) log_to(state_of(ctx), text);
    }

    // ── The DSP's own sinks ────────────────────────────────────────────────
    //
    // memory_profile.h's "<cell bytes>:<cells>,..." per kind, installed in the
    // core's table with clockwork_sink_profile (sink_profile.h). A string
    // that does not parse installs nothing: the crate's default for the kind
    // stands, and the log says which.
    static void install_sink_profiles(const clockwork::CoreState& st) {
        if (!clockwork_install_sink_classes(st.sinks, kClockworkSinkOsc, CLOCKWORK_OSC_SINK_CLASSES))
            clockwork_log(st.core, "WARNING: OSC sink classes \"%s\" not installed — using the built-in shape",
                          CLOCKWORK_OSC_SINK_CLASSES);
        if (!clockwork_install_sink_classes(st.sinks, kClockworkSinkMidi, CLOCKWORK_MIDI_SINK_CLASSES))
            clockwork_log(st.core, "WARNING: MIDI sink classes \"%s\" not installed — using the built-in shape",
                          CLOCKWORK_MIDI_SINK_CLASSES);
    }

    // dsp_api.h gives a DSP a handle and a send and no close, because a DSP's
    // sinks last exactly as long as the DSP. The table keeps that promise:
    // a sink the guest opens is marked the guest's, and every one is closed
    // when the instance is freed (close_dsp_sinks). Without it a device
    // switch — which frees and rebuilds the instance — would leak a slot per
    // sink per switch, and the leak would present as sinks that stop working
    // after a few switches rather than as anything readable.
    static ClockworkSink dsp_host_open_sink(void* ctx, ClockworkSinkKind kind,
                                            const char* target, uint32_t capacity) {
        HostCallScope host;
        return clockwork_sink_open_guest(state_of(ctx).sinks, kind, target, capacity);
    }

    static int dsp_host_send_sink(void* ctx, ClockworkSink sink,
                                  const uint8_t* bytes, uint32_t len, ClockworkTimetag when) {
        HostCallScope host;
        return clockwork_sink_send(state_of(ctx).sinks, sink, bytes, len, when);
    }

    static void close_dsp_sinks(const clockwork::CoreState& st) {
        clockwork_sink_close_guests(st.sinks);
    }

    // alloc_bytes hands out kGuestBlockAlign-aligned blocks (dsp_api.h); the
    // pool's own blocks are 16-aligned. Each block is taken kGuestBlockAlign
    // bytes larger, the pointer handed out is rounded up to kGuestBlockAlign,
    // and the pool's pointer is stored in the word just below it.
    static constexpr size_t kGuestBlockAlign = 64;

    // ── The guest's memory (clockwork_heap.h) ───────────────────────────────
    //
    // What alloc_bytes hands out: from the core's guest pool on the thread
    // holding the core's gate, lock-free; on any other thread from the
    // host's bulk tier, on that thread, and routed back by address when it
    // is given back. With no pool running there is none to give.


    static void free_to_bulk(void* p) { clockwork::mem::free(p); }

    // The core's own pool (clockwork_heap.h): what clockwork's Rust
    // subsystems make inside a door. Its block is taken once, at the core's
    // first init, from the fast tier, and given back when the core goes,
    // empty. The web takes it first in its arena, so a start-over finds it
    // where it was.
#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
    // Natively in whole chunks, as the core's block is (above).
    static void* own_pool_block() { return clockwork::pages::map(CLOCKWORK_OWN_POOL_BYTES); }
    static void own_pool_give_back(void* p) { clockwork::pages::unmap(p, CLOCKWORK_OWN_POOL_BYTES); }
#else
    static void* own_pool_block() { return clockwork::mem::alloc(clockwork::mem::Tier::Fast, CLOCKWORK_OWN_POOL_BYTES); }
    static void own_pool_give_back(void* p) { clockwork::mem::free(p); }
#endif

    static void own_pool_start(clockwork::CoreState& st) {
        if (CLOCKWORK_OWN_POOL_BYTES < 65536 || clockwork_heap_running(st.heap, CLOCKWORK_POOL_OWN)) return;
        void* span = own_pool_block();
        if (!span) {
            clockwork_log(st.core, "own pool: no %u bytes for it; clockwork's Rust subsystems use the system allocator",
                          (unsigned)CLOCKWORK_OWN_POOL_BYTES);
            return;
        }
        if (!clockwork_heap_start(st.heap, CLOCKWORK_POOL_OWN, span, CLOCKWORK_OWN_POOL_BYTES, own_pool_give_back))
            own_pool_give_back(span);
    }

#ifdef __EMSCRIPTEN__
    // After the arena the own pool lives in started over: take its block
    // again, and say whether it came back where it was. True when it did, or
    // when there is none.
    static bool own_pool_reclaim(const clockwork::CoreState& st) {
        size_t bytes = 0;
        void* span = clockwork_heap_span(st.heap, CLOCKWORK_POOL_OWN, &bytes);
        return !span || clockwork::mem::alloc(clockwork::mem::Tier::Fast, bytes) == span;
    }
#endif

    // CLOCKWORK_POOL_CHECK=1 in the environment: walk the whole pool after
    // every allocation and free by the gate's holder and stop at the first
    // that leaves it broken, naming the operation, the bad block and the
    // allocation before it (the one that most likely overran). A diagnostic,
    // not a build setting: the walk is the whole pool, every time.
    static bool pool_check_wanted() {
        static const bool wanted = [] {
            const char* v = std::getenv("CLOCKWORK_POOL_CHECK");
            return v && *v && *v != '0';
        }();
        return wanted;
    }
    static void pool_check(const clockwork::CoreState& st, const char* op, const void* ptr, size_t bytes) {
        if (!pool_check_wanted()) return;
        const ClockworkPoolCheck c = clockwork_heap_check(st.heap, CLOCKWORK_POOL_GUEST);
        if (c.code == 0) return;
        clockwork_log(st.core, "guest memory: BROKEN after %s(0x%08x, %zu): check %d at block 0x%08x; the block before it is payload 0x%08x + %zu",
                      op, (unsigned)(uintptr_t)ptr, bytes, (int)c.code, (unsigned)c.bad, (unsigned)c.prev_payload, c.prev_size);
        fprintf(stderr, "guest memory: BROKEN after %s(%p, %zu): check %d at block %p; the block before it is payload %p + %zu\n",   // stdio-ok: a hunt
                op, ptr, bytes, (int)c.code, (void*)c.bad, (void*)c.prev_payload, c.prev_size);
        std::abort();
    }

    static void* guest_take(const clockwork::CoreState& st, size_t bytes) {
        if (!clockwork_heap_running(st.heap, CLOCKWORK_POOL_GUEST)) return nullptr;
        if (!clockwork::holds_gate(st.core)) return clockwork::mem::alloc(clockwork::mem::Tier::Bulk, bytes);
        void* p = clockwork_heap_take(st.heap, CLOCKWORK_POOL_GUEST, bytes);
        pool_check(st, "alloc", p, bytes);
        return p;
    }

    static void guest_give(const clockwork::CoreState& st, void* raw) {
        const bool gate = clockwork::holds_gate(st.core);
        const uint32_t refused = gate ? clockwork_heap_census(st.heap, CLOCKWORK_POOL_GUEST).double_frees : 0;
        if (clockwork_heap_give(st.heap, CLOCKWORK_POOL_GUEST, raw)) {
            if (!gate) return;
            pool_check(st, "free", raw, 0);
            const uint32_t now = clockwork_heap_census(st.heap, CLOCKWORK_POOL_GUEST).double_frees;
            if (now != refused)
                clockwork_log(st.core, "guest memory: the guest freed 0x%08x twice (refused; %u so far)",
                              (unsigned)(uintptr_t)raw, (unsigned)now);
            return;
        }
        // The host's own memory, taken by a thread without the gate. Freed by
        // the gate's holder it would be a malloc-family call on the audio
        // thread: parked, for the next call into the core that is not a block.
        if (gate && clockwork_heap_running(st.heap, CLOCKWORK_POOL_GUEST)) clockwork_heap_park_system(st.heap, raw);
        else                                               clockwork::mem::free(raw);
    }

    void clockwork_release_parked_frees(ClockworkCore* core) {
        if (const clockwork::CoreState* st = clockwork::state(core))
            clockwork_heap_release_system(st->heap, free_to_bulk);
    }

    // Give back bytes taken through alloc_bytes (dsp_api.h). Only the
    // pointer crosses: what the bytes were is the guest's business.
    static void dsp_host_free_bytes(void* ctx, void* ptr) {
        // What arrives here came from alloc_bytes, on every target: a guest
        // whose buffer points into the client's assets (an asset, or a sample
        // the client placed there) does not hand that range to us — the
        // assets is the client's and the guest knows so (dsp_api.h, "Bulk").
        // So the free is real on the web too; a no-op here leaks every block
        // the guest frees.
        HostCallScope host;
        if (ptr) guest_give(state_of(ctx), static_cast<void**>(ptr)[-1]);
    }

    // The pair of the above: the guest's memory, above. Lock-free, so this
    // is callable on the audio thread, where it answers NULL rather than
    // blocking when it cannot meet a request (dsp_api.h).
    static void* dsp_host_alloc_bytes(void* ctx, size_t bytes) {
        HostCallScope host;
        if (bytes == 0 || bytes > SIZE_MAX - kGuestBlockAlign) return nullptr;
        void* raw = guest_take(state_of(ctx), bytes + kGuestBlockAlign);
        if (!raw) return nullptr;
        // raw is 16-aligned, so this lands 16 to 64 bytes above it.
        const uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + sizeof(void*) + kGuestBlockAlign - 1)
                                  & ~uintptr_t(kGuestBlockAlign - 1);
        void** block = reinterpret_cast<void**>(aligned);
        block[-1] = raw;
        return block;
    }

    // ── Taps (dsp_api.h, "Taps") ───────────────────────────────────────
    //
    // A DSP publishes a tap by claiming a slot and appending blocks; the
    // slot's layout (shm_tap_stream) and the anchor every block is stamped
    // with (the clock's engine frames) stay on this side. The handle passes straight
    // through to the substrate, which owns slots by the handle's address.
    static int dsp_host_tap_open(void* ctx, uint32_t index, uint32_t channels,
                                   DspTapHandle* hnd) {
        HostCallScope host;
        if (!hnd) return 0;
        if (index > (uint32_t)INT32_MAX || channels > (uint32_t)INT32_MAX) return 0;
        auto* const core = static_cast<ClockworkCore*>(ctx);
        if (!clockwork_tap_get(clockwork_core_taps(core), clockwork::arena_base(core),
                               (int)index, (int)channels, 0, hnd)) return 0;
        auto* slot = static_cast<shm_tap_stream*>(hnd->slot);
        hnd->channels = slot ? slot->channels : 0;
        return 1;
    }

    static void dsp_host_tap_write(void* ctx, DspTapHandle* hnd,
                                     const float* const* channels, uint32_t n_channels,
                                     uint32_t frames) {
        HostCallScope host;
        if (!hnd || !hnd->slot || !channels || n_channels == 0 || frames == 0) return;
        // The writer takes a full set of channel pointers; short by some, the
        // last one given fills the rest, as ScopeOut2 always did.
        std::array<const float*, TAP_STREAM_CHANNELS> padded{};
        for (uint32_t c = 0; c < TAP_STREAM_CHANNELS; ++c)
            padded[c] = channels[c < n_channels ? c : n_channels - 1];
        auto* const core = static_cast<ClockworkCore*>(ctx);
        shm_tap_stream_writer writer(static_cast<shm_tap_stream*>(hnd->slot),
                                     clockwork::layout_of(core).profile.tap_ring_frames);
        const ClockworkClock* clock = clockwork::clock_of(core);
        writer.write(padded.data(), frames, clock ? clock->engineFrames() : 0);
    }

    static void dsp_host_tap_close(void* ctx, DspTapHandle* hnd) {
        HostCallScope host;
        auto* const core = static_cast<ClockworkCore*>(ctx);
        if (hnd) clockwork_tap_release(clockwork_core_taps(core), clockwork::arena_base(core), hnd);
    }


    // ── Assets (dsp_api.h, "Assets") ───────────────────────────────────────
    //
    // The audio thread's half of the facility: check a committed range against
    // the lane, hand the guest a pointer, and keep the committed asset in the
    // core's table (Assets, core_state.h).
    using Asset = clockwork::Assets::Slot;

    void asset_emit(clockwork::CoreState& st, uint32_t origin, const char* address, uint32_t id, const char* reason) {
        char buf[192];
        osc::OutboundPacketStream ps(buf, sizeof buf);
        ps << osc::BeginMessage(address) << static_cast<int32_t>(id);
        if (reason) ps << reason;
        ps << osc::EndMessage;
        emit_osc_out(st, origin, reinterpret_cast<const uint8_t*>(ps.Data()),
                     static_cast<uint32_t>(ps.Size()));
    }

    // A new instance's first word on its ring is its /clockwork/setup
    // (docs/RESTARTS.md) whenever it replaces one: a rebuild's, or a new
    // arena's that carries a generation on. Said by the build, with the audio
    // thread stopped (announce_instance). A change the instance survives, of
    // a fact its setup named — the rate, the device's callback — is said as
    // /clockwork/setup/changed, naming the generation it has, as audio
    // resumes (clockwork_announce_facts). Either on its ring, so a reader
    // places it among the instance's words; and either, with no room on the
    // ring then — a reader far behind — owed, the instance not running until
    // it has been said (process_audio), so nothing of the instance's goes
    // ahead of it.
    using Owed = clockwork::SetupNotice::Owed;

    // "/clockwork/setup ,iii <rate> <device buffer> <generation>", or the same
    // as /clockwork/setup/changed, on the instance's ring: whichever is owed.
    // True once said, or when nothing is.
    static bool say_owed(clockwork::CoreState& st) {
        clockwork::SetupNotice& sn = st.setup;
        const Owed owed = sn.owed.load(std::memory_order_acquire);
        if (owed == Owed::Nothing) return true;
        char buf[96];
        osc::OutboundPacketStream ps(buf, sizeof buf);
        ps << osc::BeginMessage(owed == Owed::Setup ? CLOCKWORK_SYS("setup") : CLOCKWORK_SYS("setup/changed"))
           << sn.rate.load(std::memory_order_relaxed)
           << static_cast<int32_t>(sn.buffer_frames.load(std::memory_order_relaxed))
           << static_cast<int32_t>(arenaInstanceGeneration(st.arena.at()))
           << osc::EndMessage;
        if (!emit_osc_route(st, EGRESS_BROADCAST_NOTIFY, 0, reinterpret_cast<const uint8_t*>(ps.Data()),
                            static_cast<uint32_t>(ps.Size())))
            return false;
        sn.owed.store(Owed::Nothing, std::memory_order_relaxed);
        return true;
    }

    // The device's callback as setup names it: for a host with no device, the block.
    static uint32_t setup_buffer_frames(const clockwork::CoreState& st, uint32_t buffer_frames) {
        return buffer_frames ? buffer_frames : clockwork_block_size(st.core);
    }

    // The new instance announces itself: its setup, naming the device's
    // callback (or, for a host with no device yet, the block), written on its
    // ring as the build's last word, or owed until the ring has room.
    static void announce_instance(clockwork::CoreState& st, uint32_t buffer_frames) {
        InstanceRingScope instance_ring(st);
        clockwork::SetupNotice& sn = st.setup;
        sn.rate.store(static_cast<int32_t>(st.build.sample_rate), std::memory_order_relaxed);
        sn.buffer_frames.store(setup_buffer_frames(st, buffer_frames), std::memory_order_relaxed);
        sn.owed.store(Owed::Setup, std::memory_order_release);
        say_owed(st);
    }

    void clockwork_announce_facts(ClockworkCore* core, double sample_rate, uint32_t buffer_frames) {
        clockwork::CoreState* st = clockwork::state(core);
        if (!st || !st->arena.laid_out() || !st->guest.instance) return;
        InstanceRingScope instance_ring(*st);
        clockwork::SetupNotice& sn = st->setup;
        sn.rate.store(static_cast<int32_t>(sample_rate), std::memory_order_relaxed);
        sn.buffer_frames.store(setup_buffer_frames(*st, buffer_frames), std::memory_order_relaxed);
        // A setup still owed says the new facts itself.
        Owed nothing = Owed::Nothing;
        sn.owed.compare_exchange_strong(nothing, Owed::Changed, std::memory_order_acq_rel);
        say_owed(*st);
    }

    // DspHost::asset_release — the guest is done; the committing client hears.
    static int dsp_host_asset_release(void* ctx, uint32_t id) {
        HostCallScope host;
        clockwork::CoreState& st = state_of(ctx);
        Asset* s = st.assets.lookup(id);
        if (!s) return 0;
        const uint32_t origin = s->origin;
        clockwork::Assets::remove(*s);
        asset_emit(st, origin, CLOCKWORK_SYS("asset/released"), id, nullptr);
        return 1;
    }

    // Every asset released: each committer told, and the table forgotten.
    // When the engine goes (teardown_memory), and when audio comes back to an
    // assets area that has moved, where no committed asset points at
    // anything any more.
    void assets_release_all(clockwork::CoreState& st) {
        for (Asset& s : st.assets) {
            if (s.used && s.id != clockwork::Assets::kTombstone)
                asset_emit(st, s.origin, CLOCKWORK_SYS("asset/released"), s.id, nullptr);
            s = Asset{};
        }
    }

    // A new instance is handed every asset the last one held, as it is built
    // and before it says setup (docs/RESTARTS.md): the bytes are the client's,
    // in the assets area a rebuild keeps, so nothing a client lent is lost to
    // a rebuild and nothing has to be sent again. An asset the new instance
    // will not take is released there, and its committer hears so, as from a
    // guest that let go.
    static void hand_over_assets(clockwork::CoreState& st) {
        const ClockworkConfig& cfg = st.config;
        const auto* assets = static_cast<const uint8_t*>(cfg.assets);
        for (Asset& s : st.assets) {
            if (!s.used || s.id == clockwork::Assets::kTombstone) continue;
            ClockworkAsset asset {};
            asset.struct_bytes = sizeof asset;
            asset.id           = s.id;
            asset.kind         = s.kind;
            asset.origin       = s.origin;
            asset.bytes        = assets ? assets + s.offset : nullptr;
            asset.byte_count   = s.byte_count;
            asset.channels     = s.channels;
            asset.frames       = s.frames;
            asset.sample_rate  = s.sample_rate;
            int rc = 1;
            if (assets) { GuestCallScope guest(GuestCall::Processing); rc = dsp_asset(st.guest.instance, &asset); }
            if (rc == 0) continue;
            const uint32_t origin = s.origin, id = s.id;
            clockwork::Assets::remove(s);
            asset_emit(st, origin, CLOCKWORK_SYS("asset/released"), id, nullptr);
        }
    }

    // DspHost::flush_schedule — the bundles held for the guest (SCHED_TAG_SYNTH),
    // dropped. Not the default tag: that is clockwork's own schedule verb,
    // and not the guest's to clear.
    static void dsp_host_flush_schedule(void* ctx) {
        HostCallScope host;
#if CLOCKWORK_SCHEDULER
        if (EngineScheduler* q = clockwork_engine_schedule(static_cast<ClockworkCore*>(ctx))) q->flush(SCHED_TAG_SYNTH);
#else
        (void)ctx;
#endif
    }

    // DspHost::param_define, param_value, param_set — the guest's three calls
    // on the parameter table (clockwork_params.h), its entries its own.
    static int32_t dsp_host_param_define(void* ctx, const struct ClockworkParamEntry* entries, uint32_t count,
                                         const uint8_t* text, uint32_t text_bytes, uint32_t* slots) {
        HostCallScope host;
        const char* why = nullptr;
        const int32_t n = clockwork_param_define(static_cast<ClockworkCore*>(ctx), entries, count, text, text_bytes,
                                                 CLOCKWORK_PARAM_DEFINER_GUEST, slots, &why);
        if (n < 0 && why) clockwork_log(static_cast<ClockworkCore*>(ctx), "ERROR: the guest's parameters were refused: %s", why);
        return n;
    }
    static float dsp_host_param_value(void* ctx, uint32_t slot) {
        HostCallScope host;
        return clockwork_param_value(static_cast<ClockworkCore*>(ctx), slot);
    }
    static int dsp_host_param_set(void* ctx, uint32_t slot, float value) {
        HostCallScope host;
        return clockwork_param_set(static_cast<ClockworkCore*>(ctx), slot, value, CLOCKWORK_PARAM_DEFINER_GUEST);
    }

    // What the guest is handed: the callbacks, with its core as their context.
    static DspHost dsp_host_for(ClockworkCore* core) {
        return DspHost{ core, &dsp_host_emit_osc, &dsp_host_log,
                        &dsp_host_open_sink, &dsp_host_send_sink,
                        &dsp_host_free_bytes, &dsp_host_asset_release,
                        &dsp_host_alloc_bytes,
                        &dsp_host_tap_open, &dsp_host_tap_write,
                        &dsp_host_tap_close, &dsp_host_flush_schedule,
                        &dsp_host_param_define, &dsp_host_param_value, &dsp_host_param_set };
    }

#if CLOCKWORK_SYNTH
    // The RT egress as a generic ReplyChannel: emit one OSC to the OUT ring,
    // routed by the per-call token. Lets the audio thread's control routes
    // (the /clockwork/clock namespace) reply through the backend contract without
    // any DSP-specific reply type. Shared by every core with no control thread.
    void rt_reply_emit(void* ctx, uint32_t token, const uint8_t* osc, uint32_t len) {
        emit_osc_out(state_of(ctx), token, osc, len);
    }
#endif

    // The metrics that are constant for a session: which Clockwork, the rate
    // and block it runs at, and how much its scheduler can hold — events and
    // bytes, since it is full when it runs out of either. And how often the
    // rest are written, which follows from the rate and block.
    static void publish_session_metrics(clockwork::CoreState& st, double sample_rate, int buf_length) {
        const auto rate  = static_cast<uint32_t>(std::lround(sample_rate));
        const auto block = static_cast<uint32_t>(buf_length);
        PerformanceMetrics* const metrics = clockwork::metrics_in(st.arena.at());
        metrics->clockwork_commit.store(CLOCKWORK_COMMIT_WORD, std::memory_order_relaxed);   // which Clockwork (clockwork_config.h)
        metrics->audio_sample_rate.store(rate, std::memory_order_relaxed);
        metrics->audio_block_size.store(block, std::memory_order_relaxed);
        st.metrics.period.store(clockwork::MetricsFlush::period_for(rate, block), std::memory_order_relaxed);
#if CLOCKWORK_SCHEDULER
        metrics->scheduler_capacity.store(SCHEDULER_SLOT_COUNT, std::memory_order_relaxed);
        metrics->scheduler_pool_bytes.store(SCHEDULER_DATA_POOL_SIZE, std::memory_order_relaxed);
#else
        metrics->scheduler_capacity.store(0, std::memory_order_relaxed);
        metrics->scheduler_pool_bytes.store(0, std::memory_order_relaxed);
#endif
    }

    static inline void update_scheduler_depth_metric(clockwork::CoreState& st, uint32_t depth) {
        PerformanceMetrics* const metrics = clockwork::metrics_in(st.arena.at());
        if (!metrics) {
            return;
        }

        metrics->scheduler_queue_depth.store(depth, std::memory_order_relaxed);

        uint32_t observed = metrics->scheduler_queue_max.load(std::memory_order_relaxed);
        while (depth > observed &&
               !metrics->scheduler_queue_max.compare_exchange_weak(
                   observed, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    static inline void increment_scheduler_drop_metric(clockwork::CoreState& st) {
        PerformanceMetrics* const metrics = clockwork::metrics_in(st.arena.at());
        if (!metrics) {
            return;
        }
        metrics->scheduler_queue_dropped.fetch_add(1, std::memory_order_relaxed);
    }

#if !CLOCKWORK_SCHEDULER
    // "/clockwork/schedule" arrived in a build with no timed store. It cannot be
    // held and it cannot be delivered on time, so it is refused to the client
    // that sent it — and refused OUT LOUD, naming the switch, because the failure mode this option must
    // not have is a verb that silently stops working. Counted into the same
    // drop metric as a pool-full refusal, since to a client both mean "
    // clockwork did not take it".
    //
    // Rate-limited the way the LATE/SHED logs are: a client that schedules in
    // a loop would otherwise fill the debug stream with one identical line per
    // message, which buries the first one — the only one anybody reads.
    static inline void refuse_schedule_verb(clockwork::CoreState& st, const uint8_t* data, uint32_t len, uint32_t origin) {
        increment_scheduler_drop_metric(st);
        clockwork_sys_refuse(data, len, CLOCKWORK_SCHEDULER_ABSENT, arenaInstanceGeneration(st.arena.at()),
            [&st, origin](const uint8_t* d, uint32_t n) { emit_osc_out(st, origin, d, n); });
        if (st.schedule.refused_log.say())
            clockwork_log(st.core, "ERROR: " CLOCKWORK_SYS("schedule") " refused — this build has no "
                   "timed store (built with CLOCKWORK_SCHEDULER=OFF; that "
                   "also removes " CLOCKWORK_SYS("sched/flush") ", timed MIDI out and "
                   "timed OSC forwarding). Immediate messages are unaffected, "
                   "and a timestamped bundle still reaches the DSP with its "
                   "timetag.");
    }
#endif

    // The latest a timed event may play, set by the host (lanes.h): 0 allows
    // any lateness. A host whose engine can be flooded sets one, because a
    // backlog played seconds late is worse than a backlog not played;
    // clockwork itself never drops for lateness.
    EMSCRIPTEN_KEEPALIVE void clockwork_set_timed_event_late_limit_ms(ClockworkCore* core, uint32_t ms) {
        if (clockwork::Schedule* sc = clockwork::schedule(core))
            sc->late_limit_ms.store(ms, std::memory_order_relaxed);
    }
    static inline void note_late_dropped(clockwork::CoreState& st, double late_ms, uint32_t limit_ms) {
        if (PerformanceMetrics* m = clockwork::metrics_in(st.arena.at()))
            m->scheduler_late_dropped.fetch_add(1, std::memory_order_relaxed);
        if (st.schedule.late_dropped_log.say())
            clockwork_log(st.core, "[schedule] a timed event %.0f ms late was dropped: the host allows %u ms", late_ms, limit_ms);
    }

    // Mark everything pending in the IN ring as of NOW (the marks, above):
    // frames for the guest sequenced before it are answered to their sender
    // with `reason`, naming the generation as it reads now, and not
    // delivered; later ones dispatch normally, and clockwork's own are
    // delivered whatever their sequence. On the audio thread, or with it
    // stopped (the marks, above). `reason` is one of the CLOCKWORK_REFUSED_*
    // phrases.
    //
    // Taken without the ring's writer lock, so the audio thread may call it
    // (the wake's purge): a writer mid-write lands on either side, and is
    // answered either way. A teardown wants the mark exact, and takes the
    // lock round it (mark_closing_generation).
    static void mark_ingress(ClockworkCore* core, const char* reason, bool onlyDue) {
        clockwork::Ingress* in = clockwork::ingress(core);
        uint8_t* const base = clockwork::laid_out_base(core);
        if (!in || !base) return;
        in->marks.set(static_cast<uint32_t>(clockwork::cursors_in(base, clockwork::layout_of(core))
                                                ->in_sequence.load(std::memory_order_acquire)),
                       arenaInstanceGeneration(base), reason, onlyDue);
    }
    void clockwork_ingress_mark(ClockworkCore* core, const char* reason) {
        mark_ingress(core, reason ? reason : CLOCKWORK_REFUSED_REBUILDING, false);
    }

    // What is pending as of now waited while the audio was away — a stall, a
    // page's context suspended — and what is due of it is stale: refused,
    // each to its sender ("stalled"), by the next drain (the marks, above). A
    // bundle for later is not, and goes through to play at its time. On the
    // audio thread, or with it stopped.
    void clockwork_ingress_mark_stale(ClockworkCore* core) {
        mark_ingress(core, CLOCKWORK_REFUSED_STALLED, true);
    }

    // A bundle whose time is not yet come at `blockTime`, this block's start
    // in OSC time: it plays on time whatever it waited through.
    static bool bundle_for_later(const uint8_t* payload, uint32_t payload_size, clockwork::Timetag blockTime) {
        if (!clockwork_is_bundle(payload, payload_size)) return false;
        const clockwork::Timetag tt = clockwork_bundle_timetag(payload);
        return !tt.isImmediate() && tt >= blockTime;
    }

    // THE AUDIO WAS AWAY: the one rule, on every target. The gap is the time
    // that passed since the last block less the audio rendered in it — by the
    // host's monotonic clock where this host and the last both had one (a
    // system clock set by hand is not a gap), by its wall clock otherwise —
    // against the record of the last block rendered in the arena, whichever
    // host rendered it (a worklet made again over the same memory, an
    // the device thread back from a stall).
    // Measured against the audio rendered, a huge device buffer rendering a
    // block every 170 ms is not a gap. Over kClockStepSeconds the clock
    // steps at this block: the host is told so and anchors it afresh (a
    // clock converged from seconds behind runs fast until it has caught up,
    // and everything scheduled against it plays compressed). Nothing that
    // waited is dropped for it: what queued is delivered and what was
    // scheduled fires, late by the gap, as scsynth does after a
    // sleep — Clockwork drops a message only for want of room, and counts
    // it (PerformanceMetrics); a host that wants what waited gone says so
    // itself (clockwork_ingress_mark_stale, the page's purge()). One line
    // says the gap. process_audio calls audio_gap once per block, for every
    // host, before the clock moves. clockwork_audio_starts also calls it when
    // a host's audio starts, with audio_secs = -1: the gap is then judged by
    // time alone, and the first block after it is never a gap because the
    // host's frame count starts over.
    static bool audio_gap(ClockworkCore* core, double wall_ntp, double monotonic_secs, double audio_secs) {
        clockwork::CoreClock* c = clockwork::core_clock(core);
        uint8_t* const base = clockwork::laid_out_base(core);
        if (!c || !base) return false;
        uint8_t* const sample_clock = base + SAMPLE_CLOCK_START;
        const double last_wall = read_last_block_wall(sample_clock);
        const double last_mono = read_last_block_mono(sample_clock);
        const double last_audio = c->last_audio_secs;
        write_last_block_time(sample_clock, wall_ntp, monotonic_secs);
        c->last_audio_secs = audio_secs;
        const bool byMono = monotonic_secs > 0.0 && last_mono > 0.0;
        const double passed = byMono ? monotonic_secs - last_mono : (last_wall > 0.0 ? wall_ntp - last_wall : 0.0);
        if (audio_secs >= 0.0 && last_audio < 0.0) return false;   // the first block after a start
        const double rendered = (audio_secs >= 0.0 && audio_secs > last_audio) ? audio_secs - last_audio : 0.0;
        const double gap = passed - rendered;
        if (gap <= clockwork::kClockStepSeconds) return false;
        clockwork_log(core, "[clock] audio resumed after %.3fs", gap);
        return true;
    }

    EMSCRIPTEN_KEEPALIVE bool clockwork_audio_back(ClockworkCore* core, double wall_ntp, double monotonic_secs,
                                                   double audio_secs) {
        return audio_gap(core, wall_ntp, monotonic_secs, audio_secs);
    }
    // clockwork_audio_starts: the host's audio is starting (a device opened
    // with its sample count at 0, a new worklet over the same memory whose
    // AudioContext time may have restarted, an embedded host attaching). The
    // next block re-anchors the clock, never below the sample clock already
    // in the arena.
    EMSCRIPTEN_KEEPALIVE bool clockwork_audio_starts(ClockworkCore* core, double wall_ntp, double monotonic_secs) {
        clockwork::CoreClock* c = clockwork::core_clock(core);
        if (!c) return false;
        c->anchor_requested.store(true, std::memory_order_release);
        // What a host said of its transport was for frames of the count that has just ended.
        c->host_word_count = 0;
        c->host_line = false;
        return audio_gap(core, wall_ntp, monotonic_secs, -1.0);
    }

    // What the client asked to be told (lanes.h).
    EMSCRIPTEN_KEEPALIVE bool clockwork_client_wants(ClockworkCore* core, uint32_t kinds, bool on) {
        clockwork::Routing* r = clockwork::routing(core);
        kinds &= CLOCKWORK_WANTS_ALL;
        if (!r || !kinds) return false;
        const uint32_t was = on ? r->client_wants.fetch_or(kinds, std::memory_order_acq_rel)
                                : r->client_wants.fetch_and(~kinds, std::memory_order_acq_rel);
        return on ? (was & kinds) != kinds : (was & kinds) != 0u;
    }
    EMSCRIPTEN_KEEPALIVE uint32_t clockwork_client_wanted(ClockworkCore* core) {
        clockwork::Routing* r = clockwork::routing(core);
        return r ? r->client_wants.load(std::memory_order_acquire) : 0u;
    }
    EMSCRIPTEN_KEEPALIVE void clockwork_client_gone(ClockworkCore* core) {
        if (clockwork::Routing* r = clockwork::routing(core)) r->client_wants.store(0u, std::memory_order_release);
    }

    // A host that says its transport (lanes.h). What it says waits in the
    // core for the tick whose block its frame is in.
    static bool host_says(ClockworkCore* core, const clockwork::HostWord& said) {
        clockwork::CoreClock* c = clockwork::core_clock(core);
        if (!c) return false;
        if (c->host_word_count == clockwork::CoreClock::kHostWords) return false;
        c->host_words[c->host_word_count++] = said;
        c->host_said.store(true, std::memory_order_release);
        return true;
    }
    EMSCRIPTEN_KEEPALIVE bool clockwork_host_tempo(ClockworkCore* core, double bpm, uint64_t frame) {
        if (!clockwork::hostTempoValid(bpm)) return false;
        clockwork::HostWord said;
        said.kind = clockwork::HostWord::Tempo;
        said.value = bpm;
        said.frame = frame;
        return host_says(core, said);
    }
    EMSCRIPTEN_KEEPALIVE bool clockwork_host_meter(ClockworkCore* core, int32_t num, int32_t den, uint64_t frame) {
        if (!clockwork_timeline_meter_valid(num, den)) return false;
        clockwork::HostWord said;
        said.kind = clockwork::HostWord::Meter;
        said.num = num;
        said.den = den;
        said.frame = frame;
        return host_says(core, said);
    }
    EMSCRIPTEN_KEEPALIVE bool clockwork_host_playing(ClockworkCore* core, bool playing, uint64_t frame) {
        clockwork::HostWord said;
        said.kind = clockwork::HostWord::Playing;
        said.playing = playing;
        said.frame = frame;
        return host_says(core, said);
    }
    EMSCRIPTEN_KEEPALIVE bool clockwork_host_beat(ClockworkCore* core, double beat, uint32_t how, uint64_t frame) {
        if (!std::isfinite(beat) || (how != CLOCKWORK_BEAT_REQUEST && how != CLOCKWORK_BEAT_FORCE)) return false;
        clockwork::HostWord said;
        said.kind = clockwork::HostWord::Beat;
        said.value = beat;
        said.how = how;
        said.frame = frame;
        return host_says(core, said);
    }

    // The teardown's mark, exact: taken under the in ring's writer lock once
    // the generation word has gone odd. Every door asks its question under
    // the same lock (in_ring_door.h), so a message whose writer saw the
    // instance up is below the mark, and refused as the closing generation's
    // by the next drain; one written after reads the word odd and is refused
    // at the door. Called with the audio thread stopped.
    static void mark_closing_generation(clockwork::CoreState& st) {
        RingCursors* const cursors = clockwork::cursors_in(st.arena.laid_out(), st.arena.layout);
        if (!cursors) return;
        RingBufferWriter::lock(&cursors->in_write_lock);
        clockwork_ingress_mark(st.core, CLOCKWORK_REFUSED_REBUILDING);
        RingBufferWriter::unlock(&cursors->in_write_lock);
    }

    EMSCRIPTEN_KEEPALIVE
    void clear_scheduler(ClockworkCore* core) {
        clockwork::CoreState* st = clockwork::state(core);
        if (!st) return;
        st->ingress.seq_reset.store(true, std::memory_order_relaxed);
        st->ingress.gap_log.reset();
        st->ingress.corrupt_log.reset();
        st->metrics.clear_peaks();
        update_scheduler_depth_metric(*st, 0);
#if CLOCKWORK_SCHEDULER
        // Cross-thread handshake: the control thread asks, the audio thread
        // clears at a safe point (process_audio's drainPendingClear below).
        if (EngineScheduler* q = clockwork_engine_schedule(core)) q->requestClear();
#endif
    }

    // ── The two engine entry points, shared by every host (WASM/native/embedded).
    // Once OSC bytes leave the IN ring, everything funnels through these. ─────────

    // Route an OSC message to its handler NOW (the DSP, or a clockwork route).
    // `when` is the message's intended OSC timetag (or immediately) and
    // `blockTime` this block's start in OSC time. Both are passed through to
    // the DSP, which is the only side that can place an event within a block
    // (dsp_api.h); every clockwork route ignores them. `token` is the
    // sender/origin, threaded through for reply routing. Threaded explicitly
    // rather than held in a global, so a handler's inputs are all in the ctx.
    void dispatch(clockwork::CoreState& st, const uint8_t* osc, uint32_t len, uint32_t token,
                  clockwork::Timetag when, clockwork::Timetag blockTime) {
        const DrainCallCtx cc{ token, when, blockTime };
        clockwork::Routing& r = st.routing;
        OscSplit* boundary = r.split.load(std::memory_order_acquire);
        if (boundary && boundary->ingest(osc, len, &cc)) return;
        // Nothing claimed it: no matching route and no default registered (e.g.
        // a DSP-bound message in a build with no DSP attached). Drop, and log rate-limited so junk
        // can't flood the audio-thread log.
        if (r.unclaimed_log.say()) {
            uint32_t a = 0; while (a < len && osc[a] != '\0') ++a;
            clockwork_log(st.core, "ERROR: no backend for OSC %.*s — dropped",
                   static_cast<int>(a), reinterpret_cast<const char*>(osc));
        }
    }

    // Lateness accounting for an event the schedule fired, and the overload law
    // applied to it. Placing an event WITHIN a block is the DSP's arithmetic —
    // it has the block's start time and each message's own (dsp_api.h). How late
    // the event was is clockwork's, and goes into the metrics.
    //
    // Returns true when the event is dropped as later than the host allows, and must not be delivered.
    static bool note_lateness(clockwork::CoreState& st, clockwork::Timetag when, clockwork::Timetag blockTime) {
        if (when.isImmediate()) return false;

        const double time_diff_ms = when.secondsFrom(blockTime) * 1000.0;

        // Later than the host allows (clockwork_set_timed_event_late_limit_ms):
        // dropped, as the host asked; 0 allows any lateness.
        clockwork::Schedule& sc = st.schedule;
        const uint32_t limit_ms = sc.late_limit_ms.load(std::memory_order_relaxed);
        if (limit_ms > 0 && time_diff_ms < -static_cast<double>(limit_ms)) {
            note_late_dropped(st, -time_diff_ms, limit_ms);
            return true;
        }

        const clockwork::Build& b = st.build;
        PerformanceMetrics* const metrics = clockwork::metrics_in(st.arena.at());
        if (!metrics || !(b.sample_rate > 0.0) || b.block == 0)
            return false;

        // Only messages older than a full quantum are genuinely late:
        // sub-quantum arrivals simply do not align with quantum boundaries and
        // are placed at the right sub-block offset by the DSP.
        const double quantum_ms =
            (1000.0 * static_cast<double>(b.block)) / b.sample_rate;
        if (time_diff_ms >= -quantum_ms) return false;

        const double raw_late_ms = -time_diff_ms;
        const int32_t late_ms = (raw_late_ms > 10000.0) ? 10000
                                                        : static_cast<int32_t>(raw_late_ms);
        const uint32_t late_now = metrics->scheduler_lates.fetch_add(1, std::memory_order_relaxed) + 1;
        int32_t current_max = metrics->scheduler_max_late_ms.load(std::memory_order_relaxed);
        while (late_ms > current_max) {
            if (metrics->scheduler_max_late_ms.compare_exchange_weak(
                    current_max, late_ms, std::memory_order_relaxed, std::memory_order_relaxed))
                break;
        }
        metrics->scheduler_last_late_ms.store(late_ms, std::memory_order_relaxed);
        metrics->scheduler_last_late_tick.store(
            metrics->process_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        // A line a second at most, with the count so far: a burst is one line.
        if (sc.late_log.say(blockTime.ntp()))
            clockwork_log(st.core, "LATE: %.1fms (count=%u)", -time_diff_ms, late_now);
        return false;
    }

    // ── Classification: the one place the schedule branch is decided ───────
    //
    // One frame off the IN ring. Two wire forms carry a time (schedule_parse.h):
    // a timestamped "#bundle", and "/clockwork/schedule <timetag> <blob>". Everything
    // else is immediate.
    //
    // The branch on guest_holds_schedule() lives HERE and nowhere else. A DSP
    // that holds its own schedule wants a timed bundle the moment it arrives,
    // carrying its timetag, because a VM parks programs and not merely
    // messages; a DSP that does not gets it when it comes due.
    //
    // "/clockwork/schedule" is NOT subject to that branch, and deliberately: it is the
    // Clockwork's own verb, whose payload is an /clockwork/osc/send or a /clockwork/midi/*
    // clockwork delivers itself. Handing it to a DSP because that DSP happens to
    // keep a queue would move timed MIDI output into an engine that has no
    // MIDI port. The declaration governs where a message for the DSP waits,
    // not who owns clockwork's own features.
    //
    // Anything due now goes to dispatch(), which is also where a fired event
    // lands — one delivery path, whether a message waited or not.
    static void ingest_frame(clockwork::CoreState& st, const uint8_t* data, uint32_t len, uint32_t origin,
                             clockwork::Timetag blockTime) {
        if (!data || len == 0) return;

        const uint8_t* body    = data;
        uint32_t       bodyLen = len;
        clockwork::Timetag when = clockwork::Timetag::immediately();
#if CLOCKWORK_SCHEDULER
        uint32_t       tag     = SCHED_TAG_SYNTH;      // a DSP-bound bundle
        bool           clockworkHolds = !guest_holds_schedule();
#endif

        if (clockwork_is_bundle(data, len)) {
            when = clockwork_bundle_timetag(data);
        } else if (len >= CLOCKWORK_SYS_PADDED("schedule") &&
                   std::memcmp(data, CLOCKWORK_SYS("schedule"), CLOCKWORK_SYS_LEN("schedule")) == 0 &&
                   data[CLOCKWORK_SYS_LEN("schedule")] == '\0') {
#if CLOCKWORK_SCHEDULER
            const SchedulePacket sp = clockwork_parse_schedule(data, len);
            if (!sp.ok) {                       // malformed: counted, and its sender told
                increment_scheduler_drop_metric(st);
                clockwork_sys_refuse(data, len, CLOCKWORK_REFUSED_MALFORMED, arenaInstanceGeneration(st.arena.at()),
                    [&st, origin](const uint8_t* d, uint32_t n) { emit_osc_out(st, origin, d, n); });
                return;
            }
            body         = sp.blob;
            bodyLen      = sp.blobLen;
            when         = sp.when;
            tag          = SCHED_TAG_DEFAULT;
            clockworkHolds = true;                // clockwork's own verb
#else
            // No store in this build, so there is nowhere to park the payload
            // and no honest way to deliver it at the time it asked for. This
            // is the ONE thing that must not go quietly: a verb that vanishes
            // without explanation is worse than either holding it or refusing
            // it, so say what happened and name the switch that caused it.
            refuse_schedule_verb(st, data, len, origin);
            return;
#endif
        }

#if CLOCKWORK_SCHEDULER
        if (when.isImmediate() || !clockworkHolds) {
            // Immediate, or a DSP that holds its own schedule: straight
            // through, carrying whatever time it was given.
            dispatch(st, body, bodyLen, origin, when, blockTime);
            return;
        }
        EngineScheduler* q = clockwork_engine_schedule(st.core);
        if (!q || !q->add(when, tag, EngineMeta{origin}, body, bodyLen)) {
            increment_scheduler_drop_metric(st);   // pool full / oversize
            if (st.schedule.refused_log.say())
                // Rate-limited, because an overloaded queue refuses in bursts.
                // The numbers are here because "add refused" alone cannot tell
                // a full pool from a queue that was never built.
                clockwork_log(st.core, "SCHED REFUSED (%s): when=%llu len=%u slots=%d pool=%d next=%llu",
                        q && q->ready() ? "queue full" : "NO QUEUE — it never built",
                        (unsigned long long)when.ntp(), (unsigned)bodyLen,
                        (int)SCHEDULER_SLOT_COUNT, (int)SCHEDULER_DATA_POOL_SIZE,
                        (unsigned long long)(q ? q->nextTime().value_or(clockwork::Timetag()) : clockwork::Timetag()).ntp());
        }
#else
        // With no store, holds_schedule is moot: nothing to hold with and nothing to
        // hold it in, so every message goes straight through CARRYING ITS ORIGINAL
        // TIMETAG — including for a DSP that asked us to hold it. Flattening `when` to
        // "now" here would destroy the one piece of information it needs to cope.
        dispatch(st, body, bodyLen, origin, when, blockTime);
#endif
    }

    int clockwork_memory_meets(uint32_t fast_bytes, uint32_t bulk_bytes, int has_bulk_tier,
                              uint32_t fast_wanted, uint32_t bulk_wanted,
                              uint32_t* short_fast, uint32_t* short_bulk) {
        // 64-bit sums so two wants near UINT32_MAX cannot wrap into "met".
        uint64_t need_fast = fast_wanted;
        uint64_t need_bulk = bulk_wanted;
        if (!has_bulk_tier) {
            // One kind of memory: bulk has nowhere else to go, so the fast
            // arena carries both and there is nothing to check in bulk.
            need_fast += need_bulk;
            need_bulk  = 0;
        }
        const uint64_t sf = need_fast > fast_bytes ? need_fast - fast_bytes : 0;
        const uint64_t sb = need_bulk > bulk_bytes ? need_bulk - bulk_bytes : 0;
        if (short_fast) *short_fast = sf > UINT32_MAX ? UINT32_MAX : (uint32_t)sf;
        if (short_bulk) *short_bulk = sb > UINT32_MAX ? UINT32_MAX : (uint32_t)sb;
        return sf == 0 && sb == 0;
    }

    void clockwork_set_engine_config(ClockworkCore* core,
                                double sample_rate, uint32_t block_size,
                                uint32_t input_channels, uint32_t output_channels,
                                uint32_t verbosity, void* guest_memory,
                                uint32_t guest_memory_bytes,
                                uint32_t arena_bytes,
                                const void* assets, uint32_t assets_bytes,
                                void* returns, uint32_t returns_bytes) {
        ClockworkConfig& cfg = *clockwork::config_of(core);
        cfg.sample_rate        = sample_rate;
        cfg.block_size         = block_size;
        cfg.input_channels     = input_channels;
        cfg.output_channels    = output_channels;
        cfg.verbosity          = verbosity;
        cfg.guest_memory        = guest_memory;
        cfg.guest_memory_bytes  = guest_memory_bytes;
        cfg.assets              = assets;
        cfg.assets_bytes        = assets_bytes;
        cfg.returns             = returns;
        cfg.returns_bytes       = returns_bytes;
        cfg.arena_bytes        = arena_bytes;
    }

#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
    static bool engine_block_holding(clockwork::CoreState& st, size_t bytes, uint32_t verbosity) {
        clockwork::Memory& m = st.memory;
        if (m.block && m.block_bytes == bytes) return true;
        if (m.block) {
            // A build that wants another size: everything carved from the
            // block is a build's (the pool went with the last instance; the
            // spans are cut again below), so forget it all and let it go.
            clockwork_heap_stop(st.heap, CLOCKWORK_POOL_GUEST);
            clockwork_heap_release_system(st.heap, free_to_bulk);
            m.fast = nullptr; m.fast_bytes = 0;
            m.bulk = nullptr; m.bulk_bytes = 0;
            clockwork::pages::unmap(m.block, m.block_bytes);
            m.block = nullptr; m.block_bytes = 0;
        }
        void* block = bytes != SIZE_MAX && bytes != 0 ? clockwork::pages::map(bytes) : nullptr;
        if (!block) return false;
        m.block       = static_cast<uint8_t*>(block);
        m.block_bytes = bytes;
        if (verbosity > 0)
            clockwork_log(st.core, "ENGINE MEMORY: 0x%016llx + %u MB", (unsigned long long)(uintptr_t)m.block,
                          (unsigned)(m.block_bytes / (1024 * 1024)));
        return true;
    }
#endif

    // ── Every region, at every event (docs/RESTARTS.md) ─────────────────────
    //
    // The arena table says what each region's lifetime is; this is the HOW
    // for each one, and the walk below runs it WHEN the table says: a region
    // of lifetime rebuild at every build, one of lifetime cold boot when the
    // arena is new, one that lives as long as the arena never. Nothing else
    // decides, so a region added to the table without a reset is found by
    // test_restarts.cpp and not by a user after a device change.
    //
    // "Reset" is bytes for all but one group. The transport area — the
    // cursors and the three rings — is a protocol between threads, so a
    // ring's reset is a fresh epoch in its cursors (clockwork_lanes_reset_*_ring)
    // and the ring bytes are never touched. Each ring's is its own entry's,
    // at a new arena only: a rebuild keeps all three, what is in them
    // answered across it (the marks, for the in ring); the cursors' entry
    // resets the status flags alone.
    static void reset_words(ClockworkCore* core, uint32_t start, uint32_t bytes) {
        auto* w = reinterpret_cast<std::atomic<uint32_t>*>(clockwork::arena_base(core) + start);
        for (uint32_t i = 0; i < bytes / 4u; ++i) w[i].store(0, std::memory_order_relaxed);
    }
    static void reset_metrics(ClockworkCore* core) { reset_words(core, METRICS_START, METRICS_SIZE); }
    static void reset_metrics_history(ClockworkCore* core) { reset_words(core, METRICS_HISTORY_START, METRICS_HISTORY_SIZE); }
    static void reset_native_stats(ClockworkCore* core) { reset_words(core, NATIVE_STATS_START, NATIVE_STATS_SIZE); }
    // The parameter table: its header written, every slot free (params.cpp).
    static void reset_params(ClockworkCore* core) { clockwork::params::reset(core); }
    // The host's: NTP start is write-once from the host once its audio runs,
    // drift and the global offset are what it measured about this machine's
    // clocks. A new arena, a new measurement.
    static void reset_clock_anchors(ClockworkCore* core) { reset_words(core, CLOCK_ANCHORS_START, CLOCK_ANCHORS_SIZE); }
    // Not zeros: a tempo of zero makes a guest asking for the beat divide by
    // it (readClockworkClock answers a NULL with a sane default, an all-zero
    // region with that). The defaults are the session's start.
    static void reset_clock_state(ClockworkCore* core) {
        ClockworkClockState::initDefaults(
            *reinterpret_cast<ClockworkClockState*>(clockwork::arena_base(core) + CLOCK_STATE_START));
    }
    // Rewritten whole every block by the tick (publishSampleClock), from the
    // device it is rebound to; empty between the build and the first block.
    static void reset_sample_clock(ClockworkCore* core) { reset_words(core, SAMPLE_CLOCK_START, SAMPLE_CLOCK_SIZE); }
    // Empty: no device words, no streams, its seqlock even. The device's own
    // words are written when the instance is built (channel_map_set_device),
    // and a stream's when it attaches.
    static void reset_channel_map(ClockworkCore* core) { reset_words(core, CHANNEL_MAP_START, CHANNEL_MAP_SIZE); }
    // The scope: out and in, formatted at the device's live channel count
    // (up to the slot's ceiling) and written by the tick from then on. A tap
    // for a direction the device has not got is formatted with no channels
    // and left disabled, so a reader sees "nothing arrives" rather than a
    // ring of zeros. A device switch is a new stream, and a reader's cursor
    // starts again with it.
    static void reset_scope(ClockworkCore* core) {
        const ClockworkConfig& cfg = *clockwork::config_of(core);
        const clockwork::Layout& L = clockwork::layout_of(core);
        uint8_t* const base = clockwork::arena_base(core);
        memset(base + SCOPE_START, 0, L.scope_bytes);
        const auto format_tap = [&](uint32_t slot, uint32_t device_channels) {
            const uint32_t ch = std::min(device_channels, L.profile.scope_channels);
            if (ch > 0)
                shm_audio_buffer_writer(clockwork::scopeSlot(base, L, slot), L.profile.scope_frames,
                                        L.profile.scope_channels)
                    .activate(ch, static_cast<uint32_t>(cfg.sample_rate), L.profile.scope_frames);
        };
        format_tap(SCOPE_OUT_SLOT, cfg.output_channels);
        format_tap(SCOPE_IN_SLOT,  cfg.input_channels);
    }
    // Tap streams: the global header (geometry for cross-process observers)
    // then the slot array, every slot free; the producer claims and
    // activates them on demand (rust/clockwork-taps).
    static void reset_taps(ClockworkCore* core) {
        const clockwork::Layout& L = clockwork::layout_of(core);
        uint8_t* tapBase = clockwork::arena_base(core) + L.taps_start;
        memset(tapBase, 0, L.taps_bytes);
        reinterpret_cast<std::atomic<uint32_t>*>(tapBase + 0)->store(L.profile.tap_slots, std::memory_order_relaxed);       // maxTaps
        reinterpret_cast<std::atomic<uint32_t>*>(tapBase + 4)->store(0, std::memory_order_relaxed);                        // activeCount
        reinterpret_cast<std::atomic<uint32_t>*>(tapBase + 8)->store(L.profile.tap_ring_frames, std::memory_order_relaxed); // ringFrames
        reinterpret_cast<std::atomic<uint32_t>*>(tapBase + 12)->store(0, std::memory_order_relaxed);              // version
    }
    // The status flags: a fresh word with every build.
    static void reset_status_flags(ClockworkCore* core) {
        clockwork::cursors_in(clockwork::arena_base(core), clockwork::layout_of(core))
            ->status_flags.store(STATUS_OK, std::memory_order_release);
    }
    // The in ring: a fresh epoch at a new arena only, serialised against its
    // producers (clockwork_lanes_reset_in_ring). A rebuild keeps it: what
    // waited for the old instance is the next drain's to refuse, below the
    // mark the teardown set (destroy_dsp), so its cursors, its sequence, the
    // drain's place in it and the mark go on.
    static void reset_in_ring(ClockworkCore* core) {
        clockwork_lanes_reset_in_ring(core);
    }
    // The reply rings outlive an instance: what one said stays for its
    // readers, and the next one's setup follows it (docs/RESTARTS.md). A new
    // arena begins them afresh.
    static void reset_rt_out_ring(ClockworkCore* core) { clockwork_lanes_reset_rt_out_ring(core); }
    static void reset_nrt_out_ring(ClockworkCore* core) { clockwork_lanes_reset_nrt_out_ring(core); }
    // The ids are the clients', shared between them: a new arena starts them.
    static void reset_node_id_counter(ClockworkCore* core) {
        reset_words(core, clockwork::layout_of(core).node_id_counter_start, NODE_ID_COUNTER_SIZE);
    }
    // The guest's publish window: zeroed, and nothing more. What shape the
    // bytes take is the guest's, so it lays the region out itself at
    // dsp_new — clockwork cannot pre-fill a structure it does not know, and
    // a wipe that assumed one would be a guess about its empty value.
    static void reset_guest_window(ClockworkCore* core) {
        const clockwork::Layout& L = clockwork::layout_of(core);
        memset(clockwork::arena_base(core) + L.guest_window_start, 0, L.profile.window_bytes);
    }
    // The host's, copied in by clockwork_init BEFORE the build (the build
    // sizes the guest's pool from it), onto an arena that started empty. A
    // host that hands a config over an old one on a segment it kept is
    // making that copy itself. So the cold boot's reset here is the empty
    // arena's, and this does nothing more.
    static void reset_guest_config(ClockworkCore*) {}
    // Nobody but the guest can draw it, and the guest cannot tell "restored
    // after a device switch" from "a previous engine in this process left
    // its table behind": an engine that never asked for /notify would
    // inherit a registration and answer a client that had not spoken.
    // Clockwork owns the arena's lifetime, so clockwork decides.
    static void reset_guest_persist(ClockworkCore* core) {
        const clockwork::Layout& L = clockwork::layout_of(core);
        memset(clockwork::arena_base(core) + L.guest_persist_start, 0, L.profile.persist_bytes);
    }

    struct RegionReset { uint32_t id; void (*reset)(ClockworkCore* core); };
    static constexpr RegionReset kRegionResets[] = {
        { CLOCKWORK_ARENA_CLOCKWORK_METRICS,         &reset_metrics },
        { CLOCKWORK_ARENA_CLOCKWORK_METRICS_HISTORY, &reset_metrics_history },
        { CLOCKWORK_ARENA_CLOCKWORK_NATIVE_STATS,    &reset_native_stats },
        { CLOCKWORK_ARENA_CLOCKWORK_CLOCK_ANCHORS,   &reset_clock_anchors },
        { CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE,     &reset_clock_state },
        { CLOCKWORK_ARENA_CLOCKWORK_SAMPLE_CLOCK,    &reset_sample_clock },
        { CLOCKWORK_ARENA_CLOCKWORK_CHANNEL_MAP,     &reset_channel_map },
        { CLOCKWORK_ARENA_CLOCKWORK_SCOPE,           &reset_scope },
        { CLOCKWORK_ARENA_CLOCKWORK_TAPS,            &reset_taps },
        { CLOCKWORK_ARENA_CLOCKWORK_RING_CURSORS,    &reset_status_flags },
        { CLOCKWORK_ARENA_CLOCKWORK_NODE_ID_COUNTER, &reset_node_id_counter },
        { CLOCKWORK_ARENA_CLOCKWORK_IN_RING,         &reset_in_ring },
        { CLOCKWORK_ARENA_CLOCKWORK_RT_OUT_RING,     &reset_rt_out_ring },
        { CLOCKWORK_ARENA_CLOCKWORK_NRT_OUT_RING,    &reset_nrt_out_ring },
        { CLOCKWORK_ARENA_GUEST_WINDOW,              &reset_guest_window },
        { CLOCKWORK_ARENA_GUEST_CONFIG,              &reset_guest_config },
        { CLOCKWORK_ARENA_GUEST_PERSIST,             &reset_guest_persist },
        // Lifetime arena: never reset, so no reset to name. The walk skips
        // the lifetime before it looks; the list names every region anyway,
        // so the enum and the list stay held to each other.
        { CLOCKWORK_ARENA_CLOCKWORK_CONTROL_RING,    nullptr },
        { CLOCKWORK_ARENA_CLOCKWORK_PARAMS,          &reset_params },
    };
    // Every region the enum names, once. The ids are 1..N and contiguous
    // (clockwork_arena.h: "stable numbers, in the order they lie"), so a
    // region added to the enum without a reset here does not compile, and
    // one listed twice does not either. The test holds the TABLE to this
    // list; this holds the list to the enum.
    static constexpr uint32_t kRegionLast = CLOCKWORK_ARENA_CLOCKWORK_PARAMS;
    static constexpr bool region_resets_cover_every_region() {
        if (sizeof(kRegionResets) / sizeof(kRegionResets[0]) != kRegionLast) return false;
        for (uint32_t id = 1; id <= kRegionLast; ++id) {
            uint32_t n = 0;
            for (const auto& r : kRegionResets) if (r.id == id) ++n;
            if (n != 1) return false;
        }
        return true;
    }
    static_assert(region_resets_cover_every_region(),
                  "every arena region has one reset in kRegionResets (docs/RESTARTS.md)");

    static const RegionReset* region_reset(uint32_t id) {
        for (const auto& r : kRegionResets) if (r.id == id) return &r;
        return nullptr;
    }
    // The walk: every entry of the table, in the order it lies in memory,
    // reset at the event its lifetime names.
    static void reset_regions(ClockworkCore* core, bool cold_boot) {
        const auto* h = arenaHeader(clockwork::arena_base(core));
        for (uint32_t i = 0; i < h->entry_count; ++i) {
            const auto& e = h->entries[i];
            if (e.lifetime == CLOCKWORK_LIFETIME_ARENA) continue;
            if (e.lifetime == CLOCKWORK_LIFETIME_COLD_BOOT && !cold_boot) continue;
            const RegionReset* r = region_reset(e.id);
            if (!r || !r->reset) {
                clockwork_log(core, "BUG: region %u is in the arena table with no reset — not reset", (unsigned)e.id);
                continue;
            }
            r->reset(core);
        }
    }
    // lanes.h: the test that holds the table and this list to each other.
    extern "C" int clockwork_region_has_reset(uint32_t region_id) {
        const RegionReset* r = region_reset(region_id);
        return r && r->reset ? 1 : 0;
    }

    // Reserved lanes, above the device (see the boundary at the top of this
    // file). Widen both directions to hold them; the device backend copies
    // only what the device has, so the extra channels never reach hardware.
    // Recomputed on every build, and when a surviving instance is told a new
    // channel count, so a wider device moves the base up rather than
    // overlapping the lanes. The host's lanes start at the base and Link
    // Audio's sit directly above.
    static void widen_for_lanes(clockwork::CoreState& st, uint32_t& max_input_channels, uint32_t& max_output_channels) {
        clockwork::Build& b = st.build;
        b.lane_base = 0;
        b.link_audio_lane_base = 0;
        const ClockworkConfig& cfg = st.config;
        const uint32_t lanes = cfg.reserved_lanes + cfg.link_audio_lanes;
        if (lanes == 0) return;
        const uint32_t first = lane_base_for(max_input_channels, max_output_channels);
        const uint32_t top   = first + lanes;
        if (top <= clockwork::kMaxChannels) {
            b.lane_base = first;
            if (cfg.link_audio_lanes > 0) b.link_audio_lane_base = first + cfg.reserved_lanes;
            if (max_output_channels < top) max_output_channels = top;
            if (max_input_channels  < top) max_input_channels  = top;
        } else {
            clockwork_log(st.core, "WARNING: %u reserved lanes above channel %u do not fit "
                    "kMaxChannels=%d; no lanes reserved",
                    lanes, first, (int)clockwork::kMaxChannels);
        }
    }

    // ── What the guest instance survives (lanes.h) ──────────────────────────
    extern "C" uint32_t clockwork_guest_survives(void) {
        const DspInfo* info = guest_info();
        return info ? info->survives : 0;
    }

    extern "C" int clockwork_guest_reconfigure(ClockworkCore* core, double sample_rate, uint32_t input_channels,
                                               uint32_t output_channels) {
        clockwork::CoreState* const st = clockwork::state(core);
        if (!st) return 0;
        const clockwork::GateHold gate(core, CLOCKWORK_ENTRY_CONTROL, clockwork::GateHold::Take::Wait);
        ClockworkConfig& cfg = st->config;
        Dsp* const instance = st->guest.instance;
        if (!instance || !(sample_rate > 0.0)) return 0;
        // The ceilings the instance is handed, as dsp_new hands them: the
        // device's counts, clamped, widened for the lanes above the device.
        uint32_t cin  = input_channels  > clockwork::kMaxChannels ? clockwork::kMaxChannels : input_channels;
        uint32_t cout = output_channels > clockwork::kMaxChannels ? clockwork::kMaxChannels : output_channels;
        widen_for_lanes(*st, cin, cout);
        // The block's buffers for the new counts first: with none, the
        // instance is told nothing and the caller rebuilds. Refused, they go
        // back to the counts it has, which the span holds.
        clockwork::Build& b = st->build;
        if (!fit_staging(st->staging, b.block, cout, cin)) return 0;
        {
            GuestCallScope guest(GuestCall::Boot);
            if (dsp_reconfigure(instance, sample_rate, cin, cout) != 0) {
                fit_staging(st->staging, b.block, b.out_channels, b.in_channels);
                return 0;
            }
        }
        // The instance took it: the device's facts are these from now on,
        // and clockwork's own regions that follow the device are laid out
        // again for it — the scope at the new channel count, the sample
        // clock for the new rate, the channel map's device words. Nothing of
        // the rings, the metrics or the guest's regions moves: the instance
        // is the same one, and every client's picture of it still holds.
        cfg.sample_rate     = sample_rate;
        cfg.input_channels  = input_channels;
        cfg.output_channels = output_channels;
        b.config.sample_rate     = sample_rate;
        b.config.input_channels  = input_channels;
        b.config.output_channels = output_channels;
        b.sample_rate  = sample_rate;
        b.in_channels  = cin;
        b.out_channels = cout;
        reset_scope(core);
        reset_sample_clock(core);
        channel_map_set_device(*st, b.in_channels, b.out_channels);
        return 1;
    }

    // ── The clock enters service ────────────────────────────────────────
    // One order on every target, after the layout and the reset: whoever
    // owns the clock published it to the core (CoreClock) before boot — the
    // engine its member; a worklet host nothing, so the core's own clock
    // stands in — and it is bound here to the clock state (its
    // transport stamp anchored to wall time as it binds, bindStateToShm),
    // to the sample clock, and to the host's global offset. Every build: a
    // rebuild binds the same instance to the same regions again, which is a
    // no-op. After the reset on every host: a reset after the bind wipes the
    // stamp to the 1900 epoch.
    static void clock_enter_service(clockwork::CoreState& st, bool cold_boot) {
        clockwork::CoreClock* c = &st.clock;
        if (!c->clock.load(std::memory_order_acquire)) {
            if (!c->own) c->own = new (std::nothrow) ClockworkClock();
            ClockworkClock* none = nullptr;
            c->clock.compare_exchange_strong(none, c->own, std::memory_order_acq_rel);
        }
        // Every boot re-anchors the clock at the first block that follows,
        // whichever host drives it. A cold boot also resets the clock
        // (unanchor), as it resets the arena; a rebuild keeps the clock's
        // last time as a floor so time never goes backwards.
        c->anchor_requested.store(true, std::memory_order_release);
        ClockworkClock* clock = c->clock.load(std::memory_order_acquire);
        if (!clock) return;   // a host with no clock at all
        if (cold_boot) clock->unanchorAudioThread();
        uint8_t* const base = st.arena.at();
        clock->bindStateToShm(reinterpret_cast<ClockworkClockState*>(base + CLOCK_STATE_START));
        // The sample clock, published by process_audio once per block: how threads without the audio context
        // (a host's worker, OscChannel#now) read the engine's time.
        clock->bindSampleClockToShm(base + SAMPLE_CLOCK_START);
        clock->bindGlobalOffset(reinterpret_cast<std::atomic<int32_t>*>(base + GLOBAL_OFFSET_START));
    }

    // A core's own ingress root, wired once for each core (Routing::own_split):
    // at its first clockwork_init, whether that boots the arena or comes back
    // to one that is up.
    void clockwork_wire_own_routes(ClockworkCore* core) {
#if CLOCKWORK_SYNTH
        // ── Clockwork's own ingress root ──────────────────────────────────
        //
        // ONE predicate and two destinations, which is the whole of the
        // namespace rule (docs/BOUNDARY.md §3): "/clockwork/" is clockwork's,
        // everything else falls through to the DSP untouched. Behind the
        // predicate clockwork sorts its own verbs; the "clock/" route is
        // inline only on a worklet self-clock host, which has no NRT thread to
        // forward it to.
        //
        // Registered on EVERY build, not only worklet ones. A host driving the
        // lanes ABI directly — clockwork_init, clockwork_ingress_write,
        // clockwork_tick — otherwise publishes no ingress at all, and every OSC
        // message it writes is drained, finds no route, and is dropped.
        //
        // Published by compare-exchange from null so it never displaces a full
        // host's own root: ClockworkEngine registers control planes this one
        // does not have and publishes mSplit after boot, overwriting this.
        // The routes' context is the core.
        clockwork::Routing* found = clockwork::routing(core);
        if (!found) return;
        clockwork::Routing& routing = *found;
        OscSplit&           default_split        = routing.own_split;
        ClockworkSysRoutes& default_audio_routes = routing.own_routes;
        if (!default_split.wired()) {
            // The clock's verbs, on every host with no control thread.
            routing.rt_reply = ReplyChannel{ &rt_reply_emit, core };
            default_audio_routes.add("clock/", &clockCoreRoute, &routing.rt_reply);
            // The asset hand-off is audio-thread work on every host: the
            // guest is only ever reached from there.
            default_audio_routes.add("asset/", &clockwork_asset_route, core);
            default_audio_routes.add("sched/flush", &clockwork_sched_flush_route, core);
            // The parameter table (docs/PARAMS.md): defined, listed and set on the
            // thread that holds it, on every host.
            default_audio_routes.add("param/", &clockwork::params::route, core);
            // Inbound events likewise: to their audience, and to the guest.
            default_audio_routes.add("midi/in/",         &clockwork_event_route, core);
            default_audio_routes.add("midi/ports",       &clockwork_event_route, core);
            default_audio_routes.add("gamepad/in/",      &clockwork_event_route, core);
            default_audio_routes.add("gamepad/devices",  &clockwork_event_route, core);
            // The VERBS that live under those names are not events: carved
            // back out to the host's end of the chain (longest match wins).
            default_audio_routes.add("midi/in/enable",        &clockwork_host_forward_route, core);
            default_audio_routes.add("midi/ports/list",       &clockwork_host_forward_route, core);
            default_audio_routes.add("midi/ports/get",        &clockwork_host_forward_route, core);
            default_audio_routes.add("gamepad/devices/list",  &clockwork_host_forward_route, core);
            default_audio_routes.add("gamepad/devices/get",   &clockwork_host_forward_route, core);
            // No NRT thread here. The end of clockwork's chain is either this
            // thread — answer the liveness verbs, refuse anything else under
            // the prefix — or the host's front, which the audio thread
            // forwards to (Routing::host_forward). The route decides per call.
            default_audio_routes.setFallback(&clockwork_host_forward_route, core);
            default_split.setSys(&ClockworkSysRoutes::route, &default_audio_routes);
            default_split.setDsp(&clockwork_dsp_default_route, core);
            // A host that is the far end from the start (the web) is the
            // sinks' MIDI endpoint from the start too.
            if (routing.host_forward.load(std::memory_order_acquire))
                install_host_emitter(core, routing, true);
        }
        {
            OscSplit* expected = nullptr;
            routing.split.compare_exchange_strong(
                expected, &default_split, std::memory_order_release);
        }
#else
        (void)core;
#endif
    }

    void init_memory(ClockworkCore* core) {
        clockwork::CoreState& st = *clockwork::state(core);
        const ClockworkConfig& cfg = st.config;
        const double sample_rate = cfg.sample_rate;
        // A build starts with no reason not to have a guest; the first thing
        // that stops it below records one (clockwork_boot_fail, or dsp_new's).
        clear_boot_error(st);
        // The arena base is fixed for the engine's whole threaded life: native
        // gives its core the segment before any engine thread starts
        // (ClockworkConfig::segment) or the core's own arena; the WASM arena
        // is the engine's memory. So on a cold-swap rebuild the pointer family
        // below is already correct — and skipping the re-assignment is what
        // makes this call safe while unquiesced producers (transport ingress,
        // clockwork_log from any worker) read the same globals lock-free: pointers
        // are only ever written here with no reader threads alive (first
        // boot, or re-boot after teardown_memory).
#ifdef __EMSCRIPTEN__
        // A host that boots before reading the arena's base (the test harness
        // does; the worklet claims first) gets the engine's memory here, sized
        // to what it asked clockwork_init for.
        if (!g_engine_block && !clockwork_memory_claim(cfg.arena_bytes ? cfg.arena_bytes
                                                                                      : (uint32_t)CLOCKWORK_MEM_ARENA_SIZE)) {
            clockwork_boot_fail(st, "FATAL: the engine's memory could not be claimed (the arena and %u MB)",
                                (unsigned)((cfg.arena_bytes ? cfg.arena_bytes : (uint32_t)CLOCKWORK_MEM_ARENA_SIZE) / (1024 * 1024)));
            return;
        }
#endif
        uint8_t* base = cfg.segment ? cfg.segment : clockwork_local_arena(st.core);
        // A new arena — the first boot, or one after teardown_memory — is a
        // cold boot: every region is reset, the ones of lifetime cold boot
        // included (reset_regions below). A rebuild on the arena that is up
        // takes the skip path and resets only the regions of lifetime
        // rebuild.
        clockwork::Arena& arena = st.arena;
        const bool cold_boot = arena.at() != base;
        // The generation a new arena goes on from, so no generation is named
        // twice to a client that outlived the engine before (docs/RESTARTS.md,
        // "The generation"): what this core last opened or its host handed
        // over, and what the memory itself says an engine opened there before,
        // for a client that maps it still.
        uint32_t carried = cfg.carried_generation;
        if (cold_boot && arenaHeader(base)->magic == CLOCKWORK_ARENA_MAGIC)
            carried = std::max(carried, arenaInstanceGeneration(base));
#if CLOCKWORK_SYNTH
        // A new arena that carries a generation on replaces a running engine
        // (docs/RESTARTS.md, "Three events"): its instance announces itself.
        const bool replacing = cold_boot && carried > 0;
#endif
        if (cold_boot) {
            arena.base.store(base, std::memory_order_release);
            // A new engine, not a rebuild of the one that was running.
            start_engine_diagnostics_over(st);
            // The table every reader takes the layout from, first, so nothing
            // is ever placed in an arena that does not describe itself. A
            // segment's creator wrote the same bytes already; the wasm arena
            // and an embedder's memory get theirs here.
            writeArenaHeader(base, arena.layout, clockwork_core_id(core));
            // The generation goes on from the one carried: the word as the
            // last arena's stood between instances.
            if (carried > 0)
                arenaGenerationWord(base).store(2u * carried + 1u, std::memory_order_release);
            // What a cold boot resets beyond the base — the guest's
            // persistent region, the clock's defaults, the host's anchors —
            // is the table's say, in reset_regions, not this branch's.
        }
        RingCursors* const cursors = clockwork::cursors_in(base, arena.layout);

        // Every region, at this event, as the table says (reset_regions):
        // the transport area's fresh epoch, the metrics, the scope, the taps,
        // the guest's window on every build; the clock's defaults, the
        // host's anchors, the guest's persistent region on a cold boot.
        // Before anything below writes into a region.
        reset_regions(core, cold_boot);
        clock_enter_service(st, cold_boot);

        // Event sinks: the size classes each kind is made of, from the memory
        // profile. Installed before any sink can be opened, so the DSP's own
        // opens get the shape too.
        install_sink_profiles(st);

        // The regions are laid out: the rings may be written, clockwork_log
        // among their writers.
        arena.ready.store(true, std::memory_order_release);
#ifndef __EMSCRIPTEN__
        // The ring exists: whatever the boot logged before now goes onto it
        // first, so the channel carries the boot in order.
        clockwork_log_release(core);
#endif

#if CLOCKWORK_SCOPE_SECONDS != 1
        // Test-profile arena: capture rings are oversized, so every offset
        // after them differs from a production reader's expectations. Loud
        // banner so a test-sized engine staged as production is obvious.
        clockwork_log(core, "[memory] WARNING: test memory profile "
               "(CLOCKWORK_SCOPE_SECONDS=%d) — arena layout differs from "
               "production readers; do not ship this binary",
               (int)CLOCKWORK_SCOPE_SECONDS);
#endif


#if CLOCKWORK_SYNTH
        // ── DspConfig ──────────────────────────────────────────────────────
        //
        // Everything set below is something only Clockwork can know: the
        // device's rate and block, how many channels it opened, where the
        // session clock lives, which bytes the guest may publish through,
        // which bytes outlive it, and whether this is a pinned offline render.
        //
        // What is NOT here is the point. Nothing in this function reads a
        // field out of the guest's config block. It hands over base and length
        // and stops there, so the block's layout is a matter between a host
        // and its guest and cannot break clockwork.

        const uint32_t verbosity = cfg.verbosity;

        DspConfig config = {};
        config.sample_rate         = sample_rate;   // the device's rate wins, always
        config.block_size          = cfg.block_size;
        config.max_input_channels  = cfg.input_channels;
        config.max_output_channels = cfg.output_channels;

        // One clock, read by everybody through readClockworkClock(). The arena-resident
        // ClockworkClockState — the same bytes JavaScript polls and the same address
        // ClockworkClock::bindStateToShm binds on native — so the guest reads the session
        // tempo directly and nothing pushes it at one.
        config.clock = reinterpret_cast<const ClockworkClockState*>(
            base + CLOCK_STATE_START);

        // Which channel is which, from the same arena and by the same rule as
        // the clock: the guest reads it directly, and nothing pushes it.
        config.channel_map = reinterpret_cast<const ClockworkChannelMapState*>(
            base + CHANNEL_MAP_START);

        // The guest's own configuration: whatever the host put there, handed
        // over without a glance. See DspConfig::guest_config.
        const clockwork::Layout& L = st.arena.layout;
        config.guest_config       = base + L.guest_config_start;
        config.guest_config_bytes = GUEST_CONFIG_SIZE;
        // What this instance is built from, for audio coming back to it
        // (come_back): it is carried on only in the same.
        clockwork::Build& built = st.build;
        built.config = cfg;
        std::memcpy(built.guest_config.data(), base + L.guest_config_start, GUEST_CONFIG_SIZE);

        // The window the DSP may publish through: the node-tree mirror region.
        // Clockwork zeroes it at boot, hands it over, and thereafter only
        // publishes the bytes — which is exactly how a node-tree mirror
        // survives in a harness that does not know what a node tree is.
        config.shm_window       = base + L.guest_window_start;
        config.shm_window_bytes = L.profile.window_bytes;
        // Deliberately NOT zeroed here, and that is the entire feature: this
        // region is how a guest survives its own destruction, so a rebuild must
        // find exactly what the last instance left. See DspConfig::persistent.
        config.persistent       = base + L.guest_persist_start;
        config.persistent_bytes = L.profile.persist_bytes;

        config.block_size = build_block(config.block_size);
        if (config.max_output_channels > clockwork::kMaxChannels)
            config.max_output_channels = clockwork::kMaxChannels;
        if (config.max_input_channels > clockwork::kMaxChannels)
            config.max_input_channels = clockwork::kMaxChannels;

        // Reserved lanes, above the device: widened for on every build, and
        // again when a surviving instance is told a new channel count
        // (widen_for_lanes).
        widen_for_lanes(st, config.max_input_channels, config.max_output_channels);
        // ── The guest's memory region ──────────────────────────────────────
        //
        // Reserved by the host, claimed by nothing else, and zeroed here so a
        // rebuilt guest starts from a clean block rather than the last one's
        // leavings. Publishing it through the boundary is the whole point:
        // clockwork already has to know where it starts to prove its own heap
        // does not reach into it, so leaving the guest to rediscover the same
        // number was two answers to one question.
        //
        // ABSOLUTE, not relative to base. On web the address is a
        // position in the wasm heap chosen by the JS memory layout
        // (wasmHeapSize + ringBufferReserved), and the client maps it that
        // way. Adding it to base — the arena, at whatever address it was
        // given — would put the region exactly arenaBase bytes too high,
        // overrunning into the buffer pool above it, and that only shows when
        // the layout moves, because the size of the overrun IS the arena's
        // address.
        //
        /*
         * THE PLACEMENT ARENA, and it goes in before anything allocates —
         * the guest's working memory, taken from it just below, first of all.
         *
         * emscripten's malloc starts after the static data and grows upward into
         * whatever comes next, and what comes next is the guest's region. The
         * engine's real-time pool is the single largest thing clockwork ever asks
         * for, so taking it with malloc is what walks into the guest.
         *
         * One malloc, once, before any allocation, of a size the layout knows about
         * (CLOCKWORK_MEM_ARENA_SIZE mirrors memArenaSize in memory_layout.js).
         * Everything afterwards comes out of it through the same clockwork::mem
         * calls native uses, so a request too large fails against a real bound
         * instead of escaping upward.
         */
#ifdef __EMSCRIPTEN__
        if (!clockwork::mem::arena_size()) {
            // The placement arena is the rest of the engine's memory after
            // the arena (g_engine_block, claimed before boot by the worklet or
            // by get_arena_base). What the layout asked for has to fit it.
            const size_t want = cfg.arena_bytes
                              ? (size_t)cfg.arena_bytes
                              : (size_t)CLOCKWORK_MEM_ARENA_SIZE;
            const size_t arena_bytes = st.arena.layout.arena_bytes;
            const size_t have = g_engine_block_bytes > arena_bytes ? g_engine_block_bytes - arena_bytes : 0;
            if (!g_engine_block || have < want) {
                clockwork_boot_fail(st, "FATAL: the engine's memory holds %u MB after the arena and the layout asks "
                              "for %u MB - claim it with memArenaSize before the arena's base is read "
                              "(clockwork_memory_claim)",
                              (unsigned)(have / (1024 * 1024)), (unsigned)(want / (1024 * 1024)));
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR,
                                               std::memory_order_relaxed);
                return;
            }
            void* arena = g_engine_block + arena_bytes;
            clockwork::mem::set_arena(arena, have);
            // The core's own pool first, so a later start-over finds it at
            // the arena's start again (above).
            own_pool_start(st);
            if (verbosity > 0)
                clockwork_log(core, "MEM ARENA: 0x%08x + %u MB",
                        (unsigned)(uintptr_t)arena,
                        (unsigned)(want / (1024 * 1024)));
        } else {
            /*
             * A NEW ENGINE OVER A MAPPING AN EARLIER ONE USED. On the web a
             * reload (Clockwork#reload) builds a fresh worklet over the same
             * shared memory, and the arena's bookkeeping lives in that memory,
             * so it comes back holding everything the dead engine placed in it
             * — the guest's instance and its real-time pool (scsynth's is 8 MB)
             * above all. Nothing ever frees them: the old worklet is gone, not
             * torn down. Left in place, a later reload cannot fit the pool and
             * the engine comes up with no DSP, processing silence.
             *
             * Nothing of that engine is alive — its thread is gone, and the
             * product re-sends what it held (restoreClientState) — so the arena
             * starts over. Everything holding a pointer into it forgets it
             * first: the guest's pool (started over at the build
             * below) and the dead instance.
             */
            clockwork_heap_stop(st.heap, CLOCKWORK_POOL_GUEST);
            clockwork_heap_release_system(st.heap, nullptr);   // in the arena, which goes whole
            st.guest.instance = nullptr;
            // The guest's spans are in the arena too: forgotten, so they are
            // carved again below in the same order as a first boot. Kept
            // while the heap is rebuilt from the arena's start, they would
            // lie under the heap's backing.
            st.memory.fast = nullptr; st.memory.fast_bytes = 0;
            st.memory.bulk = nullptr; st.memory.bulk_bytes = 0;
            // And the staging, for the same reason.
            (void)st.staging.span.release();
            st.staging = clockwork::Staging{};
            clockwork::mem::reset_arena();
            // The core's own pool is the core's for its life and lives in this
            // arena: it was the arena's first allocation, so taking it again
            // first lands it where it was. Anything else is a boot that cannot
            // be trusted and stops.
            if (!own_pool_reclaim(st)) {
                clockwork_boot_fail(st, "FATAL: the core's own pool did not come back where it was after the arena started over");
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
            if (verbosity > 0)
                clockwork_log(core, "MEM ARENA: started over for a new engine (%u MB)",
                        (unsigned)(clockwork::mem::arena_size() / (1024 * 1024)));
        }
        // The engine's memory, in the table: the one block, the arena at its
        // start and the placement arena after it. Placed on every boot, a
        // start-over included: it is the same block.
        arenaSetArea(base, CLOCKWORK_AREA_ENGINE_MEMORY, CLOCKWORK_SPACE_PROCESS,
                     (uintptr_t)g_engine_block, g_engine_block_bytes);
#endif

        // THE GUEST'S MEMORY: TWO SPANS, ONE PER SPEED, ON EVERY HOST.
        // `memory` is the fast span the guest lays out by hand and
        // `memory_bulk` the span its pool runs over (dsp_api.h). What the
        // guest needs of each is declared statically (DspInfo::
        // memory_bytes_wanted / memory_bulk_bytes_wanted), plus what its
        // configuration adds (dsp_memory_bytes: a server guest's real-time
        // pool; dsp_memory_bulk_bytes: what it will allocate), and the host
        // profile's default is a floor under each (CLOCKWORK_GUEST_MEMORY_
        // FAST_BYTES, CLOCKWORK_GUEST_MEMORY_BULK_BYTES; a host may raise the
        // bulk one, clockwork_set_guest_bulk_bytes). A default is clamped to
        // what its tier has; a guest's own want is checked HERE, before
        // dsp_new, by clockwork_memory_meets — a want that cannot be met
        // refuses the boot with a reason rather than spilling a hot pool into
        // slow RAM with nothing reported. See the rule's comment in
        // audio_processor.h.
        //
        // Where the bytes come from: the fast span the host placed, if it
        // placed one (a tiered device may; the test fixture does), else a
        // span clockwork takes from clockwork::mem's Fast tier; the bulk
        // span from clockwork::mem's Bulk tier. The same on every host: the
        // web's engine memory, a desktop's malloc, a device's internal SRAM
        // and PSRAM — on a single-RAM host the two tiers are one RAM and the
        // spans two slices of it. Each is kept across rebuilds while the
        // want still fits it; a rebuild whose configuration wants more
        // takes a new span.
        uint32_t fast_wanted = 0, bulk_wanted = 0;
        {
            GuestCallScope guest(GuestCall::Boot);
            if (const DspInfo* info = dsp_describe()) {
                fast_wanted = info->memory_bytes_wanted;
                bulk_wanted = info->memory_bulk_bytes_wanted;
            }
            // What this configuration adds: only the guest can read its own
            // options, so it is asked, with the config block already in place.
            const uint8_t* const guest_config = base + st.arena.layout.guest_config_start;
            const uint64_t configured = dsp_memory_bytes(guest_config, GUEST_CONFIG_SIZE);
            if (configured > fast_wanted) fast_wanted = configured > UINT32_MAX ? UINT32_MAX : (uint32_t)configured;
            const uint64_t configured_bulk = dsp_memory_bulk_bytes(guest_config, GUEST_CONFIG_SIZE);
            if (configured_bulk > bulk_wanted) bulk_wanted = configured_bulk > UINT32_MAX ? UINT32_MAX : (uint32_t)configured_bulk;
        }
#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
        // The engine's memory, natively (the core's block, above): sized to
        // what this build cuts from it — the fast span, then the bulk span at
        // its figure, on a chunk of its own — and replaced by a build that
        // wants another size. The core's own pool is taken first, apart from the
        // block: it lives as long as the core, past every block. A host that
        // placed the fast span itself keeps it; the block then holds the
        // bulk span alone.
        {
            own_pool_start(st);
            const bool host_fast = cfg.guest_memory && cfg.guest_memory_bytes;
            const size_t bulk_figure = cfg.guest_bulk_bytes ? cfg.guest_bulk_bytes
                                                                           : (size_t)CLOCKWORK_GUEST_MEMORY_BULK_BYTES;
            const size_t bulk_span = bulk_figure > bulk_wanted ? bulk_figure : (size_t)bulk_wanted;
            size_t fast_span = host_fast ? 0 : (size_t)fast_wanted;
            if (!host_fast && (size_t)CLOCKWORK_GUEST_MEMORY_FAST_BYTES > fast_span)
                fast_span = (size_t)CLOCKWORK_GUEST_MEMORY_FAST_BYTES;
            const size_t fast_cut = clockwork::pages::whole_chunks(fast_span);   // the bulk span starts on a chunk
            const size_t need = (fast_cut > SIZE_MAX - bulk_span) ? SIZE_MAX : fast_cut + bulk_span;
            if (!engine_block_holding(st, need, verbosity)) {
                clockwork_boot_fail(st, "FATAL: the system could not give the engine %zu bytes for the guest's memory "
                                    "(%zu fast + %zu bulk) - lower the guest's wants or the profile's "
                                    "CLOCKWORK_GUEST_MEMORY_BULK_BYTES",
                                    need, fast_span, bulk_span);
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
            // The cut. The spans are the block's, never clockwork::mem's:
            // nothing below frees them one by one, the block goes whole.
            clockwork::Memory& m = st.memory;
            m.fast       = fast_span ? m.block : nullptr;
            m.fast_bytes = fast_span;
            m.bulk       = bulk_span ? m.block + fast_cut : nullptr;
            m.bulk_bytes = bulk_span;
            arenaSetArea(base, CLOCKWORK_AREA_ENGINE_MEMORY, CLOCKWORK_SPACE_PROCESS,
                         (uintptr_t)m.block, m.block_bytes);
        }
#endif
        // The profile's default for a tier, clamped to what the tier has
        // less its reserve (a device's audio task stack; the web's headers).
        // On a desktop with no block largest_free is SIZE_MAX and nothing
        // clamps; with the engine's block it is the block's largest free run.
        const auto tier_default = [](clockwork::mem::Tier tier, size_t figure) -> size_t {
            if (!figure) return 0;
            const size_t avail = clockwork::mem::largest_free(tier);
            const size_t reserve = (size_t)CLOCKWORK_FAST_RESERVE;
            const size_t cap = avail > reserve ? avail - reserve : 0;
            return figure > cap ? cap : figure;
        };
        // How much of each span to take: the want, or the default when the
        // default is larger. A span taken once is kept while this build
        // wants exactly it; a build that wants another size takes a new
        // span, so what a rebuild gets is what it asked for and nothing an
        // earlier build left.
        const auto take = [&](clockwork::mem::Tier tier, void*& span, size_t& span_bytes, size_t need) {
            if (span && span_bytes != need) {
                clockwork::mem::free(span);
                span = nullptr; span_bytes = 0;
            }
            if (need && !span) {
                span = clockwork::mem::alloc(tier, need, /*allow_spill=*/false);
                span_bytes = span ? need : 0;
            }
        };

        void*  guest_memory       = nullptr;
        size_t guest_memory_bytes = 0;
        if (cfg.guest_memory && cfg.guest_memory_bytes) {
            guest_memory       = cfg.guest_memory;
            guest_memory_bytes = cfg.guest_memory_bytes;
        } else {
#if defined(__EMSCRIPTEN__) || CLOCKWORK_HAS_TIERED_MEMORY
            size_t need = fast_wanted;
            const size_t floor = tier_default(clockwork::mem::Tier::Fast, (size_t)CLOCKWORK_GUEST_MEMORY_FAST_BYTES);
            if (floor > need) need = floor;
            take(clockwork::mem::Tier::Fast, st.memory.fast, st.memory.fast_bytes, need);
#endif
            guest_memory       = st.memory.fast;
            guest_memory_bytes = st.memory.fast_bytes;
        }

#if defined(__EMSCRIPTEN__) || CLOCKWORK_HAS_TIERED_MEMORY
        {
            size_t need = bulk_wanted;
            size_t figure = cfg.guest_bulk_bytes ? cfg.guest_bulk_bytes
                                                                : (size_t)CLOCKWORK_GUEST_MEMORY_BULK_BYTES;
            const size_t floor = tier_default(clockwork::mem::Tier::Bulk, figure);
            if (floor > need) need = floor;
            take(clockwork::mem::Tier::Bulk, st.memory.bulk, st.memory.bulk_bytes, need);
        }
#endif
        void*  bulk_arena       = st.memory.bulk;
        size_t bulk_arena_bytes = st.memory.bulk_bytes;

        {
            uint32_t short_fast = 0, short_bulk = 0;
            if (!clockwork_memory_meets((uint32_t)guest_memory_bytes, (uint32_t)bulk_arena_bytes,
                                       /*each want against its own span*/ 1, fast_wanted, bulk_wanted,
                                       &short_fast, &short_bulk)) {
                // Refused, and loudly: the guest said what it needs and this
                // host cannot provide it in the speed it named. Booting anyway
                // would either fail inside dsp_new with a worse message or, if
                // the guest fell back to spilling, glitch at the first chord
                // instead of stopping at boot.
                clockwork_boot_fail(st, "FATAL: guest wants %u bytes fast + %u bytes bulk; "
                        "this host offers %u fast + %u bulk — short by %u fast, %u bulk. "
                        "Give clockwork room for it (the web layout's memArenaSize; a device's "
                        "RAM; a desktop's address space) or lower the guest's wants.",
                        fast_wanted, bulk_wanted,
                        (unsigned)guest_memory_bytes, (unsigned)bulk_arena_bytes,
                        short_fast, short_bulk);
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
        }

        // Zeroed at every dsp_new, and untouched by clockwork after that
        // (the bulk span is then the pool's, which the guest reaches through
        // the allocator): a rebuilt guest starts from nothing in each.
        // Natively the spans clockwork took are the core's own mapping, and
        // are zeroed by giving their pages back (os_pages.h), so a span costs
        // the RAM its guest touches rather than all it reserves; memory the
        // host placed is the host's, and is written.
        // False when the span is lost: its pages went and could not come back
        // (clockwork::pages::Zero::Lost), so nothing may touch it.
        const auto zero_span = [](void* span, size_t bytes, bool ours) {
#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
            if (ours) {
                switch (clockwork::pages::zero(span, bytes)) {
                case clockwork::pages::Zero::Done: return true;
                case clockwork::pages::Zero::Lost: return false;
                case clockwork::pages::Zero::Kept: break;
                }
            }
#else
            (void)ours;
#endif
            memset(span, 0, bytes);
            return true;
        };
        const auto span_lost = [&] {
            clockwork_boot_fail(st, "FATAL: the guest's memory could not be zeroed: the system took its pages and gave none back");
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
        };
        if (guest_memory) {
            if (!zero_span(guest_memory, guest_memory_bytes, guest_memory == st.memory.fast)) return span_lost();
            if (verbosity > 0)
                clockwork_log(core, "GUEST MEMORY: 0x%08x + %u KB fast",
                        (unsigned)(uintptr_t)guest_memory,
                        (unsigned)(guest_memory_bytes / 1024));
        }
        if (bulk_arena) {
            if (!zero_span(bulk_arena, bulk_arena_bytes, true)) return span_lost();
            if (verbosity > 0)
                clockwork_log(core, "GUEST MEMORY: 0x%08x + %u KB bulk",
                        (unsigned)(uintptr_t)bulk_arena,
                        (unsigned)(bulk_arena_bytes / 1024));
        }
        config.memory            = guest_memory;
        config.memory_bytes      = (uint32_t)guest_memory_bytes;
        config.memory_bulk       = bulk_arena;
        config.memory_bulk_bytes = (uint32_t)bulk_arena_bytes;
        // Where the guest's two spans are, into the table: for a map to
        // draw. No client reads them.
        arenaSetArea(base, CLOCKWORK_AREA_GUEST_MEMORY_FAST,
                     guest_memory ? CLOCKWORK_SPACE_PROCESS : CLOCKWORK_SPACE_UNKNOWN,
                     (uintptr_t)guest_memory, guest_memory_bytes);
        arenaSetArea(base, CLOCKWORK_AREA_GUEST_MEMORY_BULK,
                     bulk_arena ? CLOCKWORK_SPACE_PROCESS : CLOCKWORK_SPACE_UNKNOWN,
                     (uintptr_t)bulk_arena, bulk_arena_bytes);

        // The floating-point environment, as the host declared it
        // (clockwork_declare_fp_env). A worklet cannot arm a flush, so on web
        // the answer is known without asking and the declaration is ignored.
#ifdef __EMSCRIPTEN__
        config.fp_env = DSP_FP_ENV_DENORMALS_HONOURED;
#else
        config.fp_env = (int32_t)cfg.fp_env;
#endif
        // Bulk staging is NOT zeroed: the client owns the assets area and may have
        // written into it before the guest was built.
        config.assets        = cfg.assets;
        config.assets_bytes  = cfg.assets_bytes;
        config.returns       = cfg.returns;
        config.returns_bytes = cfg.returns_bytes;
        // Where the assets and the returns are, into the table: offsets in
        // the command segment when the lanes are there (ClockworkConfig::lanes_space;
        // every reader of the arena can see them and only the peer can reach
        // them), else the addresses in this process (on the web, in wasm
        // memory).
        if (cfg.lanes_space == CLOCKWORK_SPACE_COMMAND_SEGMENT) {
            arenaSetArea(base, CLOCKWORK_AREA_ASSETS,  cfg.lanes_space, cfg.assets_at,  config.assets_bytes);
            arenaSetArea(base, CLOCKWORK_AREA_RETURNS, cfg.lanes_space, cfg.returns_at, config.returns_bytes);
        } else {
            arenaSetArea(base, CLOCKWORK_AREA_ASSETS,
                         config.assets ? CLOCKWORK_SPACE_PROCESS : CLOCKWORK_SPACE_UNKNOWN,
                         (uintptr_t)config.assets, config.assets_bytes);
            arenaSetArea(base, CLOCKWORK_AREA_RETURNS,
                         config.returns ? CLOCKWORK_SPACE_PROCESS : CLOCKWORK_SPACE_UNKNOWN,
                         (uintptr_t)config.returns, config.returns_bytes);
        }


        // The guest's pool: clockwork's allocator over the bulk span, started
        // over every build; and the core's own, at its first. The same on
        // every platform.
        own_pool_start(st);
        clockwork_heap_start(st.heap, CLOCKWORK_POOL_GUEST, bulk_arena, bulk_arena_bytes, nullptr);
#ifdef __EMSCRIPTEN__
        /*
         * A SPAN THE HOST PLACED ITSELF (the legacy guestMemorySize slice)
         * lies above malloc's frontier, and malloc grows upward into whatever
         * comes next, so the heap can run into it at boot. Failing loud
         * here costs a boot; not failing costs corruption that presents as
         * anything at all. A span clockwork took itself lies inside the
         * engine's memory, below the frontier, and overlaps nothing.
         */
        if (guest_memory && guest_memory == cfg.guest_memory) {
            const uintptr_t guest_base = (uintptr_t)guest_memory;
            extern void* sbrk(intptr_t);
            const uintptr_t frontier = (uintptr_t)sbrk(0);
            if (frontier > guest_base) {
                clockwork_boot_fail(st, "FATAL: WASM heap (sbrk=0x%08x) overlaps the guest "
                        "region (start=0x%08x) by %u bytes - reduce heap "
                        "usage or raise wasmHeapSize in js/memory_layout.js",
                        (unsigned)frontier, (unsigned)guest_base,
                        (unsigned)(frontier - guest_base));
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR,
                                               std::memory_order_relaxed);
                return;
            }
            if (verbosity > 0)
                clockwork_log(core, "MEMORY OK: malloc<0x%08x guest=[0x%08x,0x%08x)",
                        (unsigned)frontier, (unsigned)guest_base,
                        (unsigned)(guest_base + guest_memory_bytes));
        }
#endif
#if CLOCKWORK_SYNTH
        // A bulk span the pool could not run over (too small for its
        // headers) is a boot that stops here and says so. Going on would
        // boot a guest whose every allocation answers NULL, and what it
        // reported would be its own failure, not the cause. (A no-DSP core
        // has no pool at all — its boot is a stub — so there is nothing to
        // check.)
        if (bulk_arena && !clockwork_heap_running(st.heap, CLOCKWORK_POOL_GUEST)) {
            clockwork_boot_fail(st, "FATAL: clockwork could not run the guest's pool over its bulk memory "
                                "(%zu bytes) - give the guest more (its bulk want, the profile's "
                                "CLOCKWORK_GUEST_MEMORY_BULK_BYTES, clockwork_set_guest_bulk_bytes)",
                                bulk_arena_bytes);
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }
        // The core's own pool, the Rust subsystems' for the core's life
        // (the area keeps its name, the rust heap); none stays UNKNOWN.
        size_t own_bytes = 0;
        if (void* own = clockwork_heap_span(st.heap, CLOCKWORK_POOL_OWN, &own_bytes))
            arenaSetArea(base, CLOCKWORK_AREA_RUST_HEAP, CLOCKWORK_SPACE_PROCESS, (uintptr_t)own, own_bytes);
#ifdef __EMSCRIPTEN__
        // The program's own three, which a wasm module can say exactly: its
        // static data (from the start of the memory to the linker's
        // __data_end; the arena lies inside), the stack the linker reserved
        // (__stack_low to __stack_high: the module's one thread's, which on
        // the web is the audio worklet, since the worklet instantiates the
        // module itself), and, at the end of boot, what malloc and new have
        // taken (the linker's __heap_base to sbrk then: the placement arena
        // with the heap and the guest's memory inside it, and what else boot
        // made). A native host says UNKNOWN until it is taught where its
        // own are.
        {
            extern unsigned char __data_end, __stack_low, __stack_high, __heap_base;
            extern void* sbrk(intptr_t);
            const uintptr_t data_end = (uintptr_t)&__data_end, heap_base = (uintptr_t)&__heap_base;
            const uintptr_t stack_lo = (uintptr_t)&__stack_low, stack_hi = (uintptr_t)&__stack_high;
            arenaSetArea(base, CLOCKWORK_AREA_STATIC_DATA, CLOCKWORK_SPACE_PROCESS, 0, data_end);
            if (stack_hi > stack_lo)
                arenaSetArea(base, CLOCKWORK_AREA_STACK, CLOCKWORK_SPACE_PROCESS, stack_lo, stack_hi - stack_lo);
            // malloc is placed at the end of boot (below), once the guest is built: its frontier then is what boot took
            (void)heap_base;
        }
#endif
#endif

#ifdef __EMSCRIPTEN__
        // Where malloc's frontier is at each stage of boot, for the accounting
        // of what boot took (logged below, verbose).
        uintptr_t brk_after_heap = 0, brk_after_sched = 0, brk_after_dsp = 0;
        { extern void* sbrk(intptr_t); brk_after_heap = (uintptr_t)sbrk(0); }
#endif
        #if CLOCKWORK_SCHEDULER
        // Build the core's store now, with the guest's memory arranged and on
        // the control thread (clockwork_sched_warm says why not earlier).
        clockwork_sched_warm(core);
#endif
#ifdef __EMSCRIPTEN__
        { extern void* sbrk(intptr_t); brk_after_sched = (uintptr_t)sbrk(0); }
#endif

        // What this DSP is (guest_info).
        const char* dsp_name    = "?";
        const char* dsp_version = "?";
        if (const DspInfo* info = guest_info()) {
            // Which guest, and which window layout: into the table, for the
            // readers of the window (base.h arenaSetGuestWindowIdentity).
            arenaSetGuestWindowIdentity(base, info->window_magic, info->window_version);
            // Kept, not printed here. Who the guest is belongs with the rest of
            // the identity — the banner, the version, the rate — and that is
            // printed further down, once there is an engine to be identified.
            dsp_name    = info->name    ? info->name    : "?";
            dsp_version = info->version ? info->version : "?";
        }

        // The block's buffers, for the geometry the instance is built for.
        if (!fit_staging(st.staging, config.block_size, config.max_output_channels, config.max_input_channels)) {
            clockwork_boot_fail(st, "FATAL: the fast memory has no room for the block's buffers "
                                "(%u frames, %u out and %u in channels)",
                                config.block_size, config.max_output_channels, config.max_input_channels);
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        // Create the instance (see dsp_api.h). It is handed the config above
        // and the host's callbacks — emit_osc, log, open_sink, send_sink,
        // free_bytes — with its core as their context, and that is the whole
        // of what it knows about clockwork.
        const DspHost host = dsp_host_for(core);
        static_assert(DSP_REFUSAL_BYTES >= sizeof(clockwork::Guest::boot_error),
                      "a refusal is cut only by the boot's record of it");
        // As many instances in the process as the guest says it can hold
        // (DspInfo::instances, 0 being one): a place is taken before
        // dsp_new, so two cores building at once cannot both take the last.
        const uint32_t declared = guest_info() ? guest_info()->instances : 0;
        const uint32_t allowed  = declared == 0 ? 1 : declared;
        if (!clockwork_core_guest_take(core, allowed)) {
            clockwork_log(core, "ERROR: engine bring-up refused: %s runs %u to a process", dsp_name, allowed);
            if (allowed == 1)
                record_boot_error(st, "%s did not start: only one can run in a process, and one already is", dsp_name);
            else
                record_boot_error(st, "%s did not start: only %u can run in a process, and %u already are",
                                  dsp_name, allowed, allowed);
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }
        try {
            std::array<char, DSP_REFUSAL_BYTES> why{};
            clockwork::Guest& g = st.guest;
            { GuestCallScope guest(GuestCall::Boot); g.instance = dsp_new(&config, &host, why.data(), why.size()); }
            why.back() = '\0';   // a guest that filled it to the end
            if (!g.instance) {
                clockwork_core_guest_give(core);
                clockwork_log(core, "ERROR: engine bring-up failed: %s", why[0] ? why.data() : "unknown");
                record_boot_error(st, "%s did not start: %s", dsp_name, why[0] ? why.data() : "it gave no reason");
                // The generation word stays odd: there is none, and none coming.
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
            // There is an instance: a generation opens — the word goes even,
            // one on from the last, which is the number setup will name —
            // and the boundary takes messages for it again.
            arenaInstanceUp(base);
            st.config.carried_generation = std::max(st.config.carried_generation, arenaInstanceGeneration(base));
        } catch (const std::exception& e) {
            clockwork_core_guest_give(core);
            clockwork_log(core, "ERROR: engine bring-up threw exception: %s", e.what());
            record_boot_error(st, "%s did not start: %s", dsp_name, e.what());
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        } catch (...) {
            clockwork_core_guest_give(core);
            clockwork_log(core, "ERROR: engine bring-up threw unknown exception");
            record_boot_error(st, "%s did not start: it threw", dsp_name);
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        // Publish the geometry clockwork chose (the staging is laid out for
        // it, above): the audio thread passes two pointers per block and
        // does no layout arithmetic at all.
        built.block        = config.block_size;
        built.out_channels = config.max_output_channels;
        built.in_channels  = config.max_input_channels;

        // Publish the device's own channels. A cold swap onto a different
        // device lands here again with new widths; ports bound across it keep
        // their entries (channel_map_set_device rewrites only unclaimed
        // channels).
        channel_map_set_device(st, built.in_channels, built.out_channels);
        built.sample_rate  = config.sample_rate;

        // ── Tell the new instance what is already attached ──────────────────
        //
        // A DSP is created at boot and again on a cold swap, and a cold swap
        // rebuilds it while every port stays open on exactly the channels it
        // had. Nothing in dsp_api.h lets a DSP ASK what it missed — deliberately
        // — so clockwork says it again, unprompted, before the instance
        // renders a block. Without this a device switch silently leaves a DSP
        // modelling streams it can no longer name, which presents as a stream
        // that stopped rather than as an error.
        //
        // reset() first: anything still queued named the instance that has
        // just been freed, and delivering it as well as the sweep below would
        // announce the same port twice.

        // How far the block clock advances per buffer. One second is
        // kNtpUnitsPerSecond units.
        const int buf_length = static_cast<int>(built.block);
        built.block_step = (int64_t)((double)buf_length / sample_rate * clockwork::kNtpUnitsPerSecond);

        // Publish cross-platform system info into the metrics struct.
        // Constant for the session; written once now that the DSP exists. Runs on every runtime — native's initialiseDsp() also
        // routes through init_memory().
        publish_session_metrics(st, sample_rate, buf_length);
        PerformanceMetrics* const metrics = clockwork::metrics_in(base);
        metrics->audio_output_channels.store(config.max_output_channels, std::memory_order_relaxed);
        metrics->audio_input_channels.store(config.max_input_channels, std::memory_order_relaxed);

        // (Re)initialise clockwork's timed queue. Clearing IS initialising
        // for a fixed-pool store, and this runs on every target's boot path.
#if CLOCKWORK_SCHEDULER
        if (EngineScheduler* q = clockwork_engine_schedule(core)) q->clear();
#endif
        update_scheduler_depth_metric(st, 0);


#ifdef __EMSCRIPTEN__
        clockwork_log(core, "\n" CLOCKWORK_PRODUCT_BANNER);
        // the product introduces itself under its banner; the substrate it runs
        // on follows, unless the product is clockwork itself
        // (clockwork itself has no version: standalone its line is the commit it was built from)
        if (std::strcmp(CLOCKWORK_PRODUCT_NAME, "clockwork") != 0) {
            clockwork_log(core, "%s v%s", CLOCKWORK_PRODUCT_NAME, CLOCKWORK_PRODUCT_VERSION);
            clockwork_log(core, "on Clockwork " CLOCKWORK_COMMIT);
        } else {
            clockwork_log(core, "clockwork " CLOCKWORK_COMMIT);
        }
        clockwork_log(core, "%.0fkHz %dch", sample_rate / 1000, config.max_output_channels);
        clockwork_log(core, "DSP: %s %s", dsp_name, dsp_version);
        clockwork_log(core, "");
        clockwork_log(core, "> engine ready...");
        // Boot is done: every malloc from here on is one the memory model
        // says should not happen (guest_malloc.h), and is counted as such. A
        // host that opens its own client after this marks again when it is
        // done (the worklet does, through the export).
        clockwork_malloc_mark_boot();
        // What malloc and new have taken, now that boot is done: from the
        // linker's __heap_base to the frontier, with the guest built (its
        // instance and whatever its dsp_new took with plain malloc lie in
        // it). Anything taken from here on lies past it.
        {
            extern unsigned char __heap_base;
            extern void* sbrk(intptr_t);
            const uintptr_t heap_base = (uintptr_t)&__heap_base, brk = (uintptr_t)sbrk(0);
            brk_after_dsp = brk;
            if (brk > heap_base)
                arenaSetArea(base, CLOCKWORK_AREA_MALLOC, CLOCKWORK_SPACE_PROCESS, heap_base, brk - heap_base);
            size_t own_pool_bytes = 0;
            const auto own_pool = (uintptr_t)clockwork_heap_span(st.heap, CLOCKWORK_POOL_OWN, &own_pool_bytes);
            if (verbosity > 0) clockwork_log(core, "malloc at boot: base %u · guest memory fast [%u,%u) · bulk [%u,%u) · own pool [%u,%u) · after the pool %u · after scheduler %u · after dsp_new %u",
                          (unsigned)heap_base, (unsigned)(uintptr_t)guest_memory, (unsigned)((uintptr_t)guest_memory + guest_memory_bytes),
                          (unsigned)(uintptr_t)st.memory.bulk, (unsigned)((uintptr_t)st.memory.bulk + st.memory.bulk_bytes),
                          (unsigned)own_pool, (unsigned)(own_pool + own_pool_bytes),
                          (unsigned)brk_after_heap, (unsigned)brk_after_sched, (unsigned)brk_after_dsp);
            // And of those bytes, what the guest itself took with malloc and
            // new on the way up (guest_malloc.h): counted, not inferred.
            if (verbosity > 0 && clockwork_guest_malloc_counted())
                clockwork_log(core, "guest malloc at boot: %u calls, %u freed, %u bytes held",
                              (unsigned)clockwork_guest_mallocs_boot(st), (unsigned)clockwork_guest_frees(st),
                              (unsigned)clockwork_guest_malloc_bytes(st));
        }
#else
        // No banner off the web, so the guest's identity is a line of its own.
        clockwork_log(core, "DSP: %s %s", dsp_name, dsp_version);
#endif
        // Every asset the last instance held, the new one's before it says
        // anything of its own (hand_over_assets). None on a new arena.
        if (st.guest.instance) {
            hand_over_assets(st);
            if (replacing) announce_instance(st, 0);
        }
#else  // !CLOCKWORK_SYNTH
        // No-DSP core: no instance, no audio render. The scheduler/router still
        // runs — derive the block-time constants from the AudioContext rate and a
        // default block size so the fire loop's OSC-time window is correct.
        const int buf_length = clockwork::kDefaultBlockSize;
        st.build.block_step = (int64_t)((double)buf_length / sample_rate * clockwork::kNtpUnitsPerSecond);

        publish_session_metrics(st, sample_rate, buf_length);

#if CLOCKWORK_SCHEDULER
        if (EngineScheduler* q = clockwork_engine_schedule(core)) q->clear();
#endif
        update_scheduler_depth_metric(st, 0);
#endif // CLOCKWORK_SYNTH
    }

#if CLOCKWORK_SYNTH
    // destroy_dsp / rebuild_dsp — a cold restart, however it came: a device
    // change the instance cannot take, or audio coming back that it cannot be
    // carried across (come_back; docs/RESTARTS.md, "One event, every route").
    // Frees the DSP instance outright and builds a new one; nothing of the
    // old instance survives but DspConfig::persistent.
    //
    // A COLD SWAP EMPTIES WHATEVER SUBSCRIPTION SET THE DSP WAS KEEPING, and a client
    // must re-subscribe. A registry that outlives an instance is state clockwork
    // cannot ask for and the DSP cannot hand over.
    void destroy_dsp(ClockworkCore* core) {
        clockwork::CoreState* const found = clockwork::state(core);
        if (!found) return;
        clockwork::CoreState& st = *found;
        // No block renders meanwhile (the core's gate): what the instance
        // says as it goes, and what clockwork says for it, go on its ring after
        // everything it said before.
        const clockwork::GateHold gate(core, CLOCKWORK_ENTRY_CONTROL, clockwork::GateHold::Take::Wait);
        InstanceRingScope instance_ring(st);
        // From here until the next dsp_new succeeds there is no instance: the
        // generation word goes odd, before anything else goes, so a message
        // written from now on meets the boundary's refusal (in_ring_door.h).
        // The generation that is closing stays named, so a refusal from here
        // says which instance refused and a reader of the arena knows
        // "between instances" from the bytes.
        if (uint8_t* base = st.arena.at()) arenaInstanceDown(base);
        // An instance whose setup is still owed goes unannounced: it said
        // nothing, and the next one up announces itself.
        st.setup.owed.store(Owed::Nothing, std::memory_order_relaxed);
        // What was already in the in ring was written for this generation:
        // marked, for the next drain to answer (the marks).
        mark_closing_generation(st);
        // The assets it held stay committed, for the next instance (hand_over_assets):
        // the bytes are the client's, and still where they were.
        clockwork::Guest& g = st.guest;
        if (g.instance) {
            { GuestCallScope guest(GuestCall::Boot); dsp_free(g.instance); }
            g.instance = nullptr;
            // The core's place among the guest's instances stays its own: a
            // rebuild is never refused for a place another core took between
            // (clockwork_core.h). Closing the core gives it back.
        }
        // The next instance's call counts start from nothing (guest_malloc.h).
        // What this one took from malloc and never freed stays held, and stays
        // in the held count: the bytes are still the guest's.
        clockwork_guest_malloc_reset(st);
        // After dsp_free, so nothing can be sending as they go. See
        // dsp_host_open_sink: this is the other half of "clockwork closes
        // every sink a DSP opened when that DSP is freed".
        close_dsp_sinks(st);
        clockwork::Build& b = st.build;
        b.block = b.out_channels = b.in_channels = 0;
        b.sample_rate = 0.0;
        // The guest's pool goes with the instance; its span stays for the
        // next build (teardown_memory gives the spans back).
        clockwork_heap_stop(st.heap, CLOCKWORK_POOL_GUEST);
        clockwork_heap_release_system(st.heap, free_to_bulk);
#if CLOCKWORK_SCHEDULER
        if (EngineScheduler* q = clockwork_engine_schedule(core)) q->clear();
#endif
        update_scheduler_depth_metric(st, 0);
        st.ingress.seq_reset.store(true, std::memory_order_relaxed);
    }

    void rebuild_dsp(ClockworkCore* core, double sample_rate, uint32_t device_buffer_frames) {
        clockwork::CoreState* const st = clockwork::state(core);
        if (!st) return;
        // No block renders meanwhile (the core's gate), so this thread writes
        // the instance's ring: what the instance says as it is built, and
        // then its setup.
        const clockwork::GateHold gate(core, CLOCKWORK_ENTRY_CONTROL, clockwork::GateHold::Take::Wait);
        InstanceRingScope instance_ring(*st);
        // The new rate, plus whatever channel ceilings the caller aimed at
        // with clockwork_set_channel_ceilings since the last build. Everything else
        // — the guest's config block, the arena, the regions — is where it
        // was; a rebuild is init_memory again, not a different function.
        st->config.sample_rate = sample_rate;
        init_memory(core);
        // Not up: there is no instance to announce, and the failed build is
        // the host's to report.
        if (st->guest.instance) announce_instance(*st, device_buffer_frames);
    }

    void come_back(ClockworkCore* core) {
        clockwork::CoreState* const found = clockwork::state(core);
        if (!found) return;
        clockwork::CoreState& st = *found;
        const ClockworkConfig& now = st.config;
        const clockwork::Build& built = st.build;
        // What no instance is carried across, whatever it survives: there is
        // none to carry (its build failed), it is not in the memory it was
        // built in, its config block is not the one it read, or the audio
        // thread left it partway through a block.
        const bool same_memory = now.guest_memory == built.config.guest_memory
                              && now.guest_memory_bytes == built.config.guest_memory_bytes
                              && now.assets == built.config.assets
                              && now.assets_bytes == built.config.assets_bytes
                              && now.returns == built.config.returns
                              && now.returns_bytes == built.config.returns_bytes
                              && now.arena_bytes == built.config.arena_bytes
                              && now.guest_bulk_bytes == built.config.guest_bulk_bytes
                              && now.fp_env == built.config.fp_env;
        const bool same_config = std::memcmp(st.arena.at() + st.arena.layout.guest_config_start,
                                             built.guest_config.data(), GUEST_CONFIG_SIZE) == 0;
        clockwork::CoreClock& cc = st.clock;
        const bool torn = cc.block_open.load(std::memory_order_acquire);
        clockwork::RestartStep step = clockwork::RestartStep::Rebuild;
        if (st.guest.instance && same_memory && same_config && !torn) {
            const clockwork::RestartFacts built_for { built.sample_rate, built.block,
                                                      built.config.input_channels, built.config.output_channels };
            const clockwork::RestartFacts back { now.sample_rate, build_block(now.block_size),
                                                 now.input_channels, now.output_channels };
            step = clockwork::planRestart(built_for, back, clockwork_guest_survives()).step;
        }
        if (step == clockwork::RestartStep::Reconfigure
            && !clockwork_guest_reconfigure(core, now.sample_rate, now.input_channels, now.output_channels))
            step = clockwork::RestartStep::Rebuild;

        if (step == clockwork::RestartStep::Rebuild) {
            // The new instance's blocks are its own.
            cc.block_open.store(false, std::memory_order_relaxed);
            // A committed asset points into the assets area it was committed
            // in: one that has moved leaves nothing for the next instance.
            if (now.assets != built.config.assets || now.assets_bytes != built.config.assets_bytes) {
                InstanceRingScope instance_ring(st);
                assets_release_all(st);
            }
            destroy_dsp(core);
            rebuild_dsp(core, now.sample_rate, 0);   // no device: setup names the block
            return;
        }
        // The same instance, back, every region as it was, as at any hot
        // restart. The clock re-enters service and re-anchors at the next
        // block, never below its last time.
        clock_enter_service(st, /*cold_boot*/ false);
        if (step == clockwork::RestartStep::Reconfigure) clockwork_announce_facts(core, now.sample_rate, 0);
    }

#endif

#if !defined(__EMSCRIPTEN__) && CLOCKWORK_SYNTH
    // Mirror of init_memory for the engine's end: tear down the DSP instance and
    // clear the core's arena while it is still mapped, so the lanes calls
    // (Arena::laid_out) reject what comes after instead of touching a freed or
    // unmapped segment.
    void teardown_memory(ClockworkCore* core) {
        clockwork::CoreState* const found = clockwork::state(core);
        if (!found) return;
        clockwork::CoreState& st = *found;
        /*
         * THE PORT BINDINGS GO WITH THE ENGINE. A binding names the channels a
         * port occupies and the table holds CLOCKWORK_PORT_BUS_MAX of them; without
         * this, a process with more than one engine lifetime fills the table and
         * then cannot attach at all — a stream refused for no visible reason.
         *
         * HERE AND NOT IN destroy_dsp, because a REBUILD goes through that one and
         * must keep its routing: streams outlive the DSP instance and the rebuilt
         * DSP is told what it missed. Detaching there unroutes every stream across
         * a device change.
         *
         * The ports themselves belong to whoever opened them; only the routing is
         * the engine's.
         */
        clockwork_port_bus_detach_all(core);

        // And the committed assets: a rebuild hands them on, the engine going
        // releases them, and each committer hears so.
        {
            InstanceRingScope instance_ring(st);
            assets_release_all(st);
        }
        destroy_dsp(core);
        // The arenas clockwork reserved for the guest go with the engine, not
        // with the instance: a rebuild keeps them (destroy_dsp above), a
        // teardown returns them.
#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
        // The spans are cuts of the core's block, and the block goes whole:
        // an engine's, not the process's. Nothing cut from it is live by now.
        clockwork::Memory& m = st.memory;
        m.fast = nullptr; m.bulk = nullptr;
        if (m.block) {
            clockwork::pages::unmap(m.block, m.block_bytes);
            m.block = nullptr; m.block_bytes = 0;
        }
#else
        clockwork::Memory& m = st.memory;
        if (m.fast) { clockwork::mem::free(m.fast); m.fast = nullptr; }
        if (m.bulk) { clockwork::mem::free(m.bulk); m.bulk = nullptr; }
#endif
        m.fast_bytes = m.bulk_bytes = 0;
        clockwork::Arena& arena = st.arena;
        arena.ready.store(false, std::memory_order_release);
        // The core has no arena now: a client finds no core over this memory.
        clockwork_core_set_memory(core, nullptr, 0);
        arena.base.store(nullptr, std::memory_order_release);
    }
#endif

    // A refusal on the instance's ring, made only when the ring has room for
    // it (docs/RESTARTS.md, rule 1): false when it has not, and the frame
    // waits for a later block, with everything behind it, rather than be
    // refused into a full ring and lost. A refusal that cannot be encoded at
    // all is counted and the frame consumed: there is nothing truthful left
    // to say (clockwork_sys_refuse).
    static bool refuse_when_room(clockwork::CoreState& st, uint32_t sourceId, const uint8_t* payload, uint32_t size,
                                 const char* reason, uint32_t generation) {
        bool made = true;
        RingCursors* const cursors = clockwork::cursors_in(st.arena.laid_out(), st.arena.layout);
        if (!cursors) return false;
        clockwork_sys_refuse(payload, size, reason, generation,
            [&made, &st, sourceId, cursors](const uint8_t* d, uint32_t n) {
                const uint32_t footprint =
                    (static_cast<uint32_t>(sizeof(Message)) + EGRESS_ROUTE_SIZE + n + 3u) & ~3u;
                made = ring_fits(static_cast<uint32_t>(cursors->out_head.load(std::memory_order_relaxed)),
                                 static_cast<uint32_t>(cursors->out_tail.load(std::memory_order_acquire)),
                                 st.arena.layout.profile.rt_out_ring_bytes, footprint)
                    && emit_osc_out(st, sourceId, d, n);
            });
        return made;
    }

#if CLOCKWORK_SYNTH
    // A BUILD WITH NO GUEST STILL ANSWERS FOR CLOCKWORK. When the build failed
    // (clockwork_boot_error) no instance is coming, but a client still has to
    // be able to register, ask what state the engine is in and hear why — and
    // every one of those is a /clockwork/ verb, which clockwork answers. So the
    // ingress ring is walked as on any block and those are dispatched. What was
    // for the guest — its own, and clockwork's that hand it something
    // (clockwork_sys_hands_to_guest) — is refused: below the mark, as the
    // generation it was written for; above it, from a writer that reads no
    // generation word, for want of an instance. Holding it would replay it
    // into whatever build comes next. Nothing is rendered.
    static void drain_without_guest(clockwork::CoreState& st) {
        clockwork::Ingress* const in = &st.ingress;
        uint8_t* const base = st.arena.laid_out();
        if (!base) return;
        const clockwork::Layout& L = st.arena.layout;
        RingCursors* const cursors = clockwork::cursors_in(base, L);
        PerformanceMetrics* const metrics = clockwork::metrics_in(base);
        constexpr uint32_t MAX_MESSAGES_PER_FRAME = 32;
        const auto blockTime = clockwork::Timetag::fromNtp(st.clock.block_ntp.load(std::memory_order_relaxed));
        ClockworkDrainStop stop = ClockworkDrainStop::Empty;
        clockwork_drain_ring(
            base + L.in_ring_start, L.profile.in_ring_bytes,
            &cursors->in_head, &cursors->in_tail, in->drain,
            ClockworkDrainMetrics{ &metrics->messages_processed, nullptr,
                            &metrics->messages_dropped,
                            &metrics->messages_sequence_gaps },
            MAX_MESSAGES_PER_FRAME,
            [&st, blockTime, in, base](uint32_t sourceId, const uint8_t* payload,
                        uint32_t payload_size, uint32_t seq) -> ClockworkDrainVerdict {
                const RingMark* mark = in->marks.of(seq);
                if (clockwork_sys_answers_without_guest(payload, payload_size)) {
                    dispatch(st, payload, payload_size, sourceId, clockwork::Timetag::immediately(), blockTime);
                    return ClockworkDrainVerdict::Consume;
                }
                const bool refused = mark
                    ? refuse_when_room(st, sourceId, payload, payload_size, mark->reason, mark->generation)
                    : refuse_when_room(st, sourceId, payload, payload_size,
                                       CLOCKWORK_REFUSED_NO_INSTANCE, arenaInstanceGeneration(base));
                return refused ? ClockworkDrainVerdict::Consume : ClockworkDrainVerdict::Retain;
            },
            &stop);
    }

    // A host that replaces the engine's memory rather than rebuild in it —
    // the page reloading over postMessage, its worklet going with the memory
    // it ran in — closes the instance first, so what waits for it is
    // answered before the memory goes: the instance is torn down, and each
    // call refuses a block's worth of what waits, as its ring has room, for
    // the host to relay between calls. How much still waits, in bytes: 0
    // once everything has been answered.
    uint32_t clockwork_close(ClockworkCore* core) {
        clockwork::CoreState* const st = clockwork::state(core);
        RingCursors* const cursors = st ? clockwork::cursors_in(st->arena.laid_out(), st->arena.layout) : nullptr;
        if (!cursors) return 0;
        const clockwork::GateHold gate(core, CLOCKWORK_ENTRY_CONTROL, clockwork::GateHold::Take::Wait);
        if (st->guest.instance) destroy_dsp(core);
        InstanceRingScope instance_ring(*st);
        drain_without_guest(*st);
        const auto head = static_cast<uint32_t>(cursors->in_head.load(std::memory_order_acquire));
        const auto tail = static_cast<uint32_t>(cursors->in_tail.load(std::memory_order_acquire));
        return ring_used_bytes(head, tail, st->arena.layout.profile.in_ring_bytes);
    }
#endif

    static void say_host_moved(clockwork::CoreState& st, double blockNtp);

    // The per-block body on every host; clockwork_tick (lanes.h) wraps it.
    // `time` carries the host's frame count, wall and monotonic clocks,
    // output latency and Link stamp in, and gets block_ntp and
    // block_host_micros back (ClockworkBlockTime).
    // active_output_channels / active_input_channels: live channel counts
    EMSCRIPTEN_KEEPALIVE
    void process_audio(ClockworkCore* core, ClockworkBlockTime& time,
                       uint32_t active_output_channels, uint32_t active_input_channels) {
        clockwork::CoreState& st = *clockwork::state(core);
        clockwork::CoreClock& cc = st.clock;
        const clockwork::Build& build = st.build;
        InstanceRingScope instance_ring(st);   // the audio thread writes the instance's ring
        BlockOpenScope block_open(cc);     // until the block is done
        uint8_t* const base = st.arena.laid_out();
        if (!base) {
            return; // not laid out: nothing to render
        }
        const clockwork::Layout& L = st.arena.layout;
        RingCursors* const cursors = clockwork::cursors_in(base, L);
        PerformanceMetrics* const metrics = clockwork::metrics_in(base);
#if CLOCKWORK_SYNTH
        if (!st.guest.instance) {
            // The instance was destroyed for a cold swap and its successor is
            // being built: silence, and ingress waits for it. Unless the build
            // failed — then nothing is coming, and clockwork still answers.
            if (st.guest.boot_failed.load(std::memory_order_acquire))
                drain_without_guest(st);
            return;
        }
#endif

        // A new instance says nothing before its setup, nor a survived change
        // before its notice: one the ring had no room for says it now, or
        // waits, silent, for a block when it can.
        if (!say_owed(st)) return;

        // The engine's clock, kept here on every target (clock/TimeSource.h).
        // The host passed its frame count and clocks in `time`. This block:
        //   - re-anchors the clock if the audio was away (clockwork_audio_back)
        //     or the host's audio just started (clockwork_audio_starts);
        //   - otherwise slews it toward the host's wall clock;
        //   - publishes the block's first frame and its NTP time to the sample
        //     clock, as DAC time when the host reported an output latency, for
        //     readers on other threads (OscChannel#now, a client's sample clock).
        // A block that is not rendered publishes nothing. A freewheeling clock
        // has no wall clock, keeps time from its samples alone and never sees
        // a gap.
        const double rate = static_cast<double>(metrics->audio_sample_rate.load(std::memory_order_relaxed));
        const double sample_pos = static_cast<double>(time.audio_frames);
        ClockworkClock* clk = cc.clock.load(std::memory_order_acquire);
        const bool freewheel = clk && clk->freewheelClock();
        const bool gap = !freewheel && rate > 0.0
                      && clockwork_audio_back(core, time.wall_ntp, time.monotonic_secs, sample_pos / rate);

#if CLOCKWORK_SCHEDULER
        if (EngineScheduler* q = clockwork_engine_schedule(core)) q->drainPendingClear();
#endif

        double current_ntp = clk ? clk->now() : time.wall_ntp;
        uint64_t host_micros = 0;
        if (clk && rate > 0.0) {
#ifndef __EMSCRIPTEN__
            LinkAudioHost* link = cc.link_audio.load(std::memory_order_acquire);
#endif
            const bool asked = cc.anchor_requested.load(std::memory_order_acquire);
            const bool anchoring = !clk->audioThreadAnchored() || gap || asked;
            if (anchoring) {
                if (asked) cc.anchor_requested.store(false, std::memory_order_release);
                clk->resetAudioThreadTime(sample_pos, rate, time.wall_ntp,
                                          clk->floorRenderNtp(time.output_latency_frames));
#ifndef __EMSCRIPTEN__
                if (link) link->resetBlockClock();
#endif
            }
            current_ntp = clk->updateAudioThreadNTP(sample_pos, rate, time.wall_ntp);
            if (const double drift = clk->takeDriftReport(); drift != 0.0)
                clockwork_log(core, "[clock] wall clock and audio clock have diverged by %+.1f ms", drift * 1000.0);
            clk->publishSampleClock(sample_pos, rate, current_ntp, time.output_latency_frames);
            // Publish the clock metrics immediately on an anchor, not at the
            // next flush, so readers and the history see it on that block.
            if (anchoring) clk->publishClockMetrics(metrics, current_ntp, 4.0);
#ifndef __EMSCRIPTEN__
            // The block's Link stamp: the host's link_host_micros when it gave one, else derived from the clock.
            if (link) host_micros = static_cast<uint64_t>(std::max<int64_t>(0, link->blockHostMicros(sample_pos, rate)));
#endif
        }
        time.block_ntp = current_ntp;
        time.block_host_micros = time.link_host_micros ? time.link_host_micros : host_micros;
        // A host with a transport of its own: what it has said for a frame of this block, or one before it,
        // is taken now, each at its frame's own time; what it has said for a later frame waits.
        if (clk && cc.host_said.load(std::memory_order_relaxed)) {
            // The clock state says a host has a transport, a state made again by a rebuild too.
            if (!(readClockworkClock(clk->state()).flags & SC_FLAG_HOST_TIMELINE)) clk->state()->setFlag(SC_FLAG_HOST_TIMELINE, true);
            const double block_end = sample_pos + static_cast<double>(clockwork_block_size(core));
            uint32_t kept = 0;
            for (uint32_t i = 0; i < cc.host_word_count; ++i) {
                const clockwork::HostWord& said = cc.host_words[i];
                const double frame = static_cast<double>(said.frame);
                if (frame >= block_end || !(rate > 0.0)) {
                    cc.host_words[kept++] = said;
                    continue;
                }
                const double bpm_before = readClockworkClock(clk->state()).bpm;
                cc.host_moved |= clk->followHost(said, current_ntp + (frame - sample_pos) / rate, rate);
                const ClockworkClockSnapshot now = readClockworkClock(clk->state());
                if (said.kind == clockwork::HostWord::Beat && now.is_playing) {
                    cc.host_line = true;
                    cc.host_line_at = said;
                } else if (said.kind == clockwork::HostWord::Tempo && cc.host_line) {
                    // Its line runs on to this frame at the tempo it had.
                    cc.host_line_at.value += (frame - static_cast<double>(cc.host_line_at.frame)) * bpm_before / (60.0 * rate);
                    cc.host_line_at.frame = said.frame;
                } else if (!now.is_playing) {
                    cc.host_line = false;
                }
            }
            cc.host_word_count = kept;
            // The grid at this block's first frame, kept on the host's line.
            if (cc.host_line && rate > 0.0) {
                clockwork::HostWord here = cc.host_line_at;
                here.value += (sample_pos - static_cast<double>(here.frame)) * readClockworkClock(clk->state()).bpm / (60.0 * rate);
                cc.host_moved |= clk->followHost(here, current_ntp, rate);
            }
        }
        if (cc.host_moved) say_host_moved(st, current_ntp);

        // Load-add-store, not fetch_add: this counter has exactly one writer
        // (the audio thread, here) and readers only ever load it, so a
        // read-modify-write buys nothing and costs something: ESP-IDF builds
        // with -mdisable-hardware-atomics, so every atomic RMW is a call into
        // libatomic that takes a critical section, once a block, for an
        // increment nothing races.
        const uint32_t pc = metrics->process_count.load(std::memory_order_relaxed) + 1;
        metrics->process_count.store(pc, std::memory_order_relaxed);
        // The hour's history of every counter and gauge, a bucket at a time
        // on the engine's clock (metrics_history.h).
        clockwork::metrics_history::Writer(base + METRICS_HISTORY_START)
            .record(reinterpret_cast<const std::atomic<uint32_t>*>(metrics), current_ntp);

        // Host telemetry. Sample the ring fill every block so peaks stay a true
        // high-water mark (a burst can fill and drain within a few blocks); this is cheap
        // — the control reads overlap the drain and peaks are local. Flush the SAB (clock
        // readout + ring used/peaks) only at the poll rate below: a consumer reads it at
        // display rate, and per-block flushing is costly where the metrics struct is in
        // slow memory (PSRAM on ESP32). process_count stays per-block as the block counter.
        {
            const auto used = [](const std::atomic<int32_t>& head, const std::atomic<int32_t>& tail, uint32_t size) {
                return ring_used_bytes(static_cast<uint32_t>(head.load(std::memory_order_relaxed)),
                                       static_cast<uint32_t>(tail.load(std::memory_order_relaxed)), size);
            };
            const uint32_t in_used      = used(cursors->in_head, cursors->in_tail, L.profile.in_ring_bytes);
            const uint32_t out_used     = used(cursors->out_head, cursors->out_tail, L.profile.rt_out_ring_bytes);
            const uint32_t nrt_out_used = used(cursors->nrt_out_head, cursors->nrt_out_tail, L.profile.nrt_out_ring_bytes);
            clockwork::MetricsFlush& mf = st.metrics;
            mf.sample(in_used, out_used, nrt_out_used);
            if ((pc % mf.period.load(std::memory_order_relaxed)) == 0u) {
                if (clk) {
                    clk->publishClockMetrics(metrics, current_ntp, 4.0);
#ifndef __EMSCRIPTEN__
                    if (LinkAudioHost* link = cc.link_audio.load(std::memory_order_acquire)) {
                        clk->publishLinkMetrics(metrics, 4.0);
                        link->publishMetrics(metrics);
                    }
#endif
                }
                metrics->in_buffer_used_bytes.store(in_used, std::memory_order_relaxed);
                metrics->in_buffer_peak_bytes.store(mf.in_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->out_buffer_used_bytes.store(out_used, std::memory_order_relaxed);
                metrics->out_buffer_peak_bytes.store(mf.out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->nrt_out_buffer_used_bytes.store(nrt_out_used, std::memory_order_relaxed);
                metrics->nrt_out_buffer_peak_bytes.store(mf.nrt_out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                // the guest's pool's and the rust heap's census: what is in use, the most ever, and the trouble counts;
                // and the guest's mallocs (guest_malloc.h): what it holds, and how often it allocated while processing
                metrics->guest_malloc_bytes.store((uint32_t)clockwork_guest_malloc_bytes(st), std::memory_order_relaxed);
                metrics->guest_mallocs_processing.store(clockwork_guest_mallocs_processing(st), std::memory_order_relaxed);
                metrics->mallocs_after_boot.store(clockwork_mallocs_after_boot(), std::memory_order_relaxed);
                metrics->malloc_bytes_after_boot.store((uint32_t)clockwork_malloc_bytes_after_boot(), std::memory_order_relaxed);
                const ClockworkPoolCensus guest_pool = clockwork_heap_census(st.heap, CLOCKWORK_POOL_GUEST);
                metrics->guest_pool_used_bytes.store((uint32_t)guest_pool.used, std::memory_order_relaxed);
                metrics->guest_pool_peak_bytes.store((uint32_t)guest_pool.peak, std::memory_order_relaxed);
                const ClockworkPoolCensus own_pool = clockwork_heap_census(st.heap, CLOCKWORK_POOL_OWN);
                metrics->rust_heap_used_bytes.store((uint32_t)own_pool.used, std::memory_order_relaxed);
                metrics->rust_heap_fallbacks.store(own_pool.fallbacks, std::memory_order_relaxed);
            }
        }

        // Process incoming OSC messages. The walk — header validation,
        // untrusted-cursor repair, padding markers, gap tracking, tail resync on
        // corruption — is the shared lanes walker (ring_drain.h); only the
        // perform/schedule/classify policy lives here, in the callback. The drain
        // and the scheduler fire below run on every build; only the audio render
        // needs a DSP instance, so the `if (instance)` guard is kept where the
        // audio path is compiled in and elided where it is not.
#if CLOCKWORK_SYNTH
        if (st.guest.instance)
#endif
        {
            // Off-thread reset request (purge → clear_scheduler): apply it
            // here so the drain state stays audio-thread-only. Read before the
            // exchange, and take the exchange only when a request is actually
            // waiting: a byte load compiles to one instruction, while the
            // exchange is a libatomic call under -mdisable-hardware-atomics
            // (ESP-IDF), and this is the audio thread asking "has anyone
            // asked?" on a block where the answer is almost always no. The
            // exchange still does the taking, so two threads cannot both
            // claim one request.
            clockwork::Ingress& in = st.ingress;
            if (in.seq_reset.load(std::memory_order_relaxed) &&
                in.seq_reset.exchange(false, std::memory_order_relaxed))
                in.drain.lastSeq = -1;

            // Bound per block to stay within the audio budget.
            constexpr uint32_t MAX_MESSAGES_PER_FRAME = 32;

            // Snapshot the gap counter so losses this block can be surfaced
            // in the debug channel (the walker only counts them).
            uint32_t gaps_before =
                metrics->messages_sequence_gaps.load(std::memory_order_relaxed);

            // This block's OSC time window. Computed BEFORE the drain, because ingest_frame
            // hands every message this block's start alongside its own timetag — and that
            // pair is what makes sample-accurate placement the DSP's arithmetic rather than
            // clockwork's.
            const auto currentOscTime = clockwork::Timetag::fromNtpSeconds(current_ntp);
            const auto nextOscTime    = currentOscTime + build.block_step;
            cc.block_ntp.store(currentOscTime.ntp(), std::memory_order_relaxed);

            ClockworkDrainStop stop = ClockworkDrainStop::Empty;
            clockwork_drain_ring(
                base + L.in_ring_start, L.profile.in_ring_bytes,
                &cursors->in_head, &cursors->in_tail, in.drain,
                ClockworkDrainMetrics{ &metrics->messages_processed, nullptr,
                                &metrics->messages_dropped,
                                &metrics->messages_sequence_gaps },
                MAX_MESSAGES_PER_FRAME,
                [&st, currentOscTime, &in](uint32_t sourceId, const uint8_t* payload,
                                 uint32_t payload_size, uint32_t seq) -> ClockworkDrainVerdict {
                    // Below the mark: written for a generation that has
                    // closed, or while the audio was away — refused to its
                    // sender with the mark's reason and generation as the
                    // ring has room, and consumed undispatched. Clockwork's
                    // own go through unless they hand the guest something:
                    // the engine answers those whatever the guest is doing.
                    // And below a mark of what is due (the audio away), a
                    // bundle for later goes through to play at its time: what
                    // a client sends as the audio comes back is not stale.
                    if (const RingMark* mark = in.marks.of(seq);
                        mark && !clockwork_sys_answers_without_guest(payload, payload_size)
                        && !(mark->onlyDue && bundle_for_later(payload, payload_size, currentOscTime)))
                        return refuse_when_room(st, sourceId, payload, payload_size,
                                                mark->reason, mark->generation)
                            ? ClockworkDrainVerdict::Consume : ClockworkDrainVerdict::Retain;

                    // In-place delivery: the payload points into the IN ring
                    // (the consumer owns the region until we return Consume).
                    // Delivery is synchronous and anything kept is copied — a
                    // parked message is memcpy'd into the schedule's data pool,
                    // and dsp_osc is documented as not retaining bytes — so
                    // nothing outlives this callback holding the pointer.
                    ingest_frame(st, payload, payload_size, sourceId, currentOscTime);
                    return ClockworkDrainVerdict::Consume;
                },
                &stop);

            // The walker resyncs and counts on corruption; policy — rate-
            // limited logging and the status flag — stays the engine's.
            uint32_t gaps_after =
                metrics->messages_sequence_gaps.load(std::memory_order_relaxed);
            if (gaps_after != gaps_before && in.gap_log.say())
                clockwork_log(st.core, "WARNING: IN sequence gap: %u message(s) missing (total %u)",
                             gaps_after - gaps_before, gaps_after);
            if (stop == ClockworkDrainStop::BadMagic || stop == ClockworkDrainStop::BadLength ||
                stop == ClockworkDrainStop::BadCursor) {
                if (in.corrupt_log.say()) {
                    clockwork_log(st.core, "ERROR: IN ring corrupt (%s): head=%d tail=%d - pending region dropped",
                                 stop == ClockworkDrainStop::BadMagic  ? "bad magic" :
                                 stop == ClockworkDrainStop::BadLength ? "bad length" : "bad cursor",
                                 cursors->in_head.load(std::memory_order_relaxed),
                                 cursors->in_tail.load(std::memory_order_relaxed));
                }
                if (stop == ClockworkDrainStop::BadLength)
                    cursors->status_flags.fetch_or(STATUS_FRAGMENTED_MSG, std::memory_order_relaxed);
            }

            // Nothing pushes tempo at the DSP. DspConfig::clock hands it the
            // same ClockworkClockState bytes JavaScript polls, read through the
            // same readClockworkClock() helper. One clock, no copy anywhere.

            // Send any MIDI clock ticks due in the look-ahead window — a
            // midi_clock_beat burst or a follower's pulses. ClockworkClock-timed, so
            // they stay sample-locked to audio, and each carries its own time
            // to its sink — no queue is involved and this runs whether or not
            // one was compiled in. The engine that owns the clock out publishes
            // it; a host with none (the worklet) has nothing to generate.
            if (MidiClockOut* clockOut = cc.midi_clock_out.load(std::memory_order_acquire))
                clockOut->generate(current_ntp);

#if CLOCKWORK_SCHEDULER
            // Fire: drain every event due this block in time order, handing
            // each to the SAME dispatch() the immediate drain uses. The event
            // keeps its own timetag — it is due within THIS block, so the
            // difference from currentOscTime is exactly the sub-block offset
            // the DSP needs, and flattening it to "now" would throw away the
            // sample accuracy.
            if (EngineScheduler* q = clockwork_engine_schedule(core)) {
                clockwork_fire_due(*q, nextOscTime, currentOscTime,
                    [&st](const uint8_t* d, uint32_t n, uint32_t origin,
                          clockwork::Timetag when, clockwork::Timetag blockTime) {
                        if (note_lateness(st, when, blockTime)) return;   // later than the host allows
                        dispatch(st, d, n, origin, when, blockTime);
                    });
                // Publish queue depth once per block, after draining.
                update_scheduler_depth_metric(st, static_cast<uint32_t>(q->size()));
            }
#else
            // No queue: nothing waits, so nothing fires and the depth is 0 by
            // construction. The block-time window above is still computed —
            // dispatch() carries each message's own timetag across the boundary
            // either way, and the DSP subtracts the block start itself.
            (void)nextOscTime;
#endif

#if CLOCKWORK_SYNTH
            const uint32_t QUANTUM_SIZE = build.block;

            // Render one block. The DSP reads `in` and WRITES `out`. Nothing is zeroed
            // first: dsp_api.h says output is written, not accumulated, so a DSP with nothing
            // to say writes silence and owns every sample it emits.
            //
            // currentOscTime is the timetag of this block's FIRST frame, which is what lets
            // the DSP place a message due at T at frame (T - block_time) * sample_rate / 2^32
            // without clockwork poking at an offset.
            //
            // rt_dsp_guard marks the render as RT scope — always on, so the suite-wide
            // listener reports any global new/delete inside it. Scoped to the render rather
            // than the whole callback because that is the region with the hard no-alloc rule.
            {
                rt_alloc::Guard rt_dsp_guard;
                // Identity before audio: an attach or detach queued since the
                // last block is handed to the DSP here, on the verb it
                // declared, BEFORE the ports it names are pulled. That
                // ordering is the point — a DSP that models a stream as a
                // process gets to exist before the first frame of it arrives.
                // Nothing is built here (the bytes were formed on the control
                // thread) and nothing waits: an announcement a producer has
                // claimed but not published simply arrives next block. Inside
                // the RT guard, so the suite proves it allocates nothing.

                // Sources first: a port bound to an input range fills it here,
                // after the device backend and before the DSP, so a streamed
                // sample and a microphone arrive by the same door on stable
                // channels (clockwork_port_bus.h). Nothing happens at all when
                // nothing is bound.
                pull_port_sources(st, QUANTUM_SIZE);

                // Hosted plugin tracks are ports like any other: the send
                // lanes are a sink pushed below, the return lanes a source
                // pulled above, and the plugins run in another process
                // (plugin_bridge.h). The core knows nothing of them.

                // Live counts, per block: Link peers join and leave and a
                // device switch changes the interface underneath us. The
                // config carried only the ceilings.
                {
                    GuestCallScope guest(GuestCall::Processing);   // the render is the guest's; the sinks after it are not
                    dsp_process(st.guest.instance,
                                build.in_channels ? st.staging.in_ptrs : nullptr, build.in_channels,
                                st.staging.out_ptrs, build.out_channels,
                                QUANTUM_SIZE,
                                currentOscTime.abi());
                }

                // Sinks after: what a sink records is what the DSP actually
                // emitted (the "post" answer to open question 1 in docs/PORTS.md).
                push_port_sinks(st, QUANTUM_SIZE);
            }
            // Advance the engine frame count past this block; tap streams anchor on it, on every host.
            if (clk) clk->advanceEngineFrames(sample_pos + static_cast<double>(QUANTUM_SIZE));
            // The scope, at the device edge: what leaves for the device
            // this block (after the guest, after the port sinks) and what
            // arrived from it (as the guest saw it). Every host, every
            // block, at the channel count the slot was formatted with. A
            // client draws the master mix, or records it, or meters the
            // input, from these and asks the guest for nothing.
            {
                const auto tap = [&](uint32_t slot, const float* bus) {
                    shm_audio_buffer* const t = clockwork::scopeSlot(base, L, slot);
                    const uint32_t ch = t->channels;
                    if (ch == 0 || ch > L.profile.scope_channels || !bus) return;
                    const float* channel_data[CLOCKWORK_MAX_CHANNELS];
                    for (uint32_t c = 0; c < ch; ++c)
                        channel_data[c] = bus + static_cast<size_t>(c) * QUANTUM_SIZE;
                    shm_audio_buffer_writer(t, L.profile.scope_frames, L.profile.scope_channels)
                        .write(channel_data, ch, QUANTUM_SIZE);
                };
                tap(SCOPE_OUT_SLOT, st.staging.out);
                tap(SCOPE_IN_SLOT,  st.staging.in);
            }

#endif // CLOCKWORK_SYNTH
        }
    }

    // Frame a log line as a `/clockwork/debug <text>` OSC message and emit it.
    // Routing keeps the RT-out ring single-writer: on the audio thread the line
    // goes to the lock-free RT-out ring; off the audio thread (watchdog, recovery,
    // boot on native) it goes to the locked multi-producer NRT-out ring, so RT-out
    // never gets a second concurrent writer. The NRT gateway drains and forwards
    // both, so delivery is identical. WASM is single-threaded with no NRT-out
    // drainer, so it always uses RT-out. Either way the line leaves as an ordinary
    // addressed OSC message the host dispatches to its debug channel.
    // False when the core's ring is not laid out, and nothing was written.
    // The clients hear what following the host moved, as they hear a peer's
    // on a Link session: the grid (its tempo, or its bars under
    // the same tempo) as notify/tempo, on which a follower reads the grid
    // again, and playing or stopped as notify/transport. At most once in
    // kHostNotifySeconds, and what the ring has no room for is said at a
    // later block.
    static void say_host_moved(clockwork::CoreState& st, double blockNtp) {
        clockwork::CoreClock& cc = st.clock;
        // Said to a client that asked for the timeline's notices (lanes.h): one that did not reads the clock.
        if (!(st.routing.client_wants.load(std::memory_order_relaxed) & CLOCKWORK_WANTS_CLOCK)) {
            cc.host_moved = 0;
            return;
        }
        const double since = blockNtp - cc.host_said_ntp;
        if (since >= 0.0 && since < clockwork::kHostNotifySeconds) return;
        uint8_t* const base = st.arena.laid_out();
        const ClockworkClock* clk = cc.clock.load(std::memory_order_acquire);
        if (!base || !clk) return;
        const clockwork::Layout& L = st.arena.layout;
        RingCursors* const cursors = clockwork::cursors_in(base, L);
        PerformanceMetrics* const metrics = clockwork::metrics_in(base);
        const ClockworkClockSnapshot now = readClockworkClock(clk->state());
        const auto say = [&](const osc::OutboundPacketStream& s) {
            return ::ring_buffer_write(
                base + L.rt_out_ring_start, L.profile.rt_out_ring_bytes,
                &cursors->out_head, &cursors->out_tail, &cursors->out_sequence,
                0,
                s.Data(), static_cast<uint32_t>(s.Size()), &cursors->status_flags, metrics);
        };
        std::array<char, 96> buf{};
        constexpr uint32_t kGrid = clockwork::kHostMovedTempo | clockwork::kHostMovedGrid;
        if (cc.host_moved & kGrid) {
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/tempo")) << now.bpm << osc::EndMessage;
            if (say(s)) cc.host_moved &= ~kGrid;
        }
        if (cc.host_moved & clockwork::kHostMovedTransport) {
            osc::OutboundPacketStream s(buf.data(), buf.size());
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/notify/transport"))
              << static_cast<int32_t>(now.is_playing ? 1 : 0)
              << static_cast<osc::int64>(std::llround(now.is_playing_at_ntp * 1e6))
              << osc::EndMessage;
            if (say(s)) cc.host_moved &= ~clockwork::kHostMovedTransport;
        }
        cc.host_moved &= ~clockwork::kHostMovedMeter;   // a meter has no notice: a reader of the clock finds it
        cc.host_said_ntp = blockNtp;
    }

    static bool emit_debug_osc(clockwork::CoreState& st, const char* text, uint32_t len) {
        uint8_t* const base = st.arena.laid_out();
        if (!base) return false;
        const clockwork::Layout& L = st.arena.layout;
        RingCursors* const cursors = clockwork::cursors_in(base, L);
        PerformanceMetrics* const metrics = clockwork::metrics_in(base);
        if (len > 960) len = 960;  // matches buildDebugOsc's clamp; keeps the metric in sync

        std::array<char, 1024> pkt{};
        uint32_t p = clockwork::buildDebugOsc(pkt.data(), text, len);

        // The audio thread owns the lock-free RT-out ring, so it logs there. Any
        // other thread routes to the locked NRT-out ring instead — but only where a
        // backend drains it (Egress::nrt_drained, published by the native NRT
        // gateway). A single-threaded worklet target (WASM / self-driven device)
        // never drains NRT-out and has no second RT-out writer, so it always uses
        // the always-safe RT-out. Capability signal, not an __EMSCRIPTEN__ branch,
        // so every target shares one codepath.
        const bool useRtOut =
            t_instance_ring_of == &st || !st.egress.nrt_drained.load(std::memory_order_relaxed);
        if (useRtOut) {
            ::ring_buffer_write(
                base + L.rt_out_ring_start, L.profile.rt_out_ring_bytes,
                &cursors->out_head, &cursors->out_tail, &cursors->out_sequence,
                EGRESS_BROADCAST_NOTIFY, 0,  // route, source_id (debug broadcasts)
                pkt.data(), p, &cursors->status_flags, metrics);   // a refused log line is counted too
        } else {
            clockwork_egress_nrt_write(st.core, EGRESS_BROADCAST_NOTIFY, 0,
                                reinterpret_cast<const uint8_t*>(pkt.data()), p);
        }

        // Count the debug line for the metrics view.
        metrics->debug_messages_received.fetch_add(1, std::memory_order_relaxed);
        metrics->debug_bytes_received.fetch_add(len, std::memory_order_relaxed);
        return true;
    }

    /*
     * Boot-time failure reporting, which clockwork_log cannot do.
     *
     * clockwork_log writes an OSC message into the debug ring for a worker to drain
     * and forward. That works once the engine is running and is worth nothing
     * before it: every FATAL below describes a boot that is about to not
     * happen, so the ring it wrote to is a ring nobody ever reads, and an
     * early return out of init_memory would reach the client as an
     * initialisation timeout with the reason in a discarded buffer.
     *
     * So these go straight out: the browser console on web, stderr on native.
     * Both are visible with no engine, which is the only property that
     * matters here.
     */
    static void clockwork_boot_fail(clockwork::CoreState& st, const char* fmt, ...) {
        std::array<char, 512> buffer{};
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer.data(), buffer.size(), fmt, args);
        va_end(args);
        // And kept, for the host to hand its clients: a reason on stderr is a
        // reason only someone reading the terminal ever sees.
        record_boot_error(st, "%s", buffer.data());
#ifdef __EMSCRIPTEN__
        emscripten_console_error(buffer.data());
#else
        fprintf(stderr, "%s\n", buffer.data());   // stdio-ok: reporting a boot that produced no ring
#endif
    }

    // A line no ring takes: on the web the console, natively stderr. `len`
    // bytes of `text`, which need not be NUL-terminated (a Rust line is not).
    static void log_no_ring(const char* text, uint32_t len) {
#ifdef __EMSCRIPTEN__
        std::array<char, 1024> line{};
        const size_t n = std::min<size_t>(len, line.size() - 1);
        std::memcpy(line.data(), text, n);
        emscripten_console_log(line.data());
#else
        fprintf(stderr, "%.*s\n", static_cast<int>(len), text);   // stdio-ok: no engine ring exists
        fflush(stderr);                                            // stdio-ok
#endif
    }

    // Native: a core's boot holds what is logged before its ring exists
    // (EarlyLog). Bounded, so a boot that logs in a loop cannot grow it
    // without limit.
    static constexpr size_t kEarlyLogMaxLines = 256;

    // Held by `st`, if its boot holds: true when it took the line.
    static bool hold_line(clockwork::CoreState& st, const char* text, uint32_t len) {
        clockwork::EarlyLog& el = st.early_log;
        const std::lock_guard<std::mutex> lk(el.lock);
        if (!el.holding || el.lines.size() >= kEarlyLogMaxLines) return false;
        el.lines.emplace_back(text, len);
        return true;
    }

    // Onto `st`'s ring, or held by its boot: false when neither took it.
    static bool log_line(clockwork::CoreState& st, const char* text, uint32_t len) {
        return emit_debug_osc(st, text, len) || hold_line(st, text, len);
    }

#ifndef __EMSCRIPTEN__
    extern "C" void clockwork_log_hold(ClockworkCore* core) {
        clockwork::CoreState* const st = clockwork::state(core);
        if (!st) return;
        const std::lock_guard<std::mutex> lk(st->early_log.lock);
        st->early_log.holding = true;
    }

    extern "C" void clockwork_log_release(ClockworkCore* core) {
        clockwork::CoreState* const st = clockwork::state(core);
        if (!st) return;
        std::vector<std::string> held;
        {
            const std::lock_guard<std::mutex> lk(st->early_log.lock);
            st->early_log.holding = false;
            held.swap(st->early_log.lines);
        }
        for (const auto& line : held)
            if (!emit_debug_osc(*st, line.data(), static_cast<uint32_t>(line.size())))
                log_no_ring(line.data(), static_cast<uint32_t>(line.size()));   // no ring came up for this boot
    }
#endif

    // A line `st` says (DspHost::log): onto its ring, or held by its boot,
    // or where a line with no ring goes.
    static void log_to(clockwork::CoreState& st, const char* text) {
        const auto len = static_cast<uint32_t>(std::strlen(text));
        if (!log_line(st, text, len)) log_no_ring(text, len);
    }

    // A line of `core`'s log (clockwork_log): onto its ring, or held by its
    // boot; with no core, or none that took it, to the console.
    static void log_to_core(ClockworkCore* core, const char* text, uint32_t len) {
        clockwork::CoreState* const st = clockwork::state(core);
        if (!st || !log_line(*st, text, len)) log_no_ring(text, len);
    }

    static int clockwork_log_impl(ClockworkCore* core, const char* fmt, va_list args) {
        std::array<char, 1024> buffer{};
        const int result = vsnprintf(buffer.data(), buffer.size(), fmt, args);
        log_to_core(core, buffer.data(), static_cast<uint32_t>(std::strlen(buffer.data())));
        return result;
    }

    extern "C" EMSCRIPTEN_KEEPALIVE
    int clockwork_log(ClockworkCore* core, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        const int result = clockwork_log_impl(core, fmt, args);
        va_end(args);
        return result;
    }

    // For a caller holding a va_list of its own.
    extern "C" EMSCRIPTEN_KEEPALIVE
    int clockwork_log_va(ClockworkCore* core, const char* fmt, va_list args) {
        return clockwork_log_impl(core, fmt, args);
    }

    // An already-formatted line, without the copy through vsnprintf.
    extern "C" EMSCRIPTEN_KEEPALIVE
    int clockwork_log_raw(ClockworkCore* core, const char* msg, uint32_t len) {
        if (!msg || len == 0) return 0;
        log_to_core(core, msg, len);
        return static_cast<int>(len);
    }

    // The block's buses and its geometry, as the build laid them out
    // (lanes.h). A cold swap can rebuild at a different width than boot, so
    // a caller that caches the channel counts (ClockworkProcessor's clamps)
    // reads them again afterwards. Clockwork answers from what it
    // configured, never by asking the DSP.
    EMSCRIPTEN_KEEPALIVE
    uint32_t clockwork_out_channels(ClockworkCore* core) {
#if CLOCKWORK_SYNTH
        const clockwork::CoreState* st = clockwork::state(core);
        return st && st->guest.instance ? st->build.out_channels : 0;
#else
        (void)core;
        return 0;
#endif
    }

    EMSCRIPTEN_KEEPALIVE
    uint32_t clockwork_in_channels(ClockworkCore* core) {
#if CLOCKWORK_SYNTH
        const clockwork::CoreState* st = clockwork::state(core);
        return st && st->guest.instance ? st->build.in_channels : 0;
#else
        (void)core;
        return 0;
#endif
    }

    // The master mix, the output side of the core's staging: null before a
    // build has laid it out, which a host plays as silence.
    EMSCRIPTEN_KEEPALIVE
    const float* clockwork_audio_out(ClockworkCore* core) {
        const clockwork::CoreState* st = clockwork::state(core);
        return st && st->arena.laid_out() ? st->staging.out : nullptr;
    }

    // The input a host fills before the tick, channel-major like the output:
    // what dsp_process is handed as `in`. Null with no instance or no input.
    EMSCRIPTEN_KEEPALIVE
    float* clockwork_audio_in(ClockworkCore* core) {
        const clockwork::CoreState* st = clockwork::state(core);
        return st && st->arena.laid_out() && st->guest.instance ? st->staging.in : nullptr;
    }

    // The OSC time of the block being processed: what a port's transfer
    // signal, which runs inside the block but is handed no metadata, stamps
    // on what it hands to another process.
    EMSCRIPTEN_KEEPALIVE
    ClockworkTimetag clockwork_block_time(ClockworkCore* core) {
        const clockwork::CoreClock* c = clockwork::core_clock(core);
        return ClockworkTimetag{c ? c->block_ntp.load(std::memory_order_relaxed) : 0};
    }

    // The rate the DSP was initialised at; 0 until then. What a handler on
    // the audio thread divides by to turn a timetag into a frame offset.
    EMSCRIPTEN_KEEPALIVE
    double clockwork_sample_rate(ClockworkCore* core) {
        const clockwork::Build* b = clockwork::build(core);
        return b ? b->sample_rate : 0.0;
    }

    // The block in use: 128 on the web (the AudioWorklet's render quantum),
    // natively what was configured at boot; the default block with no
    // instance yet.
    EMSCRIPTEN_KEEPALIVE
    uint32_t clockwork_block_size(ClockworkCore* core) {
#if CLOCKWORK_SYNTH
        const clockwork::Build* b = clockwork::build(core);
        return b && b->block ? b->block : clockwork::kDefaultBlockSize;
#else
        (void)core;
        return clockwork::kDefaultBlockSize;
#endif
    }

} // extern "C"

// ============================================================================
// RING BUFFER HELPER FUNCTIONS (outside namespace for C++ linkage)
// ============================================================================

// Lock-free SPSC ring writer for the OUT egress ring — clockwork replies,
// whatever the DSP emits through DspHost::emit_osc, and /clockwork/debug.
// Everything reaching this ring is written from the audio thread (anything
// off-thread takes the locked NRT-out ring instead), so OUT has a single
// producer and needs no write lock.
//
// Same wire convention as every ring (RingBufferWriter.h / the JS writer):
// frames never wrap — when a frame won't fit before the end, a PADDING_MAGIC
// marker fills the remainder and the write restarts at offset 0, so readers
// parse in place. Overflow drops the message (false) and counts it.
//
// All state is passed in (head/tail, sequence counter, status-flags word,
// metrics) rather than read from engine globals, so the function has no hidden
// dependencies and is unit-tested directly (test_ring_buffer_write.cpp).
bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t route,
    uint32_t source_id,
    const RingPart* parts,
    uint32_t n_parts,
    std::atomic<uint32_t>* status_flags,
    PerformanceMetrics* pm
) {
    // The parts laid end to end are the OSC payload; more than the ring
    // could ever hold is refused as a full ring is.
    uint32_t data_size = 0;
    for (uint32_t i = 0; i < n_parts; ++i) {
        if (parts[i].size > buffer_size - data_size) {
            if (pm) pm->egress_frames_refused.fetch_add(1, std::memory_order_relaxed);
            if (status_flags) status_flags->fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
            return false;
        }
        data_size += parts[i].size;
    }

    // Egress frame: Message{sourceId = token} + [route:u32][osc]; the route word
    // counts toward the message length.
    Message header{};
    header.magic = MESSAGE_MAGIC;
    header.length = sizeof(Message) + static_cast<uint32_t>(sizeof(uint32_t)) + data_size;
    header.sequence = static_cast<uint32_t>(sequence->fetch_add(1, std::memory_order_relaxed));
    header.sourceId = source_id;

    int32_t current_head = head->load(std::memory_order_acquire);
    int32_t current_tail = tail->load(std::memory_order_acquire);

    // Frame footprint: header.length is exact; occupancy and cursor advance
    // are its 4-byte-aligned rounding, matching RingBufferWriter.h and the JS
    // writer (readers advance the tail by this footprint).
    const uint32_t footprint = (header.length + 3u) & ~3u;

    if (!ring_fits(static_cast<uint32_t>(current_head), static_cast<uint32_t>(current_tail),
                   buffer_size, footprint)) {
        if (pm) pm->egress_frames_refused.fetch_add(1, std::memory_order_relaxed);
        if (status_flags) status_flags->fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
        return false;
    }

    // A frame that does not fit before the end restarts at offset 0
    // (ring_fits said there is room there), the remainder marked as padding.
    uint32_t space_to_end = buffer_size - current_head;
    if (footprint > space_to_end) {
        // Padding marker: magic word, zeros to the end of the ring (matching
        // the other writers — when >= 16 bytes remain this doubles as a full
        // zeroed pad header; 4-byte alignment guarantees the magic fits).
        uint32_t pad = PADDING_MAGIC;
        std::memcpy(buffer_start + current_head, &pad, sizeof(pad));
        if (space_to_end > sizeof(pad)) {
            std::memset(buffer_start + current_head + sizeof(pad), 0,
                        space_to_end - sizeof(pad));
        }

        current_head = 0;
    }

    std::memcpy(buffer_start + current_head, &header, sizeof(Message));

    std::memcpy(buffer_start + current_head + sizeof(Message), &route, sizeof(uint32_t));
    uint8_t* at = buffer_start + current_head + sizeof(Message) + sizeof(uint32_t);
    for (uint32_t i = 0; i < n_parts; ++i) {
        if (parts[i].size == 0) continue;
        std::memcpy(at, parts[i].data, parts[i].size);
        at += parts[i].size;
    }

    // Zero the 0-3 alignment pad bytes (determinism: no stale ring bytes
    // inside a frame's footprint).
    if (footprint > header.length) {
        std::memset(buffer_start + current_head + header.length, 0,
                    footprint - header.length);
    }

    int32_t new_head = (current_head + footprint) % buffer_size;
    head->store(new_head, std::memory_order_release);

    // Track peak buffer usage at write time — the reader may drain the
    // buffer before the periodic metrics sampling sees the fill level.
    if (pm) {
        const uint32_t used = ring_used_bytes(static_cast<uint32_t>(new_head), static_cast<uint32_t>(current_tail), buffer_size);
        uint32_t prev = pm->out_buffer_peak_bytes.load(std::memory_order_relaxed);
        while (used > prev) {
            if (pm->out_buffer_peak_bytes.compare_exchange_weak(
                    prev, used, std::memory_order_relaxed))
                break;
        }
    }

    return true;
}

bool ring_buffer_write(
    uint8_t* buffer_start,
    uint32_t buffer_size,
    std::atomic<int32_t>* head,
    std::atomic<int32_t>* tail,
    std::atomic<int32_t>* sequence,
    uint32_t route,
    uint32_t source_id,
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags,
    PerformanceMetrics* pm
) {
    const RingPart part{ data, data_size };
    return ring_buffer_write(buffer_start, buffer_size, head, tail, sequence, route, source_id,
                             &part, 1, status_flags, pm);
}

// ── Clockwork's own timed queue ────────────────────────────────────────────
//
// The store, the fire loop and the classifier all live in this file. Holding a
// schedule is the DSP's own affair, declared with DspInfo::holds_schedule, so
// nothing bridges back across the boundary for it.

#if CLOCKWORK_SCHEDULER
EngineScheduler* clockwork_engine_schedule(const ClockworkCore* core) {
    const clockwork::Schedule* sc = clockwork::schedule(core);
    return sc && sc->store ? &sc->store->s : nullptr;
}

// Build `core`'s store, at its boot: on the control thread, once the
// engine's heap is up — built any earlier its data pool fails to allocate,
// and every add would be counted into scheduler_dropped as if it were
// overload. Idempotent.
void clockwork_sched_warm(ClockworkCore* core) {
    clockwork::Schedule* sc = clockwork::schedule(core);
    if (!sc) return;
    if (!sc->store) sc->store = new (std::nothrow) ScheduleStore();
    if (!sc->store || !sc->store->s.ready()) {
        // Loud, once, and unmistakable. A queue that failed to build refuses
        // every add exactly as a full one does, so without this the only
        // symptom is a scheduler_dropped count that looks like backpressure.
        clockwork_log(core, "FATAL: the scheduler could not be built (%d slots, %d byte pool). "
                "Nothing scheduled will ever fire.",
                (int)SCHEDULER_SLOT_COUNT, (int)SCHEDULER_DATA_POOL_SIZE);
    }
}

#endif // CLOCKWORK_SCHEDULER

#if CLOCKWORK_SYNTH

// Clockwork's own audio-thread OSC surface: the liveness verbs, and — where
// it is the LAST link in clockwork's chain (a host with no NRT thread) — the
// refusal for anything else the prefix claimed. Never reaches the DSP: the boundary
// already decided that. The verbs and their replies are in clockwork_sys.h — pure
// byte work, so they are tested without an ingress.
bool clockwork_clockwork_sys_route(void* routeCtx, const void* callCtx,
                      const uint8_t* data, std::size_t len) {
    clockwork::CoreState* const st = clockwork::state(static_cast<ClockworkCore*>(routeCtx));
    if (!st) return true;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const uint32_t token = cc ? cc->sourceId : 0;
    // The clock comes from the arena, so the snapshot verb answers against the
    // same published mirror the guest reads — not the engine's own getters,
    // which are not realtime-safe when Link is running.
    uint8_t* const base = st->arena.at();
    const auto* clockState = base
        ? reinterpret_cast<const ClockworkClockState*>(base + CLOCK_STATE_START)
        : nullptr;
    handle_clockwork_sys_osc(data, static_cast<uint32_t>(len),
        [st, token](const uint8_t* d, uint32_t n) { emit_osc_out(*st, token, d, n); },
        clockState, arenaInstanceGeneration(base));
    return true;   // the prefix is claimed whole, answered or refused
}

bool clockwork_sched_flush_route(void* routeCtx, const void* callCtx,
                                 const uint8_t* data, std::size_t len) {
    auto* const core = static_cast<ClockworkCore*>(routeCtx);
    clockwork::CoreState* const st = clockwork::state(core);
    if (!st) return true;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const uint32_t token = cc ? cc->sourceId : 0;
    const auto refuse = [&](const char* reason) {
        clockwork_sys_refuse(data, static_cast<uint32_t>(len), reason, arenaInstanceGeneration(st->arena.at()),
            [st, token](const uint8_t* d, uint32_t n) { emit_osc_out(*st, token, d, n); });
    };
#if CLOCKWORK_SCHEDULER
    // No tag, or "", is the user's tag rather than the wildcard, so a tagless
    // flush can never wipe pending synth bundles or the clock. Read without
    // oscpack: a malformed packet must not throw on this thread.
    clockwork_host::OscReader r(data, len);
    const char* tagName = nullptr;
    if (!r.ok() || (r.peekType() != 0 && !r.readString(tagName))) {
        refuse(CLOCKWORK_REFUSED_MALFORMED);
        return true;
    }
    if (EngineScheduler* q = clockwork_engine_schedule(core))
        q->flush(tagName && *tagName
            ? sched_tag_hash(tagName, std::strlen(tagName))
            : SCHED_TAG_DEFAULT);
#else
    refuse(CLOCKWORK_SCHEDULER_ABSENT);
#endif
    return true;
}

// The other end of clockwork's chain on a host with no NRT thread: the HOST.
//
// Native forwards a claimed verb the audio thread does not answer to the
// control pass (nrtForwardSink), and the refusal for an unknown one happens
// at the far end of that hop. A host with no thread to forward to refuses
// here, unless it has a FRONT — a thread of its own that answers the control
// verbs, which on the web is the client's main thread, the only place Web
// MIDI and the Gamepad API exist at all. With
// its core's Routing::host_forward set, that front is the far end: the verb goes out over the
// egress, to the origin that sent it, and the host answers or refuses it.
//
// THE FRAME CARRIES THE CALL'S TIME. A verb the scheduler fired reaches this
// thread up to a block early, and the host's device (a MIDI port that
// timestamps) wants the moment, not the block. So the verb is wrapped in a
// bundle whose timetag is the call's `when` — 1, "immediately", for a verb
// that never waited — exactly the form the client already reads a scheduled
// time from. Nothing else in the egress is a bundle under the prefix, which
// is how the host tells a forwarded verb from a reply.
//
// The liveness verbs are answered here regardless: proving this thread is
// alive is half of what they are for, and a host that has gone away must not
// take ping with it.
bool clockwork_host_forward_route(void* routeCtx, const void* callCtx,
                                  const uint8_t* data, std::size_t len) {
    clockwork::CoreState* const st = clockwork::state(static_cast<ClockworkCore*>(routeCtx));
    if (!st || !st->routing.host_forward.load(std::memory_order_acquire))
        return clockwork_clockwork_sys_route(routeCtx, callCtx, data, len);

    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const uint32_t token = cc ? cc->sourceId : 0;
    uint8_t* const base = st->arena.at();
    const auto* clockState = base
        ? reinterpret_cast<const ClockworkClockState*>(base + CLOCK_STATE_START)
        : nullptr;
    switch (handle_clockwork_sys_rt(data, static_cast<uint32_t>(len),
                [st, token](const uint8_t* d, uint32_t n) { emit_osc_out(*st, token, d, n); },
                clockState, arenaInstanceGeneration(base))) {
        case ClockworkSysRt::NotClaimed:  return false;
        case ClockworkSysRt::Answered:    return true;
        case ClockworkSysRt::UnknownVerb: break;
    }

    const RingPart verb{ data, static_cast<uint32_t>(len) };
    forward_to_host(*st, token, cc ? cc->when : clockwork::Timetag::immediately(), &verb, 1);
    return true;   // claimed whole: the host answers or refuses it from here
}

// One element to the host, wrapped with its time: "#bundle" NUL, timetag
// (big-endian 64), element size (big-endian 32), the element — laid down on
// the ring behind that header from the pieces it is in, so nothing is
// gathered first. Built by hand: oscpack's stream cannot take an
// already-encoded element. Too wide for the ring, it is refused and counted
// there, as any frame is. Whether it went on the ring.
static bool forward_to_host(clockwork::CoreState& st, uint32_t token, clockwork::Timetag when,
                            const RingPart* element, uint32_t n_parts) {
    static constexpr uint32_t kMaxParts = 4;
    if (n_parts == 0 || n_parts > kMaxParts - 1) return false;
    uint64_t len = 0;
    for (uint32_t i = 0; i < n_parts; ++i) len += element[i].size;
    if (len > UINT32_MAX) return false;
    // The wire form of immediately is 1.
    const uint64_t tt = when.isImmediate() ? 1u : when.ntp();
    std::array<uint8_t, 8 + 8 + 4> header{};
    std::memcpy(header.data(), "#bundle", 8);
    for (unsigned i = 0; i < 8; ++i) header[8 + i]  = static_cast<uint8_t>(tt >> (56u - 8u * i));
    for (unsigned i = 0; i < 4; ++i) header[16 + i] = static_cast<uint8_t>(len >> (24u - 8u * i));
    std::array<RingPart, kMaxParts> parts{};
    parts[0] = RingPart{ header.data(), static_cast<uint32_t>(header.size()) };
    for (uint32_t i = 0; i < n_parts; ++i) parts[1 + i] = element[i];
    return emit_osc_parts(st, reply_route(token), token, parts.data(), n_parts + 1);
}

// An inbound event, on the audio thread, on every host (clockwork_sys.h).
// Out to the subsystem's audience by the route word the egress already has
// for it — the transport fans it out natively; on the web the one client
// hears it and its front keeps the subscription rule — and to the guest if
// it asked. The message is passed through byte for byte, trailing arrival
// time included; the guest gets the same DrainCallCtx every message gets,
// with `when` the block it was drained in.
bool clockwork_event_route(void* routeCtx, const void* callCtx,
                           const uint8_t* data, std::size_t len) {
    clockwork::CoreState* const st = clockwork::state(static_cast<ClockworkCore*>(routeCtx));
    if (!st) return true;
    const char* verb = reinterpret_cast<const char*>(data) + CLOCKWORK_SYS_PREFIX_LEN;
    const uint32_t route = std::strncmp(verb, "gamepad/", 8) == 0
                         ? EGRESS_BROADCAST_GAMEPAD : EGRESS_BROADCAST_MIDI;
    emit_osc_route(*st, route, 0, data, static_cast<uint32_t>(len));
    if (guest_wants_events())
        clockwork_dsp_default_route(routeCtx, callCtx, data, len);
    return true;
}

// A guest's MIDI send, to the host (clockwork_event_sink.h,
// clockwork_sink_set_host_emitter): the bytes and the port as one message,
// "/clockwork/midi/sink/send ,sb <port> <bytes>", forwarded with the send's
// time exactly as a verb is. Origin 0: a sink has no caller to answer, and on
// the web the one client hears everything the egress carries. The host's
// front opens the port on demand — opening a sink onto a port opens the port,
// as it does natively — and hands the browser the bytes with the time.
static int clockwork_sink_host_emit(void* ctx, uint32_t kind, const char* target,
                                    const uint8_t* bytes, uint32_t len, ClockworkTimetag when) {
    clockwork::CoreState* const st = clockwork::state(static_cast<ClockworkCore*>(ctx));
    if (!st || kind != kClockworkSinkMidi || !target || !bytes || len == 0) return 0;
    // The message's head on the stack — its address, type tags, the port
    // and the blob's size — then the bytes where they are, then the blob's
    // padding. A port name past all reason does not encode: a drop, counted
    // by the sink.
    static constexpr char kAddress[] = CLOCKWORK_SYS("midi/sink/send");
    static constexpr uint32_t kMaxTarget = 255;
    const size_t target_len = std::strlen(target);
    if (target_len > kMaxTarget) return 0;
    constexpr auto padded = [](size_t n) { return (n + 4u) & ~size_t{3}; };   // a string and its NULs
    std::array<uint8_t, padded(sizeof kAddress - 1) + 4 + padded(kMaxTarget) + 4> head{};
    size_t at = 0;
    std::memcpy(head.data(), kAddress, sizeof kAddress - 1);
    at += padded(sizeof kAddress - 1);
    std::memcpy(head.data() + at, ",sb", 3);
    at += 4;
    std::memcpy(head.data() + at, target, target_len);
    at += padded(target_len);
    for (unsigned i = 0; i < 4; ++i) head[at + i] = static_cast<uint8_t>(len >> (24u - 8u * i));
    at += 4;
    static constexpr uint8_t kZeros[3] = {};
    const RingPart message[] = {
        { head.data(), static_cast<uint32_t>(at) },
        { bytes, len },
        { kZeros, (4u - (len & 3u)) & 3u },
    };
    // Not on the ring is not sent: the sink counts it dropped.
    return forward_to_host(*st, 0, clockwork::Timetag::fromAbi(when), message, 3) ? 1 : 0;
}

// /clockwork/asset/commit ,iiiiiif id kind offset bytes channels frames rate
// (dsp_api.h, "Assets"). Audio thread. Every check happens before the guest is
// touched, and every refusal names the id and says why.
bool clockwork_asset_route(void* routeCtx, const void* callCtx,
                           const uint8_t* data, std::size_t len) {
    clockwork::CoreState* const found = clockwork::state(static_cast<ClockworkCore*>(routeCtx));
    if (!found) return true;
    clockwork::CoreState& st = *found;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const uint32_t origin = cc ? cc->sourceId : 0;
    const char* verb = reinterpret_cast<const char*>(data) + CLOCKWORK_SYS_PREFIX_LEN;
    if (std::strcmp(verb, "asset/commit") != 0) {
        // Claimed by prefix, not a verb of this layer: refuse as the sys route does.
        handle_clockwork_sys_osc(data, static_cast<uint32_t>(len),
            [&st, origin](const uint8_t* d, uint32_t n) { emit_osc_out(st, origin, d, n); },
            nullptr, arenaInstanceGeneration(st.arena.at()));
        return true;
    }
    int32_t id = -1;
    auto refuse = [&](const char* why) {
        asset_emit(st, origin, CLOCKWORK_SYS("asset/refused"), static_cast<uint32_t>(id), why);
        return true;
    };
    int32_t kind = 0, offset = 0, bytes = 0, channels = 0, frames = 0;
    float rate = 0.f;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data), static_cast<osc::osc_bundle_element_size_t>(len)));
        auto a = msg.ArgumentsBegin();
        const auto end = msg.ArgumentsEnd();
        auto takeInt = [&](int32_t& out) {
            if (a == end || !a->IsInt32()) throw osc::WrongArgumentTypeException();
            out = (a++)->AsInt32Unchecked();
        };
        takeInt(id); takeInt(kind); takeInt(offset); takeInt(bytes); takeInt(channels); takeInt(frames);
        if (a == end) throw osc::WrongArgumentTypeException();
        if (a->IsFloat())      rate = (a++)->AsFloatUnchecked();
        else if (a->IsInt32()) rate = static_cast<float>((a++)->AsInt32Unchecked());   // a whole number of Hz, as a client's encoder may type it
        else throw osc::WrongArgumentTypeException();
    } catch (const osc::Exception&) {
        return refuse("commit takes id kind offset bytes channels frames rate");
    }
    if (id < 0 || static_cast<uint32_t>(id) == clockwork::Assets::kTombstone) return refuse("bad id");
    if (!st.guest.instance) return refuse("no guest");
    const ClockworkConfig& cfg = st.config;
    const uint8_t* assets = static_cast<const uint8_t*>(cfg.assets);
    uint64_t lane = cfg.assets_bytes;
#ifdef __EMSCRIPTEN__
    // On the web the assets area is the top of the memory and grows with it
    // (js/memory_layout.js): its size is from its base to the end of the
    // memory, measured now, since the figure handed over at boot is the size
    // at boot and a sample pool has grown past it since.
    if (assets) {
        const uintptr_t end = (uintptr_t)emscripten_get_heap_size();
        if (end > (uintptr_t)assets) lane = end - (uintptr_t)assets;
    }
#endif
    if (!assets || lane == 0) return refuse("this engine has no assets");
    if (offset < 0 || bytes <= 0) return refuse("range must be inside the assets area");
    if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > lane)
        return refuse("range runs past the assets area");
    ClockworkAsset asset {};
    asset.struct_bytes = sizeof asset;
    asset.id           = static_cast<uint32_t>(id);
    asset.kind         = static_cast<uint32_t>(kind);
    asset.origin       = origin;
    asset.bytes        = assets + offset;
    asset.byte_count   = static_cast<uint32_t>(bytes);
    switch (kind) {
    case CLOCKWORK_ASSET_RAW:
        break;
    case CLOCKWORK_ASSET_AUDIO_F32:
        if (channels <= 0 || frames <= 0) return refuse("audio needs channels and frames");
        if (static_cast<uint64_t>(frames) * static_cast<uint64_t>(channels) * sizeof(float)
            != static_cast<uint64_t>(bytes))
            return refuse("frames * channels * 4 must equal bytes");
        if (!(rate > 0.f)) return refuse("audio needs a sample rate");
        asset.channels = static_cast<uint32_t>(channels);
        asset.frames   = static_cast<uint32_t>(frames);
        asset.sample_rate = rate;
        break;
    default:
        return refuse("unknown kind");
    }
    if (st.assets.lookup(asset.id)) return refuse("id is still held; release it first");
    Asset* slot = st.assets.insert(asset.id, origin);
    if (!slot) return refuse("too many assets held");
    // All of it, for an instance built after this one (hand_over_assets).
    slot->offset      = static_cast<uint32_t>(offset);
    slot->byte_count  = asset.byte_count;
    slot->channels    = asset.channels;
    slot->frames      = asset.frames;
    slot->sample_rate = asset.sample_rate;
    slot->kind        = static_cast<uint8_t>(asset.kind);
    int rc;
    { GuestCallScope guest(GuestCall::Processing); rc = dsp_asset(st.guest.instance, &asset); }
    if (rc != 0) {
        clockwork::Assets::remove(*slot);
        return refuse("the guest refused it");
    }
    asset_emit(st, origin, CLOCKWORK_SYS("asset/committed"), asset.id, nullptr);
    return true;
}

// The DSP's route: everything the predicate did not claim, handed over
// untouched. One call for a message and a bundle alike — dsp_osc takes "one
// OSC message or bundle" and the DSP decides what that means.
//
// `when` is the message's timetag and `origin` the token to answer on; both
// travel with the call, so nothing here builds a reply object or knows how a
// reply gets back.
bool clockwork_dsp_default_route(void* routeCtx, const void* callCtx,
                          const uint8_t* data, std::size_t len) {
    Dsp* const instance = clockwork::instance_of(static_cast<ClockworkCore*>(routeCtx));
    if (!instance) return true;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    // blockTime goes too: without it the DSP can only difference against the
    // block it last RENDERED, which is one behind the block this message is
    // for.
    GuestCallScope guest(GuestCall::Processing);
    dsp_osc(instance, data, static_cast<uint32_t>(len),
            cc ? cc->when.abi() : clockwork_timetag_immediately(), cc ? cc->sourceId : 0,
            cc ? cc->blockTime.abi() : ClockworkTimetag{0});
    return true;
}
#endif // CLOCKWORK_SYNTH


// ============================================================================
// PORTS ON THE DSP's CHANNELS (clockwork_port_bus.h)
// ============================================================================
// The table is just a table, and a build with no DSP can still be told about a
// binding; only the two apply functions above touch the buses. Attach and
// detach are control-thread calls that publish with one release store, so the
// audio thread never sees a half-built entry and never waits for one.

// ── The channel map (shared_memory.h) ────────────────────────────────────────
//
// Written here because this is where a channel's identity is decided: the
// device's width at init_memory, and a port's range at attach. Control thread
// only — every caller below already holds the port bus's lock or is inside
// init_memory.
namespace {

ClockworkChannelMapState* channel_map_in(uint8_t* base) {
    return base ? reinterpret_cast<ClockworkChannelMapState*>(base + CHANNEL_MAP_START) : nullptr;
}

// The cold half is being rewritten while the generation is odd.
struct ColdWrite {
    ClockworkChannelMapState* m;
    explicit ColdWrite(ClockworkChannelMapState* map) : m(map) {
        if (m) m->generation.fetch_add(1, std::memory_order_acq_rel);   // -> odd
    }
    ~ColdWrite() {
        if (m) m->generation.fetch_add(1, std::memory_order_release);   // -> even
    }
    ColdWrite(const ColdWrite&) = delete;
    ColdWrite& operator=(const ColdWrite&) = delete;
};

// What a channel is when no port covers it: the device's, or nothing.
uint32_t bare_channel_word(uint32_t channel, uint32_t device_width) {
    return channel < device_width ? channelWord(CLOCKWORK_CH_DEVICE, 0)
                                  : channelWord(CLOCKWORK_CH_NONE, 0);
}

void channel_map_set_device(clockwork::CoreState& st, uint32_t in_width, uint32_t out_width) {
    ClockworkChannelMapState* m = channel_map_in(st.arena.at());
    if (!m) return;
    ColdWrite guard(m);
    m->device_in.store(in_width,   std::memory_order_relaxed);
    m->device_out.store(out_width, std::memory_order_relaxed);
    m->max_channels.store(CLOCKWORK_MAX_CHANNELS, std::memory_order_relaxed);
    // Rewrite only the channels no port has claimed, so a device switch does
    // not silently drop a stream that is still bound across it.
    for (uint32_t c = 0; c < CLOCKWORK_MAX_CHANNELS; ++c) {
        if (channelKind(m->in[c].load(std::memory_order_relaxed)) != CLOCKWORK_CH_PORT)
            m->in[c].store(bare_channel_word(c, in_width), std::memory_order_relaxed);
        if (channelKind(m->out[c].load(std::memory_order_relaxed)) != CLOCKWORK_CH_PORT)
            m->out[c].store(bare_channel_word(c, out_width), std::memory_order_relaxed);
    }
}

// A source occupies INPUT channels (it plays into the DSP); a sink occupies
// OUTPUT channels (it takes what the DSP wrote). Same rule pull_port_sources
// and push_port_sinks follow.
std::atomic<uint32_t>* channel_side(ClockworkChannelMapState* m, uint32_t direction) {
    return direction == kClockworkPortSource ? m->in.data() : m->out.data();
}

void channel_map_bind(ClockworkCore* core, ClockworkChannelMapState* m, const ClockworkPorts* ports, ClockworkPort port, uint32_t first) {
    if (!m || port == CLOCKWORK_PORT_NONE) return;
    const uint32_t dir   = static_cast<uint32_t>(clockwork_port_direction(ports, port));
    const uint32_t count = clockwork_port_channels(ports, port);
    const char*    name  = clockwork_port_name(ports, port);

    ColdWrite guard(m);
    uint32_t slot = 0;
    for (uint32_t i = 0; i < CHANNEL_MAP_MAX_STREAMS; ++i) {
        if (m->streams[i].port != 0) continue;
        ClockworkStreamEntry& e = m->streams[i];
        e.port = port; e.direction = dir; e.first = first; e.count = count;
        std::memset(e.name.data(), 0, sizeof e.name);
        if (name) std::strncpy(e.name.data(), name, e.name.size() - 1);
        slot = i + 1;   // stored plus one; 0 means no stream
        break;
    }
    if (slot == 0) {
        // No room to describe it. The audio still flows — the bus table is what
        // moves frames — so this is a reporting limit, said out loud rather
        // than a channel that lies about being unclaimed.
        clockwork_log(core, "[channel-map] more than %u streams; '%s' is bound but not described",
                CHANNEL_MAP_MAX_STREAMS, name ? name : "?");
        return;
    }
    std::atomic<uint32_t>* side = channel_side(m, dir);
    const uint32_t word = channelWord(CLOCKWORK_CH_PORT, slot);
    for (uint32_t c = 0; c < count && first + c < CLOCKWORK_MAX_CHANNELS; ++c)   // NOLINT(modernize-loop-convert): the index is the channel
        side[first + c].store(word, std::memory_order_release);
}

void channel_map_unbind(ClockworkChannelMapState* m, ClockworkPort port) {
    if (!m || port == CLOCKWORK_PORT_NONE) return;
    ColdWrite guard(m);
    for (ClockworkStreamEntry& e : m->streams) {
        if (e.port != port) continue;
        const uint32_t width = e.direction == kClockworkPortSource
                             ? m->device_in.load(std::memory_order_relaxed)
                             : m->device_out.load(std::memory_order_relaxed);
        std::atomic<uint32_t>* side = channel_side(m, e.direction);
        for (uint32_t c = 0; c < e.count && e.first + c < CLOCKWORK_MAX_CHANNELS; ++c)
            side[e.first + c].store(bare_channel_word(e.first + c, width),
                                    std::memory_order_release);
        e.port = 0; e.direction = 0; e.first = 0; e.count = 0;
        std::memset(e.name.data(), 0, sizeof e.name);
        return;
    }
}

} // namespace

extern "C" const ClockworkChannelMapState* clockwork_channel_map(ClockworkCore* core) {
    return channel_map_in(clockwork::arena_base(core));
}

extern "C" int clockwork_port_bus_attach(ClockworkCore* core, ClockworkPort port, uint32_t first_channel) {
    clockwork::CoreState* const st = clockwork::state(core);
    if (!st || port == CLOCKWORK_PORT_NONE || !clockwork_port_is_open(st->ports, port)) return 0;
    clockwork::PortBus& bus = st->port_bus;
    const clockwork::PortBus::Lock lock(bus);   // control side only; the audio thread never waits
    // Already attached: refuse rather than silently move it. Two entries for
    // one port would read it twice a block and the second read would be
    // silence, which is a bug that sounds like a fault in the endpoint.
    for (const auto& b : bus.bindings)
        if (b.port.load(std::memory_order_acquire) == port) return 0;
    for (auto& b : bus.bindings) {
        if (b.port.load(std::memory_order_acquire) != CLOCKWORK_PORT_NONE) continue;
        // Range first, then the MAP, then the port. The store that publishes
        // the entry is still the last one, so the audio thread cannot see a
        // port with a stale channel range; and the map describes the channels
        // BEFORE the binding goes live, so a channel is never carrying audio
        // it cannot account for.
        b.first_channel = first_channel;
        channel_map_bind(core, channel_map_in(clockwork::arena_base(core)), st->ports, port, first_channel);
        b.port.store(port, std::memory_order_release);
        return 1;
    }
    return 0;   // table full
}

extern "C" void clockwork_port_bus_detach(ClockworkCore* core, ClockworkPort port) {
    clockwork::CoreState* const st = clockwork::state(core);
    if (!st || port == CLOCKWORK_PORT_NONE) return;
    const clockwork::PortBus::Lock lock(st->port_bus);
    for (auto& b : st->port_bus.bindings) {
        if (b.port.load(std::memory_order_acquire) != port) continue;
        // Unpublish first, announce second: the DSP is never told a stream is
        // gone while the block that says so is still handing it that stream's
        // audio. first_channel is left alone — the entry is unpublished, and
        // the next attach writes it before publishing again.
        b.port.store(CLOCKWORK_PORT_NONE, std::memory_order_release);
        channel_map_unbind(channel_map_in(clockwork::arena_base(core)), port);
        return;
    }
}

extern "C" void clockwork_port_bus_detach_all(ClockworkCore* core) {
    clockwork::CoreState* const st = clockwork::state(core);
    if (!st) return;
    ClockworkChannelMapState* const m = channel_map_in(clockwork::arena_base(core));
    const clockwork::PortBus::Lock lock(st->port_bus);
    for (auto& b : st->port_bus.bindings) {
        const ClockworkPort p = b.port.exchange(CLOCKWORK_PORT_NONE, std::memory_order_acq_rel);
        if (p != CLOCKWORK_PORT_NONE) channel_map_unbind(m, p);
    }
}

extern "C" uint32_t clockwork_port_bus_attached(ClockworkCore* core) {
    const clockwork::CoreState* const st = clockwork::state(core);
    if (!st) return 0;
    uint32_t n = 0;
    for (const auto& b : st->port_bus.bindings)
        if (b.port.load(std::memory_order_acquire) != CLOCKWORK_PORT_NONE) ++n;
    return n;
}
