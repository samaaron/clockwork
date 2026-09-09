// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

#ifndef CLOCKWORK_AUDIO_PROCESSOR_H
#define CLOCKWORK_AUDIO_PROCESSOR_H

#include <emscripten/emscripten.h>
#include <atomic>
#include <cstdint>
#include <cstdarg>
#include "shared_memory.h"
#include "dsp_api.h"

struct Dsp;

// Published true by a backend that drains the NRT-out ring (the native NRT
// gateway). While true, off-audio-thread debug (clockwork_log) is routed to the locked
// NRT-out ring instead of the single-writer RT-out ring. False on worklet targets
// (WASM / self-driven device), which have no NRT-out drainer, so they use RT-out.
extern std::atomic<bool> g_nrt_egress_drained;

extern "C" {
    // Static ring buffer (allocated in WASM data segment)
    // This is separate from the engine heap, preventing memory conflicts
    // TOTAL_BUFFER_SIZE is computed in shared_memory.h from the build's
    // sizing flags; do not restate it here as a number. The figures that used
    // to sit on this line ("~1.4MB ... audio capture ~375KB") were wrong by
    // more than 4x and omitted the scope region entirely. On the web build it
    // measures 6.72MB, of which SHM_SCOPE is 4.1MB.
    extern uint8_t ring_buffer_storage[TOTAL_BUFFER_SIZE];

    // Global state
    extern uint8_t* shared_memory;
    extern ControlPointers* control;
    extern PerformanceMetrics* metrics;
    extern bool memory_initialized;
    extern struct Dsp* g_dsp;

    // Exported functions
    EMSCRIPTEN_KEEPALIVE int get_ring_buffer_base();
    // Real-pointer arena base, valid on every runtime (native + WASM).
    void* get_shared_memory_base();
    // Clockwork's own geometry, set by clockwork_init and re-read on every
    // rebuild. Held here rather than in the guest's config block, which is
    // where three of these used to make a round trip: the host wrote the block
    // size and the channel counts in at boot and init_memory read them straight
    // back out, so clockwork's own decisions arrived as if they were the
    // guest's configuration.
    struct ClockworkConfig {
        double   sample_rate        = 0.0;
        uint32_t block_size         = 0;   // 0 = platform default
        uint32_t input_channels     = 0;
        uint32_t output_channels    = 0;
        uint32_t verbosity          = 0;
        // The guest's three regions, one writer each (dsp_api.h). Absolute
        // addresses; NULL = not reserved on this host.
        void*       guest_arena       = nullptr;  // the guest's alone
        uint32_t    guest_arena_bytes = 0;
        const void* inbox             = nullptr;  // client writes, guest reads
        uint32_t    inbox_bytes       = 0;
        void*       outbox            = nullptr;  // guest writes, client reads
        uint32_t    outbox_bytes      = 0;
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
    };
    extern ClockworkConfig g_clockwork_config;

    // The one rule for a guest's memory wants (DspInfo::arena_bytes_wanted /
    // arena_bulk_bytes_wanted), stated once so the boot path and a test agree:
    //
    //   tiered host  (has_bulk_tier != 0): each want against its own tier;
    //   single-region host              : `arena` must hold BOTH wants.
    //
    // A zero want is always met. Nothing here spills: a fast want is never
    // satisfied from bulk, because that is a performance cliff with no error.
    // Returns non-zero when every want is met. On refusal `short_fast` /
    // `short_bulk` (either may be NULL) receive how many bytes short each tier
    // came, for the log line.
    int clockwork_arena_meets(uint32_t fast_bytes, uint32_t bulk_bytes, int has_bulk_tier,
                              uint32_t fast_wanted, uint32_t bulk_wanted,
                              uint32_t* short_fast, uint32_t* short_bulk);

    void clockwork_set_engine_config(double sample_rate, uint32_t block_size,
                                uint32_t input_channels, uint32_t output_channels,
                                uint32_t verbosity, void* guest_arena,
                                uint32_t guest_arena_bytes,
                                uint32_t arena_bytes = 0,
                                const void* inbox = nullptr,
                                uint32_t inbox_bytes = 0,
                                void* outbox = nullptr,
                                uint32_t outbox_bytes = 0);

    // Build the arena and the DSP from g_clockwork_config. Hosts call clockwork_init
    // (lanes.h), which fills that in first; this is the half a rebuild repeats.
    void init_memory();
    EMSCRIPTEN_KEEPALIVE bool process_audio(double current_time, uint32_t active_output_channels, uint32_t active_input_channels);
    EMSCRIPTEN_KEEPALIVE int clockwork_log(const char* fmt, ...);
    EMSCRIPTEN_KEEPALIVE int clockwork_log_va(const char* fmt, va_list args);
    EMSCRIPTEN_KEEPALIVE uint32_t get_process_count();
    EMSCRIPTEN_KEEPALIVE uint32_t get_messages_processed();
    EMSCRIPTEN_KEEPALIVE uint32_t get_messages_dropped();
    EMSCRIPTEN_KEEPALIVE uint32_t get_status_flags();

    // Scheduler control
    EMSCRIPTEN_KEEPALIVE void clear_scheduler();

    // Ask the audio thread to discard everything pending in the IN ring at
    // the top of its next drain. Callable from any thread — the consuming
    // thread applies the flush, so it cannot race producers or the drain.
    void clockwork_ingress_flush_request();

#ifndef __EMSCRIPTEN__
    // Native-only: DSP teardown/rebuild for cold swap
    void destroy_dsp();
    void rebuild_dsp(double sample_rate);
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
