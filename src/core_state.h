// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * core_state.h — what one engine's C++ holds, on its core.
 *
 * Each structure here has one job and is a member of CoreState; a core gets
 * its CoreState at its first clockwork_init and frees it when it closes
 * (clockwork_core.h). docs/ENGINES.md.9, is the design: a few
 * structures, each with one job, one owner and a known set of threads.
 */
#pragma once

#include "ReplyChannel.h"
#include "clock/clock_math.h"     // kNtpUnitsPerSecond
#include "clockwork_arena.h"      // CLOCKWORK_SPACE_UNKNOWN
#include "clockwork_core.h"
#include "clockwork_event_sink.h"  // ClockworkSinkHostEmitter
#include "clockwork_port_bus.h"    // CLOCKWORK_PORT_BUS_MAX, ClockworkPort
#include "clockwork_sys.h"         // OscSplit, ClockworkSysRoutes
#include "shared_memory.h"        // the regions' offsets, RingCursors, PerformanceMetrics
#include "lanes/lanes.h"          // ClockworkControlWaker
#include "lanes/ring_drain.h"     // ClockworkDrainState
#include "lanes/ring_marks.h"     // RingMarks
#include "log_limit.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// What a host asks of a core: set by clockwork_init and the setup calls
// before it, and read at every build. Clockwork's own geometry is held here
// rather than in the guest's config block: a host writing the block size and
// channel counts into the block at boot for init_memory to read back would
// present clockwork's own decisions as the guest's configuration.
struct ClockworkConfig {
    double   sample_rate        = 0.0;
    uint32_t block_size         = 0;   // 0 = platform default
    uint32_t input_channels     = 0;
    uint32_t output_channels    = 0;
    uint32_t verbosity          = 0;
    // The guest's three regions, one writer each (dsp_api.h). Absolute
    // addresses; NULL = not reserved on this host.
    void*       guest_memory       = nullptr;  // the guest's alone
    uint32_t    guest_memory_bytes = 0;
    const void* assets             = nullptr;  // client writes, guest reads
    uint32_t    assets_bytes       = 0;
    void*       returns            = nullptr;  // guest writes, client reads
    uint32_t    returns_bytes      = 0;
    // The span clockwork::mem allocates from, for hosts whose allocator cannot
    // be trusted with it (wasm: emscripten's malloc grows into the guest's
    // region). 0 means "use the build-time default", and on a host that
    // never calls set_arena it means "the system allocator", which is what
    // native does. It is here rather than a compile-time constant because
    // the guest's real-time pool comes out of it and how big that is is a
    // runtime question — a 128 MB pool needs a 128 MB span, and a build
    // cannot know which one it will be asked for.
    uint32_t arena_bytes        = 0;
    // The floating-point environment the host's audio thread runs the
    // guest in (DspFpEnv in dsp_api.h), as declared through
    // clockwork_declare_fp_env. 0 = UNKNOWN: nobody said. Handed to the
    // guest as DspConfig::fp_env; the web build overrides it to
    // DENORMALS_HONOURED because a worklet cannot arm a flush at all.
    uint32_t fp_env             = 0;
    // A floor under the guest's bulk memory, as the host asked for it
    // through clockwork_set_guest_bulk_bytes. 0 = the memory profile's
    // CLOCKWORK_GUEST_MEMORY_BULK_BYTES. A runtime figure for the same
    // reason arena_bytes is: how much a session will allocate is a
    // runtime question.
    size_t   guest_bulk_bytes   = 0;
    // Lanes the host keeps above the device for its own routing, and Link
    // audio's directly above them (clockwork_reserve_lanes,
    // clockwork_reserve_link_audio_lanes).
    uint32_t reserved_lanes     = 0;
    uint32_t link_audio_lanes   = 0;
    // The host's memory for the arena (a native engine's shared segment):
    // rings, metrics, scope and taps all live there, readable from other
    // processes. Null for the core's own.
    uint8_t* segment            = nullptr;
    // Where the arena's table says the assets and returns areas are when they
    // are not at this process's own addresses: the command segment's offsets
    // (docs/SEGMENTS.md).
    uint32_t lanes_space        = CLOCKWORK_SPACE_UNKNOWN;
    uint64_t assets_at          = 0;
    uint64_t returns_at         = 0;
    // The generation a new arena goes on from (docs/RESTARTS.md, "The
    // generation"): the last this core opened, raised by one its host hands
    // over from memory that has gone (clockwork_carry_generation). The
    // core's, not the process's: cores side by side number their own.
    uint32_t carried_generation = 0;
};

