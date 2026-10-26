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
std::atomic<int64_t> g_block_osc_time{0};   // this block's start, in OSC time

uint32_t g_reserved_lanes = 0;    // set before boot; read at every init_memory
uint32_t g_lane_base      = 0;    // published after init_memory; 0 = none
uint32_t g_link_audio_lanes     = 0;   // set before boot, as the host's are
uint32_t g_link_audio_lane_base = 0;   // published after init_memory; 0 = none

uint32_t lane_base_for(uint32_t device_in, uint32_t device_out) {
    const uint32_t widest = device_in > device_out ? device_in : device_out;
    uint32_t base = 32;
    if (widest > base) base = (widest + 7u) & ~7u;
    return base;
}
} // namespace

extern "C" {
void clockwork_reserve_lanes(uint32_t lanes) {
    if (lanes > CLOCKWORK_RESERVED_LANES_MAX) lanes = CLOCKWORK_RESERVED_LANES_MAX;
    g_reserved_lanes = lanes;
}
uint32_t clockwork_reserved_lanes(void) { return g_reserved_lanes; }
uint32_t clockwork_lane_base(void)      { return g_lane_base; }
void clockwork_reserve_link_audio_lanes(uint32_t lanes) {
    if (lanes > CLOCKWORK_RESERVED_LANES_MAX) lanes = CLOCKWORK_RESERVED_LANES_MAX;
    g_link_audio_lanes = lanes;
}
uint32_t clockwork_link_audio_lanes(void)     { return g_link_audio_lane_base ? g_link_audio_lanes : 0; }
uint32_t clockwork_link_audio_lane_base(void) { return g_link_audio_lane_base; }
}

#include "clock/clock_math.h"
#include "osc_debug.h"
#include "osc/OscOutboundPacketStream.h"
#include "clock/ClockworkClock.h"
#include "clock/EngineClock.h"
#include "OscIngress.h"
#include "IngressCallCtx.h"
#include "clockwork_sys.h"        // the /clockwork/ prefix rule: clockwork here, DSP everywhere else
#include "ReplyChannel.h"
#include "lanes/lanes_internal.h"   // clockwork_egress_nrt_write — off-thread debug egress
// Platform macros (CLOCKWORK_COLD_BSS, tiered-memory attributes). Header-only and
// free of any DSP dependency, so it is included in both builds — the core still
// places the ring arena + scheduler pool in bulk RAM on tiered targets.
#include "platform.h"
#include "dsp_api.h"  // the contract with the DSP
#include "restart_plan.h"   // the least audio coming back does (come_back)
#include "guest_malloc.h"       // every call into the DSP is marked, so its mallocs are counted as its own
#include "shm_tap_stream.hpp"   // the tap slots DspHost::scope_* hand a DSP
#include "clockwork_ports.h"    // frames crossing the boundary, on a stable slot
#include "clockwork_port_bus.h" // ...and which DSP channels they occupy
#include "clockwork_event_sink.h" // events leaving clockwork, on a stable slot

extern "C" {

    // The offline-render seed, handed over in DspConfig::deterministic_seed
    // (engine_support.cpp). A host sets it before boot; nothing reads it after.
    extern int32_t clockwork_deterministic_seed;
}

// The boundary the audio-thread drain classifies through (extern in clockwork_sys.h).
// Defined here — compiled by both native and wasm. Published by the engine at
// init (native: ClockworkEngine::mSplit; wasm: a file-static, see init_memory).
std::atomic<OscSplit*> g_active_split{nullptr};

// Whether a claimed verb the audio thread does not answer itself is FORWARDED
// TO THE HOST rather than refused (clockwork_host_forward_route, below). A
// host with no NRT thread has two possible ends to clockwork's chain: itself,
// when it has a front that answers the control verbs on a thread of its own
// (the web client's main thread, which owns Web MIDI and the Gamepad API), or
// this audio thread, when it has not. The worklet always has the front, so
// the web build starts forwarding; a bare lanes host does not, and keeps the
// refusal until it says otherwise (clockwork_host_forward).
static std::atomic<bool> g_host_forward{
#ifdef __EMSCRIPTEN__
    true
#else
    false
#endif
};

// The sinks' host emitter (clockwork_event_sink.h): a guest's MIDI send goes
// out over the egress the same way a forwarded verb does — a bundle carrying
// the send's time, its one element "/clockwork/midi/sink/send ,sb <port>
// <bytes>" — for the host's front to hand to the port with that time. Safe
// on the audio thread: a ring write and nothing else. Installed and removed
// with the forward switch, because they are one fact: the host is the far
// end. Defined below with the forward route.
static int clockwork_sink_host_emit(uint32_t kind, const char* target,
                                    const uint8_t* bytes, uint32_t len, int64_t when);
static void forward_to_host(uint32_t token, int64_t when, const uint8_t* element, uint32_t len);

void clockwork_host_forward(int on) {
    g_host_forward.store(on != 0, std::memory_order_release);
    clockwork_sink_set_host_emit(on ? &clockwork_sink_host_emit : nullptr);
}
int clockwork_host_forwards(void) {
    return g_host_forward.load(std::memory_order_acquire) ? 1 : 0;
}

// Published true by a backend that drains the NRT-out ring (the native NRT
// gateway). While true, emit_debug_osc routes OFF-audio-thread debug to the
// locked NRT-out ring instead of the single-writer RT-out ring, keeping RT-out's
// sole-writer invariant. Stays false on single-threaded worklet targets (WASM /
// self-driven device), which have no NRT-out drainer and no second RT-out writer,
// so those always use RT-out. A capability signal, not an __EMSCRIPTEN__ branch.
std::atomic<bool> g_nrt_egress_drained{false};

// See audio_processor.h. Set and cleared by process_audio (BlockOpenScope).
std::atomic<bool> g_block_open{false};
namespace {
struct BlockOpenScope {
    BlockOpenScope()  { g_block_open.store(true,  std::memory_order_relaxed); }
    ~BlockOpenScope() { g_block_open.store(false, std::memory_order_release); }
    BlockOpenScope(const BlockOpenScope&) = delete;
    BlockOpenScope& operator=(const BlockOpenScope&) = delete;
};
} // namespace

// Fallback ClockworkClock for hosts that have no object above the engine to
// own one: the worklet, the self-driven device build, an embedded host and a
// bare lanes host. clock_enter_service installs it as g_active_clockwork_clock
// if the host did not publish a clock before boot. ClockworkEngine publishes
// its own member clock, so the native engine never uses this one.
static ClockworkClock& clockworkClock() {
    static ClockworkClock instance;
    return instance;
}
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
#include "native/LinkAudioHost.h"   // the block's stamp in Link's clock (g_active_link_audio)
#endif
#include "workers/RingBufferWriter.h"   // the in ring's writer lock, which a teardown's mark takes

// Pre-allocated heap for RT-safe allocations
#include "clockwork_pool.h"
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
void clockwork_sched_warm();
#endif

// Thread-local RT guard for allocation detection (read by test binary only)
#include "rt_alloc.h"

// Definition of the slot-array pointer declared in shm_audio_buffer.hpp.
// Assigned once at init; the DSP reads it to locate its slots.
shm_audio_buffer* g_shm_audio_buffers = nullptr;

// Engine sample position of the block being rendered (declared in
// shm_tap_stream.hpp): anchors tap-stream writes on the sample clock.
std::atomic<uint64_t> g_engine_frames{0};

// The tap substrate (rust/clockwork-taps): an opaque context it never
// reads, and a handle of the four-field shape DspTapHandle spells out.
extern "C" {
int  clockwork_tap_get(void* ctx, int index, int channels, int max_frames, void* hnd);
void clockwork_tap_release(void* ctx, void* hnd);
}

#if CLOCKWORK_WORKLET_CLOCK
// Audio-thread /clock handler (the wasm ingress route). Handles the cheap
// clock-core verbs inline and replies via the OUT ring. /clockwork/clock is
// clockwork's own namespace and is always consumed here — it never reaches the
// DSP. (Native registers a different clock route that forwards to the NRT
// thread.)
static bool clockCoreRoute(void* routeCtx, const void* callCtx,
                           const uint8_t* data, size_t len) {
    ClockworkClock* clk = g_active_clockwork_clock.load(std::memory_order_acquire);
    if (!clk) return true;   // clock route owns /clockwork/clock/; drop during startup, don't error
    auto* cc  = static_cast<const DrainCallCtx*>(callCtx);
    auto* chan = static_cast<ReplyChannel*>(routeCtx);   // RT egress, bound at registration
    uint32_t token = cc ? cc->sourceId : 0;              // reply metadata, threaded in
    const bool handled = handleClockCoreOsc(*clk, data, static_cast<uint32_t>(len),
        [chan, token](const uint8_t* d, uint32_t n) {
            if (chan) chan->reply(token, d, n);
        });
    if (!handled && chan) {
        // Not a clock-core verb. Before refusing, offer it to clockwork's own
        // system surface: /clockwork/clock/state/get lives there
        // (clockwork_sys.h), because it answers against the published clock
        // MIRROR rather than the clock object, and this route claimed the
        // whole /clockwork/clock/ namespace ahead of it — so without this
        // hand-off the verb was implemented and unreachable, and every web
        // client asking for the snapshot got "unsupported" instead.
        const auto* clockState = shared_memory
            ? reinterpret_cast<const ClockworkClockState*>(shared_memory + CLOCK_STATE_START)
            : nullptr;
        const auto rt = handle_clockwork_sys_rt(data, static_cast<uint32_t>(len),
            [chan, token](const uint8_t* d, uint32_t n) { chan->reply(token, d, n); },
            clockState, arenaInstanceGeneration(shared_memory));
        if (rt == ClockworkSysRt::Answered) return true;

        // A /clock verb this build doesn't answer (native-only Link-session
        // surface, or a typo): refuse explicitly instead of dropping silently,
        // so clients can tell "unsupported here" from "lost datagram". Pair
        // with /clockwork/clock/capabilities/get for feature detection.
        replyClockUnsupported(data, static_cast<uint32_t>(len),
            [chan, token](const uint8_t* d, uint32_t n) {
                chan->reply(token, d, n);
            });
    }
    return true;
}
#endif  // CLOCKWORK_WORKLET_CLOCK

// The engine namespace. Kept declared in both builds so the `using namespace
// engine;` in the scheduler-bridge functions below resolves either way.
// Defined with the rest of the channel-map writer, down beside the port bus
// that mostly drives it. Declared here because init_memory publishes the
// device's own channels, and that runs first.
namespace { void channel_map_set_device(uint32_t in_width, uint32_t out_width); }

namespace engine {
}

// Forward declare ring buffer write function (defined after namespace).
// Dependencies (sequence counter, status-flags word, metrics) are passed in
// rather than read from engine globals; default-null args here keep callers
// terse and let the function be unit-tested directly. Defaults live on this
// declaration only — never repeated on the definition.
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
    // Static ring buffer in the WASM data segment, separate from the engine heap.
    // ARENA_SIZE is computed in shared_memory.h from the build's sizing flags
    // — deliberately not restated as a number here.
    //
    // 16-byte aligned for shm_audio_buffer slots at SCOPE_START; JS Float64Array
    // needs only 8. FAST RAM, on a tiered device too: the arena's cursors,
    // rings, scope and metrics are touched every block (the table says which,
    // per region: ClockworkArenaEntry::tier, clockwork_arena.h), and a hot
    // region in PSRAM is the measured distortion case. The bulk-tier regions
    // in it — the taps, the window, the guest's persistent store — are kept
    // small by the tiered profiles (memory_profile.h) so the whole span fits
    // beside the heap and the audio task's stack. It used to carry
    // CLOCKWORK_COLD_BSS, which put all of it in PSRAM.
#ifndef __EMSCRIPTEN__
    alignas(16) uint8_t arena_storage[ARENA_SIZE];

    static_assert(ARENA_SIZE <= sizeof(arena_storage),
                  "Buffer layout exceeds allocated storage!");

    uint8_t* clockwork_local_arena() { return arena_storage; }
#else
    // THE ENGINE'S MEMORY, on the web: one block for the life of the process
    // (CLOCKWORK_AREA_ENGINE_MEMORY). The arena is its first ARENA_SIZE
    // bytes; the placement arena — the RT router's block, the guest's
    // working memory, the heap — is the rest. Claimed once,
    // through clockwork_memory_claim, before anything else is allocated, so
    // it is the first thing after the linker's heap base and the same on
    // every boot. A reload instantiates a new module over the same memory:
    // these two survive it (nothing re-initialises the zeroed data), so the
    // new engine finds the block where it was and starts the placement
    // arena over inside it.
    uint8_t* g_engine_block       = nullptr;
    size_t   g_engine_block_bytes = 0;

    uint8_t* clockwork_local_arena() { return g_engine_block; }
#endif

#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
    // THE ENGINE'S MEMORY, NATIVELY: one block the guest's two spans are
    // carved from, as on the web, so a desktop engine's memory is
    // one thing to see and one malloc to make. Unlike the web's block it
    // does not hold the arena: natively the arena is static storage, or a
    // shared-memory segment other processes map, and the guest's memory
    // must not be in a segment any process can reach. Nor the RT router's
    // block, which is the process's for life while this block is an
    // engine's: a build whose guest wants more than the block holds takes a
    // new one, and the router's block stays where it was. Everything carved
    // from the block is given back before it is replaced (the pool and the
    // spans are all per-build), so replacing it frees nothing live. THE
    // BLOCK IS CUT IN TWO BY HAND — the fast span, then the bulk span — and
    // clockwork::mem is not involved: natively it stays the system
    // allocator, as it always was. (A first version installed the block as
    // the placement arena; a test fixture that boots one engine for the
    // whole process then had every later test's "system" allocation come
    // out of the engine's block and freed as the system's, which CI's
    // one-process shuffled run found, 2026-10-15.) The block goes with the
    // engine (teardown_memory). A tiered device has real regions to place
    // into and no block.
    uint8_t* g_engine_block       = nullptr;
    size_t   g_engine_block_bytes = 0;

    // Have a block that holds `bytes`: the one there is when it does, else a
    // new one with everything carved from the old one forgotten first.
    // False when the system has no such block, with no spans, so the boot
    // refuses with the figures.
    static bool engine_block_holding(size_t bytes, uint32_t verbosity);
