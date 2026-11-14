// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * engine_support.cpp — the numbers clockwork publishes about its DSP.
 *
 * Statistics written into shared memory from what the DSP is willing to report
 * about itself. Everything a guest needs FROM clockwork is in dsp_api.h:
 * DspHost::log, DspConfig's shm_window and clock, and DspHost::emit_osc in
 * place of a reply object.
 */
#include <atomic>
#include <cstring>
#include <cstdint>

#include "dsp_api.h"
#include "shared_memory.h"
#include "core_state.h"        // the core's arena
#include "audio_processor.h"   // ClockworkDeviceTime

/* ── Native statistics ───────────────────────────────────────────────────────
 *
 * Written straight into the shared-memory block the JavaScript side polls.
 * Relaxed ordering throughout: each value is a scalar that stands alone, and a
 * reader seeing last block's number is not a problem worth a fence.
 */
namespace {
inline void store_stat(ClockworkCore* core, size_t offset, uint32_t value) {
    uint8_t* const base = clockwork::arena_base(core);
    if (!base) return;
    reinterpret_cast<std::atomic<uint32_t>*>(base + NATIVE_STATS_START + offset)->store(value, std::memory_order_relaxed);
}
} // namespace

/* A guest publishes its own counters — it declares them and writes them into
 * the guest metric range or its own DspConfig::shm_window bytes. Clockwork does
 * not ask the guest for numbers about concepts it cannot see.
 */

extern "C" void clockwork_publish_audio_load(ClockworkCore* core, uint32_t cpuAvgCenti, uint32_t cpuPeakCenti,
                                             uint32_t callbackOverruns) {
    store_stat(core, NATIVE_STAT_CPU_AVG_CENTI, cpuAvgCenti);
    store_stat(core, NATIVE_STAT_CPU_PEAK_CENTI, cpuPeakCenti);
    store_stat(core, NATIVE_STAT_CB_OVERRUNS, callbackOverruns);
}

extern "C" void clockwork_publish_nrt_blocking(ClockworkCore* core, uint32_t maxPassUs, uint32_t recentWorstUs,
                                               uint32_t inFlightUs) {
    store_stat(core, NATIVE_STAT_NRT_MAX_PASS_US, maxPassUs);
    store_stat(core, NATIVE_STAT_NRT_RECENT_WORST_US, recentWorstUs);
    store_stat(core, NATIVE_STAT_NRT_IN_FLIGHT_US, inFlightUs);
}

/* How the device kept time, into the metrics block (words 66-71): the
 * processor's own totals, so a block a restart cleared is whole again at the
 * next callback. */
extern "C" void clockwork_publish_device_time(ClockworkCore* core, const ClockworkDeviceTime* t) {
    PerformanceMetrics* const m = clockwork::metrics_in(clockwork::laid_out_base(core));
    if (!m || !t) return;
    m->device_missed_cycles.store(t->missed_cycles, std::memory_order_relaxed);
    m->device_clock_jumps.store(t->clock_jumps, std::memory_order_relaxed);
    m->device_clock_jump_frames.store(t->clock_jump_frames, std::memory_order_relaxed);
    m->silent_callbacks.store(t->silent_callbacks, std::memory_order_relaxed);
    m->callback_gap_recent_us.store(t->callback_gap_recent_us, std::memory_order_relaxed);
    m->callback_gap_max_us.store(t->callback_gap_max_us, std::memory_order_relaxed);
}

extern "C" ClockworkWorkCounts clockwork_work_counts(ClockworkCore* core) {
    ClockworkWorkCounts c{};
    const PerformanceMetrics* const m = clockwork::metrics_in(clockwork::laid_out_base(core));
    if (!m) return c;
    c.messages      = m->messages_processed.load(std::memory_order_relaxed);
    return c;
}