// A guest instance (dsp_api.h).
struct Dsp;
// A core's timed store (scheduler/engine_schedule.h).
struct ScheduleStore;
// The clocks a core runs on (clock/).
class ClockworkClock;
class LinkAudioHost;
class MidiClockOut;

namespace clockwork {

// Writing both egress rings, draining them in this process, and telling a
// waiter the engine said something. RT-out has one writer, the thread inside
// the core's block; NRT-out any engine thread, under nrt_write_lock; each
// ring one consumer; `said` any thread.
struct Egress {
    // RT-out's sequence when the last block's bump of the core's said word
    // (clockwork_core_said_bump) covered it. The audio thread's; zeroed
    // with the ring.
    int32_t rt_out_said = 0;
    // NRT-out's producer lock: control threads serialise their
    // [route][osc] framing through it.
    std::atomic<int32_t> nrt_write_lock{0};
    // Each lane's consumer state, reset with its ring's sequence.
    ClockworkDrainState rt_drain;
    ClockworkDrainState nrt_drain;
    // A control pass waiting for NRT-out room; the reader that makes room
    // wakes it.
    std::atomic<uint32_t> nrt_room_wanted{0};
    // Whether a backend drains NRT-out (the native NRT gateway). While it
    // does, a line written off the audio thread goes to the locked NRT-out
    // ring rather than the audio thread's RT-out. A worklet target never
    // drains NRT-out and has no second RT-out writer, so it keeps RT-out.
    std::atomic<bool> nrt_drained{false};
};

// The audio thread's side of the IN ring, and who wakes the control pass.
// The drain and the marks are the audio thread's (the marks are also set
// with it stopped); the waker is the host's to set and anyone's to fire.
struct Ingress {
    // The IN ring's consumer state.
    ClockworkDrainState drain;
    // A fresh epoch asked of the drain from another thread (a purge, a
    // teardown); the audio thread takes it before its next drain so `drain`
    // stays single-threaded.
    std::atomic<bool> seq_reset{false};
    // Below each mark, what waited for an instance that has gone is refused
    // rather than delivered (docs/RESTARTS.md).
    RingMarks marks;
    // Who drains the control pass: woken when a producer writes the control
    // ring, or the NRT egress ring makes room the pass waited for.
    std::atomic<const ClockworkControlWaker*> control_waker{nullptr};
    // The drain's trouble, said the first few times after each purge: a
    // sequence gap, and a corrupt ring.
    LogLimit gap_log = LogLimit::first(5);
    LogLimit corrupt_log = LogLimit::first(5);
};

// Whether a host forwards the verbs the audio thread does not answer, until
// it says otherwise: the web's worklet always has a front to forward to; a
// bare lanes host has none.
#ifdef __EMSCRIPTEN__
inline constexpr bool kHostForwardsByDefault = true;
#else
inline constexpr bool kHostForwardsByDefault = false;
#endif

// Where a message from the IN ring goes, and what happens to a claimed verb
// nothing on the audio thread answers. Read by the audio thread every
// dispatch.
struct Routing {
    // The OSC boundary the audio thread classifies through: a full host's
    // own (the native engine publishes its split, and withdraws it when it
    // stops), else the core's.
    std::atomic<OscSplit*> split{nullptr};
    // The core's own split and routes, for a host that brings none: a
    // worklet, an embedded host, a bare lanes host. Wired at the core's
    // first build; their route context is the core.
    OscSplit own_split;
    ClockworkSysRoutes own_routes;
    // Whether a claimed verb the audio thread does not answer itself is
    // forwarded to the host rather than refused
    // (clockwork_host_forward_route): the host's to set.
    std::atomic<bool> host_forward{kHostForwardsByDefault};
    // The RT egress as a reply channel, for the audio-thread clock routes
    // of a host whose clock is the engine's (CLOCKWORK_WORKLET_CLOCK).
    ReplyChannel rt_reply;
    // The sinks' host emitter while the host is the far end of the chain
    // (clockwork_sink_set_host_emitter): its context is the core.
    ClockworkSinkHostEmitter host_emitter{};
    bool host_emitter_installed = false;
    // A message nothing claims: said the first few times.
    LogLimit unclaimed_log = LogLimit::first(16);
};

// The timed store and its policy. The store is Scheduler.h's, where the
// build has one (CLOCKWORK_SCHEDULER), reached through a pointer so this
// struct is the same in every file that includes it; built at the core's
// first boot, once the engine's heap is up (clockwork_sched_warm), freed with
// the core. The audio thread fires it; the host sets the late limit.
struct Schedule {
    ScheduleStore* store = nullptr;
    // The latest a held timed event may play: one that comes due more than
    // this many ms after its time is dropped instead, counted. 0, the
    // default, allows any lateness (clockwork_set_timed_event_late_limit_ms).
    std::atomic<uint32_t> late_limit_ms{0};
    // A timed event the store refused (full, or never built) and one dropped
    // as later than the limit: said once in so many, as both come in bursts.
    // An event that played late: a line a second of block time at most.
    LogLimit refused_log = LogLimit::every(32);
    LogLimit late_dropped_log = LogLimit::every(64);
    LogSpacing late_log{static_cast<uint64_t>(kNtpUnitsPerSecond)};
};

// The clocks a core's blocks run on, and the core's own facts about its
// flow of audio. A full host publishes its clock, its Link audio and its MIDI
// clock out before the core's first boot; a host that brings no clock gets
// the core's own, made as the clock enters service. The audio thread reads
// them every block.
struct CoreClock {
    // The clock the core's blocks run on: the host's, else `own`.
    std::atomic<ClockworkClock*> clock{nullptr};
    // The core's own clock, for a host that brings none (a worklet, an
    // embedded host, a bare lanes host); freed with the core.
    ClockworkClock* own = nullptr;
    // The native host's Link audio and MIDI clock out; null elsewhere.
    std::atomic<LinkAudioHost*> link_audio{nullptr};
    std::atomic<MidiClockOut*> midi_clock_out{nullptr};
    // The block in flight: its start, as Timetag::ntp(). The audio thread's.
    std::atomic<uint64_t> block_ntp{0};
    // True from the start of a block to its end. Found true when audio comes
    // back (come_back), it says the audio thread was stopped partway through
    // one (a worklet thread the browser ended, say): whatever that block was
    // changing is half changed, so the instance is built again. Atomic
    // because the thread that comes back is not the one that left.
    std::atomic<bool> block_open{false};
    // The host's count of its audio rendered, in seconds, at the last block;
    // below 0 when its audio has just started (clockwork_audio_back).
    double last_audio_secs = -1.0;
    // Re-anchor the clock at the next block: the host's audio is starting,
    // or the core has just booted (clockwork_audio_starts).
    std::atomic<bool> anchor_requested{false};
};

// Where a core's regions are: the arena each is addressed in, where in it
// each lies (its layout, from the core's profile: shared_memory.h), and
// whether they are laid out. The base is set at a cold boot and cleared at
// the engine's end, with no other engine thread running; the layout is set
// before the base is published and kept for the core's life; `ready` is set
// once the regions are laid out and cleared first at the end. Every thread
// that writes the rings asks `laid_out()` first.
struct Arena {
    std::atomic<uint8_t*> base{nullptr};
    std::atomic<bool> ready{false};
    Layout layout = kDefaultLayout;   // default-layout-ok: a core starts from the target's, until its profile

