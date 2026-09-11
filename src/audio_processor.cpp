// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

#include "audio_processor.h"
#include "clockwork_product.h"

#include "audio_config.h"
#include "lanes/lanes.h"    // clockwork_reserve_lanes / clockwork_lane_base, defined below

#include <atomic>

namespace {
// ─── Reserved lanes ──────────────────────────────────────────────────────────
// Channels clockwork keeps ABOVE the device, for its own routing. A hosted plugin
// track needs a channel the DSP can write a send to and one it can read a return
// from, and neither may reach a speaker or arrive from a microphone. So the DSP
// is built wider than the device, the backend copies only the channels it has,
// and the block above is clockwork's alone. Both directions widen by the same
// amount so a lane index means the same thing either way.
//
// The base is stable: 32, or the first multiple of 8 above a wider device. Stable
// matters — a lane's index is published to clients as the channel they route to,
// and a base that moved with every device switch would invalidate every route
// across a cold swap.
std::atomic<int64_t> g_block_osc_time{0};   // this block's start, in OSC time

uint32_t g_reserved_lanes = 0;    // set before boot; read at every init_memory
uint32_t g_lane_base      = 0;    // published after init_memory; 0 = none

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
#include "dsp_api.h"  // the seven-call contract with the DSP
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

// The composition-root ClockworkClock for a worklet self-clock host (WASM + the
// self-driven device). clockwork_clock_wasm_init binds the SAB region + the worklet
// TimeSource onto g_active_clockwork_clock; clockworkClock() is the host-owned instance.
// (External-clock hosts — native/JUCE — own their ClockworkClock elsewhere and never
// reach this.) Capability-gated, not __EMSCRIPTEN__, so WASM and the device share
// ONE codepath that every worklet target's tests exercise.
#if CLOCKWORK_WORKLET_CLOCK
extern "C" void clockwork_clock_wasm_init(ClockworkClockState* clockwork_clock_state,
                                      const double* ntp_start_time_ptr,
                                      const std::atomic<int32_t>* drift_offset_ptr,
                                      const std::atomic<int32_t>* global_offset_ptr);

static ClockworkClock& clockworkClock() {
    static ClockworkClock instance;
    return instance;
}
#endif
#include <emscripten/webaudio.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <string>

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

// Pre-allocated heap for RT-safe allocations
#include "clockwork_heap.h"
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
// shm_scope_stream.hpp): anchors scope-stream writes on the sample clock.
std::atomic<uint64_t> g_engine_frames{0};

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
            clockState);
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
    PerformanceMetrics* metrics = nullptr
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
    // TOTAL_BUFFER_SIZE is computed in shared_memory.h from the build's sizing flags
    // — deliberately not restated as a number here.
    //
    // 16-byte aligned for shm_audio_buffer slots at SHM_AUDIO_START; JS Float64Array
    // needs only 8. CLOCKWORK_COLD_BSS puts the whole arena in bulk RAM on tiered
    // targets, keeping scarce fast RAM for the audio path (platform.h).
    alignas(16) CLOCKWORK_COLD_BSS uint8_t ring_buffer_storage[TOTAL_BUFFER_SIZE];

    static_assert(TOTAL_BUFFER_SIZE <= sizeof(ring_buffer_storage),
                  "Buffer layout exceeds allocated storage!");

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

    // IN-ring flush request (native purge: sleep/wake recovery, cold swap;
    // callable from any thread). Holds the in_sequence snapshot taken at
    // request time, -1 = no request. The drain discards pending frames whose
    // seq predates the snapshot and dispatches everything newer, so the flush
    // removes exactly what was queued at request time. The ring cursors have
    // fixed owners — producers advance head under in_write_lock, the drain
    // owns tail — so the discard runs on the consuming thread via the normal
    // consume path; no cursor is written from the requesting thread.
    std::atomic<int64_t> g_in_flush_below{-1};

    // Audio-thread-only discard state armed from g_in_flush_below: while
    // active, frames with seq before the threshold are consumed undispatched.
    bool     g_in_discard_active = false;
    uint32_t g_in_discard_below  = 0;

    void*  g_guest_arena       = nullptr;
    size_t g_guest_arena_bytes = 0;

    // The guest's arenas when CLOCKWORK reserved them rather than the host: a
    // host that passed no region (headless native, an embedded board) and a
    // guest that declared wants (DspInfo::arena_bytes_wanted) meet here, and
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
    // `shared_memory` here instead of the process-local ring_buffer_storage, so
    // the entire shared_memory.h blob — rings, control, metrics, node-tree,
    // audio taps and scope — lives in the segment and is observable
    // cross-process.
    uint8_t* g_external_segment = nullptr;

    uint8_t* shared_memory = nullptr;
    ControlPointers* control = nullptr;
    PerformanceMetrics* metrics = nullptr;
    double* ntp_start_time = nullptr;
    std::atomic<int32_t>* drift_offset = nullptr;
    std::atomic<int32_t>* global_offset = nullptr;
    bool memory_initialized = false;

    // Defined beside clockwork_log; see the note there for why boot failures
    // cannot use it.
    static void clockwork_boot_fail(const char* fmt, ...);
    struct Dsp* g_dsp = nullptr;

    // The session's audio geometry. CLOCKWORK CHOOSES THESE and writes them into
    // DspConfig, so it never asks the DSP what block size or channel count it
    // settled on.
    uint32_t g_block_size   = 0;
    uint32_t g_out_channels = 0;
    uint32_t g_in_channels  = 0;
    double   g_dsp_sample_rate = 0.0;

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
            float* ch[clockwork::kMaxChannels];
            for (uint32_t c = 0; c < n; ++c)
                ch[c] = static_audio_in + static_cast<size_t>(first + c) * g_block_size;
            // The return value is deliberately ignored: the shortfall is
            // already silence in the buffer and already counted on the port,
            // and there is nothing useful the audio thread could do with it.
            (void)clockwork_port_read(p, ch, n, frames);
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
            const float* ch[clockwork::kMaxChannels];
            for (uint32_t c = 0; c < n; ++c)
                ch[c] = static_audio_bus + static_cast<size_t>(first + c) * g_block_size;
            (void)clockwork_port_write(p, ch, n, frames);
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

    // True only while this thread is inside process_audio(). emit_debug_osc()
    // reads it to keep the RT-out ring single-writer: the audio thread logs to
    // the lock-free RT-out ring; any other thread (watchdog, recovery, boot on
    // native) routes to the locked NRT-out ring, so RT-out never gets a second
    // concurrent writer. Set via AudioThreadScope at the top of process_audio.
    thread_local bool t_on_audio_thread = false;
    struct AudioThreadScope {
        AudioThreadScope()  { t_on_audio_thread = true;  }
        ~AudioThreadScope() { t_on_audio_thread = false; }
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
    EMSCRIPTEN_KEEPALIVE
    int get_ring_buffer_base() {
        if (arenaHeader(ring_buffer_storage)->magic != CLOCKWORK_ARENA_MAGIC)
            writeArenaHeader(ring_buffer_storage);
        return reinterpret_cast<int>(ring_buffer_storage);
    }
#endif

    // Real-pointer base of the unified shared-memory arena, valid on every
    // runtime (the int-returning get_ring_buffer_base() above is WASM-only and
    // would truncate a 64-bit native pointer). Returns the arena base set by
    // init_memory(): ring_buffer_storage locally, or the public segment when an
    // external arena was supplied. Used by the scope-stream claim path.
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
        if (!memory_initialized || !control || !shared_memory || !osc || len == 0)
            return false;
        const bool useRtOut =
            t_on_audio_thread ||
            !g_nrt_egress_drained.load(std::memory_order_relaxed);
        if (useRtOut) {
            return ::ring_buffer_write(
                shared_memory + OUT_BUFFER_START, OUT_BUFFER_SIZE,
                &control->out_head, &control->out_tail, &control->out_sequence,
                route, origin, osc, len, &control->status_flags, metrics);
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

    // Give back bytes clockwork allocated (dsp_api.h). Only the pointer
    // crosses: what the bytes were is the guest's business, and was the whole
    // problem with the OSC verb this replaces.
    static void dsp_host_free_bytes(void* /*ctx*/, void* ptr) {
        if (!ptr) return;
#ifdef __EMSCRIPTEN__
        // On web the guest's large blocks come from the CLIENT's pool inside
        // the guest region, not from clockwork_heap — the client frees them when it
        // sees the guest's own notification. Handing one of those to
        // clockwork_heap_free would free a pointer this allocator never issued.
        (void)ptr;
#else
        clockwork_heap_free(ptr);
#endif
    }


    // ── Assets (dsp_api.h, "Assets") ───────────────────────────────────────
    //
    // The audio thread's half of the facility: check a committed range against
    // the lane, hand the guest a pointer, remember who committed each id so a
    // release can be told to the right client. A fixed table, probed by id —
    // no allocation on this thread, and a full table is a refusal.
    struct AssetSlot { uint32_t id; uint32_t origin; bool used; };
    constexpr uint32_t kAssetSlots = 4096;   // power of two
    AssetSlot g_assets[kAssetSlots] = {};

    AssetSlot* asset_lookup(uint32_t id) {
        for (uint32_t i = 0, h = id * 2654435761u; i < kAssetSlots; ++i, ++h) {
            AssetSlot& s = g_assets[h & (kAssetSlots - 1)];
            if (s.used && s.id == id) return &s;
            if (!s.used) return nullptr;
        }
        return nullptr;
    }
    AssetSlot* asset_insert(uint32_t id, uint32_t origin) {
        for (uint32_t i = 0, h = id * 2654435761u; i < kAssetSlots; ++i, ++h) {
            AssetSlot& s = g_assets[h & (kAssetSlots - 1)];
            if (!s.used) { s = AssetSlot{ id, origin, true }; return &s; }
        }
        return nullptr;
    }
    // Removal keeps the probe chains intact: the slot is marked as a tombstone
    // by leaving `used` true with an id no client can commit (the table is
    // rebuilt whenever the guest is destroyed, which also clears tombstones).
    constexpr uint32_t kAssetTombstone = 0xFFFFFFFFu;
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

    // DspHost::asset_release — the guest is done; the committing client hears.
    static int dsp_host_asset_release(void* /*ctx*/, uint32_t id) {
        AssetSlot* s = asset_lookup(id);
        if (!s) return 0;
        const uint32_t origin = s->origin;
        asset_remove(s);
        asset_emit(origin, CLOCKWORK_SYS("asset/released"), id, nullptr);
        return 1;
    }

    // Every asset dies with the instance that held it: tell each committer,
    // then forget the table. Called before the guest is freed.
    void assets_release_all() {
        for (AssetSlot& s : g_assets) {
            if (s.used && s.id != kAssetTombstone)
                asset_emit(s.origin, CLOCKWORK_SYS("asset/released"), s.id, nullptr);
            s = AssetSlot{};
        }
    }

    const DspHost g_dsp_host = { /*ctx*/ nullptr, &dsp_host_emit_osc, &dsp_host_log,
                                 &dsp_host_open_sink, &dsp_host_send_sink,
                                 &dsp_host_free_bytes, &dsp_host_asset_release };

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
    // held and it cannot be delivered on time, so it is refused — and refused
    // OUT LOUD, naming the switch, because the failure mode this option must
    // not have is a verb that silently stops working. Counted into the same
    // drop metric as a pool-full refusal, since to a client both mean "
    // clockwork did not take it".
    //
    // Rate-limited the way the LATE/SHED logs are: a client that schedules in
    // a loop would otherwise fill the debug stream with one identical line per
    // message, which buries the first one — the only one anybody reads.
    static inline void refuse_schedule_verb() {
        increment_scheduler_drop_metric();
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

    // The overload law: an event more than SCHEDULER_SHED_LATE_MS late is dropped
    // rather than played, because a backlog played late is worse than a backlog not
    // played. 0 (the default) never sheds.
    static inline void note_shed(double late_ms) {
        increment_scheduler_drop_metric();
        static std::atomic<uint32_t> shedLog{0};
        if (shedLog.fetch_add(1, std::memory_order_relaxed) % 64 == 0)
            clockwork_log("SHED: %.0fms late, dropped (overload)", late_ms);
    }

    // Clear the event scheduler (everything pending for the DSP, plus outbound
    // MIDI/OSC), and request that the audio thread discard everything pending in the
    // IN ring as of NOW: frames sequenced before this snapshot are dropped, later
    // ones dispatch normally. Callable from any thread — the caller only requests,
    // the consuming thread applies (g_in_flush_below).
    //
    // The IN ring is drained separately by the JS worklet in its message handler,
    // eagerly, so stale messages are discarded before the ack is sent.
    void clockwork_ingress_flush_request() {
        if (!memory_initialized || !control) return;
        g_in_flush_below.store(
            control->in_sequence.load(std::memory_order_acquire),
            std::memory_order_release);
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
        static std::atomic<uint32_t> noBackendLog{0};
        if (noBackendLog.fetch_add(1, std::memory_order_relaxed) < 16) {
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
    // Returns true when the event was shed and must not be delivered.
    //
    // The immediacy check is `== 0 || == 1`, NOT `<= 1`: a present-day OSC
    // timetag is huge and, taken as int64, has its sign bit set — i.e. it is
    // NEGATIVE. `<= 1` would misclassify every real scheduled event as
    // immediate and skip its accounting entirely.
    static bool note_lateness(int64_t when, int64_t blockTime) {
        if (when == 0 || when == 1) return false;   // OSC "immediate" sentinels

        const double time_diff_ms =
            (static_cast<double>(when - blockTime) / 4294967296.0) * 1000.0;

        if (SCHEDULER_SHED_LATE_MS > 0 &&
            time_diff_ms < -static_cast<double>(SCHEDULER_SHED_LATE_MS)) {
            note_shed(-time_diff_ms);
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
        static int64_t last_late_log_osc = 0;
        const bool second_elapsed =
            (blockTime - last_late_log_osc) >= static_cast<int64_t>(4294967296LL);
        if (late_now == 1 || late_now % 100 == 0 || second_elapsed) {
            last_late_log_osc = blockTime;
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
            if (!sp.ok) {                       // malformed: counted, not silent
                increment_scheduler_drop_metric();
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
            refuse_schedule_verb();
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
    // ring_buffer_storage (WASM, and headless native with no shm). Either way
    // every region is addressed by its shared_memory.h offset from this base.
    ClockworkConfig g_clockwork_config;

    int clockwork_arena_meets(uint32_t fast_bytes, uint32_t bulk_bytes, int has_bulk_tier,
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
                                uint32_t verbosity, void* guest_arena,
                                uint32_t guest_arena_bytes,
                                uint32_t arena_bytes,
                                const void* inbox, uint32_t inbox_bytes,
                                void* outbox, uint32_t outbox_bytes) {
        g_clockwork_config.sample_rate        = sample_rate;
        g_clockwork_config.block_size         = block_size;
        g_clockwork_config.input_channels     = input_channels;
        g_clockwork_config.output_channels    = output_channels;
        g_clockwork_config.verbosity          = verbosity;
        g_clockwork_config.guest_arena        = guest_arena;
        g_clockwork_config.guest_arena_bytes  = guest_arena_bytes;
        g_clockwork_config.inbox              = inbox;
        g_clockwork_config.inbox_bytes        = inbox_bytes;
        g_clockwork_config.outbox             = outbox;
        g_clockwork_config.outbox_bytes       = outbox_bytes;
        g_clockwork_config.arena_bytes        = arena_bytes;
    }

    void init_memory() {
        const double sample_rate = g_clockwork_config.sample_rate;
        // The arena base is fixed for the engine's whole threaded life: native
        // binds g_external_segment before any engine thread starts (and nulls
        // it only after they are all joined); the WASM arena is the static
        // ring_buffer_storage. So on a cold-swap rebuild the pointer family
        // below is already correct — and skipping the re-assignment is what
        // makes this call safe while unquiesced producers (transport ingress,
        // clockwork_log from any worker) read the same globals lock-free: pointers
        // are only ever written here with no reader threads alive (first
        // boot, or re-boot after teardown_memory).
        uint8_t* base = g_external_segment ? g_external_segment : ring_buffer_storage;
        if (shared_memory != base) {
            shared_memory = base;
            // The table every reader takes the layout from, first, so nothing
            // is ever placed in an arena that does not describe itself. A
            // segment's creator wrote the same bytes already; the wasm arena
            // and an embedder's memory get theirs here.
            writeArenaHeader(shared_memory);
            control = reinterpret_cast<ControlPointers*>(shared_memory + CONTROL_START);
            // Metrics live in the arena at their fixed offset — which is the
            // public segment when one was supplied, so external observers read
            // them with no redirect.
            metrics = reinterpret_cast<PerformanceMetrics*>(shared_memory + METRICS_START);

            ntp_start_time = reinterpret_cast<double*>(shared_memory + NTP_START_TIME_START);
            drift_offset = reinterpret_cast<std::atomic<int32_t>*>(shared_memory + DRIFT_OFFSET_START);
            global_offset = reinterpret_cast<std::atomic<int32_t>*>(shared_memory + GLOBAL_OFFSET_START);

            // A COLD BOOT CLEARS THE GUEST'S PERSISTENT REGION. A REBUILD DOES NOT.
            //
            // This branch is the difference: it runs on a first boot, and again
            // after teardown_memory (which nulls the base), but a cold-swap
            // rebuild takes the skip path above because the arena is unchanged.
            // That is exactly the line DspConfig::persistent needs — survive a
            // rebuild, start empty on a fresh engine.
            //
            // Nobody but the guest can draw it. The guest cannot tell "restored
            // after a device switch" from "a previous engine in this process
            // left its table behind", and guessing wrong is not harmless: an
            // engine that never asked for /notify inherited a registration and
            // started answering a client that had not spoken. Clockwork knows,
            // because it owns the arena's lifetime, so clockwork decides.
            std::memset(shared_memory + GUEST_PERSIST_START, 0, GUEST_PERSIST_SIZE);

            // THE CLOCK REGION, ON EVERY BUILD, AND ONLY ON A COLD BOOT.
            //
            // NOT under the worklet-clock guard. A host that binds no
            // ClockworkClock — anything driving lanes.h directly, which is the
            // whole minimal-host contract — would then get DspConfig::clock
            // pointing at zeros. That is WORSE than the NULL dsp_api.h allows
            // for: readClockworkClock answers a null pointer with a sane
            // default, and an all-zero region with a tempo of zero, so a guest
            // asking for the beat divides by it.
            //
            // And it belongs in this branch rather than after it, because
            // running it on a rebuild would reset the session tempo every time
            // a device switched.
            ClockworkClockState::initDefaults(
                *reinterpret_cast<ClockworkClockState*>(
                    shared_memory + CLOCK_STATE_START));
        }

        // NTP_START_TIME is write-once from JavaScript after AudioContext starts —
        // don't touch it.
        drift_offset->store(0, std::memory_order_relaxed);
        global_offset->store(0, std::memory_order_relaxed);

#if CLOCKWORK_WORKLET_CLOCK
        // Hand SAB pointers to the composition-root ClockworkClock at boot — shared by
        // every worklet self-clock host (WASM + the self-driven device). Publish the
        // instance first: clockwork_clock_wasm_init binds the SAB region + the worklet
        // clock onto whatever g_active_clockwork_clock points at. (External-clock hosts —
        // native/JUCE — bind their own ClockworkClock elsewhere, never here.)
        ClockworkClockState* clockwork_clock_state =
            reinterpret_cast<ClockworkClockState*>(shared_memory + CLOCK_STATE_START);

        g_active_clockwork_clock.store(&clockworkClock(), std::memory_order_release);
        clockwork_clock_wasm_init(clockwork_clock_state, ntp_start_time, drift_offset, global_offset);
#endif

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

        // Fresh ring epoch. Serialised against the IN / NRT-out producers that
        // keep running through a cold-swap rebuild (details in
        // clockwork_lanes_reset_rings). Note the IN writer lock is NOT re-zeroed: a
        // producer may hold it right now, and its unlocked state is exactly
        // the 0 its release store leaves behind.
        clockwork_lanes_reset_rings();
        control->status_flags.store(STATUS_OK, std::memory_order_relaxed);

        // Ring sequences restarted → restart the lanes drains' gap tracking,
        // and the IN drain's alongside. A flush requested against the old
        // ring must not carry over and discard fresh writes.
        clockwork_lanes_reset_drains();
        g_in_drain.lastSeq = -1;
        g_in_flush_below.store(-1, std::memory_order_relaxed);
        g_in_discard_active = false;

        metrics->process_count.store(0, std::memory_order_relaxed);
        metrics->messages_processed.store(0, std::memory_order_relaxed);
        metrics->messages_dropped.store(0, std::memory_order_relaxed);
        metrics->scheduler_queue_depth.store(0, std::memory_order_relaxed);
        metrics->scheduler_queue_max.store(0, std::memory_order_relaxed);
        metrics->scheduler_queue_dropped.store(0, std::memory_order_relaxed);
        metrics->messages_sequence_gaps.store(0, std::memory_order_relaxed);
        metrics->scheduler_lates.store(0, std::memory_order_relaxed);

        metrics->scheduler_max_late_ms.store(0, std::memory_order_relaxed);
        metrics->scheduler_last_late_ms.store(0, std::memory_order_relaxed);
        metrics->scheduler_last_late_tick.store(0, std::memory_order_relaxed);

        // The guest's publish window: zeroed, and nothing more. What shape the
        // bytes take is the guest's, so it lays the region out itself at
        // dsp_new — clockwork cannot pre-fill a structure it does not know,
        // and a wipe that assumed one would be a guess about its empty value.
        memset(shared_memory + SHM_WINDOW_START, 0, SHM_WINDOW_SIZE);

        // The audio taps: out and in, formatted here at the device's live
        // channel count (up to the slot's ceiling) and written by the tick
        // from then on. A tap for a direction the device has not got is
        // formatted with no channels and left disabled, so a reader sees
        // "nothing arrives" rather than a ring of zeros. Re-formatted on
        // every boot: a device switch is a new stream, and a reader's
        // cursor starts again with it.
        shm_audio_buffer* slots = reinterpret_cast<shm_audio_buffer*>(
            shared_memory + SHM_AUDIO_START);
        memset(static_cast<void*>(slots), 0, SHM_AUDIO_SLOTS * sizeof(shm_audio_buffer));
        const auto format_tap = [&](uint32_t slot, uint32_t device_channels) {
            const uint32_t ch = device_channels < SHM_AUDIO_CHANNELS ? device_channels : SHM_AUDIO_CHANNELS;
            if (ch > 0)
                shm_audio_buffer_writer(&slots[slot]).activate(ch, static_cast<uint32_t>(sample_rate), SHM_AUDIO_FRAMES);
        };
        format_tap(SHM_AUDIO_OUT_SLOT, g_clockwork_config.output_channels);
        format_tap(SHM_AUDIO_IN_SLOT,  g_clockwork_config.input_channels);
        g_shm_audio_buffers = slots;

        // Event sinks: the size classes each kind is made of, from the memory
        // profile. Installed before any sink can be opened, so the DSP's own
        // opens get the shape too.
        install_sink_profiles();

        // Scope streams: global header (geometry for cross-process observers)
        // then the slot array. Slots start zeroed (state=free); the producer
        // claims and activates them on demand (rust/clockwork-scope).
        {
            uint8_t* scopeBase = shared_memory + SHM_SCOPE_START;
            memset(scopeBase, 0, SHM_SCOPE_TOTAL_SIZE);
            reinterpret_cast<std::atomic<uint32_t>*>(scopeBase + 0)->store(
                SHM_SCOPE_MAX_SCOPES, std::memory_order_relaxed);   // maxScopes
            reinterpret_cast<std::atomic<uint32_t>*>(scopeBase + 4)->store(
                0, std::memory_order_relaxed);                      // activeCount
            reinterpret_cast<std::atomic<uint32_t>*>(scopeBase + 8)->store(
                SHM_SCOPE_RING_FRAMES, std::memory_order_relaxed);  // ringFrames
            reinterpret_cast<std::atomic<uint32_t>*>(scopeBase + 12)->store(
                0, std::memory_order_relaxed);                      // version
            // The engine's track taps, in the block: slots only, start free.
            if (SHM_TRACK_TAPS_SIZE)
                memset(shared_memory + SHM_TRACK_TAPS_START, 0, SHM_TRACK_TAPS_SIZE);
        }

        // Enable clockwork_log. Write-once per mapping (like the pointer family
        // above): on a rebuild this is already true and producers read it
        // concurrently, so don't store over it.
        if (!memory_initialized)
            memory_initialized = true;

#if CLOCKWORK_SHM_AUDIO_SECONDS != 1
        // Test-profile arena: capture rings are oversized, so every offset
        // after them differs from a production reader's expectations. Loud
        // banner so a test-sized engine staged as production is obvious.
        clockwork_log("[memory] WARNING: test memory profile "
               "(CLOCKWORK_SHM_AUDIO_SECONDS=%d) — arena layout differs from "
               "production readers; do not ship this binary",
               (int)CLOCKWORK_SHM_AUDIO_SECONDS);
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

        // The window the DSP may publish through: the node-tree mirror region.
        // Clockwork zeroes it at boot, hands it over, and thereafter only
        // publishes the bytes — which is exactly how a node-tree mirror
        // survives in a harness that does not know what a node tree is.
        config.shm_window       = shared_memory + SHM_WINDOW_START;
        config.shm_window_bytes = SHM_WINDOW_SIZE;
        // Deliberately NOT zeroed here, and that is the entire feature: this
        // region is how a guest survives its own destruction, so a rebuild must
        // find exactly what the last instance left. See DspConfig::persistent.
        config.persistent       = shared_memory + GUEST_PERSIST_START;
        config.persistent_bytes = GUEST_PERSIST_SIZE;

        config.deterministic_seed = clockwork_deterministic_seed;

        // The staging buffers are fixed-size, so the configuration cannot ask
        // for more than they hold. A bad block size falls back to the default
        // rather than rendering past the end of the arena.
        if (config.block_size == 0 || config.block_size > clockwork::kMaxBlockSize)
            config.block_size = clockwork::kDefaultBlockSize;
        if (config.max_output_channels > clockwork::kMaxChannels)
            config.max_output_channels = clockwork::kMaxChannels;
        if (config.max_input_channels > clockwork::kMaxChannels)
            config.max_input_channels = clockwork::kMaxChannels;

        // Reserved lanes, above the device (see the boundary at the top of this
        // file). Widen both directions to hold them; the device backend copies
        // only what the device has, so the extra channels never reach hardware.
        // Recomputed on every build so a cold swap onto a wider device moves
        // the base up rather than letting the device overlap the lanes.
        g_lane_base = 0;
        if (g_reserved_lanes > 0) {
            const uint32_t base = lane_base_for(config.max_input_channels,
                                                config.max_output_channels);
            const uint32_t top  = base + g_reserved_lanes;
            if (top <= clockwork::kMaxChannels) {
                g_lane_base = base;
                if (config.max_output_channels < top) config.max_output_channels = top;
                if (config.max_input_channels  < top) config.max_input_channels  = top;
            } else {
                clockwork_log("WARNING: %u reserved lanes above channel %u do not fit "
                        "kMaxChannels=%d; no lanes reserved",
                        g_reserved_lanes, base, (int)clockwork::kMaxChannels);
            }
        }
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
        // way. Adding it to shared_memory — ring_buffer_storage, a static
        // array at whatever address the linker picked — put the region exactly
        // ringBufferBase bytes too high, overrunning into the buffer pool
        // above it. That only shows when the static layout moves, because the
        // size of the overrun IS ring_buffer_storage's address.
        //
        // TWO TIERS, ONE RULE. `arena` is the fast tier and `arena_bulk` the
        // bulk one (dsp_api.h). What the guest needs of each is declared
        // statically (DspInfo::arena_bytes_wanted / arena_bulk_bytes_wanted)
        // and checked HERE, before dsp_new, by clockwork_arena_meets — and a
        // want that cannot be met refuses the boot with a reason rather than
        // spilling a hot pool into slow RAM with nothing reported. See the
        // rule's comment in audio_processor.h.
        //
        // Where the bytes come from, in order:
        //   fast: the span the host reserved (web: the JS layout; native:
        //         ClockworkEngine's block; the test fixture: a static array),
        //         else — when the host passed none and the guest wants some —
        //         a span clockwork takes from clockwork::mem's Fast tier once
        //         and keeps across rebuilds.
        //   bulk: only on a tiered device (CLOCKWORK_HAS_TIERED_MEMORY), from
        //         clockwork::mem's Bulk tier, sized by the guest's want or the
        //         profile's CLOCKWORK_ARENA_BULK_BYTES. Everywhere else NULL/0:
        //         one kind of memory means nothing to place differently, and
        //         the fast arena is checked to hold both wants instead.
        uint32_t fast_wanted = 0, bulk_wanted = 0;
        if (const DspInfo* info = dsp_describe()) {
            fast_wanted = info->arena_bytes_wanted;
            bulk_wanted = info->arena_bulk_bytes_wanted;
        }

        g_guest_arena       = nullptr;
        g_guest_arena_bytes = 0;
        if (g_clockwork_config.guest_arena && g_clockwork_config.guest_arena_bytes) {
            g_guest_arena       = g_clockwork_config.guest_arena;
            g_guest_arena_bytes = g_clockwork_config.guest_arena_bytes;
        } else if (fast_wanted || bulk_wanted) {
            // No host span, but a guest with a figure: reserve it ourselves.
            // Fast placement, no spill — the whole point of the want is that
            // its tier is not negotiable. Sized to the want, and on a
            // single-region host to both wants, since bulk has nowhere else to
            // go there. Kept across rebuilds; a bigger want after a rebuild
            // cannot happen (dsp_describe is static), so a span that once fit
            // still fits.
#if CLOCKWORK_HAS_TIERED_MEMORY
            const size_t need = fast_wanted;
#else
            const size_t need = (size_t)fast_wanted + (size_t)bulk_wanted;
#endif
            if (need && !g_owned_fast) {
                g_owned_fast = clockwork::mem::alloc(clockwork::mem::Tier::Fast, need,
                                                     /*allow_spill=*/false);
                g_owned_fast_bytes = g_owned_fast ? need : 0;
            }
            g_guest_arena       = g_owned_fast;
            g_guest_arena_bytes = g_owned_fast_bytes;
        }

        void*  bulk_arena       = nullptr;
        size_t bulk_arena_bytes = 0;
#if CLOCKWORK_HAS_TIERED_MEMORY
        {
            const size_t need = bulk_wanted ? bulk_wanted : (size_t)CLOCKWORK_ARENA_BULK_BYTES;
            if (need && !g_owned_bulk) {
                g_owned_bulk = clockwork::mem::alloc(clockwork::mem::Tier::Bulk, need,
                                                     /*allow_spill=*/false);
                g_owned_bulk_bytes = g_owned_bulk ? need : 0;
            }
            bulk_arena       = g_owned_bulk;
            bulk_arena_bytes = g_owned_bulk_bytes;
        }
        constexpr int kHasBulkTier = 1;
#else
        constexpr int kHasBulkTier = 0;
#endif

        {
            uint32_t short_fast = 0, short_bulk = 0;
            if (!clockwork_arena_meets((uint32_t)g_guest_arena_bytes, (uint32_t)bulk_arena_bytes,
                                       kHasBulkTier, fast_wanted, bulk_wanted,
                                       &short_fast, &short_bulk)) {
                // Refused, and loudly: the guest said what it needs and this
                // host cannot provide it in the tier it named. Booting anyway
                // would either fail inside dsp_new with a worse message or, if
                // the guest fell back to spilling, glitch at the first chord
                // instead of stopping at boot.
                clockwork_boot_fail("FATAL: guest wants %u bytes fast + %u bytes bulk; "
                        "this host offers %u fast + %u bulk (%s) — short by %u fast, %u bulk. "
                        "Raise the host's reservation (CLOCKWORK_ARENA_BYTES / "
                        "CLOCKWORK_ARENA_BULK_BYTES, or the web layout's guestMemorySize) "
                        "or lower the guest's wants.",
                        fast_wanted, bulk_wanted,
                        (unsigned)g_guest_arena_bytes, (unsigned)bulk_arena_bytes,
                        kHasBulkTier ? "tiered: each want in its own tier"
                                     : "single-region: the fast arena must hold both",
                        short_fast, short_bulk);
                control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
        }

        // Zeroed at every dsp_new, and untouched by clockwork after that. Both
        // tiers: a rebuilt guest's carve-up starts from nothing in each.
        if (g_guest_arena) {
            memset(g_guest_arena, 0, g_guest_arena_bytes);
            if (verbosity > 0)
                clockwork_log("GUEST ARENA: 0x%08x + %u KB fast",
                        (unsigned)(uintptr_t)g_guest_arena,
                        (unsigned)(g_guest_arena_bytes / 1024));
        }
        if (bulk_arena) {
            memset(bulk_arena, 0, bulk_arena_bytes);
            if (verbosity > 0)
                clockwork_log("GUEST ARENA: 0x%08x + %u KB bulk",
                        (unsigned)(uintptr_t)bulk_arena,
                        (unsigned)(bulk_arena_bytes / 1024));
        }
        config.arena            = g_guest_arena;
        config.arena_bytes      = (uint32_t)g_guest_arena_bytes;
        config.arena_bulk       = bulk_arena;
        config.arena_bulk_bytes = (uint32_t)bulk_arena_bytes;

        // The floating-point environment, as the host declared it
        // (clockwork_declare_fp_env). A worklet cannot arm a flush, so on web
        // the answer is known without asking and the declaration is ignored.
#ifdef __EMSCRIPTEN__
        config.fp_env = DSP_FP_ENV_DENORMALS_HONOURED;
#else
        config.fp_env = (int32_t)g_clockwork_config.fp_env;
#endif
        // Bulk staging is NOT zeroed: the client owns the inbox and may have
        // written into it before the guest was built.
        config.inbox        = g_clockwork_config.inbox;
        config.inbox_bytes  = g_clockwork_config.inbox_bytes;
        config.outbox       = g_clockwork_config.outbox;
        config.outbox_bytes = g_clockwork_config.outbox_bytes;

        /*
         * THE PLACEMENT ARENA, and it goes in before anything allocates.
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
            const size_t want = g_clockwork_config.arena_bytes
                              ? (size_t)g_clockwork_config.arena_bytes
                              : (size_t)CLOCKWORK_MEM_ARENA_SIZE;
            void* arena = std::malloc(want);
            if (!arena) {
                clockwork_boot_fail("FATAL: placement arena of %u MB could not be "
                              "claimed - lower memArenaSize in "
                              "js/memory_layout.js",
                              (unsigned)(want / (1024 * 1024)));
                control->status_flags.fetch_or(STATUS_WASM_ERROR,
                                               std::memory_order_relaxed);
                return;
            }
            clockwork::mem::set_arena(arena, want);
            if (verbosity > 0)
                clockwork_log("MEM ARENA: 0x%08x + %u MB",
                        (unsigned)(uintptr_t)arena,
                        (unsigned)(want / (1024 * 1024)));
        }
#endif

        // Clockwork's own heap, claimed the same way on every platform.
#ifdef __EMSCRIPTEN__
        /*
         * CLOCKWORK NEVER ALLOCATES INSIDE THE GUEST'S REGION, and in particular its
         * heap is never carved from it: the guest's own allocator starts at exactly
         * the same address, since the base handed over as DspConfig::arena is
         * the base a carve would take. Both pools then write the same bytes, and no
         * amount of resizing the region helps because the collision is at the base.
         */
        clockwork_heap_init(CLOCKWORK_HEAP_FAST_SIZE);

        /*
         * AND PROVES IT. On web the heap's backing is claimed with malloc, so where
         * it ENDS falls out of emscripten's sbrk rather than being a number anyone
         * chose, while the JS layout reserves wasmHeapSize + ringBufferReserved
         * below the guest region and assumes clockwork fits. Measured 2026-08-31 it
         * did not: the heap ran to 17.08 MB against a guest base of 14.00 MB,
         * overlapping by 3.08 MB on every boot.
         *
         * Failing loud here costs a boot; not failing costs corruption that presents
         * as anything at all.
         */
        if (g_guest_arena) {
            const uintptr_t guest_base = (uintptr_t)g_guest_arena;

            /*
             * RUNS AFTER clockwork_heap_init, and must. Placed before the
             * allocation it is meant to see, it reads sbrk(0) ahead of the single
             * largest allocation below the line and passes forever.
             *
             * sbrk first, then the narrower one naming clockwork_heap, so an
             * overrun caused by something else is still caught.
             */
            extern void* sbrk(intptr_t);
            {
                const uintptr_t heap_end = (uintptr_t)sbrk(0);
                if (heap_end > guest_base) {
                    clockwork_boot_fail("FATAL: WASM heap (sbrk=0x%08x) overlaps the guest "
                            "region (start=0x%08x) by %u bytes - reduce heap "
                            "usage or raise wasmHeapSize in js/memory_layout.js",
                            (unsigned)heap_end, (unsigned)guest_base,
                            (unsigned)(heap_end - guest_base));
                    control->status_flags.fetch_or(STATUS_WASM_ERROR,
                                                   std::memory_order_relaxed);
                    return;
                }
            }

            const uintptr_t heap_end = clockwork_heap_backing_end();
            if (heap_end > guest_base) {
                clockwork_boot_fail("FATAL: clockwork heap ends at 0x%08x but the guest "
                        "region starts at 0x%08x - overlapping by %u bytes. "
                        "Raise wasmHeapSize in js/memory_layout.js.",
                        (unsigned)heap_end, (unsigned)guest_base,
                        (unsigned)(heap_end - guest_base));
                control->status_flags.fetch_or(STATUS_WASM_ERROR,
                                               std::memory_order_relaxed);
                return;
            }

            if (verbosity > 0)
                clockwork_log("MEMORY OK: heap<0x%08x clockwork_heap<0x%08x guest=[0x%08x,0x%08x)",
                        (unsigned)(uintptr_t)sbrk(0), (unsigned)heap_end,
                        (unsigned)guest_base,
                        (unsigned)(guest_base + g_guest_arena_bytes));
        }
#else
        clockwork_heap_init(CLOCKWORK_HEAP_FAST_SIZE);
#endif

        #if CLOCKWORK_SCHEDULER
        // Build the scheduler's pools now, with the heap arranged and on the
        // control thread. See clockwork_engine_schedule() for why this is not left
        // to static initialisation.
        clockwork_sched_warm();
#endif

        // What this DSP is, and its terms. dsp_describe() is static — it
        // answers before any instance exists — so the schedule declaration is
        // read once, here, and the branch it governs is decided nowhere else.
        const char* dsp_name    = "?";
        const char* dsp_version = "?";
        if (const DspInfo* info = dsp_describe()) {
            g_dsp_holds_schedule = info->holds_schedule != 0;
            g_dsp_wants_events   = info->wants_events != 0;
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
            g_dsp = dsp_new(&config, &g_dsp_host, &err);
            if (!g_dsp) {
                clockwork_log("ERROR: engine bring-up failed: %s", err ? err : "unknown");
                control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
                return;
            }
        } catch (const std::exception& e) {
            clockwork_log("ERROR: engine bring-up threw exception: %s", e.what());
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
            return;
        } catch (...) {
            clockwork_log("ERROR: engine bring-up threw unknown exception");
            control->status_flags.fetch_or(STATUS_WASM_ERROR, std::memory_order_relaxed);
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

        // Publish cross-platform system info into the metrics struct (slots
        // 58-64). Constant for the session; written once now that the DSP
        // exists. Runs on every runtime — native's initialiseDsp() also
        // routes through init_memory().
        metrics->clockwork_version_major.store(CLOCKWORK_VERSION_MAJOR, std::memory_order_relaxed);
        metrics->clockwork_version_minor.store(CLOCKWORK_VERSION_MINOR, std::memory_order_relaxed);
        metrics->clockwork_version_patch.store(CLOCKWORK_VERSION_PATCH, std::memory_order_relaxed);
        metrics->audio_sample_rate.store(static_cast<uint32_t>(sample_rate + 0.5), std::memory_order_relaxed);
        metrics->audio_block_size.store(static_cast<uint32_t>(buf_length), std::memory_order_relaxed);
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
        clockwork_log("v%d.%d.%d",
                     CLOCKWORK_VERSION_MAJOR, CLOCKWORK_VERSION_MINOR, CLOCKWORK_VERSION_PATCH);
        clockwork_log("%.0fkHz %dch", sample_rate / 1000, config.max_output_channels);
        clockwork_log("DSP: %s %s", dsp_name, dsp_version);
        clockwork_log("");
        clockwork_log("> engine ready...");
#else
        // No banner off the web, so the guest's identity is a line of its own.
        clockwork_log("DSP: %s %s", dsp_name, dsp_version);
#endif
#else  // !CLOCKWORK_SYNTH
        // No-DSP core: no instance, no audio render. The scheduler/router still
        // runs — derive the block-time constants from the AudioContext rate and a
        // default block size so the fire loop's OSC-time window is correct.
        const int buf_length = clockwork::kDefaultBlockSize;
        g_osc_increment = (int64_t)((double)buf_length / sample_rate * clockwork::kNtpUnitsPerSecond);
        g_osc_to_samples = sample_rate / clockwork::kNtpUnitsPerSecond;

        metrics->clockwork_version_major.store(CLOCKWORK_VERSION_MAJOR, std::memory_order_relaxed);
        metrics->clockwork_version_minor.store(CLOCKWORK_VERSION_MINOR, std::memory_order_relaxed);
        metrics->clockwork_version_patch.store(CLOCKWORK_VERSION_PATCH, std::memory_order_relaxed);
        metrics->audio_sample_rate.store(static_cast<uint32_t>(sample_rate + 0.5), std::memory_order_relaxed);
        metrics->audio_block_size.store(static_cast<uint32_t>(buf_length), std::memory_order_relaxed);

#if CLOCKWORK_SCHEDULER
        clockwork_engine_schedule().clear();
#endif
        update_scheduler_depth_metric(0);
#endif // CLOCKWORK_SYNTH
    }

#if !defined(__EMSCRIPTEN__) && CLOCKWORK_SYNTH
    // destroy_dsp / rebuild_dsp — for native cold swap (device sample rate change).
    // Frees the DSP instance outright and builds a new one at the new rate; nothing
    // of the old instance survives but DspConfig::persistent.
    //
    // A COLD SWAP EMPTIES WHATEVER SUBSCRIPTION SET THE DSP WAS KEEPING, and a client
    // must re-subscribe. A registry that outlives an instance is state clockwork
    // cannot ask for and the DSP cannot hand over.
    void destroy_dsp() {
        if (g_dsp) {
            assets_release_all();   // the instance's assets go with it; committers hear
            dsp_free(g_dsp);
            g_dsp = nullptr;
        }
        // After dsp_free, so nothing can be sending as they go. See
        // dsp_host_open_sink: this is the other half of "clockwork closes
        // every sink a DSP opened when that DSP is freed".
        close_dsp_sinks();
        g_block_size = g_out_channels = g_in_channels = 0;
        g_dsp_sample_rate = 0.0;
        clockwork_heap_destroy();
#if CLOCKWORK_SCHEDULER
        clockwork_engine_schedule().clear();
#endif
        update_scheduler_depth_metric(0);
        g_in_seq_reset.store(true, std::memory_order_relaxed);
    }

    void rebuild_dsp(double sample_rate) {
        // The new rate, plus whatever channel ceilings the caller aimed at
        // with clockwork_set_channel_ceilings since the last build. Everything else
        // — the guest's config block, the arena, the regions — is where it
        // was; a rebuild is init_memory again, not a different function.
        g_clockwork_config.sample_rate = sample_rate;
        init_memory();
    }

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

        destroy_dsp();
        // The arenas clockwork reserved for the guest go with the engine, not
        // with the instance: a rebuild keeps them (destroy_dsp above), a
        // teardown returns them.
        if (g_owned_fast) { clockwork::mem::free(g_owned_fast); g_owned_fast = nullptr; }
        if (g_owned_bulk) { clockwork::mem::free(g_owned_bulk); g_owned_bulk = nullptr; }
        g_owned_fast_bytes = g_owned_bulk_bytes = 0;
        memory_initialized = false;
        shared_memory = nullptr;
        control = nullptr;
        metrics = nullptr;
    }
#endif

    // Main audio processing function - called once per block (the lanes tick,
    // clockwork_tick, wraps this).
    // current_time: AudioContext.currentTime (WASM) or wall-clock NTP (native)
    // active_output_channels / active_input_channels: live channel counts
    EMSCRIPTEN_KEEPALIVE
    bool process_audio(double current_time, uint32_t active_output_channels, uint32_t active_input_channels) {
        AudioThreadScope _audio_thread_scope;   // this thread owns RT-out for clockwork_log routing
        clockwork_heap_register_engine_thread();  // pool ownership follows the audio thread
#if CLOCKWORK_SYNTH
        if (!memory_initialized || !g_dsp) {
            return true; // Not ready or instance destroyed during cold swap — output silence
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

#if CLOCKWORK_SCHEDULER
        clockwork_engine_schedule().drainPendingClear();
#endif

        // WASM derives NTP via ClockworkClock, reading ntp_start_time from shared memory
        // every frame rather than caching, so a timing resync after resume takes effect
        // at once. On native, current_time is already the ClockworkClock-derived NTP.
#if CLOCKWORK_WORKLET_CLOCK
        // Worklet self-clock host (WASM + the self-driven device): derive NTP from
        // the sample clock via the bound ClockworkClock, and mirror the readout into the
        // cross-platform clock metrics (slots 46-49). External-clock hosts (native/
        // JUCE) take the #else: current_time is already the ClockworkClock-derived NTP
        // from the callback.
        const double current_ntp = clockworkClock().nowAt(current_time);
#else
        const double current_ntp = current_time;
#endif

        // Load-add-store, not fetch_add: this counter has exactly one writer
        // (the audio thread, here) and readers only ever load it, so the
        // read-modify-write bought nothing. It cost something, though —
        // ESP-IDF builds with -mdisable-hardware-atomics, so every atomic RMW
        // is a call into libatomic that takes a critical section, once a
        // block, for an increment nobody was racing.
        const uint32_t pc = metrics->process_count.load(std::memory_order_relaxed) + 1;
        metrics->process_count.store(pc, std::memory_order_relaxed);

        // Host telemetry. Sample the ring fill every block so peaks stay a true
        // high-water mark (a burst can fill and drain within a few blocks); this is cheap
        // — the control reads overlap the drain and peaks are local. Flush the SAB (clock
        // readout + ring used/peaks) only at the poll rate below: a consumer reads it at
        // display rate, and per-block flushing is costly where the metrics struct is in
        // slow memory (PSRAM on ESP32). process_count stays per-block as the block counter.
        {
            int32_t in_head = control->in_head.load(std::memory_order_relaxed);
            int32_t in_tail = control->in_tail.load(std::memory_order_relaxed);
            uint32_t in_used = (in_head - in_tail + IN_BUFFER_SIZE) % IN_BUFFER_SIZE;
            if (in_used > local_in_peak.load(std::memory_order_relaxed))
                local_in_peak.store(in_used, std::memory_order_relaxed);

            int32_t out_head = control->out_head.load(std::memory_order_relaxed);
            int32_t out_tail = control->out_tail.load(std::memory_order_relaxed);
            uint32_t out_used = (out_head - out_tail + OUT_BUFFER_SIZE) % OUT_BUFFER_SIZE;
            if (out_used > local_out_peak.load(std::memory_order_relaxed))
                local_out_peak.store(out_used, std::memory_order_relaxed);

            int32_t nrt_out_head = control->nrt_out_head.load(std::memory_order_relaxed);
            int32_t nrt_out_tail = control->nrt_out_tail.load(std::memory_order_relaxed);
            uint32_t nrt_out_used = (nrt_out_head - nrt_out_tail + NRT_OUT_BUFFER_SIZE) % NRT_OUT_BUFFER_SIZE;
            if (nrt_out_used > local_nrt_out_peak.load(std::memory_order_relaxed))
                local_nrt_out_peak.store(nrt_out_used, std::memory_order_relaxed);

            // Flush period in blocks, derived once from the audio rate to target ~30Hz
            // (tracks sample rate / block size instead of a fixed divisor); 16 until the
            // audio config is published.
            static uint32_t s_flush_period = 0;
            if (s_flush_period == 0u) {
                const uint32_t blk = metrics->audio_block_size.load(std::memory_order_relaxed);
                const uint32_t sr  = metrics->audio_sample_rate.load(std::memory_order_relaxed);
                if (blk && sr) {            // round(sr / (30 * blk)) in integer math
                    uint32_t p = (sr + 15u * blk) / (30u * blk);
                    s_flush_period = (p < 1u) ? 1u : p;
                }
            }
            const uint32_t flush_period = (s_flush_period != 0u) ? s_flush_period : 16u;
            if ((pc % flush_period) == 0u) {
#if CLOCKWORK_WORKLET_CLOCK
                clockworkClock().publishClockMetrics(metrics, current_ntp, 4.0);
#endif
                metrics->in_buffer_used_bytes.store(in_used, std::memory_order_relaxed);
                metrics->in_buffer_peak_bytes.store(local_in_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->out_buffer_used_bytes.store(out_used, std::memory_order_relaxed);
                metrics->out_buffer_peak_bytes.store(local_out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
                metrics->nrt_out_buffer_used_bytes.store(nrt_out_used, std::memory_order_relaxed);
                metrics->nrt_out_buffer_peak_bytes.store(local_nrt_out_peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
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
            // Flush request (purge): arm the discard threshold here so
            // g_in_discard_* stays audio-thread-only, like g_in_drain below.
            {
                const int64_t below =
                    g_in_flush_below.exchange(-1, std::memory_order_acquire);
                if (below >= 0) {
                    g_in_discard_active = true;
                    g_in_discard_below  = static_cast<uint32_t>(below);
                }
            }

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
                shared_memory + IN_BUFFER_START, IN_BUFFER_SIZE,
                &control->in_head, &control->in_tail, g_in_drain,
                ClockworkDrainMetrics{ &metrics->messages_processed, nullptr,
                                &metrics->messages_dropped,
                                &metrics->messages_sequence_gaps },
                MAX_MESSAGES_PER_FRAME,
                [currentOscTime](uint32_t sourceId, const uint8_t* payload,
                                 uint32_t payload_size, uint32_t seq) -> ClockworkDrainVerdict {
                    // Purge in progress: frames sequenced before the flush
                    // snapshot are stale — consume them undispatched. The
                    // signed delta stays correct across uint32 seq rollover
                    // (pending frames are always far fewer than 2^31 apart).
                    if (g_in_discard_active) {
                        if (static_cast<int32_t>(seq - g_in_discard_below) < 0)
                            return ClockworkDrainVerdict::Consume;
                        g_in_discard_active = false;
                    }

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
                                 control->in_head.load(std::memory_order_relaxed),
                                 control->in_tail.load(std::memory_order_relaxed));
                    corruption_count.fetch_add(1, std::memory_order_relaxed);
                }
                if (stop == ClockworkDrainStop::BadLength)
                    control->status_flags.fetch_or(STATUS_FRAGMENTED_MSG, std::memory_order_relaxed);
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
                dsp_process(g_dsp,
                            g_in_channels ? g_in_ptrs : nullptr, g_in_channels,
                            g_out_ptrs, g_out_channels,
                            QUANTUM_SIZE,
                            currentOscTime);

                // Sinks after: what a sink records is what the DSP actually
                // emitted, which is the "post" answer to open question 1 in
                // docs/PORTS.md for the only tap that exists so far.
                push_port_sinks(QUANTUM_SIZE);
            }
            // The audio taps, at the device edge: what leaves for the device
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
                    const float* channel_data[SHM_AUDIO_CHANNELS];
                    for (uint32_t c = 0; c < ch; ++c)
                        channel_data[c] = bus + static_cast<size_t>(c) * QUANTUM_SIZE;
                    shm_audio_buffer_writer(t).write(channel_data, QUANTUM_SIZE);
                };
                tap(SHM_AUDIO_OUT_SLOT, static_audio_bus);
                tap(SHM_AUDIO_IN_SLOT,  static_audio_in);
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

        char pkt[1024];
        uint32_t p = clockwork::buildDebugOsc(pkt, text, len);

        // The audio thread owns the lock-free RT-out ring, so it logs there. Any
        // other thread routes to the locked NRT-out ring instead — but only where a
        // backend drains it (g_nrt_egress_drained, published by the native NRT
        // gateway). A single-threaded worklet target (WASM / self-driven device)
        // never drains NRT-out and has no second RT-out writer, so it always uses
        // the always-safe RT-out. Capability signal, not an __EMSCRIPTEN__ branch,
        // so every target shares one codepath.
        const bool useRtOut =
            t_on_audio_thread ||
            !g_nrt_egress_drained.load(std::memory_order_relaxed);
        if (useRtOut) {
            ::ring_buffer_write(
                shared_memory + OUT_BUFFER_START, OUT_BUFFER_SIZE,
                &control->out_head, &control->out_tail, &control->out_sequence,
                EGRESS_BROADCAST_NOTIFY, 0,  // route, source_id (debug broadcasts)
                pkt, p, &control->status_flags);
        } else {
            clockwork_egress_nrt_write(EGRESS_BROADCAST_NOTIFY, 0,
                                reinterpret_cast<const uint8_t*>(pkt), p);
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
        char buffer[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);
#ifdef __EMSCRIPTEN__
        emscripten_console_error(buffer);
#else
        fprintf(stderr, "%s\n", buffer);
#endif
    }

    static int clockwork_log_impl(const char* fmt, va_list args) {
        if (!memory_initialized) return 0;
        char buffer[1024];
        int result = vsnprintf(buffer, sizeof(buffer), fmt, args);
        uint32_t len = 0;
        while (buffer[len] != '\0' && len < sizeof(buffer)) len++;
        emit_debug_osc(buffer, len);
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
        return control ? control->status_flags.load(std::memory_order_relaxed) : 0;
    }

    // Live channel widths. A cold swap can rebuild at a different width than
    // boot, so callers that cache channel counts (JuceAudioCallback's
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
    PerformanceMetrics* metrics
) {
    // Egress frame: Message{sourceId = token} + [route:u32][osc]; the route word
    // counts toward the message length.
    Message header;
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

    uint32_t available = (buffer_size - 1 - current_head + current_tail) % buffer_size;

    if (available < footprint) {
        if (metrics) metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
        if (status_flags) status_flags->fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
        return false;
    }

    // Check if message fits contiguously, otherwise write padding and wrap to 0.
    //
    // After wrapping, we must re-check that there's enough space between position 0
    // and tail — the initial available-space check included bytes that will be wasted
    // as padding, so it can overestimate the usable space at the front.
    uint32_t space_to_end = buffer_size - current_head;
    if (footprint > space_to_end) {
        // Verify space at front after wrap (tail-1 to avoid head==tail ambiguity)
        uint32_t space_at_front = (current_tail > 0) ? (current_tail - 1) : 0;
        if (space_at_front < footprint) {
            if (metrics) metrics->messages_dropped.fetch_add(1, std::memory_order_relaxed);
            if (status_flags) status_flags->fetch_or(STATUS_BUFFER_FULL, std::memory_order_relaxed);
            return false;
        }

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
    if (metrics) {
        uint32_t used = (new_head - current_tail + buffer_size) % buffer_size;
        uint32_t prev = metrics->out_buffer_peak_bytes.load(std::memory_order_relaxed);
        while (used > prev) {
            if (metrics->out_buffer_peak_bytes.compare_exchange_weak(
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
 * constructor runs before init_memory and clockwork_heap_init, so its 4MB data
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
        clockState);
    return true;   // the prefix is claimed whole, answered or refused
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
                clockState)) {
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
    static uint8_t frame[kHeader + 66560];
    if (len > sizeof(frame) - kHeader) {
        static std::atomic<uint32_t> oversize{0};
        if (oversize.fetch_add(1, std::memory_order_relaxed) < 8)
            clockwork_log("WARNING: %s of %u bytes dropped — wider than the host forward carries",
                          reinterpret_cast<const char*>(element), static_cast<unsigned>(len));
        return;
    }
    if (when == 0) when = 1;   // both spell "immediately"; the wire form is 1
    const uint64_t tt = static_cast<uint64_t>(when);
    std::memcpy(frame, "#bundle", 8);
    for (int i = 0; i < 8; ++i) frame[8 + i]  = static_cast<uint8_t>(tt >> (56 - 8 * i));
    for (int i = 0; i < 4; ++i) frame[16 + i] = static_cast<uint8_t>(len >> (24 - 8 * i));
    std::memcpy(frame + kHeader, element, len);
    emit_osc_out(token, frame, kHeader + len);
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
    static char element[64 + 256 + 66560];
    try {
        osc::OutboundPacketStream ps(element, sizeof(element));
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
            [origin](const uint8_t* d, uint32_t n) { emit_osc_out(origin, d, n); });
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
        if (a == end || !a->IsFloat()) throw osc::WrongArgumentTypeException();
        rate = (a++)->AsFloatUnchecked();
    } catch (const osc::Exception&) {
        return refuse("commit takes id kind offset bytes channels frames rate");
    }
    if (id < 0 || static_cast<uint32_t>(id) == kAssetTombstone) return refuse("bad id");
    if (!g_dsp) return refuse("no guest");
    const uint8_t* inbox = static_cast<const uint8_t*>(g_clockwork_config.inbox);
    const uint64_t lane  = g_clockwork_config.inbox_bytes;
    if (!inbox || lane == 0) return refuse("this engine has no inbox");
    if (offset < 0 || bytes <= 0) return refuse("range must be inside the inbox");
    if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > lane)
        return refuse("range runs past the inbox");
    ClockworkAsset asset {};
    asset.struct_bytes = sizeof asset;
    asset.id           = static_cast<uint32_t>(id);
    asset.kind         = static_cast<uint32_t>(kind);
    asset.origin       = origin;
    asset.bytes        = inbox + offset;
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
    const int rc = dsp_asset(g_dsp, &asset);
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
    return direction == kClockworkPortSource ? m->in : m->out;
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
        std::memset(e.name, 0, sizeof e.name);
        if (name) std::strncpy(e.name, name, sizeof(e.name) - 1);
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
    for (uint32_t c = 0; c < count && first + c < CLOCKWORK_MAX_CHANNELS; ++c)
        side[first + c].store(word, std::memory_order_release);
}

void channel_map_unbind(ClockworkPort port) {
    ClockworkChannelMapState* m = channelMap();
    if (!m || port == CLOCKWORK_PORT_NONE) return;
    ColdWrite guard(m);
    for (uint32_t i = 0; i < CHANNEL_MAP_MAX_STREAMS; ++i) {
        ClockworkStreamEntry& e = m->streams[i];
        if (e.port != port) continue;
        const uint32_t width = e.direction == kClockworkPortSource
                             ? m->device_in.load(std::memory_order_relaxed)
                             : m->device_out.load(std::memory_order_relaxed);
        std::atomic<uint32_t>* side = channel_side(m, e.direction);
        for (uint32_t c = 0; c < e.count && e.first + c < CLOCKWORK_MAX_CHANNELS; ++c)
            side[e.first + c].store(bare_channel_word(e.first + c, width),
                                    std::memory_order_release);
        e.port = 0; e.direction = 0; e.first = 0; e.count = 0;
        std::memset(e.name, 0, sizeof e.name);
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
