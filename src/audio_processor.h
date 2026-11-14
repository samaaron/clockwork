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
#include "core_state.h"   // ClockworkConfig

struct Dsp;

extern "C" {
    // A core's arena is its layout's size (shared_memory.h,
    // clockwork::Layout); do not restate it here as a number: it changes
    // with the profile.
    // A core's arena when its host gave it no segment (ClockworkConfig::
    // segment): natively the core's own, made at its first ask and freed
    // when the core goes (clockwork_free_local_arena); on a device, where a
    // process holds one core, static storage; on the web the start of the
    // engine's memory, one block claimed once from the module's allocator
    // (clockwork_memory_claim) that the placement arena follows in, null
    // until claimed.
    uint8_t* clockwork_local_arena(struct ClockworkCore* core);

    // Exported functions
    EMSCRIPTEN_KEEPALIVE int get_arena_base();
    EMSCRIPTEN_KEEPALIVE const char* clockwork_product_version();

    // A native engine's own figures into `core`'s native stats
    // (NATIVE_STATS_START): the audio callback's load, and how long the
    // control pass blocks (engine_support.cpp).
    void clockwork_publish_audio_load(struct ClockworkCore* core, uint32_t cpuAvgCenti, uint32_t cpuPeakCenti,
                                      uint32_t callbackOverruns);
    void clockwork_publish_nrt_blocking(struct ClockworkCore* core, uint32_t maxPassUs, uint32_t recentWorstUs,
                                        uint32_t inFlightUs);
    // How a native engine's device kept time (the metrics block's words
    // 66-71), as its processor counted since the engine started.
    typedef struct ClockworkDeviceTime {
        uint32_t missed_cycles;        // cycles the driver says the device missed
        uint32_t clock_jumps;          // times its sample clock moved by other than the frames played
        uint32_t clock_jump_frames;    // the frames it skipped over in them
        uint32_t silent_callbacks;     // callbacks played as silence for want of a block
        uint32_t callback_gap_recent_us;   // longest time between two callbacks' starts, this minute and the last
        uint32_t callback_gap_max_us;      // and since the engine started
    } ClockworkDeviceTime;
    void clockwork_publish_device_time(struct ClockworkCore* core, const ClockworkDeviceTime* time);
    // What the engine has done so far, as its metrics block counts it: read
    // either side of a callback, the difference is what that callback did
    // (the [overrun] line). Memory reads only; zeros before the block is laid out.
    typedef struct ClockworkWorkCounts {
        uint32_t messages;        // messages handled (messages_processed)
    } ClockworkWorkCounts;
    ClockworkWorkCounts clockwork_work_counts(struct ClockworkCore* core);

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

    void clockwork_set_engine_config(struct ClockworkCore* core,
                                double sample_rate, uint32_t block_size,
                                uint32_t input_channels, uint32_t output_channels,
                                uint32_t verbosity, void* guest_memory,
                                uint32_t guest_memory_bytes,
                                uint32_t arena_bytes = 0,
                                const void* assets = nullptr,
                                uint32_t assets_bytes = 0,
                                void* returns = nullptr,
                                uint32_t returns_bytes = 0);

    // Build `core`'s arena and DSP from its config. Hosts call clockwork_init
    // (lanes.h), which fills that in first; this is the half a rebuild repeats.
    void init_memory(struct ClockworkCore* core);
    // A core's own ingress root, wired once for each core at its first
    // clockwork_init (Routing::own_split).
    void clockwork_wire_own_routes(struct ClockworkCore* core);
    // The per-block body (clockwork_tick, lanes.h): the host's block time in, the engine's out.
    // `core` has state: the tick ticks no other.
    void process_audio(struct ClockworkCore* core, ClockworkBlockTime& time,
                       uint32_t active_output_channels, uint32_t active_input_channels);
    EMSCRIPTEN_KEEPALIVE int clockwork_log(struct ClockworkCore* core, const char* fmt, ...);
    EMSCRIPTEN_KEEPALIVE int clockwork_log_va(struct ClockworkCore* core, const char* fmt, va_list args);
    EMSCRIPTEN_KEEPALIVE int clockwork_log_raw(struct ClockworkCore* core, const char* msg, uint32_t len);

    // Scheduler control
    EMSCRIPTEN_KEEPALIVE void clear_scheduler(ClockworkCore* core);

    // Mark everything pending in the IN ring for the guest: the drain refuses
    // each to its sender with `reason` (a CLOCKWORK_REFUSED_* phrase,
    // clockwork_sys.h), naming the generation as it reads now; clockwork's
    // own verbs are delivered. On the audio thread, or with it stopped: the
    // marks are the drain's state (docs/RESTARTS.md, rule 1).
    void clockwork_ingress_mark(ClockworkCore* core, const char* reason);
    EMSCRIPTEN_KEEPALIVE void clockwork_ingress_mark_stale(ClockworkCore* core);
    // The instance closed with the memory it ran in: torn down, and what
    // waits for it refused a block's worth a call. Bytes still waiting.
    EMSCRIPTEN_KEEPALIVE uint32_t clockwork_close(struct ClockworkCore* core);

    // A cold restart (docs/RESTARTS.md): the instance torn down, with the
    // audio thread stopped...
    void destroy_dsp(struct ClockworkCore* core);
    // ...and a new one at `sample_rate`, on a device whose callback is
    // `device_buffer_frames` (0 for a host with no device: its block): what
    // its /clockwork/setup names, the last thing this writes (or its first
    // block's, when the instance's ring has no room for it yet).
    void rebuild_dsp(struct ClockworkCore* core, double sample_rate, uint32_t device_buffer_frames);
    // A device change the instance survived changed a fact its setup named:
    // told as /clockwork/setup/changed on its ring, with the audio thread
    // stopped (docs/RESTARTS.md, "Three events"). A buffer of 0, for a host
    // with no device, names the block.
    void clockwork_announce_facts(struct ClockworkCore* core, double sample_rate, uint32_t buffer_frames);
    // Audio coming back to the instance in the arena that is up, at the
    // configuration a host's clockwork_init on it has just applied (a page
    // reloaded over its memory, a host whose device returned), with the
    // audio thread stopped: the least that gets the engine back where it was
    // (restart_plan.h). The instance carries on if nothing it was built for
    // changed, is told a change it survives, and is built again otherwise.
    void come_back(struct ClockworkCore* core);
    // Release the frees of the host's memory a block parked
    // (clockwork_heap.h): any call into the core that is not a block.
    void clockwork_release_parked_frees(struct ClockworkCore* core);

#ifndef __EMSCRIPTEN__
    // The engine's end, as its core closes (core_state.cpp): destroys the DSP
    // instance and clears the core's arena so the lanes entry points reject
    // before the host unmaps the segment.
    void teardown_memory(struct ClockworkCore* core);
#endif
#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
    // Natively, the arena clockwork_local_arena made the core, freed as the
    // core goes (core_state.cpp).
    void clockwork_free_local_arena(struct ClockworkCore* core);
#endif
    // One frame out over the egress, as every reply and report leaves, to
    // the one client. For clockwork's own features in other files
    // (src/params). Returns whether the frame was queued.
    bool clockwork_emit_osc(struct ClockworkCore* core, const uint8_t* osc, uint32_t len);
}

#endif // CLOCKWORK_AUDIO_PROCESSOR_H