    // The base, whether or not the regions are laid out yet.
    uint8_t* at() const { return base.load(std::memory_order_acquire); }
    // The base once the regions are laid out, else null.
    uint8_t* laid_out() const {
        return ready.load(std::memory_order_acquire) ? base.load(std::memory_order_acquire) : nullptr;
    }
};

// What lies in an arena laid out as `layout`, or null for no arena.
inline RingCursors* cursors_in(uint8_t* base, const Layout& layout) {
    return base ? reinterpret_cast<RingCursors*>(base + layout.ring_cursors_start) : nullptr;
}
inline PerformanceMetrics* metrics_in(uint8_t* base) {
    return base ? reinterpret_cast<PerformanceMetrics*>(base + METRICS_START) : nullptr;
}

// What the instance the core runs was built from and for. Clockwork chooses
// the geometry and writes it into DspConfig, so it never asks the guest what
// it settled on. Written by the control thread with the audio thread stopped
// (a build, a change the instance survived, its end); read by the audio
// thread.
struct Build {
    // Clockwork's configuration and the guest's config block as they stood
    // at the build: audio coming back carries the instance on only in the
    // same (come_back).
    ClockworkConfig config{};
    std::array<uint8_t, GUEST_CONFIG_SIZE> guest_config{};
    double sample_rate = 0.0;
    uint32_t block = 0;
    // The channels the instance renders: the device's, widened for the lanes
    // above them.
    uint32_t in_channels = 0;
    uint32_t out_channels = 0;
    // Where the lanes the host reserved start, and Link audio's directly
    // above them; 0 when there are none.
    uint32_t lane_base = 0;
    uint32_t link_audio_lane_base = 0;
    // How far the block clock advances per block, in NTP units.
    int64_t block_step = 0;
};

// The block's audio, as the guest is handed it and a host fills and reads
// it (lanes.h: clockwork_audio_in, clockwork_audio_out): channel-major,
// Build::block frames a channel, and the channel pointers dsp_process is
// passed. One span from the fast tier, holding the pointers, the output's
// channels and then the input's; taken by the build that needs it and kept
// while a later build fits it. Laid out with the audio thread stopped; read
// by it.
struct Staging {
    struct Free { void operator()(void* span) const; };   // clockwork::mem::free
    std::unique_ptr<void, Free> span;
    size_t span_bytes = 0;
    float* out = nullptr;                 // Build::out_channels channels; null with none
    float* in = nullptr;                  // Build::in_channels channels; null with none
    float** out_ptrs = nullptr;           // a pointer a channel into `out`
    const float** in_ptrs = nullptr;      // a pointer a channel into `in`
};

// Ports bound to the guest's channels (clockwork_port_bus.h). A fixed table,
// never compacted: an entry keeps its channel range for as long as it is
// attached, and detaching one leaves the others exactly where they were —
// the stable-slot rule dsp_api.h states for channels and clockwork_ports.h
// for ports, applied to the one thing joining them.
//
// Written by a control thread, read by the audio thread, so an entry's
// `port` is atomic and is the publication point: the first channel is
// stored BEFORE the port is published and left alone AFTER it is taken
// away, so the audio thread never sees a half-built entry. The audio
// thread's read is one load per entry and never waits.
struct PortBus {
    struct Binding {
        std::atomic<ClockworkPort> port{CLOCKWORK_PORT_NONE};
        uint32_t first_channel = 0;
    };
    std::array<Binding, CLOCKWORK_PORT_BUS_MAX> bindings{};

