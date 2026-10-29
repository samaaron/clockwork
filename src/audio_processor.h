// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

#ifndef CLOCKWORK_AUDIO_PROCESSOR_H
#define CLOCKWORK_AUDIO_PROCESSOR_H

#include <emscripten/emscripten.h>
#include <atomic>
#include <cstdint>
#include <cstdarg>
#include "shared_memory.h"
#include "lanes/lanes.h"   // ClockworkBlockTime
#include "dsp_api.h"

struct Dsp;

// Published true by a backend that drains the NRT-out ring (the native NRT
// gateway). While true, off-audio-thread debug (clockwork_log) is routed to the locked
// NRT-out ring instead of the single-writer RT-out ring. False on worklet targets
// (WASM / self-driven device), which have no NRT-out drainer, so they use RT-out.
extern std::atomic<bool> g_nrt_egress_drained;

// True from the start of a block (process_audio) to its end. Found true when
// audio comes back (come_back), it says the audio thread was stopped partway
// through one — a worklet thread the browser ended, say — and whatever that
// block was changing is half changed: the instance is built again rather
// than carried on. Atomic because the thread that comes back is not the one
// that left.
extern std::atomic<bool> g_block_open;

extern "C" {
    // Static ring buffer (allocated in WASM data segment)
    // This is separate from the engine heap, preventing memory conflicts
    // ARENA_SIZE is computed in shared_memory.h from the build's
    // sizing flags; do not restate it here as a number: it changes with the
    // profile.
#ifndef __EMSCRIPTEN__
    extern uint8_t arena_storage[ARENA_SIZE];
#endif
    // The process-local arena: natively arena_storage above; on the web the
    // start of the engine's memory, one block claimed once from the module's
    // allocator (clockwork_memory_claim) that the placement arena follows in.
    // Null on the web until claimed. An external segment, when the host
    // supplied one, is resolved by the callers (g_external_segment).
    uint8_t* clockwork_local_arena();

    // Global state
    extern uint8_t* shared_memory;
    extern RingCursors* cursors;
    extern PerformanceMetrics* metrics;
    extern bool memory_initialized;
    extern struct Dsp* g_dsp;

    // Exported functions
    EMSCRIPTEN_KEEPALIVE int get_arena_base();
    EMSCRIPTEN_KEEPALIVE const char* clockwork_product_version();
    // Real-pointer arena base, valid on every runtime (native + WASM).
    void* get_shared_memory_base();
    // Clockwork's own geometry, set by clockwork_init and re-read on every
    // rebuild. Held here rather than in the guest's config block: a host
    // writing the block size and channel counts into the block at boot for
    // init_memory to read back would present clockwork's own decisions as
    // the guest's configuration.
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
    };
    extern ClockworkConfig g_clockwork_config;

    // Why the last build has no guest: the first FATAL of init_memory, or the
    // guest's own reason when dsp_new refused. NULL when the build has one.
    // Cleared at the start of every build.
    const char* clockwork_last_boot_error();

    // The one rule for a guest's memory wants (DspInfo::memory_bytes_wanted /
    // memory_bulk_bytes_wanted), stated once so the boot path and a test agree:
    //
    //   has_bulk_tier != 0: each want against its own span — what the boot
    //                       uses, since the bulk span exists on every host
    //                       (dsp_api.h);
    //   has_bulk_tier == 0: `arena` must hold BOTH wants (the single-region
    //                       rule, kept for its test).
    //
    // A zero want is always met. Nothing here spills: a fast want is never
    // satisfied from bulk, because that is a performance cliff with no error.
    // Returns non-zero when every want is met. On refusal `short_fast` /
    // `short_bulk` (either may be NULL) receive how many bytes short each tier
    // came, for the log line.
    int clockwork_memory_meets(uint32_t fast_bytes, uint32_t bulk_bytes, int has_bulk_tier,
                              uint32_t fast_wanted, uint32_t bulk_wanted,
                              uint32_t* short_fast, uint32_t* short_bulk);

    void clockwork_set_engine_config(double sample_rate, uint32_t block_size,
                                uint32_t input_channels, uint32_t output_channels,
                                uint32_t verbosity, void* guest_memory,
                                uint32_t guest_memory_bytes,
                                uint32_t arena_bytes = 0,
                                const void* assets = nullptr,
                                uint32_t assets_bytes = 0,
                                void* returns = nullptr,
                                uint32_t returns_bytes = 0);

    // Build the arena and the DSP from g_clockwork_config. Hosts call clockwork_init
    // (lanes.h), which fills that in first; this is the half a rebuild repeats.
    void init_memory();
    // The per-block body (clockwork_tick, lanes.h): the host's block time in, the engine's out.
    bool process_audio(ClockworkBlockTime& time, uint32_t active_output_channels, uint32_t active_input_channels);
    EMSCRIPTEN_KEEPALIVE int clockwork_log(const char* fmt, ...);
    EMSCRIPTEN_KEEPALIVE int clockwork_log_va(const char* fmt, va_list args);
    EMSCRIPTEN_KEEPALIVE uint32_t get_process_count();
    EMSCRIPTEN_KEEPALIVE uint32_t get_messages_processed();
    EMSCRIPTEN_KEEPALIVE uint32_t get_messages_dropped();
    EMSCRIPTEN_KEEPALIVE uint32_t get_status_flags();

    // Scheduler control
    EMSCRIPTEN_KEEPALIVE void clear_scheduler();

    // Mark everything pending in the IN ring for the guest: the drain refuses
    // each to its sender with `reason` (a CLOCKWORK_REFUSED_* phrase,
    // clockwork_sys.h), naming the generation as it reads now; clockwork's
    // own verbs are delivered. On the audio thread, or with it stopped: the
    // marks are the drain's state (docs/RESTARTS.md, rule 1).
    void clockwork_ingress_mark(const char* reason);
    EMSCRIPTEN_KEEPALIVE void clockwork_ingress_mark_stale();
    // The generation a new arena carries on from (the last this process
    // opened, or one a host hands over from memory now gone).
    EMSCRIPTEN_KEEPALIVE void clockwork_carry_generation(uint32_t generation);
    // The instance closed with the memory it ran in: torn down, and what
    // waits for it refused a block's worth a call. Bytes still waiting.
    EMSCRIPTEN_KEEPALIVE uint32_t clockwork_close();

    // A cold restart (docs/RESTARTS.md): the instance torn down, with the
    // audio thread stopped...
    void destroy_dsp();
    // ...and a new one at `sample_rate`, on a device whose callback is
    // `device_buffer_frames` (0 for a host with no device: its block): what
    // its /clockwork/setup names, the last thing this writes (or its first
    // block's, when the instance's ring has no room for it yet).
    void rebuild_dsp(double sample_rate, uint32_t device_buffer_frames);
    // A device change the instance survived changed a fact its setup named:
    // told as /clockwork/setup/changed on its ring, with the audio thread
    // stopped (docs/RESTARTS.md, "Three events"). A buffer of 0, for a host
    // with no device, names the block.
    void clockwork_announce_facts(double sample_rate, uint32_t buffer_frames);
    // Audio coming back to the instance in the arena that is up, at the
    // configuration a host's clockwork_init on it has just applied (a page
    // reloaded over its memory, a host whose device returned), with the
    // audio thread stopped: the least that gets the engine back where it was
    // (restart_plan.h). The instance carries on if nothing it was built for
    // changed, is told a change it survives, and is built again otherwise.
    void come_back();

#ifndef __EMSCRIPTEN__
    // Native-only: full arena teardown for engine shutdown — destroys the
    // DSP instance and clears memory_initialized/shared_memory/control/metrics so
    // the lanes entry points reject before the host unmaps the segment.
    void teardown_memory();
#endif

    // engine audio bus functions
    EMSCRIPTEN_KEEPALIVE uintptr_t get_audio_output_bus();
    EMSCRIPTEN_KEEPALIVE uintptr_t get_audio_input_bus();
    EMSCRIPTEN_KEEPALIVE int get_audio_buffer_samples();
    EMSCRIPTEN_KEEPALIVE double get_audio_sample_rate();
    EMSCRIPTEN_KEEPALIVE int64_t get_audio_block_time();
    EMSCRIPTEN_KEEPALIVE double get_time_offset();
}

#endif // CLOCKWORK_AUDIO_PROCESSOR_H