#endif

    // Clockwork's own audio staging, sized to the compile-time max so any
    // runtime block size up to that cap fits without reallocation. (128 * 128
    // * 4 = 64 KB on web, up to 512 KB on native.) Channel-major: channel c
    // occupies [c * block, (c+1) * block).
    //
    // These are the buffers the DSP is handed. It WRITES the output one —
    // dsp_process fills channel pointers into this array. Clockwork allocates
    // the samples and names the channels, and never learns where the DSP keeps
    // anything.
    alignas(16) float static_audio_bus[clockwork::kMaxBlockSize * clockwork::kMaxChannels];
    alignas(16) float static_audio_in[clockwork::kMaxBlockSize * clockwork::kMaxChannels];

    // IN-ring drain state for the shared walker (ring_drain.h): sequence-gap
    // tracking. Frames are contiguous by wire invariant and parsed in place
    // from the ring — there is no copy buffer. Audio-thread only.
    ClockworkDrainState g_in_drain;

    // Sequence-tracking reset requested off-thread (purge → clear_scheduler
    // runs on a control thread on native); the audio thread applies it before
    // its next drain so g_in_drain stays single-threaded.
    std::atomic<bool> g_in_seq_reset{false};

    // The last generation opened in this process, or handed over by a host
    // whose engine ran in memory now gone (clockwork_carry_generation): a new
    // arena carries it on, so no generation is named twice to a client that
    // outlived the engine before (docs/RESTARTS.md, "The generation").
    // Outlives teardown_memory, as the process does.
    uint32_t g_carried_generation = 0;

    void clockwork_carry_generation(uint32_t generation) {
        if (generation > g_carried_generation) g_carried_generation = generation;
    }

    // THE IN RING'S MARKS (lanes/ring_marks.h): a teardown sets one for the
    // generation closing (destroy_dsp); a purge of what went stale while the
    // audio thread was stalled sets one for the generation it has
    // (clockwork_ingress_mark). The drain refuses each frame for the guest
    // below one to its sender, as the ring it answers on has room
    // (refuse_when_room), and takes everything of clockwork's own as it comes
    // — so whatever replaced the instance in between, what was written for the
    // old one is answered once, in order, and nothing is dropped without a
    // word. Set only where the drain is not running — on the audio thread
    // itself, or with it stopped — so they are the consuming thread's state,
    // handed over by the audio thread's stop and start.
    RingMarks g_in_marks;

    void*  g_guest_memory       = nullptr;
    size_t g_guest_memory_bytes = 0;

    // The guest's arenas when CLOCKWORK reserved them rather than the host: a
    // host that passed no region (headless native, an embedded board) and a
    // guest that declared wants (DspInfo::memory_bytes_wanted) meet here, and
    // on a tiered device the bulk arena is always clockwork's to place, since
    // only clockwork::mem knows which RAM is which. Taken once from
    // clockwork::mem at the first build that needs them, reused across
    // rebuilds exactly as a host-owned span is, and given back at
    // teardown_memory — never in destroy_dsp, which a rebuild goes through.
    void*  g_owned_fast        = nullptr;
    size_t g_owned_fast_bytes  = 0;
    void*  g_owned_bulk        = nullptr;
    size_t g_owned_bulk_bytes  = 0;

    // Optional override for the whole shared-memory arena base. When set
    // (native backend with a public POSIX shm segment), init_memory() points
    // `shared_memory` here instead of the process-local arena_storage, so
    // the entire shared_memory.h blob — rings, control, metrics, node-tree,
    // scope and taps — lives in the segment and is observable
    // cross-process.
    uint8_t* g_external_segment = nullptr;

    uint8_t* shared_memory = nullptr;
    RingCursors* cursors = nullptr;
    PerformanceMetrics* metrics = nullptr;
    static clockwork::metrics_history::Writer g_metrics_history;   // the hour's history of the metrics, the audio thread's to write
    std::atomic<int32_t>* global_offset = nullptr;
    bool memory_initialized = false;

    // Defined beside clockwork_log; see the note there for why boot failures
    // cannot use it.
    static void clockwork_boot_fail(const char* fmt, ...);
    struct Dsp* g_dsp = nullptr;

    // WHY THIS BUILD HAS NO GUEST, for the host to report (clockwork_boot_error).
    // Written on the control thread while the build runs, before the audio
    // thread is let at it, and read by the host after; g_boot_failed is the
    // flag the audio thread reads to know the instance is not coming. The first
    // reason wins: what stopped the build, not what went wrong because of it.
    // A fixed buffer, so recording a failure allocates nothing.
    static std::array<char, 512> g_boot_error{};
    static std::atomic<bool> g_boot_failed{false};

    static void clear_boot_error() {
        g_boot_error[0] = '\0';
        g_boot_failed.store(false, std::memory_order_release);
    }

    static void record_boot_error(const char* fmt, ...) {
        if (g_boot_failed.load(std::memory_order_relaxed)) return;
        va_list args;
        va_start(args, fmt);
        vsnprintf(g_boot_error.data(), g_boot_error.size(), fmt, args);
        va_end(args);
        g_boot_failed.store(true, std::memory_order_release);
    }

    const char* clockwork_last_boot_error() {
        return g_boot_failed.load(std::memory_order_acquire) ? g_boot_error.data() : nullptr;
    }

    // The session's audio geometry. CLOCKWORK CHOOSES THESE and writes them into
    // DspConfig, so it never asks the DSP what block size or channel count it
    // settled on.
    uint32_t g_block_size   = 0;
    uint32_t g_out_channels = 0;
    uint32_t g_in_channels  = 0;
    double   g_dsp_sample_rate = 0.0;

    // The block a build is laid out in, for the block a host asked for. The
    // staging buffers are fixed-size, so the configuration cannot ask for more
    // than they hold, and a bad block size falls back to the default rather
    // than rendering past the end of the arena.
    static uint32_t build_block(uint32_t asked) {
        return (asked == 0 || asked > clockwork::kMaxBlockSize) ? clockwork::kDefaultBlockSize : asked;
    }

    // What the instance in the arena was built from: clockwork's configuration
    // as it stood, and the guest's config block. Audio coming back carries the
    // instance on only in the same (come_back); a change it was told of
    // (clockwork_guest_reconfigure) is what it is built for from then on.
    static ClockworkConfig g_built_config{};
    static std::array<uint8_t, GUEST_CONFIG_SIZE> g_built_guest_config{};

    // Channel pointer arrays handed to dsp_process, built once at init so the
    // audio thread does no arithmetic beyond passing two pointers.
    float*       g_out_ptrs[clockwork::kMaxChannels] = {};
    const float* g_in_ptrs[clockwork::kMaxChannels]  = {};

    // ── Ports bound to DSP channels (clockwork_port_bus.h) ────────────────────────
    //
    // A fixed table, never compacted: entry i keeps its channel range for as
    // long as it is attached, and detaching entry 1 leaves 0 and 2 exactly
    // where they were. That is the same stable-slot rule dsp_api.h states for
    // channels and clockwork_ports.h states for ports, applied to the one thing
    // joining them.
    //
    // Written by a control thread, read by the audio thread, so `port` is
    // atomic and is the publication point: the channel count and first channel
    // are stored BEFORE the port is published and cleared AFTER it is taken
    // away, so the audio thread never sees a half-built entry. Nothing here
    // ever blocks, and the audio thread's read is one relaxed load per entry.
    struct PortBinding {
        std::atomic<ClockworkPort> port{CLOCKWORK_PORT_NONE};
        uint32_t first_channel = 0;
    };
    PortBinding g_port_bus[CLOCKWORK_PORT_BUS_MAX];

    // Serialises the CONTROL side of the routing table, and nothing else: the
    // audio thread never touches it, so it can never be made to wait. It buys
    // one thing the old compare-exchange could not — claiming a table entry,
    // announcing it and publishing it as a single step, so an announcement is
    // never queued for a binding that then loses its slot, and a rebuild's
    // re-announcement can never interleave with an attach and name a port
    // twice. A spin lock rather than a mutex because the critical section is a
    // table scan and a memcpy of at most a few hundred bytes, and because this
    // file compiles for targets where <mutex> is not free. A word, not
    // std::atomic_flag: the flag is a byte, and CLOCKWORK_HAS_BYTE_ATOMICS is 0 on
    // Xtensa (platform.h) — there __atomic_test_and_set is an undefined
    // reference at link, while a 32-bit exchange is native everywhere.
    std::atomic<uint32_t> g_port_ctl{0};
    struct PortCtlLock {
        PortCtlLock()  { while (g_port_ctl.exchange(1, std::memory_order_acquire)) {} }
        ~PortCtlLock() { g_port_ctl.store(0, std::memory_order_release); }
        PortCtlLock(const PortCtlLock&) = delete;
        PortCtlLock& operator=(const PortCtlLock&) = delete;
    };


    // Sources into the input bus. Called immediately before dsp_process, after
    // whatever the device backend wrote there: a bound source OWNS its range
    // for the block.
    static void pull_port_sources(uint32_t frames) {
        for (auto& b : g_port_bus) {
            const ClockworkPort p = b.port.load(std::memory_order_acquire);
            if (p == CLOCKWORK_PORT_NONE) continue;
            if (clockwork_port_direction(p) != kClockworkPortSource) continue;
            const uint32_t first = b.first_channel;
            if (first >= g_in_channels) continue;
            uint32_t n = clockwork_port_channels(p);
            if (n > g_in_channels - first) n = g_in_channels - first;
            if (n == 0) continue;
            std::array<float*, clockwork::kMaxChannels> ch{};
            for (uint32_t c = 0; c < n; ++c)
                ch[c] = static_audio_in + static_cast<size_t>(first + c) * g_block_size;
            // The return value is deliberately ignored: the shortfall is
            // already silence in the buffer and already counted on the port,
            // and there is nothing useful the audio thread could do with it.
            (void)clockwork_port_read(p, ch.data(), n, frames);
        }
    }

    // The DSP's output into sinks. Called immediately after dsp_process, so a
    // sink records what actually left clockwork.
    static void push_port_sinks(uint32_t frames) {
        for (auto& b : g_port_bus) {
            const ClockworkPort p = b.port.load(std::memory_order_acquire);
            if (p == CLOCKWORK_PORT_NONE) continue;
            if (clockwork_port_direction(p) != kClockworkPortSink) continue;
            const uint32_t first = b.first_channel;
            if (first >= g_out_channels) continue;
            uint32_t n = clockwork_port_channels(p);
            if (n > g_out_channels - first) n = g_out_channels - first;
            if (n == 0) continue;
            std::array<const float*, clockwork::kMaxChannels> ch{};
            for (uint32_t c = 0; c < n; ++c)
                ch[c] = static_audio_bus + static_cast<size_t>(first + c) * g_block_size;
            (void)clockwork_port_write(p, ch.data(), n, frames);
        }
    }

    // dsp_describe()->holds_schedule, read ONCE at boot. The whole schedule
    // branch in this file turns on this one bool (see ingest_frame): a DSP
    // that holds its own schedule receives a timed message the moment it
    // arrives, carrying its timetag; one that does not has clockwork hold it
    // until due. Nothing else anywhere consults it.
    bool g_dsp_holds_schedule = false;
    // DspInfo::wants_events, read at the same moment: whether an inbound
    // event (clockwork_event_route) is also the guest's.
    bool g_dsp_wants_events = false;
    // DspInfo::survives, read once at boot: the device changes this guest
    // takes live (lanes.h clockwork_guest_survives).
    uint32_t g_dsp_survives = 0;

    // True while this thread is the one writer of the instance's ring
    // (RT-out): the audio thread inside process_audio(), and the restart
    // thread inside destroy_dsp() and rebuild_dsp(), which run with the audio
    // thread stopped. What an instance says goes out on that ring, in order
    // (emit_osc_route), and so do this thread's log lines (emit_debug_osc);
    // any other thread (watchdog, recovery, boot on native) writes the locked
    // NRT-out ring, so the instance's ring never has a second concurrent writer.
    thread_local bool t_writes_instance_ring = false;
    struct InstanceRingScope {
        InstanceRingScope()  : prev(t_writes_instance_ring) { t_writes_instance_ring = true; }
        ~InstanceRingScope() { t_writes_instance_ring = prev; }
        InstanceRingScope(const InstanceRingScope&) = delete;
        InstanceRingScope& operator=(const InstanceRingScope&) = delete;
        const bool prev;
    };

    // File-scope state shared across threads: written by clear_scheduler()
    // on the control thread, read/updated by process_audio() on the audio
    // thread. Relaxed ordering — these are diagnostic counters with no
    // cross-variable invariants.
    std::atomic<uint32_t> local_in_peak{0};
    std::atomic<uint32_t> local_out_peak{0};
    std::atomic<uint32_t> local_nrt_out_peak{0};
    std::atomic<uint32_t> corruption_count{0};
    std::atomic<uint32_t> gap_log_count{0};
    std::atomic<int> late_count{0};
    // The "say it a few times, then stop" log limiters, and the late log's
    // one-a-second stamp. Here rather than static in the functions that use
    // them so that a new engine can start them over (below).
    std::atomic<uint32_t> no_backend_log_count{0};
    std::atomic<uint32_t> oversize_forward_log_count{0};
    std::atomic<int64_t>  last_late_log_osc{0};
    // Blocks between metrics flushes (~30 Hz), derived from the rate and block
    // size init_memory publishes; 0 until derived. Zeroed at every
    // init_memory, because a rebuild may be at another rate.
    std::atomic<uint32_t> metrics_flush_period{0};

    // A NEW ENGINE STARTS THESE OVER. They are the process's, not an engine's,
    // so a second engine in one process — a test suite, the NIF, an embedder
    // that closes and boots again — used to inherit the first one's: it
    // published the last engine's buffer peaks as its own from its first
    // flush, and a limiter the last engine had used up kept this one's first
    // error out of the log. (A purge clears the peaks too: clear_scheduler.)
    static void reset_engine_diagnostics() {
        local_in_peak.store(0, std::memory_order_relaxed);
        local_out_peak.store(0, std::memory_order_relaxed);
        local_nrt_out_peak.store(0, std::memory_order_relaxed);
        corruption_count.store(0, std::memory_order_relaxed);
        gap_log_count.store(0, std::memory_order_relaxed);
        late_count.store(0, std::memory_order_relaxed);
        no_backend_log_count.store(0, std::memory_order_relaxed);
        oversize_forward_log_count.store(0, std::memory_order_relaxed);
        last_late_log_osc.store(0, std::memory_order_relaxed);
    }

    // Block-time constants, in NTP units (see clock_math.h).
    int64_t g_osc_increment = 0;             // NTP units per buffer
    double g_osc_to_samples = 0.0;           // NTP units -> samples conversion
    double g_time_zero_osc = 0.0;            // AudioContext time -> OSC time offset
    bool g_time_initialized = false;         // Have we set up time conversion?

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
        const size_t bytes = (size_t)ARENA_SIZE + (size_t)placement_bytes;
        void* block = std::malloc(bytes);   // 16-aligned, as the arena needs
        if (!block) return 0;
        std::memset(block, 0, ARENA_SIZE);  // the arena starts zeroed, as static storage did
        g_engine_block       = static_cast<uint8_t*>(block);
        g_engine_block_bytes = bytes;
        return reinterpret_cast<int>(g_engine_block);
    }

    EMSCRIPTEN_KEEPALIVE
    int get_arena_base() {
        if (!g_engine_block && !clockwork_memory_claim((uint32_t)CLOCKWORK_MEM_ARENA_SIZE)) return 0;
        if (arenaHeader(g_engine_block)->magic != CLOCKWORK_ARENA_MAGIC)
            writeArenaHeader(g_engine_block);
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

    // Real-pointer base of the unified shared-memory arena, valid on every
    // runtime (the int-returning get_arena_base() above is WASM-only and
    // would truncate a 64-bit native pointer). Returns the arena base set by
    // init_memory(): arena_storage locally, or the public segment when an
    // external arena was supplied. Used by the tap-stream claim path.
    void* get_shared_memory_base() {
        return shared_memory;
    }

    // Set time offset from JavaScript (AudioContext -> NTP conversion).
    EMSCRIPTEN_KEEPALIVE
    void set_time_offset(double offset) {
        g_time_zero_osc = offset;
        g_time_initialized = true;
        clockwork_log("Time offset set from JavaScript: %.6f", offset);
    }

    inline int64_t ntp_to_osc_timetag(double ntp) {
        return clockwork::ntpToOscTimetag(ntp);
    }

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
    static bool emit_osc_route(uint32_t route, uint32_t origin, const uint8_t* osc, uint32_t len);
    static bool emit_osc_out(uint32_t origin, const uint8_t* osc, uint32_t len) {
        return emit_osc_route(origin ? EGRESS_REPLY : EGRESS_BROADCAST_NOTIFY, origin, osc, len);
    }

    // The same, to a named audience (EgressRoute): what an inbound event
    // takes to reach the clients subscribed to its subsystem.
    static bool emit_osc_route(uint32_t route, uint32_t origin, const uint8_t* osc, uint32_t len) {
        if (!memory_initialized || !cursors || !shared_memory || !osc || len == 0)
            return false;
        const bool useRtOut =
            t_writes_instance_ring ||
            !g_nrt_egress_drained.load(std::memory_order_relaxed);
        if (useRtOut) {
            return ::ring_buffer_write(
                shared_memory + RT_OUT_RING_START, RT_OUT_RING_SIZE,
                &cursors->out_head, &cursors->out_tail, &cursors->out_sequence,
                route, origin, osc, len, &cursors->status_flags, metrics);
        }
        return clockwork_egress_nrt_write(route, origin, osc, len);
    }

    // ── What the DSP may call back into (dsp_api.h, DspHost) ───────────────
    //
    // Five function pointers, defined below in order: emit_osc is the egress above,
    // so a DSP's reply takes the route every other reply takes; log is clockwork_log,
    // so its diagnostics land in the same debug stream; open_sink/send_sink are its
    // own output path; free_bytes gives back what clockwork allocated. A DSP needs no
    // client registry held on its behalf — it answers the origin it was given.
    static int dsp_host_emit_osc(void* /*ctx*/, uint32_t origin,
                           const uint8_t* bytes, uint32_t len) {
        return emit_osc_out(origin, bytes, len) ? 1 : 0;
    }

    static void dsp_host_log(void* /*ctx*/, int /*level*/, const char* text) {
        if (text) clockwork_log("%s", text);
    }

    // ── The DSP's own sinks ────────────────────────────────────────────────
    //
    // dsp_api.h gives a DSP a handle and a send and no close, because a DSP's
    // sinks last exactly as long as the DSP. That promise has to be kept by
    // somebody, and it is kept here: every sink a DSP opens is remembered, and
    // every one is closed when the instance is freed. Without this a device
    // switch — which frees and rebuilds the instance — would leak a slot per
    // sink per switch, and the leak would present as sinks that stop working
    // after a few switches rather than as anything readable.
    //
    // Atomics rather than a lock, and a fixed array rather than a container:
    // opening is a control-thread call by contract, but destroy_dsp can run
    // while one is in flight and clockwork has no business allocating here.
    // memory_profile.h's "<cell bytes>:<cells>,..." per kind, installed with
    // clockwork_sink_profile (sink_profile.h). A string that does not parse
    // installs nothing: the crate's default for the kind stands, and the log
    // says which.
    static void install_sink_profiles() {
        if (!clockwork_install_sink_classes(kClockworkSinkOsc, CLOCKWORK_OSC_SINK_CLASSES))
            clockwork_log("WARNING: OSC sink classes \"%s\" not installed — using the built-in shape",
                          CLOCKWORK_OSC_SINK_CLASSES);
        if (!clockwork_install_sink_classes(kClockworkSinkMidi, CLOCKWORK_MIDI_SINK_CLASSES))
            clockwork_log("WARNING: MIDI sink classes \"%s\" not installed — using the built-in shape",
                          CLOCKWORK_MIDI_SINK_CLASSES);
    }

    constexpr uint32_t kMaxDspSinks = 16;
    std::atomic<ClockworkSink> g_dsp_sinks[kMaxDspSinks];

    static ClockworkSink dsp_host_open_sink(void* /*ctx*/, ClockworkSinkKind kind,
                                      const char* target, uint32_t capacity) {
        const ClockworkSink sink = clockwork_sink_open(kind, target, capacity);
        if (sink == CLOCKWORK_SINK_NONE) return CLOCKWORK_SINK_NONE;
        for (auto& slot : g_dsp_sinks) {
            ClockworkSink expected = CLOCKWORK_SINK_NONE;
            if (slot.compare_exchange_strong(expected, sink,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
                return sink;
            }
        }
        // Nowhere to remember it. Handing it back anyway would be handing back
        // a sink nothing will ever close, so it is closed and refused instead:
        // the DSP sees the same CLOCKWORK_SINK_NONE it must already handle.
        clockwork_sink_close(sink);
        clockwork_log("WARNING: DSP asked for more than %u sinks; refused", kMaxDspSinks);
        return CLOCKWORK_SINK_NONE;
    }

    static int dsp_host_send_sink(void* /*ctx*/, ClockworkSink sink,
                                  const uint8_t* bytes, uint32_t len, int64_t when) {
        return clockwork_sink_send(sink, bytes, len, when);
    }

    void close_dsp_sinks() {
        for (auto& slot : g_dsp_sinks) {
            const ClockworkSink sink = slot.exchange(CLOCKWORK_SINK_NONE, std::memory_order_acq_rel);
            if (sink != CLOCKWORK_SINK_NONE) clockwork_sink_close(sink);
        }
    }

    // Give back bytes taken through alloc_bytes (dsp_api.h). Only the
    // pointer crosses: what the bytes were is the guest's business, and was
    // the whole problem with the OSC verb this replaces.
    static void dsp_host_free_bytes(void* /*ctx*/, void* ptr) {
        // What arrives here came from alloc_bytes, on every target: a guest
        // whose buffer points into the client's assets (an asset, or a sample
        // the client placed there) does not hand that range to us — the
        // assets is the client's and the guest knows so (dsp_api.h, "Bulk").
        // This used to be a no-op on web for fear of exactly that pointer,
        // which meant every block a web guest ever freed was leaked.
        if (ptr) clockwork_pool_free(ptr);
    }

    // The pair of the above: the guest's pool, over its bulk memory
    // (clockwork_pool.h). Lock-free, so this is callable on the audio
    // thread, where it answers NULL rather than blocking when it cannot meet
    // a request (dsp_api.h).
    static void* dsp_host_alloc_bytes(void* /*ctx*/, size_t bytes) {
        return bytes ? clockwork_pool_alloc(bytes) : nullptr;
    }

    // ── Taps (dsp_api.h, "Taps") ───────────────────────────────────────
    //
    // A DSP publishes a tap by claiming a slot and appending blocks; the
    // slot's layout (shm_tap_stream) and the anchor every block is stamped
    // with (g_engine_frames) stay on this side. The handle passes straight
    // through to the substrate, which owns slots by the handle's address.
    static int dsp_host_tap_open(void* /*ctx*/, uint32_t index, uint32_t channels,
                                   DspTapHandle* hnd) {
        if (!hnd) return 0;
        if (index > (uint32_t)INT32_MAX || channels > (uint32_t)INT32_MAX) return 0;
        if (!clockwork_tap_get(nullptr, (int)index, (int)channels, 0, hnd)) return 0;
        auto* slot = static_cast<shm_tap_stream*>(hnd->slot);
        hnd->channels = slot ? slot->channels : 0;
        return 1;
    }

    static void dsp_host_tap_write(void* /*ctx*/, DspTapHandle* hnd,
                                     const float* const* channels, uint32_t n_channels,
                                     uint32_t frames) {
        if (!hnd || !hnd->slot || !channels || n_channels == 0 || frames == 0) return;
        // The writer takes a full set of channel pointers; short by some, the
        // last one given fills the rest, as ScopeOut2 always did.
        std::array<const float*, TAP_STREAM_CHANNELS> padded{};
        for (uint32_t c = 0; c < TAP_STREAM_CHANNELS; ++c)
            padded[c] = channels[c < n_channels ? c : n_channels - 1];
        shm_tap_stream_writer writer(static_cast<shm_tap_stream*>(hnd->slot));
        writer.write(padded.data(), frames, g_engine_frames.load(std::memory_order_relaxed));
    }

    static void dsp_host_tap_close(void* /*ctx*/, DspTapHandle* hnd) {
        if (hnd) clockwork_tap_release(nullptr, hnd);
    }


    // ── Assets (dsp_api.h, "Assets") ───────────────────────────────────────
    //
    // The audio thread's half of the facility: check a committed range against
    // the lane, hand the guest a pointer, and keep the committed asset — who
    // committed it, and what it is — so a release is told to the right client
    // and an instance built again is handed every asset the last one held
    // (hand_over_assets). A fixed table, probed by id — no allocation on this
    // thread, and a full table is a refusal.
    struct AssetSlot {
        uint32_t id;
        uint32_t origin;
        uint32_t offset;        // into the assets area
        uint32_t byte_count;
        uint32_t channels;
        uint32_t frames;
        float    sample_rate;
        uint8_t  kind;
        bool     used;
    };
    constexpr uint32_t kAssetSlots = CLOCKWORK_ASSET_SLOTS;
    static_assert(kAssetSlots > 0 && (kAssetSlots & (kAssetSlots - 1)) == 0,
                  "CLOCKWORK_ASSET_SLOTS is a power of two");
    AssetSlot g_assets[kAssetSlots] = {};
    // An asset released leaves its slot `used`, under an id no client can
    // commit, so the probe chains through it stay intact; the next insert on
    // its chain takes it.
    constexpr uint32_t kAssetTombstone = 0xFFFFFFFFu;

    AssetSlot* asset_lookup(uint32_t id) {
        for (uint32_t i = 0, h = id * 2654435761u; i < kAssetSlots; ++i, ++h) {
            AssetSlot& s = g_assets[h & (kAssetSlots - 1)];
            if (s.used && s.id == id) return &s;
            if (!s.used) return nullptr;
        }
        return nullptr;
    }
    // A slot for an id asset_lookup did not find: the first on its chain that
    // is free, or a released asset's.
    AssetSlot* asset_insert(uint32_t id, uint32_t origin) {
        for (uint32_t i = 0, h = id * 2654435761u; i < kAssetSlots; ++i, ++h) {
            AssetSlot& s = g_assets[h & (kAssetSlots - 1)];
            if (!s.used || s.id == kAssetTombstone) {
                s = AssetSlot{};
                s.id = id; s.origin = origin; s.used = true;
                return &s;
            }
        }
        return nullptr;
    }
    void asset_remove(AssetSlot* s) { s->id = kAssetTombstone; s->origin = 0; }

    void asset_emit(uint32_t origin, const char* address, uint32_t id, const char* reason) {
        char buf[192];
        osc::OutboundPacketStream ps(buf, sizeof buf);
        ps << osc::BeginMessage(address) << static_cast<int32_t>(id);
        if (reason) ps << reason;
        ps << osc::EndMessage;
        emit_osc_out(origin, reinterpret_cast<const uint8_t*>(ps.Data()),
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
    enum class Owed : uint8_t { Nothing, Setup, Changed };
    static std::atomic<Owed>     g_owed{Owed::Nothing};
    static std::atomic<int32_t>  g_setup_rate{0};            // the rate, as setup names it
    static std::atomic<uint32_t> g_setup_buffer_frames{0};   // the device's callback, as setup names it

    // "/clockwork/setup ,iii <rate> <device buffer> <generation>", or the same
    // as /clockwork/setup/changed, on the instance's ring: whichever is owed.
    // True once said, or when nothing is.
    static bool say_owed() {
        const Owed owed = g_owed.load(std::memory_order_acquire);
        if (owed == Owed::Nothing) return true;
        char buf[96];
        osc::OutboundPacketStream ps(buf, sizeof buf);
        ps << osc::BeginMessage(owed == Owed::Setup ? CLOCKWORK_SYS("setup") : CLOCKWORK_SYS("setup/changed"))
           << g_setup_rate.load(std::memory_order_relaxed)
           << static_cast<int32_t>(g_setup_buffer_frames.load(std::memory_order_relaxed))
           << static_cast<int32_t>(arenaInstanceGeneration(shared_memory))
           << osc::EndMessage;
        if (!emit_osc_route(EGRESS_BROADCAST_NOTIFY, 0, reinterpret_cast<const uint8_t*>(ps.Data()),
                            static_cast<uint32_t>(ps.Size())))
            return false;
        g_owed.store(Owed::Nothing, std::memory_order_relaxed);
        return true;
    }

    // The device's callback as setup names it: for a host with no device, the block.
    static uint32_t setup_buffer_frames(uint32_t buffer_frames) {
        return buffer_frames ? buffer_frames : static_cast<uint32_t>(get_audio_buffer_samples());
    }

    // The new instance announces itself: its setup, naming the device's
    // callback (or, for a host with no device yet, the block), written on its
    // ring as the build's last word, or owed until the ring has room.
    static void announce_instance(uint32_t buffer_frames) {
        InstanceRingScope instance_ring;
        g_setup_rate.store(static_cast<int32_t>(g_dsp_sample_rate), std::memory_order_relaxed);
        g_setup_buffer_frames.store(setup_buffer_frames(buffer_frames), std::memory_order_relaxed);
        g_owed.store(Owed::Setup, std::memory_order_release);
        say_owed();
    }

    void clockwork_announce_facts(double sample_rate, uint32_t buffer_frames) {
        if (!memory_initialized || !g_dsp) return;
        InstanceRingScope instance_ring;
        g_setup_rate.store(static_cast<int32_t>(sample_rate), std::memory_order_relaxed);
        g_setup_buffer_frames.store(setup_buffer_frames(buffer_frames), std::memory_order_relaxed);
        // A setup still owed says the new facts itself.
        Owed nothing = Owed::Nothing;
        g_owed.compare_exchange_strong(nothing, Owed::Changed, std::memory_order_acq_rel);
        say_owed();
    }

    // DspHost::asset_release — the guest is done; the committing client hears.
    static int dsp_host_asset_release(void* /*ctx*/, uint32_t id) {
        AssetSlot* s = asset_lookup(id);
        if (!s) return 0;
        const uint32_t origin = s->origin;
        asset_remove(s);
        asset_emit(origin, CLOCKWORK_SYS("asset/released"), id, nullptr);
        return 1;
    }

    // Every asset released: each committer told, and the table forgotten.
    // When the engine goes (teardown_memory), and when audio comes back to an
    // assets area that has moved, where no committed asset points at
    // anything any more.
    void assets_release_all() {
        for (AssetSlot& s : g_assets) {
            if (s.used && s.id != kAssetTombstone)
                asset_emit(s.origin, CLOCKWORK_SYS("asset/released"), s.id, nullptr);
            s = AssetSlot{};
        }
    }

    // A new instance is handed every asset the last one held, as it is built
    // and before it says setup (docs/RESTARTS.md): the bytes are the client's,
    // in the assets area a rebuild keeps, so nothing a client lent is lost to
    // a rebuild and nothing has to be sent again. An asset the new instance
    // will not take is released there, and its committer hears so, as from a
    // guest that let go.
    static void hand_over_assets() {
        const auto* assets = static_cast<const uint8_t*>(g_clockwork_config.assets);
        for (AssetSlot& s : g_assets) {
            if (!s.used || s.id == kAssetTombstone) continue;
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
            if (assets) { GuestCallScope guest(GuestCall::Processing); rc = dsp_asset(g_dsp, &asset); }
            if (rc == 0) continue;
            const uint32_t origin = s.origin, id = s.id;
            asset_remove(&s);
            asset_emit(origin, CLOCKWORK_SYS("asset/released"), id, nullptr);
        }
    }

    // DspHost::flush_schedule — the bundles held for the guest (SCHED_TAG_SYNTH),
    // dropped. Not the default tag: that is clockwork's own schedule verb,
    // and not the guest's to clear.
    static void dsp_host_flush_schedule(void* /*ctx*/) {
#if CLOCKWORK_SCHEDULER
        clockwork_engine_schedule().flush(SCHED_TAG_SYNTH);
#endif
    }

    const DspHost g_dsp_host = { /*ctx*/ nullptr, &dsp_host_emit_osc, &dsp_host_log,
                                 &dsp_host_open_sink, &dsp_host_send_sink,
                                 &dsp_host_free_bytes, &dsp_host_asset_release,
                                 &dsp_host_alloc_bytes,
                                 &dsp_host_tap_open, &dsp_host_tap_write,
                                 &dsp_host_tap_close, &dsp_host_flush_schedule };

#if CLOCKWORK_WORKLET_CLOCK && CLOCKWORK_SYNTH
    // The RT egress as a generic ReplyChannel: emit one OSC to the OUT ring,
    // routed by the per-call token. Lets the worklet audio-thread control routes
    // (the /clockwork/clock namespace) reply through the backend contract without
    // any DSP-specific reply type. Shared by every worklet host (WASM + the
    // self-driven device).
    void rt_reply_emit(void*, uint32_t token, const uint8_t* osc, uint32_t len) {
        emit_osc_out(token, osc, len);
    }
    ReplyChannel g_rt_reply{ &rt_reply_emit, nullptr };
#endif

    // The metrics that are constant for a session: which Clockwork, the rate
    // and block it runs at, and how much its scheduler can hold — events and
    // bytes, since it is full when it runs out of either.
    static void publish_session_metrics(double sample_rate, int buf_length) {
        metrics->clockwork_commit.store(CLOCKWORK_COMMIT_WORD, std::memory_order_relaxed);   // which Clockwork (clockwork_config.h)
        metrics->audio_sample_rate.store(static_cast<uint32_t>(std::lround(sample_rate)), std::memory_order_relaxed);
        metrics->audio_block_size.store(static_cast<uint32_t>(buf_length), std::memory_order_relaxed);
#if CLOCKWORK_SCHEDULER
        metrics->scheduler_capacity.store(SCHEDULER_SLOT_COUNT, std::memory_order_relaxed);
        metrics->scheduler_pool_bytes.store(SCHEDULER_DATA_POOL_SIZE, std::memory_order_relaxed);
#else
        metrics->scheduler_capacity.store(0, std::memory_order_relaxed);
        metrics->scheduler_pool_bytes.store(0, std::memory_order_relaxed);
#endif
    }

    static inline void update_scheduler_depth_metric(uint32_t depth) {
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

    static inline void increment_scheduler_drop_metric() {
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
    static inline void refuse_schedule_verb(const uint8_t* data, uint32_t len, uint32_t origin) {
        increment_scheduler_drop_metric();
        clockwork_sys_refuse(data, len, CLOCKWORK_SCHEDULER_ABSENT, arenaInstanceGeneration(shared_memory),
            [origin](const uint8_t* d, uint32_t n) { emit_osc_out(origin, d, n); });
        static std::atomic<uint32_t> refusals{0};
        const uint32_t n = refusals.fetch_add(1, std::memory_order_relaxed);
        if (n == 0 || n % 100 == 0)
            clockwork_log("ERROR: " CLOCKWORK_SYS("schedule") " refused — this build has no "
                   "timed store (built with CLOCKWORK_SCHEDULER=OFF; that "
                   "also removes " CLOCKWORK_SYS("sched/flush") ", timed MIDI out and "
                   "timed OSC forwarding). Immediate messages are unaffected, "
                   "and a timestamped bundle still reaches the DSP with its "
                   "timetag. [%u so far]", n + 1);
    }
#endif

    // The latest a timed event may play, set by the host (lanes.h): 0 allows
    // any lateness. A host whose engine can be flooded sets one, because a
    // backlog played minutes late is worse than a backlog not played (a
    // mashed bench once snowballed to 4.5 s behind); clockwork itself never
    // drops for lateness.
    static std::atomic<uint32_t> g_timed_event_late_limit_ms{0};
    EMSCRIPTEN_KEEPALIVE void clockwork_set_timed_event_late_limit_ms(uint32_t ms) {
        g_timed_event_late_limit_ms.store(ms, std::memory_order_relaxed);
    }
    static inline void note_late_dropped(double late_ms, uint32_t limit_ms) {
        if (metrics) metrics->scheduler_late_dropped.fetch_add(1, std::memory_order_relaxed);
        static std::atomic<uint32_t> saidLog{0};
        if (saidLog.fetch_add(1, std::memory_order_relaxed) % 64 == 0)
            clockwork_log("[schedule] a timed event %.0f ms late was dropped: the host allows %u ms", late_ms, limit_ms);
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
    static void mark_ingress(const char* reason, bool onlyDue) {
        if (!memory_initialized || !cursors) return;
        g_in_marks.set(static_cast<uint32_t>(cursors->in_sequence.load(std::memory_order_acquire)),
                       arenaInstanceGeneration(shared_memory), reason, onlyDue);
    }
    void clockwork_ingress_mark(const char* reason) {
        mark_ingress(reason ? reason : CLOCKWORK_REFUSED_REBUILDING, false);
    }

    // What is pending as of now waited while the audio was away — a stall, a
    // page's context suspended — and what is due of it is stale: refused,
    // each to its sender ("stalled"), by the next drain (the marks, above). A
    // bundle for later is not, and goes through to play at its time. On the
    // audio thread, or with it stopped.
    void clockwork_ingress_mark_stale() {
        mark_ingress(CLOCKWORK_REFUSED_STALLED, true);
    }

    // A bundle whose time is not yet come at `blockTime`, this block's start
    // in OSC time: it plays on time whatever it waited through. Timetags are
    // compared unsigned (ingest_frame: a present-day one has its top bit set).
    static bool bundle_for_later(const uint8_t* payload, uint32_t payload_size, int64_t blockTime) {
        if (!clockwork_is_bundle(payload, payload_size)) return false;
        const uint64_t tt = clockwork_bundle_timetag(payload);
        return tt > 1u && tt >= static_cast<uint64_t>(blockTime);
    }

    // THE AUDIO WAS AWAY: the one rule, on every target. The gap is the time
    // that passed since the last block less the audio rendered in it — by the
    // host's monotonic clock where this host and the last both had one (a
    // system clock set by hand is not a gap), by its wall clock otherwise —
    // against the record of the last block rendered in the arena, whichever
    // host rendered it (a worklet made again over the same memory, an
    // embedded host attaching again, the device thread back from a stall).
    // Measured against the audio rendered, a huge device buffer rendering a
    // block every 170 ms is not a gap. Over kClockStepSeconds the clock
    // steps at this block: the host is told so and anchors it afresh (a
    // clock converged from seconds behind runs fast until it has caught up,
    // and everything scheduled against it plays compressed). Nothing that
    // waited is dropped for it: what queued is delivered and what was
    // scheduled fires, late by the gap, as scsynth has always done after a
    // sleep — Clockwork drops a message only for want of room, and counts
    // it (PerformanceMetrics); a host that wants what waited gone says so
    // itself (clockwork_ingress_mark_stale, the page's purge()). One line
    // says the gap. process_audio calls audio_gap once per block, for every
    // host, before the clock moves. clockwork_audio_starts also calls it when
    // a host's audio starts, with audio_secs = -1: the gap is then judged by
    // time alone, and the first block after it is never a gap because the
    // host's frame count starts over.
    static double g_last_audio_secs = -1.0;   // the host's count at the last block; < 0: its audio just started

    static bool audio_gap(double wall_ntp, double monotonic_secs, double audio_secs) {
        if (!memory_initialized || !shared_memory) return false;
        uint8_t* const sample_clock = shared_memory + SAMPLE_CLOCK_START;
        const double last_wall = read_last_block_wall(sample_clock);
        const double last_mono = read_last_block_mono(sample_clock);
        const double last_audio = g_last_audio_secs;
        write_last_block_time(sample_clock, wall_ntp, monotonic_secs);
        g_last_audio_secs = audio_secs;
        const bool byMono = monotonic_secs > 0.0 && last_mono > 0.0;
        const double passed = byMono ? monotonic_secs - last_mono : (last_wall > 0.0 ? wall_ntp - last_wall : 0.0);
        if (audio_secs >= 0.0 && last_audio < 0.0) return false;   // the first block after a start
        const double rendered = (audio_secs >= 0.0 && audio_secs > last_audio) ? audio_secs - last_audio : 0.0;
        const double gap = passed - rendered;
        if (gap <= clockwork::kClockStepSeconds) return false;
        clockwork_log("[clock] audio resumed after %.3fs", gap);
        return true;
    }

    EMSCRIPTEN_KEEPALIVE bool clockwork_audio_back(double wall_ntp, double monotonic_secs, double audio_secs) {
        return audio_gap(wall_ntp, monotonic_secs, audio_secs);
    }
    // clockwork_audio_starts: the host's audio is starting (a device opened
    // with its sample count at 0, a new worklet over the same memory whose
    // AudioContext time may have restarted, an embedded host attaching). The
    // next block re-anchors the clock, never below the sample clock already
    // in the arena.
    static bool g_anchor_at_first_block = false;
    EMSCRIPTEN_KEEPALIVE bool clockwork_audio_starts(double wall_ntp, double monotonic_secs) {
        g_anchor_at_first_block = true;
        return audio_gap(wall_ntp, monotonic_secs, -1.0);
    }

    // The teardown's mark, exact: taken under the in ring's writer lock once
    // the generation word has gone odd. Every door asks its question under
    // the same lock (in_ring_door.h), so a message whose writer saw the
    // instance up is below the mark, and refused as the closing generation's
    // by the next drain; one written after reads the word odd and is refused
    // at the door. Called with the audio thread stopped.
    static void mark_closing_generation() {
        if (!memory_initialized || !cursors) return;
        RingBufferWriter::lock(&cursors->in_write_lock);
        clockwork_ingress_mark(CLOCKWORK_REFUSED_REBUILDING);
        RingBufferWriter::unlock(&cursors->in_write_lock);
    }

    EMSCRIPTEN_KEEPALIVE
    void clear_scheduler() {
        g_in_seq_reset.store(true, std::memory_order_relaxed);
        local_in_peak.store(0, std::memory_order_relaxed);
        local_out_peak.store(0, std::memory_order_relaxed);
        local_nrt_out_peak.store(0, std::memory_order_relaxed);
        corruption_count.store(0, std::memory_order_relaxed);
        gap_log_count.store(0, std::memory_order_relaxed);
        late_count.store(0, std::memory_order_relaxed);
        update_scheduler_depth_metric(0);
#if CLOCKWORK_SCHEDULER
        // Cross-thread handshake: the control thread asks, the audio thread
        // clears at a safe point (process_audio's drainPendingClear below).
        clockwork_engine_schedule().requestClear();
#endif
    }

    // ── The two engine entry points, shared by every host (WASM/native/embedded).
    // Once OSC bytes leave the IN ring, everything funnels through these. ─────────

    // Route an OSC message to its handler NOW (the DSP, or a clockwork route).
    // `when` is the message's intended OSC timetag (0 = immediate) and
    // `blockTime` this block's start in OSC time. Both are passed through to
    // the DSP, which is the only side that can place an event within a block
    // (dsp_api.h); every clockwork route ignores them. `token` is the
    // sender/origin, threaded through for reply routing. Threaded explicitly
    // rather than held in a global, so a handler's inputs are all in the ctx.
    void dispatch(const uint8_t* osc, uint32_t len, uint32_t token,
                  int64_t when, int64_t blockTime) {
        const DrainCallCtx cc{ token, when, blockTime };
        OscSplit* boundary = g_active_split.load(std::memory_order_acquire);
        if (boundary && boundary->ingest(osc, len, &cc)) return;
        // Nothing claimed it: no matching route and no default registered (e.g.
        // a DSP-bound message in a build with no DSP attached). Drop, and log rate-limited so junk
        // can't flood the audio-thread log.
        if (no_backend_log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
            uint32_t a = 0; while (a < len && osc[a] != '\0') ++a;
            clockwork_log("ERROR: no backend for OSC %.*s — dropped",
                   static_cast<int>(a), reinterpret_cast<const char*>(osc));
        }
    }

    // Lateness accounting for an event the schedule fired, and the overload law
    // applied to it. Placing an event WITHIN a block is the DSP's arithmetic —
    // it has the block's start time and each message's own (dsp_api.h). How late
    // the event was is clockwork's, and goes into the metrics.
    //
    // Returns true when the event is dropped as later than the host allows, and must not be delivered.
    //
    // The immediacy check is `== 0 || == 1`, NOT `<= 1`: a present-day OSC
    // timetag is huge and, taken as int64, has its sign bit set — i.e. it is
    // NEGATIVE. `<= 1` would misclassify every real scheduled event as
    // immediate and skip its accounting entirely.
    static bool note_lateness(int64_t when, int64_t blockTime) {
        if (when == 0 || when == 1) return false;   // OSC "immediate" sentinels

        const double time_diff_ms =
            (static_cast<double>(when - blockTime) / 4294967296.0) * 1000.0;

        // Later than the host allows (clockwork_set_timed_event_late_limit_ms):
        // dropped, as the host asked; 0 allows any lateness.
        const uint32_t limit_ms = g_timed_event_late_limit_ms.load(std::memory_order_relaxed);
        if (limit_ms > 0 && time_diff_ms < -static_cast<double>(limit_ms)) {
            note_late_dropped(-time_diff_ms, limit_ms);
            return true;
        }

        if (!metrics || !(g_dsp_sample_rate > 0.0) || g_block_size == 0)
            return false;

        // Only messages older than a full quantum are genuinely late:
        // sub-quantum arrivals simply do not align with quantum boundaries and
        // are placed at the right sub-block offset by the DSP.
        const double quantum_ms =
            (1000.0 * static_cast<double>(g_block_size)) / g_dsp_sample_rate;
        if (time_diff_ms >= -quantum_ms) return false;

        const double raw_late_ms = -time_diff_ms;
        const int32_t late_ms = (raw_late_ms > 10000.0) ? 10000
                                                        : static_cast<int32_t>(raw_late_ms);
        const int late_now = late_count.fetch_add(1, std::memory_order_relaxed) + 1;
        metrics->scheduler_lates.fetch_add(1, std::memory_order_relaxed);
        int32_t current_max = metrics->scheduler_max_late_ms.load(std::memory_order_relaxed);
        while (late_ms > current_max) {
            if (metrics->scheduler_max_late_ms.compare_exchange_weak(
                    current_max, late_ms, std::memory_order_relaxed, std::memory_order_relaxed))
                break;
        }
        metrics->scheduler_last_late_ms.store(late_ms, std::memory_order_relaxed);
        metrics->scheduler_last_late_tick.store(
            metrics->process_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        // Count-based sampling alone hides lates 2..99, so a burst can leave no
        // log trace. Keep the count milestones but also emit at most one line
        // per second of block time.
        const bool second_elapsed =
            (blockTime - last_late_log_osc.load(std::memory_order_relaxed))
                >= static_cast<int64_t>(4294967296LL);
        if (late_now == 1 || late_now % 100 == 0 || second_elapsed) {
            last_late_log_osc.store(blockTime, std::memory_order_relaxed);
            clockwork_log("LATE: %.1fms (count=%d)", -time_diff_ms, late_now);
        }
        return false;
    }

    // ── Classification: the one place the schedule branch is decided ───────
    //
    // One frame off the IN ring. Two wire forms carry a time (schedule_parse.h):
    // a timestamped "#bundle", and "/clockwork/schedule <timetag> <blob>". Everything
    // else is immediate.
    //
    // The branch on g_dsp_holds_schedule lives HERE and nowhere else. A DSP
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
    static void ingest_frame(const uint8_t* data, uint32_t len, uint32_t origin,
                             int64_t blockTime) {
        if (!data || len == 0) return;

        const uint8_t* body    = data;
        uint32_t       bodyLen = len;
        int64_t        when    = 1;                    // OSC "immediately"
#if CLOCKWORK_SCHEDULER
        uint32_t       tag     = SCHED_TAG_SYNTH;      // a DSP-bound bundle
        bool           clockworkHolds = !g_dsp_holds_schedule;
#endif

        if (clockwork_is_bundle(data, len)) {
            // UNSIGNED comparison, and it matters: a present-day OSC timetag has its top bit
            // set, so as an int64 it is NEGATIVE and a signed `tt > 1` is false for every
            // real time — classifying every timestamped bundle as immediate, so clockwork
            // never held one at all.
            const uint64_t tt = clockwork_bundle_timetag(data);
            if (tt > 1u) when = static_cast<int64_t>(tt);
        } else if (len >= CLOCKWORK_SYS_PADDED("schedule") &&
                   std::memcmp(data, CLOCKWORK_SYS("schedule"), CLOCKWORK_SYS_LEN("schedule")) == 0 &&
                   data[CLOCKWORK_SYS_LEN("schedule")] == '\0') {
#if CLOCKWORK_SCHEDULER
            const SchedulePacket sp = clockwork_parse_schedule(data, len);
            if (!sp.ok) {                       // malformed: counted, and its sender told
                increment_scheduler_drop_metric();
                clockwork_sys_refuse(data, len, CLOCKWORK_REFUSED_MALFORMED, arenaInstanceGeneration(shared_memory),
                    [origin](const uint8_t* d, uint32_t n) { emit_osc_out(origin, d, n); });
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
            refuse_schedule_verb(data, len, origin);
            return;
#endif
        }

#if CLOCKWORK_SCHEDULER
        // `== 0 || == 1`, NOT `<= 1`, for the reason note_lateness spells out:
        // a real timetag is negative as an int64, so `<= 1` sent every
        // scheduled message straight through instead of parking it.
        if (when == 0 || when == 1 || !clockworkHolds) {
            // Immediate, or a DSP that holds its own schedule: straight
            // through, carrying whatever time it was given.
            dispatch(body, bodyLen, origin, when, blockTime);
            return;
        }
        if (!clockwork_engine_schedule().add(when, tag, EngineMeta{origin}, body, bodyLen)) {
            increment_scheduler_drop_metric();   // pool full / oversize
            static std::atomic<uint32_t> addFail{0};
            if (addFail.fetch_add(1, std::memory_order_relaxed) % 32 == 0)
                // Rate-limited, because an overloaded queue refuses in bursts.
                // The numbers are here because "add refused" alone cannot tell
                // a full pool from a queue that was never built.
                clockwork_log("SCHED REFUSED (%s): when=%lld len=%u slots=%d pool=%d next=%lld",
                        clockwork_engine_schedule().ready() ? "queue full" : "NO QUEUE — it never built",
                        (long long)when, (unsigned)bodyLen,
                        (int)SCHEDULER_SLOT_COUNT, (int)SCHEDULER_DATA_POOL_SIZE,
                        (long long)clockwork_engine_schedule().nextTime());
        }
#else
        // With no store, holds_schedule is moot: nothing to hold with and nothing to
        // hold it in, so every message goes straight through CARRYING ITS ORIGINAL
        // TIMETAG — including for a DSP that asked us to hold it. Flattening `when` to
        // "now" here would destroy the one piece of information it needs to cope.
        dispatch(body, bodyLen, origin, when, blockTime);
#endif
    }

    // Initialize memory pointers. The arena is the public POSIX segment when
    // the native backend supplied one (g_external_segment), else the in-band
    // arena_storage (WASM, and headless native with no shm). Either way
    // every region is addressed by its shared_memory.h offset from this base.
    ClockworkConfig g_clockwork_config;

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

    void clockwork_set_engine_config(double sample_rate, uint32_t block_size,
                                uint32_t input_channels, uint32_t output_channels,
                                uint32_t verbosity, void* guest_memory,
                                uint32_t guest_memory_bytes,
                                uint32_t arena_bytes,
                                const void* assets, uint32_t assets_bytes,
                                void* returns, uint32_t returns_bytes) {
        g_clockwork_config.sample_rate        = sample_rate;
        g_clockwork_config.block_size         = block_size;
        g_clockwork_config.input_channels     = input_channels;
        g_clockwork_config.output_channels    = output_channels;
        g_clockwork_config.verbosity          = verbosity;
        g_clockwork_config.guest_memory        = guest_memory;
        g_clockwork_config.guest_memory_bytes  = guest_memory_bytes;
        g_clockwork_config.assets              = assets;
        g_clockwork_config.assets_bytes        = assets_bytes;
        g_clockwork_config.returns             = returns;
        g_clockwork_config.returns_bytes       = returns_bytes;
        g_clockwork_config.arena_bytes        = arena_bytes;
    }

#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
    static bool engine_block_holding(size_t bytes, uint32_t verbosity) {
        if (g_engine_block && g_engine_block_bytes == bytes) return true;
        if (g_engine_block) {
            // A build that wants another size: everything carved from the
            // block is a build's (the pool went with the last instance; the
            // spans are cut again below), so forget it all and let it go.
            clockwork_pool_abandon();
            g_owned_fast = nullptr; g_owned_fast_bytes = 0;
            g_owned_bulk = nullptr; g_owned_bulk_bytes = 0;
            std::free(g_engine_block);
            g_engine_block = nullptr; g_engine_block_bytes = 0;
        }
        void* block = nullptr;
        if (bytes != SIZE_MAX && bytes != 0) {
            // 16-aligned, as the spans need: what malloc gives on every
            // desktop ABI, and checked rather than assumed.
            block = std::malloc(bytes);
            if (block && (reinterpret_cast<uintptr_t>(block) & 15u)) { std::free(block); block = nullptr; }
        }
        if (!block) return false;
        g_engine_block       = static_cast<uint8_t*>(block);
        g_engine_block_bytes = bytes;
        if (verbosity > 0)
            clockwork_log("ENGINE MEMORY: 0x%016llx + %u MB", (unsigned long long)(uintptr_t)g_engine_block,
                          (unsigned)(g_engine_block_bytes / (1024 * 1024)));
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
    static void reset_words(uint32_t start, uint32_t bytes) {
        auto* w = reinterpret_cast<std::atomic<uint32_t>*>(shared_memory + start);
        for (uint32_t i = 0; i < bytes / 4u; ++i) w[i].store(0, std::memory_order_relaxed);
    }
    static void reset_metrics()      { reset_words(METRICS_START, METRICS_SIZE); }
    static void reset_metrics_history() { reset_words(METRICS_HISTORY_START, METRICS_HISTORY_SIZE); }
    static void reset_native_stats() { reset_words(NATIVE_STATS_START, NATIVE_STATS_SIZE); }
    // The host's: NTP start is write-once from the host once its audio runs,
    // drift and the global offset are what it measured about this machine's
    // clocks. A new arena, a new measurement.
    static void reset_clock_anchors() { reset_words(CLOCK_ANCHORS_START, CLOCK_ANCHORS_SIZE); }
    // Not zeros: a tempo of zero makes a guest asking for the beat divide by
    // it (readClockworkClock answers a NULL with a sane default, an all-zero
    // region with that). The defaults are the session's start.
    static void reset_clock_state() {
        ClockworkClockState::initDefaults(
            *reinterpret_cast<ClockworkClockState*>(shared_memory + CLOCK_STATE_START));
    }
    // Rewritten whole every block by the tick (publishSampleClock), from the
    // device it is rebound to; empty between the build and the first block.
    static void reset_sample_clock() { reset_words(SAMPLE_CLOCK_START, SAMPLE_CLOCK_SIZE); }
    // Empty: no device words, no streams, its seqlock even. The device's own
    // words are written when the instance is built (channel_map_set_device),
    // and a stream's when it attaches.
    static void reset_channel_map()  { reset_words(CHANNEL_MAP_START, CHANNEL_MAP_SIZE); }
    // The scope: out and in, formatted at the device's live channel count
    // (up to the slot's ceiling) and written by the tick from then on. A tap
    // for a direction the device has not got is formatted with no channels
    // and left disabled, so a reader sees "nothing arrives" rather than a
    // ring of zeros. A device switch is a new stream, and a reader's cursor
    // starts again with it.
    static void reset_scope() {
        shm_audio_buffer* slots = reinterpret_cast<shm_audio_buffer*>(shared_memory + SCOPE_START);
        memset(shared_memory + SCOPE_START, 0, SCOPE_SIZE);
        const auto format_tap = [&](uint32_t slot, uint32_t device_channels) {
            const uint32_t ch = device_channels < SCOPE_CHANNELS ? device_channels : SCOPE_CHANNELS;
            if (ch > 0)
                shm_audio_buffer_writer(&slots[slot]).activate(
                    ch, static_cast<uint32_t>(g_clockwork_config.sample_rate), SCOPE_FRAMES);
        };
        format_tap(SCOPE_OUT_SLOT, g_clockwork_config.output_channels);
        format_tap(SCOPE_IN_SLOT,  g_clockwork_config.input_channels);
        g_shm_audio_buffers = slots;
    }
    // Tap streams: the global header (geometry for cross-process observers)
    // then the slot array, every slot free; the producer claims and
    // activates them on demand (rust/clockwork-taps).
    static void reset_taps() {
        uint8_t* tapBase = shared_memory + TAPS_START;
        memset(tapBase, 0, TAPS_SIZE);
        reinterpret_cast<std::atomic<uint32_t>*>(tapBase + 0)->store(TAPS_SLOTS, std::memory_order_relaxed);       // maxTaps
        reinterpret_cast<std::atomic<uint32_t>*>(tapBase + 4)->store(0, std::memory_order_relaxed);               // activeCount
        reinterpret_cast<std::atomic<uint32_t>*>(tapBase + 8)->store(TAP_RING_FRAMES, std::memory_order_relaxed); // ringFrames
        reinterpret_cast<std::atomic<uint32_t>*>(tapBase + 12)->store(0, std::memory_order_relaxed);              // version
    }
    // The status flags: a fresh word with every build.
    static void reset_status_flags() {
        cursors->status_flags.store(STATUS_OK, std::memory_order_release);
    }
    // The in ring: a fresh epoch at a new arena only, serialised against its
    // producers (clockwork_lanes_reset_in_ring). A rebuild keeps it: what
    // waited for the old instance is the next drain's to refuse, below the
    // mark the teardown set (destroy_dsp), so its cursors, its sequence, the
    // drain's place in it and the mark go on.
    static void reset_in_ring() {
        clockwork_lanes_reset_in_ring();
        g_in_drain.lastSeq = -1;
        g_in_marks.clear();
    }
    // The reply rings outlive an instance: what one said stays for its
    // readers, and the next one's setup follows it (docs/RESTARTS.md). A new
    // arena begins them afresh.
    static void reset_rt_out_ring()  { clockwork_lanes_reset_rt_out_ring(); }
    static void reset_nrt_out_ring() { clockwork_lanes_reset_nrt_out_ring(); }
    // The ids are the clients', shared between them: a new arena starts them.
    static void reset_node_id_counter() { reset_words(NODE_ID_COUNTER_START, NODE_ID_COUNTER_SIZE); }
    // The guest's publish window: zeroed, and nothing more. What shape the
    // bytes take is the guest's, so it lays the region out itself at
    // dsp_new — clockwork cannot pre-fill a structure it does not know, and
    // a wipe that assumed one would be a guess about its empty value.
    static void reset_guest_window() { memset(shared_memory + GUEST_WINDOW_START, 0, GUEST_WINDOW_SIZE); }
    // The host's, copied in by clockwork_init BEFORE the build (the build
    // sizes the guest's pool from it), onto an arena that started empty. A
    // host that hands a config over an old one on a segment it kept is
    // making that copy itself. So the cold boot's reset here is the empty
    // arena's, and this does nothing more.
    static void reset_guest_config() {}
    // Nobody but the guest can draw it, and the guest cannot tell "restored
    // after a device switch" from "a previous engine in this process left
    // its table behind": an engine that never asked for /notify once
    // inherited a registration and started answering a client that had not
    // spoken. Clockwork owns the arena's lifetime, so clockwork decides.
    static void reset_guest_persist() { memset(shared_memory + GUEST_PERSIST_START, 0, GUEST_PERSIST_SIZE); }

    struct RegionReset { uint32_t id; void (*reset)(); };
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
    };
    // Every region the enum names, once. The ids are 1..N and contiguous
    // (clockwork_arena.h: "stable numbers, in the order they lie"), so a
    // region added to the enum without a reset here does not compile, and
    // one listed twice does not either. The test holds the TABLE to this
    // list; this holds the list to the enum.
    static constexpr uint32_t kRegionLast = CLOCKWORK_ARENA_CLOCKWORK_METRICS_HISTORY;
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
    static void reset_regions(bool cold_boot) {
        const auto* h = arenaHeader(shared_memory);
        for (uint32_t i = 0; i < h->entry_count; ++i) {
            const auto& e = h->entries[i];
            if (e.lifetime == CLOCKWORK_LIFETIME_ARENA) continue;
            if (e.lifetime == CLOCKWORK_LIFETIME_COLD_BOOT && !cold_boot) continue;
            const RegionReset* r = region_reset(e.id);
            if (!r || !r->reset) {
                clockwork_log("BUG: region %u is in the arena table with no reset — not reset", (unsigned)e.id);
                continue;
            }
            r->reset();
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
    static void widen_for_lanes(uint32_t& max_input_channels, uint32_t& max_output_channels) {
        g_lane_base = 0;
        g_link_audio_lane_base = 0;
        const uint32_t lanes = g_reserved_lanes + g_link_audio_lanes;
        if (lanes == 0) return;
        const uint32_t first = lane_base_for(max_input_channels, max_output_channels);
        const uint32_t top   = first + lanes;
        if (top <= clockwork::kMaxChannels) {
            g_lane_base = first;
            if (g_link_audio_lanes > 0) g_link_audio_lane_base = first + g_reserved_lanes;
            if (max_output_channels < top) max_output_channels = top;
            if (max_input_channels  < top) max_input_channels  = top;
        } else {
            clockwork_log("WARNING: %u reserved lanes above channel %u do not fit "
                    "kMaxChannels=%d; no lanes reserved",
                    lanes, first, (int)clockwork::kMaxChannels);
        }
    }

    // ── What the guest instance survives (lanes.h) ──────────────────────────
    extern "C" uint32_t clockwork_guest_survives(void) {
        return g_dsp_survives;
    }

    extern "C" int clockwork_guest_reconfigure(double sample_rate, uint32_t input_channels,
                                               uint32_t output_channels) {
        if (!g_dsp || !(sample_rate > 0.0)) return 0;
        // The ceilings the instance is handed, as dsp_new hands them: the
        // device's counts, clamped, widened for the lanes above the device.
        uint32_t cin  = input_channels  > clockwork::kMaxChannels ? clockwork::kMaxChannels : input_channels;
        uint32_t cout = output_channels > clockwork::kMaxChannels ? clockwork::kMaxChannels : output_channels;
        widen_for_lanes(cin, cout);
        {
            GuestCallScope guest(GuestCall::Boot);
            if (dsp_reconfigure(g_dsp, sample_rate, cin, cout) != 0) return 0;
        }
        // The instance took it: the device's facts are these from now on,
        // and clockwork's own regions that follow the device are laid out
        // again for it — the scope at the new channel count, the sample
        // clock for the new rate, the channel map's device words. Nothing of
        // the rings, the metrics or the guest's regions moves: the instance
        // is the same one, and every client's picture of it still holds.
        g_clockwork_config.sample_rate     = sample_rate;
        g_clockwork_config.input_channels  = input_channels;
        g_clockwork_config.output_channels = output_channels;
        g_built_config.sample_rate     = sample_rate;
        g_built_config.input_channels  = input_channels;
        g_built_config.output_channels = output_channels;
        g_dsp_sample_rate = sample_rate;
        g_in_channels     = cin;
        g_out_channels    = cout;
        metrics_flush_period.store(0, std::memory_order_relaxed);
        reset_scope();
        reset_sample_clock();
        channel_map_set_device(g_in_channels, g_out_channels);
        return 1;
    }

    // ── The clock enters service ────────────────────────────────────────
    // One order on every target, after the layout and the reset: whoever
    // owns the clock published it to g_active_clockwork_clock before boot —
    // the engine its member; a worklet host nothing, so the processor's own
    // instance stands in — and it is bound here to the clock state (its
    // transport stamp anchored to wall time as it binds, bindStateToShm),
    // to the sample clock, and to the host's global offset. Every build: a rebuild binds the same instance to
    // the same regions again, which is a no-op. Before this step was one
    // step, native bound after boot and the worklet inside it, and a reset
    // added between the two wiped the web's stamp to the 1900 epoch
    // (2026-10-16).
    static void clock_enter_service(bool cold_boot) {
        ClockworkClock* none = nullptr;
        g_active_clockwork_clock.compare_exchange_strong(none, &clockworkClock(), std::memory_order_acq_rel);
        // Every boot re-anchors the clock at the first block that follows,
        // whichever host drives it. A cold boot also resets the clock
        // (unanchor), as it resets the arena; a rebuild keeps the clock's
        // last time as a floor so time never goes backwards.
        g_anchor_at_first_block = true;
        if (cold_boot)
            if (ClockworkClock* c = g_active_clockwork_clock.load(std::memory_order_acquire)) c->unanchorAudioThread();
        ClockworkClock* clock = g_active_clockwork_clock.load(std::memory_order_acquire);
        if (!clock) return;   // a host with no clock at all
        clock->bindStateToShm(reinterpret_cast<ClockworkClockState*>(shared_memory + CLOCK_STATE_START));
        // The sample clock, published by process_audio once per block: how threads without the audio context
        // (a host's worker, OscChannel#now) read the engine's time.
        clock->bindSampleClockToShm(shared_memory + SAMPLE_CLOCK_START);
        clock->bindGlobalOffset(global_offset);
    }

    void init_memory() {
        const double sample_rate = g_clockwork_config.sample_rate;
        // A build starts with no reason not to have a guest; the first thing
        // that stops it below records one (clockwork_boot_fail, or dsp_new's).
        clear_boot_error();
        // The arena base is fixed for the engine's whole threaded life: native
        // binds g_external_segment before any engine thread starts (and nulls
        // it only after they are all joined); the WASM arena is the static
        // arena_storage. So on a cold-swap rebuild the pointer family
        // below is already correct — and skipping the re-assignment is what
        // makes this call safe while unquiesced producers (transport ingress,
        // clockwork_log from any worker) read the same globals lock-free: pointers
        // are only ever written here with no reader threads alive (first
        // boot, or re-boot after teardown_memory).
#ifdef __EMSCRIPTEN__
        // A host that boots before reading the arena's base (the test harness
        // does; the worklet claims first) gets the engine's memory here, sized
        // to what it asked clockwork_init for.
        if (!g_engine_block && !clockwork_memory_claim(g_clockwork_config.arena_bytes ? g_clockwork_config.arena_bytes
                                                                                      : (uint32_t)CLOCKWORK_MEM_ARENA_SIZE)) {
            clockwork_boot_fail("FATAL: the engine's memory could not be claimed (the arena and %u MB)",
                                (unsigned)((g_clockwork_config.arena_bytes ? g_clockwork_config.arena_bytes : (uint32_t)CLOCKWORK_MEM_ARENA_SIZE) / (1024 * 1024)));
            return;
        }
#endif
        uint8_t* base = g_external_segment ? g_external_segment : clockwork_local_arena();
        // Every build of the engine, a cold swap included: the rate or the
        // block size may have changed, and the period is derived from both.
        metrics_flush_period.store(0, std::memory_order_relaxed);
        // A new arena — the first boot, or one after teardown_memory — is a
        // cold boot: every region is reset, the ones of lifetime cold boot
        // included (reset_regions below). A rebuild on the arena that is up
        // takes the skip path and resets only the regions of lifetime
        // rebuild.
        const bool cold_boot = shared_memory != base;
#if CLOCKWORK_SYNTH
        // A new arena that carries a generation on replaces a running engine
        // (docs/RESTARTS.md, "Three events"): its instance announces itself.
        const bool replacing = cold_boot && g_carried_generation > 0;
#endif
        if (cold_boot) {
            shared_memory = base;
            // A new engine, not a rebuild of the one that was running.
            reset_engine_diagnostics();
            // The table every reader takes the layout from, first, so nothing
            // is ever placed in an arena that does not describe itself. A
            // segment's creator wrote the same bytes already; the wasm arena
            // and an embedder's memory get theirs here.
            writeArenaHeader(shared_memory);
            // The generation goes on from the last this process opened: the
            // word as the last arena's stood between instances.
            if (g_carried_generation > 0)
                arenaGenerationWord(shared_memory).store(2u * g_carried_generation + 1u,
                                                         std::memory_order_release);
            cursors = reinterpret_cast<RingCursors*>(shared_memory + RING_CURSORS_START);
            // Metrics live in the arena at their fixed offset — which is the
            // public segment when one was supplied, so external observers read
            // them with no redirect.
            metrics = reinterpret_cast<PerformanceMetrics*>(shared_memory + METRICS_START);
            g_metrics_history.bind(shared_memory + METRICS_HISTORY_START);

            global_offset = reinterpret_cast<std::atomic<int32_t>*>(shared_memory + GLOBAL_OFFSET_START);

            // What a cold boot resets beyond the pointers — the guest's
            // persistent region, the clock's defaults, the host's anchors —
            // is the table's say, in reset_regions, not this branch's.
        }

        // Every region, at this event, as the table says (reset_regions):
        // the transport area's fresh epoch, the metrics, the scope, the taps,
        // the guest's window on every build; the clock's defaults, the
        // host's anchors, the guest's persistent region on a cold boot.
        // Before anything below writes into a region.
        reset_regions(cold_boot);
        clock_enter_service(cold_boot);

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
        static OscSplit      default_split;
        static ClockworkSysRoutes default_audio_routes;
        if (!default_split.wired()) {
#if CLOCKWORK_WORKLET_CLOCK
            default_audio_routes.add("clock/", &clockCoreRoute, &g_rt_reply);
#endif
            // The asset hand-off is audio-thread work on every host: the
            // guest is only ever reached from there.
            default_audio_routes.add("asset/", &clockwork_asset_route, nullptr);
            default_audio_routes.add("sched/flush", &clockwork_sched_flush_route, nullptr);
            // Inbound events likewise: to their audience, and to the guest.
            default_audio_routes.add("midi/in/",         &clockwork_event_route, nullptr);
            default_audio_routes.add("midi/ports",       &clockwork_event_route, nullptr);
            default_audio_routes.add("gamepad/in/",      &clockwork_event_route, nullptr);
            default_audio_routes.add("gamepad/devices",  &clockwork_event_route, nullptr);
            // The VERBS that live under those names are not events: carved
            // back out to the host's end of the chain (longest match wins).
            default_audio_routes.add("midi/in/enable",        &clockwork_host_forward_route, nullptr);
            default_audio_routes.add("midi/ports/list",       &clockwork_host_forward_route, nullptr);
            default_audio_routes.add("midi/ports/get",        &clockwork_host_forward_route, nullptr);
            default_audio_routes.add("gamepad/devices/list",  &clockwork_host_forward_route, nullptr);
            default_audio_routes.add("gamepad/devices/get",   &clockwork_host_forward_route, nullptr);
            // No NRT thread here. The end of clockwork's chain is either this
            // thread — answer the liveness verbs, refuse anything else under
            // the prefix — or the host's front, which the audio thread
            // forwards to (g_host_forward, above). The route decides per call.
            default_audio_routes.setFallback(&clockwork_host_forward_route, nullptr);
            default_split.setSys(&ClockworkSysRoutes::route, &default_audio_routes);
            default_split.setDsp(&clockwork_dsp_default_route, nullptr);
            // A host that is the far end from the start (the web) is the
            // sinks' MIDI endpoint from the start too.
            if (g_host_forward.load(std::memory_order_acquire))
                clockwork_sink_set_host_emit(&clockwork_sink_host_emit);
        }
        {
            OscSplit* expected = nullptr;
            g_active_split.compare_exchange_strong(
                expected, &default_split, std::memory_order_release);
        }
#endif
        // Event sinks: the size classes each kind is made of, from the memory
        // profile. Installed before any sink can be opened, so the DSP's own
        // opens get the shape too.
        install_sink_profiles();

        // Enable clockwork_log. Write-once per mapping (like the pointer family
        // above): on a rebuild this is already true and producers read it
        // concurrently, so don't store over it.
        if (!memory_initialized)
            memory_initialized = true;
#ifndef __EMSCRIPTEN__
        // The ring exists: whatever the boot logged before now goes onto it
        // first, so the channel carries the boot in order.
        clockwork_log_release();
#endif

#if CLOCKWORK_SCOPE_SECONDS != 1
        // Test-profile arena: capture rings are oversized, so every offset
        // after them differs from a production reader's expectations. Loud
        // banner so a test-sized engine staged as production is obvious.
        clockwork_log("[memory] WARNING: test memory profile "
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

        const uint32_t verbosity = g_clockwork_config.verbosity;

        DspConfig config = {};
        config.sample_rate         = sample_rate;   // the device's rate wins, always
        config.block_size          = g_clockwork_config.block_size;
        config.max_input_channels  = g_clockwork_config.input_channels;
        config.max_output_channels = g_clockwork_config.output_channels;

        // One clock, read by everybody through readClockworkClock(). The arena-resident
        // ClockworkClockState — the same bytes JavaScript polls and the same address
        // ClockworkClock::bindStateToShm binds on native — so the guest reads the session
        // tempo directly and nothing pushes it at one.
        config.clock = reinterpret_cast<const ClockworkClockState*>(
            shared_memory + CLOCK_STATE_START);

        // Which channel is which, from the same arena and by the same rule as
        // the clock: the guest reads it directly, and nothing pushes it.
        config.channel_map = reinterpret_cast<const ClockworkChannelMapState*>(
            shared_memory + CHANNEL_MAP_START);

        // The guest's own configuration: whatever the host put there, handed
        // over without a glance. See DspConfig::guest_config.
        config.guest_config       = shared_memory + GUEST_CONFIG_START;
        config.guest_config_bytes = GUEST_CONFIG_SIZE;
        // What this instance is built from, for audio coming back to it
        // (come_back): it is carried on only in the same.
        g_built_config = g_clockwork_config;
        std::memcpy(g_built_guest_config.data(), shared_memory + GUEST_CONFIG_START, GUEST_CONFIG_SIZE);

        // The window the DSP may publish through: the node-tree mirror region.
        // Clockwork zeroes it at boot, hands it over, and thereafter only
        // publishes the bytes — which is exactly how a node-tree mirror
        // survives in a harness that does not know what a node tree is.
        config.shm_window       = shared_memory + GUEST_WINDOW_START;
        config.shm_window_bytes = GUEST_WINDOW_SIZE;
        // Deliberately NOT zeroed here, and that is the entire feature: this
        // region is how a guest survives its own destruction, so a rebuild must
        // find exactly what the last instance left. See DspConfig::persistent.
        config.persistent       = shared_memory + GUEST_PERSIST_START;
        config.persistent_bytes = GUEST_PERSIST_SIZE;

        config.deterministic_seed = clockwork_deterministic_seed;

        config.block_size = build_block(config.block_size);
        if (config.max_output_channels > clockwork::kMaxChannels)
            config.max_output_channels = clockwork::kMaxChannels;
        if (config.max_input_channels > clockwork::kMaxChannels)
            config.max_input_channels = clockwork::kMaxChannels;

        // Reserved lanes, above the device: widened for on every build, and
        // again when a surviving instance is told a new channel count
        // (widen_for_lanes).
        widen_for_lanes(config.max_input_channels, config.max_output_channels);
        // ── The guest's memory region ──────────────────────────────────────
        //
        // Reserved by the host, claimed by nothing else, and zeroed here so a
        // rebuilt guest starts from a clean block rather than the last one's
        // leavings. Publishing it through the boundary is the whole point:
        // clockwork already has to know where it starts to prove its own heap
        // does not reach into it, so leaving the guest to rediscover the same
        // number was two answers to one question.
        //
        // ABSOLUTE, not relative to shared_memory. On web the address is a
        // position in the wasm heap chosen by the JS memory layout
        // (wasmHeapSize + ringBufferReserved), and the client maps it that
        // way. Adding it to shared_memory — arena_storage, a static
        // array at whatever address the linker picked — put the region exactly
        // arenaBase bytes too high, overrunning into the buffer pool
        // above it. That only shows when the static layout moves, because the
        // size of the overrun IS arena_storage's address.
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
            const size_t want = g_clockwork_config.arena_bytes
                              ? (size_t)g_clockwork_config.arena_bytes
                              : (size_t)CLOCKWORK_MEM_ARENA_SIZE;
            const size_t have = g_engine_block_bytes > ARENA_SIZE ? g_engine_block_bytes - ARENA_SIZE : 0;
            if (!g_engine_block || have < want) {
                clockwork_boot_fail("FATAL: the engine's memory holds %u MB after the arena and the layout asks "
                              "for %u MB - claim it with memArenaSize before the arena's base is read "
                              "(clockwork_memory_claim)",
                              (unsigned)(have / (1024 * 1024)), (unsigned)(want / (1024 * 1024)));
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR,
                                               std::memory_order_relaxed);
                return;
            }
            void* arena = g_engine_block + ARENA_SIZE;
            clockwork::mem::set_arena(arena, have);
            // The RT router's block first, so a later start-over finds it at
            // the arena's start again (above).
            clockwork_router_reserve();
            if (verbosity > 0)
                clockwork_log("MEM ARENA: 0x%08x + %u MB",
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
             * torn down. The first reload still fit; the second asked for the
             * pool, found half of it, and the engine came up with no DSP,
             * processing silence with no error anywhere a page could see.
             *
             * Nothing of that engine is alive — its thread is gone, and the
             * product re-sends what it held (restoreClientState) — so the arena
             * starts over. Everything holding a pointer into it forgets it
             * first: the guest's pool (started over by clockwork_pool_boot
             * below) and the dead instance.
             */
            clockwork_pool_abandon();
            g_dsp = nullptr;
            // The guest's spans are in the arena too: forgotten, so they are
            // carved again below in the same order as a first boot. Keeping
            // them while the heap was rebuilt from the arena's start put the
            // heap's backing over the guest's memory (found by the reload
            // test the day the guest's tables moved into that memory).
            g_owned_fast = nullptr; g_owned_fast_bytes = 0;
            g_owned_bulk = nullptr; g_owned_bulk_bytes = 0;
            clockwork::mem::reset_arena();
            // The RT router's block is for the life of the process and lives
            // in this arena: it was the arena's first allocation, so taking it
            // again first lands it where it was. Anything else is a boot that
            // cannot be trusted and stops.
            if (!clockwork_router_reclaim()) {
                clockwork_boot_fail("FATAL: the RT router's block did not come back where it was after the arena started over");
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
            if (verbosity > 0)
                clockwork_log("MEM ARENA: started over for a new engine (%u MB)",
                        (unsigned)(clockwork::mem::arena_size() / (1024 * 1024)));
        }
        // The engine's memory, in the table: the one block, the arena at its
        // start and the placement arena after it. Placed on every boot, a
        // start-over included: it is the same block.
        arenaSetArea(shared_memory, CLOCKWORK_AREA_ENGINE_MEMORY, CLOCKWORK_SPACE_PROCESS,
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
            const uint64_t configured = dsp_memory_bytes(shared_memory + GUEST_CONFIG_START, GUEST_CONFIG_SIZE);
            if (configured > fast_wanted) fast_wanted = configured > UINT32_MAX ? UINT32_MAX : (uint32_t)configured;
            const uint64_t configured_bulk = dsp_memory_bulk_bytes(shared_memory + GUEST_CONFIG_START, GUEST_CONFIG_SIZE);
            if (configured_bulk > bulk_wanted) bulk_wanted = configured_bulk > UINT32_MAX ? UINT32_MAX : (uint32_t)configured_bulk;
        }
#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
        // The engine's memory, natively (g_engine_block above): sized to
        // exactly what this build cuts from it — the fast span, then the
        // bulk span at its figure — and replaced by a build that wants
        // another size. The router's block is taken first, from the system,
        // so it is not in the block (it outlives every block). A host that
        // placed the fast span itself keeps it; the block then holds the
        // bulk span alone.
        {
            clockwork_router_reserve();
            const bool host_fast = g_clockwork_config.guest_memory && g_clockwork_config.guest_memory_bytes;
            const size_t bulk_figure = g_clockwork_config.guest_bulk_bytes ? g_clockwork_config.guest_bulk_bytes
                                                                           : (size_t)CLOCKWORK_GUEST_MEMORY_BULK_BYTES;
            const size_t bulk_span = bulk_figure > bulk_wanted ? bulk_figure : (size_t)bulk_wanted;
            size_t fast_span = host_fast ? 0 : (size_t)fast_wanted;
            if (!host_fast && (size_t)CLOCKWORK_GUEST_MEMORY_FAST_BYTES > fast_span)
                fast_span = (size_t)CLOCKWORK_GUEST_MEMORY_FAST_BYTES;
            const size_t fast_cut = (fast_span + 15u) & ~(size_t)15u;   // the bulk span starts 16-aligned
            const size_t need = (fast_cut > SIZE_MAX - bulk_span) ? SIZE_MAX : fast_cut + bulk_span;
            if (!engine_block_holding(need, verbosity)) {
                clockwork_boot_fail("FATAL: the system could not give the engine %zu bytes for the guest's memory "
                                    "(%zu fast + %zu bulk) - lower the guest's wants or the profile's "
                                    "CLOCKWORK_GUEST_MEMORY_BULK_BYTES",
                                    need, fast_span, bulk_span);
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
            // The cut. The spans are the block's, never clockwork::mem's:
            // nothing below frees them one by one, the block goes whole.
            g_owned_fast       = fast_span ? g_engine_block : nullptr;
            g_owned_fast_bytes = fast_span;
            g_owned_bulk       = bulk_span ? g_engine_block + fast_cut : nullptr;
            g_owned_bulk_bytes = bulk_span;
            arenaSetArea(shared_memory, CLOCKWORK_AREA_ENGINE_MEMORY, CLOCKWORK_SPACE_PROCESS,
                         (uintptr_t)g_engine_block, g_engine_block_bytes);
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

        g_guest_memory       = nullptr;
        g_guest_memory_bytes = 0;
        if (g_clockwork_config.guest_memory && g_clockwork_config.guest_memory_bytes) {
            g_guest_memory       = g_clockwork_config.guest_memory;
            g_guest_memory_bytes = g_clockwork_config.guest_memory_bytes;
        } else {
#if defined(__EMSCRIPTEN__) || CLOCKWORK_HAS_TIERED_MEMORY
            size_t need = fast_wanted;
            const size_t floor = tier_default(clockwork::mem::Tier::Fast, (size_t)CLOCKWORK_GUEST_MEMORY_FAST_BYTES);
            if (floor > need) need = floor;
            take(clockwork::mem::Tier::Fast, g_owned_fast, g_owned_fast_bytes, need);
#endif
            g_guest_memory       = g_owned_fast;
            g_guest_memory_bytes = g_owned_fast_bytes;
        }

#if defined(__EMSCRIPTEN__) || CLOCKWORK_HAS_TIERED_MEMORY
        {
            size_t need = bulk_wanted;
            size_t figure = g_clockwork_config.guest_bulk_bytes ? g_clockwork_config.guest_bulk_bytes
                                                                : (size_t)CLOCKWORK_GUEST_MEMORY_BULK_BYTES;
            const size_t floor = tier_default(clockwork::mem::Tier::Bulk, figure);
            if (floor > need) need = floor;
            take(clockwork::mem::Tier::Bulk, g_owned_bulk, g_owned_bulk_bytes, need);
        }
#endif
        void*  bulk_arena       = g_owned_bulk;
        size_t bulk_arena_bytes = g_owned_bulk_bytes;

        {
            uint32_t short_fast = 0, short_bulk = 0;
            if (!clockwork_memory_meets((uint32_t)g_guest_memory_bytes, (uint32_t)bulk_arena_bytes,
                                       /*each want against its own span*/ 1, fast_wanted, bulk_wanted,
                                       &short_fast, &short_bulk)) {
                // Refused, and loudly: the guest said what it needs and this
                // host cannot provide it in the speed it named. Booting anyway
                // would either fail inside dsp_new with a worse message or, if
                // the guest fell back to spilling, glitch at the first chord
                // instead of stopping at boot.
                clockwork_boot_fail("FATAL: guest wants %u bytes fast + %u bytes bulk; "
                        "this host offers %u fast + %u bulk — short by %u fast, %u bulk. "
                        "Give clockwork room for it (the web layout's memArenaSize; a device's "
                        "RAM; a desktop's address space) or lower the guest's wants.",
                        fast_wanted, bulk_wanted,
                        (unsigned)g_guest_memory_bytes, (unsigned)bulk_arena_bytes,
                        short_fast, short_bulk);
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
        }

        // Zeroed at every dsp_new, and untouched by clockwork after that
        // (the bulk span is then the pool's, which the guest reaches through
        // the allocator): a rebuilt guest starts from nothing in each.
        if (g_guest_memory) {
            memset(g_guest_memory, 0, g_guest_memory_bytes);
            if (verbosity > 0)
                clockwork_log("GUEST MEMORY: 0x%08x + %u KB fast",
                        (unsigned)(uintptr_t)g_guest_memory,
                        (unsigned)(g_guest_memory_bytes / 1024));
        }
        if (bulk_arena) {
            memset(bulk_arena, 0, bulk_arena_bytes);
            if (verbosity > 0)
                clockwork_log("GUEST MEMORY: 0x%08x + %u KB bulk",
                        (unsigned)(uintptr_t)bulk_arena,
                        (unsigned)(bulk_arena_bytes / 1024));
        }
        config.memory            = g_guest_memory;
        config.memory_bytes      = (uint32_t)g_guest_memory_bytes;
        config.memory_bulk       = bulk_arena;
        config.memory_bulk_bytes = (uint32_t)bulk_arena_bytes;
        // Where the guest's two spans are, into the table: for a map to
        // draw. No client reads them.
        arenaSetArea(shared_memory, CLOCKWORK_AREA_GUEST_MEMORY_FAST,
                     g_guest_memory ? CLOCKWORK_SPACE_PROCESS : CLOCKWORK_SPACE_UNKNOWN,
                     (uintptr_t)g_guest_memory, g_guest_memory_bytes);
        arenaSetArea(shared_memory, CLOCKWORK_AREA_GUEST_MEMORY_BULK,
                     bulk_arena ? CLOCKWORK_SPACE_PROCESS : CLOCKWORK_SPACE_UNKNOWN,
                     (uintptr_t)bulk_arena, bulk_arena_bytes);

        // The floating-point environment, as the host declared it
        // (clockwork_declare_fp_env). A worklet cannot arm a flush, so on web
        // the answer is known without asking and the declaration is ignored.
#ifdef __EMSCRIPTEN__
        config.fp_env = DSP_FP_ENV_DENORMALS_HONOURED;
#else
        config.fp_env = (int32_t)g_clockwork_config.fp_env;
#endif
        // Bulk staging is NOT zeroed: the client owns the assets area and may have
        // written into it before the guest was built.
        config.assets        = g_clockwork_config.assets;
        config.assets_bytes  = g_clockwork_config.assets_bytes;
        config.returns       = g_clockwork_config.returns;
        config.returns_bytes = g_clockwork_config.returns_bytes;
        // Where the assets and the returns are, into the table: a segment's
        // creator already wrote them as segment offsets, which every reader
        // of the segment can use; an arena in plain memory gets the
        // addresses in this process (on the web, in wasm memory).
        if (!g_external_segment) {
            arenaSetArea(shared_memory, CLOCKWORK_AREA_ASSETS,
                         config.assets ? CLOCKWORK_SPACE_PROCESS : CLOCKWORK_SPACE_UNKNOWN,
                         (uintptr_t)config.assets, config.assets_bytes);
            arenaSetArea(shared_memory, CLOCKWORK_AREA_RETURNS,
                         config.returns ? CLOCKWORK_SPACE_PROCESS : CLOCKWORK_SPACE_UNKNOWN,
                         (uintptr_t)config.returns, config.returns_bytes);
        }


        // The guest's pool: clockwork's allocator over the bulk span, started
        // over every build; with it the RT router's block (once). The same on
        // every platform.
        clockwork_pool_boot(bulk_arena, bulk_arena_bytes);
#ifdef __EMSCRIPTEN__
        /*
         * A SPAN THE HOST PLACED ITSELF (the legacy guestMemorySize slice)
         * lies above malloc's frontier, and malloc grows upward into whatever
         * comes next. Measured 2026-08-31, before the engine's memory was one
         * block, the engine ran 3.08 MB into it on every boot. Failing loud
         * here costs a boot; not failing costs corruption that presents as
         * anything at all. A span clockwork took itself lies inside the
         * engine's memory, below the frontier, and overlaps nothing.
         */
        if (g_guest_memory && g_guest_memory == g_clockwork_config.guest_memory) {
            const uintptr_t guest_base = (uintptr_t)g_guest_memory;
            extern void* sbrk(intptr_t);
            const uintptr_t frontier = (uintptr_t)sbrk(0);
            if (frontier > guest_base) {
                clockwork_boot_fail("FATAL: WASM heap (sbrk=0x%08x) overlaps the guest "
                        "region (start=0x%08x) by %u bytes - reduce heap "
                        "usage or raise wasmHeapSize in js/memory_layout.js",
                        (unsigned)frontier, (unsigned)guest_base,
                        (unsigned)(frontier - guest_base));
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR,
                                               std::memory_order_relaxed);
                return;
            }
            if (verbosity > 0)
                clockwork_log("MEMORY OK: malloc<0x%08x guest=[0x%08x,0x%08x)",
                        (unsigned)frontier, (unsigned)guest_base,
                        (unsigned)(guest_base + g_guest_memory_bytes));
        }
#endif
#if CLOCKWORK_SYNTH
        // A bulk span the pool could not run over (too small for its
        // headers) is a boot that stops here and says so. Going on would
        // boot a guest whose every allocation answers NULL, and what it
        // reported would be its own failure, not the cause. (A no-DSP core
        // has no pool at all — its boot is a stub — so there is nothing to
        // check.)
        if (bulk_arena && !clockwork_pool_holds(bulk_arena)) {
            clockwork_boot_fail("FATAL: clockwork could not run the guest's pool over its bulk memory "
                                "(%zu bytes) - give the guest more (its bulk want, the profile's "
                                "CLOCKWORK_GUEST_MEMORY_BULK_BYTES, clockwork_set_guest_bulk_bytes)",
                                bulk_arena_bytes);
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }
        // The RT router's block, the Rust subsystems' for the life of the
        // process: placed once; an unarmed router stays UNKNOWN.
        if (clockwork_router_bytes())
            arenaSetArea(shared_memory, CLOCKWORK_AREA_RUST_HEAP, CLOCKWORK_SPACE_PROCESS,
                         clockwork_router_begin(), clockwork_router_bytes());
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
            arenaSetArea(shared_memory, CLOCKWORK_AREA_STATIC_DATA, CLOCKWORK_SPACE_PROCESS, 0, data_end);
            if (stack_hi > stack_lo)
                arenaSetArea(shared_memory, CLOCKWORK_AREA_STACK, CLOCKWORK_SPACE_PROCESS, stack_lo, stack_hi - stack_lo);
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
        // Build the scheduler's pools now, with the guest's memory arranged
        // and on the control thread. See clockwork_engine_schedule() for why this is not left
        // to static initialisation.
        clockwork_sched_warm();
#endif
#ifdef __EMSCRIPTEN__
        { extern void* sbrk(intptr_t); brk_after_sched = (uintptr_t)sbrk(0); }
#endif

        // What this DSP is, and its terms. dsp_describe() is static — it
        // answers before any instance exists — so the schedule declaration is
        // read once, here, and the branch it governs is decided nowhere else.
        const char* dsp_name    = "?";
        const char* dsp_version = "?";
        // Every call into the guest is marked (guest_malloc.h): what it takes
        // from malloc while the call is open is counted as its own.
        const DspInfo* info = nullptr;
        { GuestCallScope guest(GuestCall::Boot); info = dsp_describe(); }
        if (info) {
            g_dsp_holds_schedule = info->holds_schedule != 0;
            g_dsp_wants_events   = info->wants_events != 0;
            g_dsp_survives       = info->survives;
            // Which guest, and which window layout: into the table, for the
            // readers of the window (shared_memory.h arenaSetGuestWindowIdentity).
            arenaSetGuestWindowIdentity(shared_memory, info->window_magic, info->window_version);
            // The other declaration read here: the address this DSP wants to
            // Kept, not printed here. Who the guest is belongs with the rest of
            // the identity — the banner, the version, the rate — and that is
            // printed further down, once there is an engine to be identified.
            dsp_name    = info->name    ? info->name    : "?";
            dsp_version = info->version ? info->version : "?";
        }

        // Create the instance (see dsp_api.h). It is handed the config above
        // and the callbacks in g_dsp_host — emit_osc, log, open_sink,
        // send_sink, free_bytes — and that is the whole of what it knows about
        // clockwork.
        try {
            const char* err = nullptr;
            { GuestCallScope guest(GuestCall::Boot); g_dsp = dsp_new(&config, &g_dsp_host, &err); }
            if (!g_dsp) {
                clockwork_log("ERROR: engine bring-up failed: %s", err ? err : "unknown");
                record_boot_error("%s did not start: %s", dsp_name, err ? err : "it gave no reason");
                // The generation word stays odd: there is none, and none coming.
                cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
            // There is an instance: a generation opens — the word goes even,
            // one on from the last, which is the number setup will name —
            // and the boundary takes messages for it again.
            arenaInstanceUp(shared_memory);
            clockwork_carry_generation(arenaInstanceGeneration(shared_memory));
        } catch (const std::exception& e) {
            clockwork_log("ERROR: engine bring-up threw exception: %s", e.what());
            record_boot_error("%s did not start: %s", dsp_name, e.what());
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        } catch (...) {
            clockwork_log("ERROR: engine bring-up threw unknown exception");
            record_boot_error("%s did not start: it threw", dsp_name);
            cursors->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        }

        // Publish the geometry clockwork chose, and build the channel
        // pointer arrays dsp_process is handed. Done once: the audio thread
        // passes two pointers per block and does no layout arithmetic at all.
        g_block_size       = config.block_size;
        g_out_channels     = config.max_output_channels;
        g_in_channels      = config.max_input_channels;

        // Publish the device's own channels. A cold swap onto a different
        // device lands here again with new widths; ports bound across it keep
        // their entries (channel_map_set_device rewrites only unclaimed
        // channels).
        channel_map_set_device(g_in_channels, g_out_channels);
        g_dsp_sample_rate  = config.sample_rate;
        for (uint32_t c = 0; c < clockwork::kMaxChannels; ++c) {
            g_out_ptrs[c] = (c < g_out_channels)
                ? static_audio_bus + static_cast<size_t>(c) * g_block_size : nullptr;
            g_in_ptrs[c]  = (c < g_in_channels)
                ? static_audio_in + static_cast<size_t>(c) * g_block_size : nullptr;
        }

        // Zero both staging buffers: a host reading clockwork_audio_out() before the
        // first tick, or an input channel nothing fills, must see silence.
        memset(static_audio_bus, 0, sizeof(static_audio_bus));
        memset(static_audio_in, 0, sizeof(static_audio_in));

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

        // How far the block clock advances per buffer, and how to turn an NTP
        // interval back into samples. One second is kNtpUnitsPerSecond units.
        const int buf_length = static_cast<int>(g_block_size);
        g_osc_increment = (int64_t)((double)buf_length / sample_rate * clockwork::kNtpUnitsPerSecond);
        g_osc_to_samples = sample_rate / clockwork::kNtpUnitsPerSecond;

        // Publish cross-platform system info into the metrics struct.
        // Constant for the session; written once now that the DSP exists. Runs on every runtime — native's initialiseDsp() also
        // routes through init_memory().
        publish_session_metrics(sample_rate, buf_length);
        metrics->audio_output_channels.store(config.max_output_channels, std::memory_order_relaxed);
        metrics->audio_input_channels.store(config.max_input_channels, std::memory_order_relaxed);

        // (Re)initialise clockwork's timed queue. Clearing IS initialising
        // for a fixed-pool store, and this runs on every target's boot path.
#if CLOCKWORK_SCHEDULER
        clockwork_engine_schedule().clear();
#endif
        update_scheduler_depth_metric(0);


#ifdef __EMSCRIPTEN__
        clockwork_log("\n" CLOCKWORK_PRODUCT_BANNER);
        // the product introduces itself under its banner; the substrate it runs
        // on follows, unless the product is clockwork itself
        // (clockwork itself has no version: standalone its line is the commit it was built from)
        if (std::strcmp(CLOCKWORK_PRODUCT_NAME, "clockwork") != 0) {
            clockwork_log("%s v%s", CLOCKWORK_PRODUCT_NAME, CLOCKWORK_PRODUCT_VERSION);
            clockwork_log("on Clockwork " CLOCKWORK_COMMIT);
        } else {
            clockwork_log("clockwork " CLOCKWORK_COMMIT);
        }
        clockwork_log("%.0fkHz %dch", sample_rate / 1000, config.max_output_channels);
        clockwork_log("DSP: %s %s", dsp_name, dsp_version);
        clockwork_log("");
        clockwork_log("> engine ready...");
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
                arenaSetArea(shared_memory, CLOCKWORK_AREA_MALLOC, CLOCKWORK_SPACE_PROCESS, heap_base, brk - heap_base);
            if (verbosity > 0) clockwork_log("malloc at boot: base %u · guest memory fast [%u,%u) · bulk [%u,%u) · rust heap [%u,%u) · after the pool %u · after scheduler %u · after dsp_new %u",
                          (unsigned)heap_base, (unsigned)(uintptr_t)g_guest_memory, (unsigned)((uintptr_t)g_guest_memory + g_guest_memory_bytes),
                          (unsigned)(uintptr_t)g_owned_bulk, (unsigned)((uintptr_t)g_owned_bulk + g_owned_bulk_bytes),
                          (unsigned)clockwork_router_begin(), (unsigned)(clockwork_router_begin() + clockwork_router_bytes()),
                          (unsigned)brk_after_heap, (unsigned)brk_after_sched, (unsigned)brk_after_dsp);
            // And of those bytes, what the guest itself took with malloc and
            // new on the way up (guest_malloc.h): counted, not inferred.
            if (verbosity > 0 && clockwork_guest_malloc_counted())
                clockwork_log("guest malloc at boot: %u calls, %u freed, %u bytes held",
                              (unsigned)clockwork_guest_mallocs_boot(), (unsigned)clockwork_guest_frees(),
                              (unsigned)clockwork_guest_malloc_bytes());
        }
#else
        // No banner off the web, so the guest's identity is a line of its own.
        clockwork_log("DSP: %s %s", dsp_name, dsp_version);
#endif
        // Every asset the last instance held, the new one's before it says
        // anything of its own (hand_over_assets). None on a new arena.
        if (g_dsp) hand_over_assets();
        if (replacing && g_dsp) announce_instance(0);
#else  // !CLOCKWORK_SYNTH
        // No-DSP core: no instance, no audio render. The scheduler/router still
        // runs — derive the block-time constants from the AudioContext rate and a
        // default block size so the fire loop's OSC-time window is correct.
        const int buf_length = clockwork::kDefaultBlockSize;
        g_osc_increment = (int64_t)((double)buf_length / sample_rate * clockwork::kNtpUnitsPerSecond);
        g_osc_to_samples = sample_rate / clockwork::kNtpUnitsPerSecond;

        publish_session_metrics(sample_rate, buf_length);

#if CLOCKWORK_SCHEDULER
        clockwork_engine_schedule().clear();
#endif
        update_scheduler_depth_metric(0);
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
    void destroy_dsp() {
        // The audio thread is stopped (the caller's contract): what the instance
        // says as it goes, and what clockwork says for it, go on its ring after
        // everything it said before.
        InstanceRingScope instance_ring;
        // From here until the next dsp_new succeeds there is no instance: the
        // generation word goes odd, before anything else goes, so a message
        // written from now on meets the boundary's refusal (in_ring_door.h).
        // The generation that is closing stays named, so a refusal from here
        // says which instance refused and a reader of the arena knows
        // "between instances" from the bytes.
        if (shared_memory) arenaInstanceDown(shared_memory);
        // An instance whose setup is still owed goes unannounced: it said
        // nothing, and the next one up announces itself.
        g_owed.store(Owed::Nothing, std::memory_order_relaxed);
        // What was already in the in ring was written for this generation:
        // marked, for the next drain to answer (the marks).
        mark_closing_generation();
        // The assets it held stay committed, for the next instance (hand_over_assets):
        // the bytes are the client's, and still where they were.
        if (g_dsp) {
            { GuestCallScope guest(GuestCall::Boot); dsp_free(g_dsp); }
            g_dsp = nullptr;
        }
        // The next instance's call counts start from nothing (guest_malloc.h).
        // What this one took from malloc and never freed stays held, and stays
        // in the held count: the bytes are still the guest's.
        clockwork_guest_malloc_reset();
        // After dsp_free, so nothing can be sending as they go. See
        // dsp_host_open_sink: this is the other half of "clockwork closes
        // every sink a DSP opened when that DSP is freed".
        close_dsp_sinks();
        g_block_size = g_out_channels = g_in_channels = 0;
        g_dsp_sample_rate = 0.0;
        // The guest's pool goes with the instance; its span stays for the
        // next build (teardown_memory gives the spans back).
        clockwork_pool_teardown();
#if CLOCKWORK_SCHEDULER
        clockwork_engine_schedule().clear();
#endif
        update_scheduler_depth_metric(0);
        g_in_seq_reset.store(true, std::memory_order_relaxed);
    }

    void rebuild_dsp(double sample_rate, uint32_t device_buffer_frames) {
        // The audio thread is stopped (the caller's contract), so this thread
        // writes the instance's ring: what the instance says as it is built,
        // and then its setup.
        InstanceRingScope instance_ring;
        // The new rate, plus whatever channel ceilings the caller aimed at
        // with clockwork_set_channel_ceilings since the last build. Everything else
        // — the guest's config block, the arena, the regions — is where it
        // was; a rebuild is init_memory again, not a different function.
        g_clockwork_config.sample_rate = sample_rate;
        init_memory();
        // Not up: there is no instance to announce, and the failed build is
        // the host's to report.
        if (g_dsp) announce_instance(device_buffer_frames);
    }

    void come_back() {
        const ClockworkConfig& now = g_clockwork_config;
        // What no instance is carried across, whatever it survives: there is
        // none to carry (its build failed), it is not in the memory it was
        // built in, its config block is not the one it read, or the audio
        // thread left it partway through a block.
        const bool same_memory = now.guest_memory == g_built_config.guest_memory
                              && now.guest_memory_bytes == g_built_config.guest_memory_bytes
                              && now.assets == g_built_config.assets
                              && now.assets_bytes == g_built_config.assets_bytes
                              && now.returns == g_built_config.returns
                              && now.returns_bytes == g_built_config.returns_bytes
                              && now.arena_bytes == g_built_config.arena_bytes
                              && now.guest_bulk_bytes == g_built_config.guest_bulk_bytes
                              && now.fp_env == g_built_config.fp_env;
        const bool same_config = std::memcmp(shared_memory + GUEST_CONFIG_START, g_built_guest_config.data(),
                                             GUEST_CONFIG_SIZE) == 0;
        const bool torn = g_block_open.load(std::memory_order_acquire);
        clockwork::RestartStep step = clockwork::RestartStep::Rebuild;
        if (g_dsp && same_memory && same_config && !torn) {
            const clockwork::RestartFacts built_for { g_dsp_sample_rate, g_block_size,
                                                      g_built_config.input_channels, g_built_config.output_channels };
            const clockwork::RestartFacts back { now.sample_rate, build_block(now.block_size),
                                                 now.input_channels, now.output_channels };
            step = clockwork::planRestart(built_for, back, g_dsp_survives).step;
        }
        if (step == clockwork::RestartStep::Reconfigure
            && !clockwork_guest_reconfigure(now.sample_rate, now.input_channels, now.output_channels))
            step = clockwork::RestartStep::Rebuild;

        if (step == clockwork::RestartStep::Rebuild) {
            // The new instance's blocks are its own.
            g_block_open.store(false, std::memory_order_relaxed);
            // A committed asset points into the assets area it was committed
            // in: one that has moved leaves nothing for the next instance.
            if (now.assets != g_built_config.assets || now.assets_bytes != g_built_config.assets_bytes) {
                InstanceRingScope instance_ring;
                assets_release_all();
            }
            destroy_dsp();
            rebuild_dsp(now.sample_rate, 0);   // no device: setup names the block
            return;
        }
        // The same instance, back, every region as it was, as at any hot
        // restart. The clock re-enters service and re-anchors at the next
        // block, never below its last time.
        clock_enter_service(/*cold_boot*/ false);
        if (step == clockwork::RestartStep::Reconfigure) clockwork_announce_facts(now.sample_rate, 0);
    }

#endif

#if !defined(__EMSCRIPTEN__) && CLOCKWORK_SYNTH
    // Mirror of init_memory for engine shutdown: tear down the DSP instance and drop
    // the engine's global view of the arena while it is still mapped, so the
    // lanes guards (memory_initialized / control) reject post-shutdown calls
    // instead of touching a freed or unmapped segment.
    void teardown_memory() {
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
        clockwork_port_bus_detach_all();

        // And the committed assets: a rebuild hands them on, the engine going
        // releases them, and each committer hears so.
        {
            InstanceRingScope instance_ring;
            assets_release_all();
        }
        destroy_dsp();
        // The arenas clockwork reserved for the guest go with the engine, not
        // with the instance: a rebuild keeps them (destroy_dsp above), a
        // teardown returns them.
#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
        // The spans are cuts of the engine's block (g_engine_block), and the
        // block goes whole: an engine's, not the process's. Nothing cut from
        // it is live by now.
        g_owned_fast = nullptr; g_owned_bulk = nullptr;
        if (g_engine_block) {
            std::free(g_engine_block);
            g_engine_block = nullptr; g_engine_block_bytes = 0;
        }
#else
        if (g_owned_fast) { clockwork::mem::free(g_owned_fast); g_owned_fast = nullptr; }
        if (g_owned_bulk) { clockwork::mem::free(g_owned_bulk); g_owned_bulk = nullptr; }
#endif
        g_owned_fast_bytes = g_owned_bulk_bytes = 0;
        memory_initialized = false;
        shared_memory = nullptr;
        cursors = nullptr;
        metrics = nullptr;
    }
#endif

    // A refusal on the instance's ring, made only when the ring has room for
    // it (docs/RESTARTS.md, rule 1): false when it has not, and the frame
    // waits for a later block, with everything behind it, rather than be
    // refused into a full ring and lost. A refusal that cannot be encoded at
    // all is counted and the frame consumed: there is nothing truthful left
    // to say (clockwork_sys_refuse).
    static bool refuse_when_room(uint32_t sourceId, const uint8_t* payload, uint32_t size,
                                 const char* reason, uint32_t generation) {
        bool made = true;
        clockwork_sys_refuse(payload, size, reason, generation,
            [&made, sourceId](const uint8_t* d, uint32_t n) {
                const uint32_t footprint =
                    (static_cast<uint32_t>(sizeof(Message)) + EGRESS_ROUTE_SIZE + n + 3u) & ~3u;
                made = ring_fits(static_cast<uint32_t>(cursors->out_head.load(std::memory_order_relaxed)),
                                 static_cast<uint32_t>(cursors->out_tail.load(std::memory_order_acquire)),
                                 RT_OUT_RING_SIZE, footprint)
                    && emit_osc_out(sourceId, d, n);
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
    static void drain_without_guest() {
        constexpr uint32_t MAX_MESSAGES_PER_FRAME = 32;
        const int64_t blockTime = g_block_osc_time.load(std::memory_order_relaxed);
        ClockworkDrainStop stop = ClockworkDrainStop::Empty;
        clockwork_drain_ring(
            shared_memory + IN_RING_START, IN_RING_SIZE,
            &cursors->in_head, &cursors->in_tail, g_in_drain,
            ClockworkDrainMetrics{ &metrics->messages_processed, nullptr,
                            &metrics->messages_dropped,
                            &metrics->messages_sequence_gaps },
            MAX_MESSAGES_PER_FRAME,
            [blockTime](uint32_t sourceId, const uint8_t* payload,
                        uint32_t payload_size, uint32_t seq) -> ClockworkDrainVerdict {
                const RingMark* mark = g_in_marks.of(seq);
                if (clockwork_sys_answers_without_guest(payload, payload_size)) {
                    dispatch(payload, payload_size, sourceId, 1, blockTime);
                    return ClockworkDrainVerdict::Consume;
                }
                const bool refused = mark
                    ? refuse_when_room(sourceId, payload, payload_size, mark->reason, mark->generation)
                    : refuse_when_room(sourceId, payload, payload_size,
                                       CLOCKWORK_REFUSED_NO_INSTANCE, arenaInstanceGeneration(shared_memory));
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
    uint32_t clockwork_close() {
        if (!memory_initialized || !cursors) return 0;
        if (g_dsp) destroy_dsp();
        InstanceRingScope instance_ring;
        drain_without_guest();
        const auto head = static_cast<uint32_t>(cursors->in_head.load(std::memory_order_acquire));
        const auto tail = static_cast<uint32_t>(cursors->in_tail.load(std::memory_order_acquire));
        return (head - tail + IN_RING_SIZE) % IN_RING_SIZE;
    }
#endif

    // The per-block body on every host; clockwork_tick (lanes.h) wraps it.
    // `time` carries the host's frame count, wall and monotonic clocks,
    // output latency and Link stamp in, and gets block_ntp and
    // block_host_micros back (ClockworkBlockTime).
    // active_output_channels / active_input_channels: live channel counts
    EMSCRIPTEN_KEEPALIVE
    bool process_audio(ClockworkBlockTime& time, uint32_t active_output_channels, uint32_t active_input_channels) {
        InstanceRingScope instance_ring;   // the audio thread writes the instance's ring
        BlockOpenScope block_open;         // until the block is done (g_block_open)
        clockwork_pool_register_engine_thread();  // pool ownership follows the audio thread
#if CLOCKWORK_SYNTH
        if (!memory_initialized) {
            return true; // Not ready — output silence
        }
        if (!g_dsp) {
            // The instance was destroyed for a cold swap and its successor is
            // being built: silence, and ingress waits for it. Unless the build
            // failed — then nothing is coming, and clockwork still answers.
            if (g_boot_failed.load(std::memory_order_acquire) && cursors && metrics)
                drain_without_guest();
            return true;
        }
#else
        // No-DSP core: there is no instance; the scheduler/router still ticks.
        if (!memory_initialized) {
            return true;
        }
#endif

        if (!metrics) {
            return false;
        }

        // A new instance says nothing before its setup, nor a survived change
        // before its notice: one the ring had no room for says it now, or
        // waits, silent, for a block when it can.
        if (!say_owed()) return true;

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
        ClockworkClock* clk = g_active_clockwork_clock.load(std::memory_order_acquire);
        const bool freewheel = clk && clk->freewheelClock();
        const bool gap = !freewheel && rate > 0.0
                      && clockwork_audio_back(time.wall_ntp, time.monotonic_secs, sample_pos / rate);

#if CLOCKWORK_SCHEDULER
        clockwork_engine_schedule().drainPendingClear();
#endif

        double current_ntp = clk ? clk->now() : time.wall_ntp;
        uint64_t host_micros = 0;
        if (clk && rate > 0.0) {
#ifndef __EMSCRIPTEN__
            LinkAudioHost* link = g_active_link_audio.load(std::memory_order_acquire);
#endif
            const bool anchoring = !clk->audioThreadAnchored() || gap || g_anchor_at_first_block;
            if (anchoring) {
                g_anchor_at_first_block = false;
                clk->resetAudioThreadTime(sample_pos, rate, time.wall_ntp,
                                          clk->floorRenderNtp(time.output_latency_frames));
#ifndef __EMSCRIPTEN__
                if (link) link->resetBlockClock();
#endif
            }
            current_ntp = clk->updateAudioThreadNTP(sample_pos, rate, time.wall_ntp);
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

        // Load-add-store, not fetch_add: this counter has exactly one writer
        // (the audio thread, here) and readers only ever load it, so the
        // read-modify-write bought nothing. It cost something, though —
        // ESP-IDF builds with -mdisable-hardware-atomics, so every atomic RMW
        // is a call into libatomic that takes a critical section, once a
        // block, for an increment nobody was racing.
        const uint32_t pc = metrics->process_count.load(std::memory_order_relaxed) + 1;
        metrics->process_count.store(pc, std::memory_order_relaxed);
        // The hour's history of every counter and gauge, a bucket at a time
        // on the engine's clock (metrics_history.h).
        g_metrics_history.record(reinterpret_cast<const std::atomic<uint32_t>*>(metrics), current_ntp);

        // Host telemetry. Sample the ring fill every block so peaks stay a true
        // high-water mark (a burst can fill and drain within a few blocks); this is cheap
        // — the control reads overlap the drain and peaks are local. Flush the SAB (clock
        // readout + ring used/peaks) only at the poll rate below: a consumer reads it at
        // display rate, and per-block flushing is costly where the metrics struct is in
        // slow memory (PSRAM on ESP32). process_count stays per-block as the block counter.
        {
            int32_t in_head = cursors->in_head.load(std::memory_order_relaxed);
            int32_t in_tail = cursors->in_tail.load(std::memory_order_relaxed);
            uint32_t in_used = (in_head - in_tail + IN_RING_SIZE) % IN_RING_SIZE;
            if (in_used > local_in_peak.load(std::memory_order_relaxed))
                local_in_peak.store(in_used, std::memory_order_relaxed);

            int32_t out_head = cursors->out_head.load(std::memory_order_relaxed);
            int32_t out_tail = cursors->out_tail.load(std::memory_order_relaxed);
            uint32_t out_used = (out_head - out_tail + RT_OUT_RING_SIZE) % RT_OUT_RING_SIZE;
            if (out_used > local_out_peak.load(std::memory_order_relaxed))
                local_out_peak.store(out_used, std::memory_order_relaxed);

            int32_t nrt_out_head = cursors->nrt_out_head.load(std::memory_order_relaxed);
            int32_t nrt_out_tail = cursors->nrt_out_tail.load(std::memory_order_relaxed);
            uint32_t nrt_out_used = (nrt_out_head - nrt_out_tail + NRT_OUT_RING_SIZE) % NRT_OUT_RING_SIZE;
            if (nrt_out_used > local_nrt_out_peak.load(std::memory_order_relaxed))
                local_nrt_out_peak.store(nrt_out_used, std::memory_order_relaxed);

            // Flush period in blocks, derived once from the audio rate to target ~30Hz
            // (tracks sample rate / block size instead of a fixed divisor); 16 until the
            // audio config is published.
            uint32_t derived = metrics_flush_period.load(std::memory_order_relaxed);
            if (derived == 0u) {
                const uint32_t blk = metrics->audio_block_size.load(std::memory_order_relaxed);
                const uint32_t sr  = metrics->audio_sample_rate.load(std::memory_order_relaxed);
                if (blk && sr) {            // round(sr / (30 * blk)) in integer math
                    uint32_t p = (sr + 15u * blk) / (30u * blk);
                    derived = (p < 1u) ? 1u : p;
                    metrics_flush_period.store(derived, std::memory_order_relaxed);
                }
            }
            const uint32_t flush_period = (derived != 0u) ? derived : 16u;
            if ((pc % flush_period) == 0u) {
                if (clk) {
                    clk->publishClockMetrics(metrics, current_ntp, 4.0);
#ifndef __EMSCRIPTEN__
                    if (LinkAudioHost* link = g_active_link_audio.load(std::memory_order_acquire)) {
                        clk->publishLinkMetrics(metrics, 4.0);
                        link->publishMetrics(metrics);
                    }
#endif
                }
                metrics->in_buffer_used_bytes.store(in_used, std::memory_order_relaxed);
                metrics->in_buffer_peak_bytes.store(local_in_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->out_buffer_used_bytes.store(out_used, std::memory_order_relaxed);
                metrics->out_buffer_peak_bytes.store(local_out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->nrt_out_buffer_used_bytes.store(nrt_out_used, std::memory_order_relaxed);
                metrics->nrt_out_buffer_peak_bytes.store(local_nrt_out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                // the guest's pool's and the rust heap's census: what is in use, the most ever, and the trouble counts;
                // and the guest's mallocs (guest_malloc.h): what it holds, and how often it allocated while processing
                metrics->guest_malloc_bytes.store((uint32_t)clockwork_guest_malloc_bytes(), std::memory_order_relaxed);
                metrics->guest_mallocs_processing.store(clockwork_guest_mallocs_processing(), std::memory_order_relaxed);
                metrics->mallocs_after_boot.store(clockwork_mallocs_after_boot(), std::memory_order_relaxed);
                metrics->malloc_bytes_after_boot.store((uint32_t)clockwork_malloc_bytes_after_boot(), std::memory_order_relaxed);
                metrics->guest_pool_used_bytes.store((uint32_t)clockwork_pool_used_bytes(), std::memory_order_relaxed);
                metrics->guest_pool_peak_bytes.store((uint32_t)clockwork_pool_peak_bytes(), std::memory_order_relaxed);
                metrics->rust_heap_used_bytes.store((uint32_t)clockwork_router_used_bytes(), std::memory_order_relaxed);
                metrics->rust_heap_fallbacks.store(clockwork_router_fallback_allocs(), std::memory_order_relaxed);
            }
        }

        // Process incoming OSC messages. The walk — header validation,
        // untrusted-cursor repair, padding markers, gap tracking, tail resync on
        // corruption — is the shared lanes walker (ring_drain.h); only the
        // perform/schedule/classify policy lives here, in the callback. The drain
        // and the scheduler fire below run on every build; only the audio render
        // needs a DSP instance, so the `if (g_dsp)` guard is kept where the
        // audio path is compiled in and elided where it is not.
#if CLOCKWORK_SYNTH
        if (g_dsp)
#endif
        {
            // Off-thread reset request (purge → clear_scheduler): apply it
            // here so g_in_drain stays audio-thread-only. Read before the
            // exchange, and take the exchange only when a request is actually
            // waiting: a byte load compiles to one instruction, while the
            // exchange is a libatomic call under -mdisable-hardware-atomics
            // (ESP-IDF), and this is the audio thread asking "has anyone
            // asked?" on a block where the answer is almost always no. The
            // exchange still does the taking, so two threads cannot both
            // claim one request.
            if (g_in_seq_reset.load(std::memory_order_relaxed) &&
                g_in_seq_reset.exchange(false, std::memory_order_relaxed))
                g_in_drain.lastSeq = -1;

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
            const int64_t currentOscTime = ntp_to_osc_timetag(current_ntp);
            const int64_t nextOscTime    = currentOscTime + g_osc_increment;
            g_block_osc_time.store(currentOscTime, std::memory_order_relaxed);

            ClockworkDrainStop stop = ClockworkDrainStop::Empty;
            clockwork_drain_ring(
                shared_memory + IN_RING_START, IN_RING_SIZE,
                &cursors->in_head, &cursors->in_tail, g_in_drain,
                ClockworkDrainMetrics{ &metrics->messages_processed, nullptr,
                                &metrics->messages_dropped,
                                &metrics->messages_sequence_gaps },
                MAX_MESSAGES_PER_FRAME,
                [currentOscTime](uint32_t sourceId, const uint8_t* payload,
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
                    if (const RingMark* mark = g_in_marks.of(seq);
                        mark && !clockwork_sys_answers_without_guest(payload, payload_size)
                        && !(mark->onlyDue && bundle_for_later(payload, payload_size, currentOscTime)))
                        return refuse_when_room(sourceId, payload, payload_size,
                                                mark->reason, mark->generation)
                            ? ClockworkDrainVerdict::Consume : ClockworkDrainVerdict::Retain;

                    // In-place delivery: the payload points into the IN ring
                    // (the consumer owns the region until we return Consume).
                    // Delivery is synchronous and anything kept is copied — a
                    // parked message is memcpy'd into the schedule's data pool,
                    // and dsp_osc is documented as not retaining bytes — so
                    // nothing outlives this callback holding the pointer.
                    ingest_frame(payload, payload_size, sourceId, currentOscTime);
                    return ClockworkDrainVerdict::Consume;
                },
                &stop);

            // The walker resyncs and counts on corruption; policy — rate-
            // limited logging and the status flag — stays the engine's.
            uint32_t gaps_after =
                metrics->messages_sequence_gaps.load(std::memory_order_relaxed);
            if (gaps_after != gaps_before &&
                gap_log_count.load(std::memory_order_relaxed) < 5) {
                clockwork_log("WARNING: IN sequence gap: %u message(s) missing (total %u)",
                             gaps_after - gaps_before, gaps_after);
                gap_log_count.fetch_add(1, std::memory_order_relaxed);
            }
            if (stop == ClockworkDrainStop::BadMagic || stop == ClockworkDrainStop::BadLength ||
                stop == ClockworkDrainStop::BadCursor) {
                if (corruption_count.load(std::memory_order_relaxed) < 5) {
                    clockwork_log("ERROR: IN ring corrupt (%s): head=%d tail=%d - pending region dropped",
                                 stop == ClockworkDrainStop::BadMagic  ? "bad magic" :
                                 stop == ClockworkDrainStop::BadLength ? "bad length" : "bad cursor",
                                 cursors->in_head.load(std::memory_order_relaxed),
                                 cursors->in_tail.load(std::memory_order_relaxed));
                    corruption_count.fetch_add(1, std::memory_order_relaxed);
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
            if (MidiClockOut* clockOut = g_active_midi_clock_out.load(std::memory_order_acquire))
                clockOut->generate(current_ntp);

#if CLOCKWORK_SCHEDULER
            // Fire: drain every event due this block in time order, handing
            // each to the SAME dispatch() the immediate drain uses. The event
            // keeps its own timetag — it is due within THIS block, so the
            // difference from currentOscTime is exactly the sub-block offset
            // the DSP needs, and flattening it to "now" would throw away the
            // sample accuracy the old dsp_set_sample_offset path had.
            clockwork_fire_due(clockwork_engine_schedule(), nextOscTime, currentOscTime,
                [](const uint8_t* d, uint32_t n, uint32_t origin,
                   int64_t when, int64_t blockTime) {
                    if (note_lateness(when, blockTime)) return;   // shed: too late to play
                    dispatch(d, n, origin, when, blockTime);
                });
            // Publish queue depth once per block, after draining.
            update_scheduler_depth_metric(
                static_cast<uint32_t>(clockwork_engine_schedule().size()));
#else
            // No queue: nothing waits, so nothing fires and the depth is 0 by
            // construction. The block-time window above is still computed —
            // dispatch() carries each message's own timetag across the boundary
            // either way, and the DSP subtracts the block start itself.
            (void)nextOscTime;
#endif

#if CLOCKWORK_SYNTH
            const uint32_t QUANTUM_SIZE = g_block_size;

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
                pull_port_sources(QUANTUM_SIZE);

                // Hosted plugin tracks are ports like any other: the send
                // lanes are a sink pushed below, the return lanes a source
                // pulled above, and the plugins run in another process
                // (plugin_bridge.h). The core knows nothing of them.

                // Live counts, per block: Link peers join and leave and a
                // device switch changes the interface underneath us. The
                // config carried only the ceilings.
                {
                    GuestCallScope guest(GuestCall::Processing);   // the render is the guest's; the sinks after it are not
                    dsp_process(g_dsp,
                                g_in_channels ? g_in_ptrs : nullptr, g_in_channels,
                                g_out_ptrs, g_out_channels,
                                QUANTUM_SIZE,
                                currentOscTime);
                }

                // Sinks after: what a sink records is what the DSP actually
                // emitted, which is the "post" answer to open question 1 in
                // docs/PORTS.md for the only tap that exists so far.
                push_port_sinks(QUANTUM_SIZE);
            }
            // Advance the engine frame count past this block; tap streams anchor on it (every host, as
            // the device path did before).
            if (clk) clk->advanceEngineFrames(sample_pos + static_cast<double>(QUANTUM_SIZE));
            // The scope, at the device edge: what leaves for the device
            // this block (after the guest, after the port sinks) and what
            // arrived from it (as the guest saw it). Every host, every
            // block, at the channel count the slot was formatted with. A
            // client draws the master mix, or records it, or meters the
            // input, from these and asks the guest for nothing.
            if (g_shm_audio_buffers) {
                const auto tap = [&](uint32_t slot, const float* bus) {
                    auto* t = &g_shm_audio_buffers[slot];
                    const uint32_t ch = t->channels;
                    if (ch == 0) return;
                    const float* channel_data[SCOPE_CHANNELS];
                    for (uint32_t c = 0; c < ch; ++c)
                        channel_data[c] = bus + static_cast<size_t>(c) * QUANTUM_SIZE;
                    shm_audio_buffer_writer(t).write(channel_data, ch, QUANTUM_SIZE);
                };
                tap(SCOPE_OUT_SLOT, static_audio_bus);
                tap(SCOPE_IN_SLOT,  static_audio_in);
            }

#endif // CLOCKWORK_SYNTH
        }

        return true; // Keep processor alive
    }

    // Frame a log line as a `/clockwork/debug <text>` OSC message and emit it.
    // Routing keeps the RT-out ring single-writer: on the audio thread the line
    // goes to the lock-free RT-out ring; off the audio thread (watchdog, recovery,
    // boot on native) it goes to the locked multi-producer NRT-out ring, so RT-out
    // never gets a second concurrent writer. The NRT gateway drains and forwards
    // both, so delivery is identical. WASM is single-threaded with no NRT-out
    // drainer, so it always uses RT-out. Either way the line leaves as an ordinary
    // addressed OSC message the host dispatches to its debug channel.
    static void emit_debug_osc(const char* text, uint32_t len) {
        if (!memory_initialized) return;
        if (len > 960) len = 960;  // matches buildDebugOsc's clamp; keeps the metric in sync

        std::array<char, 1024> pkt{};
        uint32_t p = clockwork::buildDebugOsc(pkt.data(), text, len);

        // The audio thread owns the lock-free RT-out ring, so it logs there. Any
        // other thread routes to the locked NRT-out ring instead — but only where a
        // backend drains it (g_nrt_egress_drained, published by the native NRT
        // gateway). A single-threaded worklet target (WASM / self-driven device)
        // never drains NRT-out and has no second RT-out writer, so it always uses
        // the always-safe RT-out. Capability signal, not an __EMSCRIPTEN__ branch,
        // so every target shares one codepath.
        const bool useRtOut =
            t_writes_instance_ring ||
            !g_nrt_egress_drained.load(std::memory_order_relaxed);
        if (useRtOut) {
            ::ring_buffer_write(
                shared_memory + RT_OUT_RING_START, RT_OUT_RING_SIZE,
                &cursors->out_head, &cursors->out_tail, &cursors->out_sequence,
                EGRESS_BROADCAST_NOTIFY, 0,  // route, source_id (debug broadcasts)
                pkt.data(), p, &cursors->status_flags, metrics);   // a refused log line is counted too
        } else {
            clockwork_egress_nrt_write(EGRESS_BROADCAST_NOTIFY, 0,
                                reinterpret_cast<const uint8_t*>(pkt.data()), p);
        }

        // Count the debug line for the metrics view.
        if (metrics) {
            metrics->debug_messages_received.fetch_add(1, std::memory_order_relaxed);
            metrics->debug_bytes_received.fetch_add(len, std::memory_order_relaxed);
        }
    }

    /*
     * Boot-time failure reporting, which clockwork_log cannot do.
     *
     * clockwork_log writes an OSC message into the debug ring for a worker to drain
     * and forward. That works once the engine is running and is worth nothing
     * before it: every FATAL below describes a boot that is about to not
     * happen, so the ring it wrote to is a ring nobody ever reads. Measured
     * 2026-09-01 — an early return out of init_memory surfaced to the client
     * as "AudioWorklet initialization timeout", with the actual reason sitting
     * in a buffer that was discarded.
     *
     * So these go straight out: the browser console on web, stderr on native.
     * Both are visible with no engine, which is the only property that
     * matters here.
     */
    static void clockwork_boot_fail(const char* fmt, ...) {
        std::array<char, 512> buffer{};
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer.data(), buffer.size(), fmt, args);
        va_end(args);
        // And kept, for the host to hand its clients: a reason on stderr is a
        // reason only someone reading the terminal ever sees.
        record_boot_error("%s", buffer.data());
#ifdef __EMSCRIPTEN__
        emscripten_console_error(buffer.data());
#else
        fprintf(stderr, "%s\n", buffer.data());   // stdio-ok: reporting a boot that produced no ring
#endif
    }

#ifndef __EMSCRIPTEN__
    // Native pre-ring hold (see clockwork_log in clockwork_config.h). Lines
    // logged while an engine is booting and the ring does not exist yet wait
    // here; release() replays them onto the ring in order, or prints them to
    // stderr when the boot never reached the ring. Bounded so a boot that
    // logs in a loop cannot grow without limit — past the cap, lines go to
    // stderr directly.
    static std::mutex g_early_log_mutex;
    static std::vector<std::string> g_early_log;
    static bool g_early_log_holding = false;
    static constexpr size_t kEarlyLogMaxLines = 256;

    extern "C" void clockwork_log_hold(void) {
        std::lock_guard<std::mutex> lk(g_early_log_mutex);
        g_early_log_holding = true;
    }

    extern "C" void clockwork_log_release(void) {
        std::vector<std::string> held;
        {
            std::lock_guard<std::mutex> lk(g_early_log_mutex);
            g_early_log_holding = false;
            held.swap(g_early_log);
        }
        for (auto& line : held) {
            if (memory_initialized)
                emit_debug_osc(line.data(), static_cast<uint32_t>(line.size()));
            else
                fprintf(stderr, "%s\n", line.c_str());   // stdio-ok: no ring came up for this boot
        }
        if (!memory_initialized && !held.empty()) fflush(stderr);   // stdio-ok
    }

    // A line with no ring to go to: held during a boot, otherwise stderr.
    static void clockwork_log_no_ring(const char* text) {
        {
            std::lock_guard<std::mutex> lk(g_early_log_mutex);
            if (g_early_log_holding && g_early_log.size() < kEarlyLogMaxLines) {
                g_early_log.emplace_back(text);
                return;
            }
        }
        fprintf(stderr, "%s\n", text);   // stdio-ok: no engine ring exists
        fflush(stderr);                   // stdio-ok
    }
#endif

    static int clockwork_log_impl(const char* fmt, va_list args) {
        std::array<char, 1024> buffer{};
        int result = vsnprintf(buffer.data(), buffer.size(), fmt, args);
        if (!memory_initialized) {
#ifdef __EMSCRIPTEN__
            emscripten_console_log(buffer.data());
#else
            clockwork_log_no_ring(buffer.data());
#endif
            return result;
        }
        uint32_t len = 0;
        while (len < buffer.size() && buffer[len] != '\0') len++;
        emit_debug_osc(buffer.data(), len);
        return result;
    }

    // Variadic version - for direct C++ calls
    extern "C" EMSCRIPTEN_KEEPALIVE
    int clockwork_log(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        int result = clockwork_log_impl(fmt, args);
        va_end(args);
        return result;
    }

    // va_list version - for function pointers (matches PrintFunc signature)
    extern "C" EMSCRIPTEN_KEEPALIVE
    int clockwork_log_va(const char* fmt, va_list args) {
        return clockwork_log_impl(fmt, args);
    }

    // Raw version - already-formatted string (avoids the vsnprintf double-copy).
    extern "C" EMSCRIPTEN_KEEPALIVE
    int clockwork_log_raw(const char* msg, uint32_t len) {
        if (!memory_initialized || !msg || len == 0) return 0;
        emit_debug_osc(msg, len);
        return (int)len;
    }

    EMSCRIPTEN_KEEPALIVE
    uint32_t get_process_count() {
        return metrics ? metrics->process_count.load(std::memory_order_relaxed) : 0;
    }

    EMSCRIPTEN_KEEPALIVE
    uint32_t get_messages_processed() {
        return metrics ? metrics->messages_processed.load(std::memory_order_relaxed) : 0;
    }

    EMSCRIPTEN_KEEPALIVE
    uint32_t get_messages_dropped() {
        return metrics ? metrics->messages_dropped.load(std::memory_order_relaxed) : 0;
    }

    EMSCRIPTEN_KEEPALIVE
    uint32_t get_status_flags() {
        return cursors ? cursors->status_flags.load(std::memory_order_relaxed) : 0;
    }

    // Live channel widths. A cold swap can rebuild at a different width than
    // boot, so callers that cache channel counts (ClockworkProcessor's
    // output/input clamps) re-sync from these afterwards rather than trusting
    // boot-time values. Clockwork answers from what it CONFIGURED, never by
    // asking the DSP.
#if CLOCKWORK_SYNTH
    EMSCRIPTEN_KEEPALIVE
    int get_audio_num_output_buses() {
        return g_dsp ? static_cast<int>(g_out_channels) : 0;
    }

    EMSCRIPTEN_KEEPALIVE
    int get_audio_num_input_buses() {
        return g_dsp ? static_cast<int>(g_in_channels) : 0;
    }
#endif // CLOCKWORK_SYNTH

    // engine audio output accessor — the master mix staging buffer. In the
    // no-synth build it is never written (no render), so the host reads silence;
    // the symbol stays so lanes.cpp's clockwork_audio_out() links either way.
    EMSCRIPTEN_KEEPALIVE
    uintptr_t get_audio_output_bus() {
        if (!memory_initialized) {
            return 0;
        }

        return reinterpret_cast<uintptr_t>(static_audio_bus);
    }

    EMSCRIPTEN_KEEPALIVE
    int64_t get_audio_block_time() {
        // The OSC time of the block being processed: what a port's transfer
        // signal, which runs inside the block but is handed no metadata,
        // stamps on what it hands to another process.
        return g_block_osc_time.load(std::memory_order_relaxed);
    }

    EMSCRIPTEN_KEEPALIVE
    double get_audio_sample_rate() {
        // The rate the DSP was initialised at; 0 until then. What a handler on
        // the audio thread divides by to turn a timetag into a frame offset.
        return g_dsp_sample_rate;
    }

    EMSCRIPTEN_KEEPALIVE
    int get_audio_buffer_samples() {
        // Block size currently in use. On web this is always 128
        // (AudioWorklet render quantum). On native it reflects whatever was
        // configured at boot. No instance yet: the default block.
#if CLOCKWORK_SYNTH
        return g_block_size ? static_cast<int>(g_block_size)
                            : clockwork::kDefaultBlockSize;
#else
        return clockwork::kDefaultBlockSize;
#endif
    }

    // The input staging buffer a host fills before the tick. It is the
    // clockwork's memory, channel-major like the output, and it is what
    // dsp_process is handed as `in`.
    EMSCRIPTEN_KEEPALIVE
    uintptr_t get_audio_input_bus() {
#if CLOCKWORK_SYNTH
        if (!memory_initialized || !g_dsp || g_in_channels == 0) {
            return 0;
        }
        return reinterpret_cast<uintptr_t>(static_audio_in);
#else
        return 0;
#endif
    }

    // Return the time conversion offset (NTP seconds when AudioContext was 0).
    EMSCRIPTEN_KEEPALIVE
    double get_time_offset() {
        return g_time_zero_osc;
    }

} // namespace engine

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
    const void* data,
    uint32_t data_size,
    std::atomic<uint32_t>* status_flags,
    PerformanceMetrics* pm
) {
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
    std::memcpy(buffer_start + current_head + sizeof(Message) + sizeof(uint32_t), data, data_size);

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
        uint32_t used = (new_head - current_tail + buffer_size) % buffer_size;
        uint32_t prev = pm->out_buffer_peak_bytes.load(std::memory_order_relaxed);
        while (used > prev) {
            if (pm->out_buffer_peak_bytes.compare_exchange_weak(
                    prev, used, std::memory_order_relaxed))
                break;
        }
    }

    return true;
}

// ── Clockwork's own timed queue ────────────────────────────────────────────
//
// The store, the fire loop and the classifier all live in this file. Holding a
// schedule is the DSP's own affair, declared with DspInfo::holds_schedule, so
// nothing bridges back across the boundary for it.

// Process lifetime, like every other piece of clockwork state (one arena, one
// clock, one ingress). File scope rather than a function-local static so the
// audio thread's every use is a plain address and not a guard check.
// CLOCKWORK_COLD_BSS puts the data pool in bulk RAM on tiered targets; the
// constructor still runs, placing it there.
#if CLOCKWORK_SCHEDULER
/*
 * CONSTRUCTED ON FIRST USE, NOT AT STATIC-INIT. As a file-scope static its
 * constructor runs before init_memory and clockwork_pool_boot, so its 4MB data
 * pool fails to allocate and clockwork_sched_new returns null — after which every
 * add is silently counted into scheduler_dropped and looks exactly like ordinary
 * overload backpressure.
 *
 * clockwork_sched_warm() below is called from init_memory so the first
 * construction happens on the control thread at boot, not on the audio thread
 * mid-block.
 */
EngineScheduler& clockwork_engine_schedule() {
    static EngineScheduler s;
    return s;
}

// Force the construction above to happen here, at a moment when allocating is
// safe. Cheap and idempotent; the point is the side effect.
void clockwork_sched_warm() {
    if (!clockwork_engine_schedule().ready()) {
        // Loud, once, and unmistakable. A queue that failed to build refuses
        // every add exactly as a full one does, so without this the only
        // symptom is a scheduler_dropped count that looks like backpressure.
        clockwork_log("FATAL: the scheduler could not be built (%d slots, %d byte pool). "
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
bool clockwork_clockwork_sys_route(void* /*routeCtx*/, const void* callCtx,
                      const uint8_t* data, std::size_t len) {
    using namespace engine;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const uint32_t token = cc ? cc->sourceId : 0;
    // The clock comes from the arena, so the snapshot verb answers against the
    // same published mirror the guest reads — not the engine's own getters,
    // which are not realtime-safe when Link is running.
    const auto* clockState = shared_memory
        ? reinterpret_cast<const ClockworkClockState*>(shared_memory + CLOCK_STATE_START)
        : nullptr;
    handle_clockwork_sys_osc(data, static_cast<uint32_t>(len),
        [token](const uint8_t* d, uint32_t n) { emit_osc_out(token, d, n); },
        clockState, arenaInstanceGeneration(shared_memory));
    return true;   // the prefix is claimed whole, answered or refused
}

bool clockwork_sched_flush_route(void* /*routeCtx*/, const void* callCtx,
                                 const uint8_t* data, std::size_t len) {
    using namespace engine;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const uint32_t token = cc ? cc->sourceId : 0;
    const auto refuse = [&](const char* reason) {
        clockwork_sys_refuse(data, static_cast<uint32_t>(len), reason, arenaInstanceGeneration(shared_memory),
            [token](const uint8_t* d, uint32_t n) { emit_osc_out(token, d, n); });
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
    clockwork_engine_schedule().flush(tagName && *tagName
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
// at the far end of that hop. A host with no thread to forward to has, so
// far, refused here. But a host may have a FRONT — a thread of its own that
// answers the control verbs, which on the web is the client's main thread,
// the only place Web MIDI and the Gamepad API exist at all. With
// g_host_forward set, that front is the far end: the verb goes out over the
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
    using namespace engine;
    if (!g_host_forward.load(std::memory_order_acquire))
        return clockwork_clockwork_sys_route(routeCtx, callCtx, data, len);

    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const uint32_t token = cc ? cc->sourceId : 0;
    const auto* clockState = shared_memory
        ? reinterpret_cast<const ClockworkClockState*>(shared_memory + CLOCK_STATE_START)
        : nullptr;
    switch (handle_clockwork_sys_rt(data, static_cast<uint32_t>(len),
                [token](const uint8_t* d, uint32_t n) { emit_osc_out(token, d, n); },
                clockState, arenaInstanceGeneration(shared_memory))) {
        case ClockworkSysRt::NotClaimed:  return false;
        case ClockworkSysRt::Answered:    return true;
        case ClockworkSysRt::UnknownVerb: break;
    }

    forward_to_host(token, cc ? cc->when : 1, data, static_cast<uint32_t>(len));
    return true;   // claimed whole: the host answers or refuses it from here
}

// One element to the host, wrapped with its time: "#bundle" NUL, timetag
// (big-endian 64), element size (big-endian 32), the element. Built by hand:
// oscpack's stream cannot take an already-encoded element. The buffer is the
// audio thread's — the only caller — and is sized for the widest verb the IN
// ring carries, which is not something to put on a stack.
static void forward_to_host(uint32_t token, int64_t when, const uint8_t* element, uint32_t len) {
    using namespace engine;
    static constexpr uint32_t kHeader = 8 + 8 + 4;
    static std::array<uint8_t, kHeader + 66560> frame;
    if (len > frame.size() - kHeader) {
        if (oversize_forward_log_count.fetch_add(1, std::memory_order_relaxed) < 8)
            clockwork_log("WARNING: %s of %u bytes dropped — wider than the host forward carries",
                          reinterpret_cast<const char*>(element), static_cast<unsigned>(len));
        return;
    }
    if (when == 0) when = 1;   // both spell "immediately"; the wire form is 1
    const uint64_t tt = static_cast<uint64_t>(when);
    std::memcpy(frame.data(), "#bundle", 8);
    for (unsigned i = 0; i < 8; ++i) frame[8 + i]  = static_cast<uint8_t>(tt >> (56u - 8u * i));
    for (unsigned i = 0; i < 4; ++i) frame[16 + i] = static_cast<uint8_t>(len >> (24u - 8u * i));
    std::memcpy(frame.data() + kHeader, element, len);
    emit_osc_out(token, frame.data(), kHeader + len);
}

// An inbound event, on the audio thread, on every host (clockwork_sys.h).
// Out to the subsystem's audience by the route word the egress already has
// for it — the transport fans it out natively; on the web the one client
// hears it and its front keeps the subscription rule — and to the guest if
// it asked. The message is passed through byte for byte, trailing arrival
// time included; the guest gets the same DrainCallCtx every message gets,
// with `when` the block it was drained in.
bool clockwork_event_route(void* /*routeCtx*/, const void* callCtx,
                           const uint8_t* data, std::size_t len) {
    using namespace engine;
    const char* verb = reinterpret_cast<const char*>(data) + CLOCKWORK_SYS_PREFIX_LEN;
    const uint32_t route = std::strncmp(verb, "gamepad/", 8) == 0
                         ? EGRESS_BROADCAST_GAMEPAD : EGRESS_BROADCAST_MIDI;
    emit_osc_route(route, 0, data, static_cast<uint32_t>(len));
    if (g_dsp_wants_events)
        clockwork_dsp_default_route(nullptr, callCtx, data, len);
    return true;
}

// A guest's MIDI send, to the host (clockwork_event_sink.h,
// clockwork_sink_set_host_emit): the bytes and the port as one message,
// "/clockwork/midi/sink/send ,sb <port> <bytes>", forwarded with the send's
// time exactly as a verb is. Origin 0: a sink has no caller to answer, and on
// the web the one client hears everything the egress carries. The host's
// front opens the port on demand — opening a sink onto a port opens the port,
// as it does natively — and hands the browser the bytes with the time.
static int clockwork_sink_host_emit(uint32_t kind, const char* target,
                                    const uint8_t* bytes, uint32_t len, int64_t when) {
    if (kind != kClockworkSinkMidi || !target || !bytes || len == 0) return 0;
    static std::array<char, 64 + 256 + 66560> element;
    try {
        osc::OutboundPacketStream ps(element.data(), element.size());
        ps << osc::BeginMessage(CLOCKWORK_SYS("midi/sink/send"))
           << target
           << osc::Blob(bytes, static_cast<osc::osc_bundle_element_size_t>(len))
           << osc::EndMessage;
        forward_to_host(0, when, reinterpret_cast<const uint8_t*>(ps.Data()),
                        static_cast<uint32_t>(ps.Size()));
        return 1;
    } catch (...) {
        return 0;   // would not encode (a port name past all reason): a drop, counted by the sink
    }
}

// /clockwork/asset/commit ,iiiiiif id kind offset bytes channels frames rate
// (dsp_api.h, "Assets"). Audio thread. Every check happens before the guest is
// touched, and every refusal names the id and says why.
bool clockwork_asset_route(void* /*routeCtx*/, const void* callCtx,
                           const uint8_t* data, std::size_t len) {
    using namespace engine;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const uint32_t origin = cc ? cc->sourceId : 0;
    const char* verb = reinterpret_cast<const char*>(data) + CLOCKWORK_SYS_PREFIX_LEN;
    if (std::strcmp(verb, "asset/commit") != 0) {
        // Claimed by prefix, not a verb of this layer: refuse as the sys route does.
        handle_clockwork_sys_osc(data, static_cast<uint32_t>(len),
            [origin](const uint8_t* d, uint32_t n) { emit_osc_out(origin, d, n); },
            nullptr, arenaInstanceGeneration(shared_memory));
        return true;
    }
    int32_t id = -1;
    auto refuse = [&](const char* why) {
        asset_emit(origin, CLOCKWORK_SYS("asset/refused"), static_cast<uint32_t>(id), why);
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
    if (id < 0 || static_cast<uint32_t>(id) == kAssetTombstone) return refuse("bad id");
    if (!g_dsp) return refuse("no guest");
    const uint8_t* assets = static_cast<const uint8_t*>(g_clockwork_config.assets);
    uint64_t lane = g_clockwork_config.assets_bytes;
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
    if (asset_lookup(asset.id)) return refuse("id is still held; release it first");
    AssetSlot* slot = asset_insert(asset.id, origin);
    if (!slot) return refuse("too many assets held");
    // All of it, for an instance built after this one (hand_over_assets).
    slot->offset      = static_cast<uint32_t>(offset);
    slot->byte_count  = asset.byte_count;
    slot->channels    = asset.channels;
    slot->frames      = asset.frames;
    slot->sample_rate = asset.sample_rate;
    slot->kind        = static_cast<uint8_t>(asset.kind);
    int rc;
    { GuestCallScope guest(GuestCall::Processing); rc = dsp_asset(g_dsp, &asset); }
    if (rc != 0) {
        asset_remove(slot);
        return refuse("the guest refused it");
    }
    asset_emit(origin, CLOCKWORK_SYS("asset/committed"), asset.id, nullptr);
    return true;
}

// The DSP's route: everything the predicate did not claim, handed over
// untouched. One call for a message and a bundle alike — dsp_osc takes "one
// OSC message or bundle" and the DSP decides what that means.
//
// `when` is the message's timetag and `origin` the token to answer on; both
// travel with the call, so nothing here builds a reply object or knows how a
// reply gets back.
bool clockwork_dsp_default_route(void* /*routeCtx*/, const void* callCtx,
                          const uint8_t* data, std::size_t len) {
    using namespace engine;
    if (!g_dsp) return true;
    auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    // blockTime was already on the context and simply was not being passed:
    // without it the DSP can only difference against the block it last
    // RENDERED, which is one behind the block this message is for.
    GuestCallScope guest(GuestCall::Processing);
    dsp_osc(g_dsp, data, static_cast<uint32_t>(len),
            cc ? cc->when : 1, cc ? cc->sourceId : 0,
            cc ? cc->blockTime : 0);
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
// only — every caller below already holds PortCtlLock or is inside
// init_memory.
namespace {

ClockworkChannelMapState* channelMap() {
    if (!shared_memory) return nullptr;
    return reinterpret_cast<ClockworkChannelMapState*>(shared_memory + CHANNEL_MAP_START);
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

void channel_map_set_device(uint32_t in_width, uint32_t out_width) {
    ClockworkChannelMapState* m = channelMap();
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

void channel_map_bind(ClockworkPort port, uint32_t first) {
    ClockworkChannelMapState* m = channelMap();
    if (!m || port == CLOCKWORK_PORT_NONE) return;
    const uint32_t dir   = static_cast<uint32_t>(clockwork_port_direction(port));
    const uint32_t count = clockwork_port_channels(port);
    const char*    name  = clockwork_port_name(port);

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
        clockwork_log("[channel-map] more than %u streams; '%s' is bound but not described",
                CHANNEL_MAP_MAX_STREAMS, name ? name : "?");
        return;
    }
    std::atomic<uint32_t>* side = channel_side(m, dir);
    const uint32_t word = channelWord(CLOCKWORK_CH_PORT, slot);
    for (uint32_t c = 0; c < count && first + c < CLOCKWORK_MAX_CHANNELS; ++c)   // NOLINT(modernize-loop-convert): the index is the channel
        side[first + c].store(word, std::memory_order_release);
}

void channel_map_unbind(ClockworkPort port) {
    ClockworkChannelMapState* m = channelMap();
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

extern "C" const ClockworkChannelMapState* clockwork_channel_map(void) {
    return channelMap();
}

extern "C" int clockwork_port_bus_attach(ClockworkPort port, uint32_t first_channel) {
    if (port == CLOCKWORK_PORT_NONE || !clockwork_port_is_open(port)) return 0;
    PortCtlLock lock;   // control side only; the audio thread never waits
    // Already attached: refuse rather than silently move it. Two entries for
    // one port would read it twice a block and the second read would be
    // silence, which is a bug that sounds like a fault in the endpoint.
    for (auto& b : g_port_bus)
        if (b.port.load(std::memory_order_acquire) == port) return 0;
    for (auto& b : g_port_bus) {
        if (b.port.load(std::memory_order_acquire) != CLOCKWORK_PORT_NONE) continue;
        // Range first, then the MAP, then the port. The store that publishes
        // the entry is still the last one, so the audio thread cannot see a
        // port with a stale channel range; and the map describes the channels
        // BEFORE the binding goes live, so a channel is never carrying audio
        // it cannot account for.
        b.first_channel = first_channel;
        channel_map_bind(port, first_channel);
        b.port.store(port, std::memory_order_release);
        return 1;
    }
    return 0;   // table full
}

extern "C" void clockwork_port_bus_detach(ClockworkPort port) {
    if (port == CLOCKWORK_PORT_NONE) return;
    PortCtlLock lock;
    for (auto& b : g_port_bus) {
        if (b.port.load(std::memory_order_acquire) != port) continue;
        // Unpublish first, announce second: the DSP is never told a stream is
        // gone while the block that says so is still handing it that stream's
        // audio. first_channel is left alone — the entry is unpublished, and
        // the next attach writes it before publishing again.
        b.port.store(CLOCKWORK_PORT_NONE, std::memory_order_release);
        channel_map_unbind(port);
        return;
    }
}

extern "C" void clockwork_port_bus_detach_all(void) {
    PortCtlLock lock;
    for (auto& b : g_port_bus) {
        const ClockworkPort p = b.port.exchange(CLOCKWORK_PORT_NONE, std::memory_order_acq_rel);
        if (p != CLOCKWORK_PORT_NONE) channel_map_unbind(p);
    }
}

extern "C" uint32_t clockwork_port_bus_attached(void) {
    uint32_t n = 0;
    for (auto& b : g_port_bus)
        if (b.port.load(std::memory_order_acquire) != CLOCKWORK_PORT_NONE) ++n;
    return n;
}