    // Serialises the CONTROL side, and nothing else: the audio thread never
    // touches it, so it can never be made to wait. It makes claiming an
    // entry, describing it in the channel map and publishing it one step, so
    // a rebuild's re-announcement can never interleave with an attach. A
    // spin on a word rather than a mutex because the critical section is a
    // table scan, and a word rather than std::atomic_flag because the flag
    // is a byte and CLOCKWORK_HAS_BYTE_ATOMICS is 0 on Xtensa (platform.h).
    std::atomic<uint32_t> ctl{0};
    class Lock {
    public:
        explicit Lock(PortBus& bus) : ctl_(bus.ctl) { while (ctl_.exchange(1, std::memory_order_acquire)) {} }
        ~Lock() { ctl_.store(0, std::memory_order_release); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    private:
        std::atomic<uint32_t>& ctl_;
    };
};

// The assets committed to the guest, by id (dsp_api.h, "Assets"): who
// committed each, and what it is, so a release is told to the right client
// and an instance built again is handed every asset the last one held. A
// fixed table probed by id, of the profile's size (CLOCKWORK_ASSET_SLOTS, a
// power of two), made with the core's state: no allocation on the audio
// thread, and a full table is a refusal. The audio thread's, and the build's
// with the audio thread stopped.
struct Assets {
    struct Slot {
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
    // An id no client can commit. A released asset keeps its slot under it,
    // `used`, so the probe chains through the slot stay intact; the next
    // insert on its chain takes it.
    static constexpr uint32_t kTombstone = 0xFFFFFFFFu;

