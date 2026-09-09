// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * engine_support.cpp — the numbers clockwork publishes about its DSP.
 *
 * Statistics written into shared memory from what the DSP is willing to report
 * about itself. Everything a guest needs FROM clockwork is in dsp_api.h:
 * DspHost::log, DspConfig's shm_window and clock, and an origin token in place
 * of a reply object.
 */
#include <atomic>
#include <cstring>
#include <cstdint>

#include "dsp_api.h"
#include "shared_memory.h"
#include "audio_processor.h"   // get_shared_memory_base

/* ── Native statistics ───────────────────────────────────────────────────────
 *
 * Written straight into the shared-memory block the JavaScript side polls.
 * Relaxed ordering throughout: each value is a scalar that stands alone, and a
 * reader seeing last block's number is not a problem worth a fence.
 */
namespace {
inline std::atomic<uint32_t>* stat_at(size_t offset) {
    auto* base = reinterpret_cast<uint8_t*>(get_shared_memory_base());
    if (!base) return nullptr;
    return reinterpret_cast<std::atomic<uint32_t>*>(base + NATIVE_STATS_START + offset);
}

inline void store_stat(size_t offset, uint32_t value) {
    if (auto* p = stat_at(offset)) p->store(value, std::memory_order_relaxed);
}
} // namespace

/* A guest publishes its own counters — it declares them and writes them into
 * the guest metric range or its own DspConfig::shm_window bytes. Clockwork does
 * not ask the guest for numbers about concepts it cannot see.
 */

extern "C" void clockwork_publish_audio_load(uint32_t cpuAvgCenti, uint32_t cpuPeakCenti,
                                       uint32_t callbackOverruns) {
    store_stat(NATIVE_STAT_CPU_AVG_CENTI, cpuAvgCenti);
    store_stat(NATIVE_STAT_CPU_PEAK_CENTI, cpuPeakCenti);
    store_stat(NATIVE_STAT_CB_OVERRUNS, callbackOverruns);
}

extern "C" void clockwork_publish_nrt_blocking(uint32_t maxPassUs, uint32_t recentWorstUs,
                                         uint32_t inFlightUs) {
    store_stat(NATIVE_STAT_NRT_MAX_PASS_US, maxPassUs);
    store_stat(NATIVE_STAT_NRT_RECENT_WORST_US, recentWorstUs);
    store_stat(NATIVE_STAT_NRT_IN_FLIGHT_US, inFlightUs);
}

/*
 * The seed an offline render pins so two runs agree.
 *
 * clockwork's knob rather than any DSP's — nrt_render sets it from --seed —
 * so it lives here and is handed over in DspConfig::deterministic_seed at
 * dsp_new(). It was named g_dsp_deterministic_seed while the DSP reached in
 * and read it; nothing reaches in any more, so it takes clockwork prefix.
 */
extern "C" { int32_t clockwork_deterministic_seed = 0; }