    Assets();
    // The committed asset `id`, or null.
    Slot* lookup(uint32_t id);
    // A slot for an id lookup did not find: the first on its chain that is
    // free, or a released asset's; null when the table is full.
    Slot* insert(uint32_t id, uint32_t origin);
    static void remove(Slot& s) { s.id = kTombstone; s.origin = 0; }
    Slot* begin() { return slots.get(); }
    Slot* end() { return slots.get() + count; }

private:
    std::unique_ptr<Slot[]> slots;
    uint32_t count = 0;
};

// What the core's boot logs before its ring exists (clockwork_log_hold):
// held, and put on the ring ahead of everything after as soon as the ring
// is up (clockwork_log_release), so its host's debug channel carries the
// whole boot in order. Bounded: past the bound a line goes where a line
// with no ring goes. Any thread, under the lock.
struct EarlyLog {
    std::mutex lock;
    std::vector<std::string> lines;
    bool holding = false;
};

// The guest's instance, and why the last build has none. The control thread
// builds and frees the instance with the audio thread stopped; the audio
// thread runs it.
struct Guest {
    Dsp* instance = nullptr;
    // What stopped the last build, for the host to report
    // (clockwork_boot_error): the first reason wins, not what went wrong
    // because of it. `boot_failed` says it is there, and tells the audio
    // thread no instance is coming. A fixed buffer, so recording a failure
    // allocates nothing.
    std::array<char, 512> boot_error{};
    std::atomic<bool> boot_failed{false};
};

// What a new instance owes before its first word: /clockwork/setup, or
// /clockwork/setup/changed for a change of device it survived
// (docs/RESTARTS.md). Said on its ring as the build's last word, or owed
// until the ring has room, the instance not running until it has been said.
// Set with the audio thread stopped; cleared by whoever says it.
struct SetupNotice {
    enum class Owed : uint8_t { Nothing, Setup, Changed };
    std::atomic<Owed> owed{Owed::Nothing};
    std::atomic<int32_t> rate{0};             // the rate, as setup names it
    std::atomic<uint32_t> buffer_frames{0};   // the device's callback, as setup names it
};

// The ring fills a core reports in its metrics. The audio thread samples
// each ring every block and keeps the most it has seen, so a burst that fills
// and drains within a few blocks still shows, and writes them out with the
// rest of the metrics every `period` blocks, about kHz times a second. A purge
// clears the most seen (clear_scheduler).
struct MetricsFlush {
    static constexpr uint32_t kHz = 30;
    // Blocks between writes before the core is first built.
    static constexpr uint32_t kUnbuiltPeriod = 16;

    // The nearest whole number of blocks to kHz writes a second at `rate` Hz
    // and `block` frames, at least one.
    static constexpr uint32_t period_for(uint32_t rate, uint32_t block) {
        if (rate == 0 || block == 0) return kUnbuiltPeriod;
        const uint32_t p = (rate + (kHz / 2) * block) / (kHz * block);
        return p < 1 ? 1 : p;
    }

    std::atomic<uint32_t> in_peak{0};
    std::atomic<uint32_t> out_peak{0};
    std::atomic<uint32_t> nrt_out_peak{0};
    // Set each time the core is built (publish_session_metrics).
    std::atomic<uint32_t> period{kUnbuiltPeriod};

    // This block's fills. The audio thread's.
    void sample(uint32_t in_used, uint32_t out_used, uint32_t nrt_out_used) {
        raise(in_peak, in_used);
        raise(out_peak, out_used);
        raise(nrt_out_peak, nrt_out_used);
    }
    void clear_peaks() {
        in_peak.store(0, std::memory_order_relaxed);
        out_peak.store(0, std::memory_order_relaxed);
        nrt_out_peak.store(0, std::memory_order_relaxed);
    }

private:
    static void raise(std::atomic<uint32_t>& peak, uint32_t used) {
        if (used > peak.load(std::memory_order_relaxed)) peak.store(used, std::memory_order_relaxed);
    }
};

// The core's memory (docs/ENGINES.md). Natively each core
// takes its own: the arena when its host gives it no segment, and the block
// the guest's two spans are cut from, given back when the core goes. On the
// web and on a device a process holds one core: the arena is the engine's
// memory or static storage, and the spans come from the memory tiers.
struct Memory {
    // Natively, the arena when the host gave no segment: zeroed, made at the
    // core's first init, freed when the core goes.
    uint8_t* arena = nullptr;
    // Natively, the block the guest's spans are cut from: whole 64 KiB
    // chunks on a chunk (clockwork_heap.h), replaced by a build that wants
    // another size.
    uint8_t* block = nullptr;
    size_t   block_bytes = 0;
    // The guest's two spans (dsp_api.h): fast, which it lays out by hand, and
    // bulk, which its pool runs over.
    void*  fast = nullptr;
    size_t fast_bytes = 0;
    void*  bulk = nullptr;
    size_t bulk_bytes = 0;
};

// What the core's guest took from malloc (guest_malloc.h): counted where the
// link lets clockwork stand between the program and its allocator (the web),
// each call charged to the core whose guest call made it.
struct GuestMallocs {
    std::atomic<int64_t>  held{0};         // signed: a free of something taken outside a guest call cannot wrap
    std::atomic<uint32_t> boot{0};         // calls inside the boot entry points
    std::atomic<uint32_t> processing{0};   // calls inside the processing entry points
    std::atomic<uint32_t> frees{0};        // free calls inside any entry point
};

struct CoreState {
    explicit CoreState(ClockworkCore* c)
        : core(c), ports(clockwork_core_ports(c)), sinks(clockwork_core_sinks(c)), heap(clockwork_core_heap(c)) {}
    CoreState(const CoreState&) = delete;
    CoreState& operator=(const CoreState&) = delete;

    // The core this is the state of: for the calls that take the handle.
    ClockworkCore* const core;
    // Its ports (clockwork_ports.h), which the render reads every block, its
    // event sinks (clockwork_event_sink.h), and its heap (clockwork_heap.h).
    const ClockworkPorts* const ports;
    const ClockworkSinks* const sinks;
    const ClockworkHeap* const heap;
    Memory memory;
    GuestMallocs guest_mallocs;
    ClockworkConfig config;
    Arena arena;
    Build build;
    Staging staging;
    PortBus port_bus;
    Assets assets;
    Guest guest;
    SetupNotice setup;
    Egress egress;
    Ingress ingress;
    Routing routing;
    Schedule schedule;
    CoreClock clock;
    MetricsFlush metrics;
    EarlyLog early_log;
};

// `core`'s state, or null before its first clockwork_init (or when `core` is
// not live). Any thread.
inline CoreState* state(const ClockworkCore* core) {
    return static_cast<CoreState*>(clockwork_core_state(core));
}

// Its parts, or null before the first clockwork_init.
inline Egress* egress(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->egress : nullptr;
}
inline Ingress* ingress(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->ingress : nullptr;
}
inline Routing* routing(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->routing : nullptr;
}
inline Schedule* schedule(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->schedule : nullptr;
}
inline CoreClock* core_clock(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->clock : nullptr;
}
inline ClockworkConfig* config_of(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->config : nullptr;
}
inline Arena* arena_of(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->arena : nullptr;
}
// Where `core`'s regions lie: the default's for a core with no state yet,
// whose base is null.
inline const Layout& layout_of(const ClockworkCore* core) {
    const Arena* a = arena_of(core);
    return a ? a->layout : kDefaultLayout;   // default-layout-ok: no state, no base: nothing is addressed by it
}
// `core`'s arena base, laid out or not (Arena::at); null with none.
inline uint8_t* arena_base(const ClockworkCore* core) {
    const Arena* a = arena_of(core);
    return a ? a->at() : nullptr;
}
// `core`'s arena base once its regions are laid out (Arena::laid_out); null
// before, and with none.
inline uint8_t* laid_out_base(const ClockworkCore* core) {
    const Arena* a = arena_of(core);
    return a ? a->laid_out() : nullptr;
}
inline Build* build(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->build : nullptr;
}
inline Guest* guest(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->guest : nullptr;
}
// The guest instance `core` runs, or null.
inline Dsp* instance_of(const ClockworkCore* core) {
    Guest* g = guest(core);
    return g ? g->instance : nullptr;
}
inline SetupNotice* setup_notice(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->setup : nullptr;
}
inline MetricsFlush* core_metrics(const ClockworkCore* core) {
    CoreState* st = state(core);
    return st ? &st->metrics : nullptr;
}
// The clock `core`'s blocks run on, or null.
inline ClockworkClock* clock_of(const ClockworkCore* core) {
    CoreClock* c = core_clock(core);
    return c ? c->clock.load(std::memory_order_acquire) : nullptr;
}

// `core`'s state, made and handed to the core if it has none. Control
// thread; false when the core is not live.
bool make_state(ClockworkCore* core);

// The core's gate held for as long as a scope lasts, with the thread's entry
// saying so: a call of `kind` working inside `core`. Take::Try, a block's,
// takes it if it is free and never waits, and counts a block it could not
// Whether this thread holds `core`'s gate: its entry says so
// (clockwork_core.h). Any thread; two thread-local reads.
inline bool holds_gate(const ClockworkCore* core) {
    const ClockworkEntry here = clockwork_core_entry();
    return here.gate && here.core == core;
}

// render; Take::Wait, a control call's, waits out a block in flight, then
// gives back the host's memory blocks parked (clockwork_heap.h). A thread
// that holds it already holds it again for nothing. The gate is the
// handle's (clockwork_core.h), so it is there before the core has state and
// after the state has gone: closed() says the core is closing or gone, and
// the scope holds nothing. held() says whether the scope holds it.
class GateHold {
public:
    enum class Take : uint8_t { Try, Wait };
    GateHold(ClockworkCore* core, ClockworkEntryKind kind, Take take);
    ~GateHold();
    GateHold(const GateHold&) = delete;
    GateHold& operator=(const GateHold&) = delete;
    bool held() const { return held_; }
    bool closed() const { return closed_; }

private:
    ClockworkCore* core_ = nullptr;
    bool taken_ = false;    // this scope took the gate, and gives it back as it ends
    bool held_ = false;
    bool closed_ = false;
    ClockworkEntry previous_{};
};

} // namespace clockwork
