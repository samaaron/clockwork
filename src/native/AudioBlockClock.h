// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * AudioBlockClock.h — the per-block clock sequence every native driver runs.
 *
 * Three drivers render clockwork's audio blocks: the JUCE device callback, the
 * headless thread, and the engine's manual pump. Before a block each must
 * step the audio-thread NTP, publish the sample-clock anchor, take the Link
 * Audio stamp, and mirror the clock into the dashboard metrics. Each used to
 * compose that by hand, with its own subset (the headless pair never
 * touched the dashboard, so clock slots 46-49 froze without a device). One
 * entry point keeps the three identical.
 *
 * Audio-thread code: no locks, no allocation — every call below is the
 * RT-safe one the drivers already made.
 */
#pragma once

#include "clock/ClockworkClock.h"
#include "native/LinkAudioHost.h"
#include "shared_memory.h"

#include <algorithm>
#include <cstdint>

namespace clockwork {

// What a block is rendered against: its NTP time, and its stamp in Link's
// clock domain for Link Audio publish (0 without Link).
struct BlockTime {
    double   ntp;
    uint64_t hostMicros;
};

// Re-anchor both audio-thread clocks after a discontinuity: device start,
// resume after a swap, a sleep/wake gap, the first manual block.
inline void resetAudioBlockClock(ClockworkClock& clock, LinkAudioHost& linkAudio,
                                 double samplePos, double sampleRate) {
    clock.resetAudioThreadTime(samplePos, sampleRate);
    linkAudio.resetBlockClock();
}

// Once per driver callback, before rendering. `outputLatencyFrames` is what
// the device reports between render and audible (0 when there is no device:
// headless and the manual pump render straight into the segment); `metrics`
// is the segment-resident dashboard block, null where there is no segment.
inline BlockTime beginAudioBlock(ClockworkClock& clock, LinkAudioHost& linkAudio,
                                 double samplePos, double sampleRate,
                                 uint32_t outputLatencyFrames,
                                 PerformanceMetrics* metrics, double quantum = 4.0) {
    const double ntp = clock.updateAudioThreadNTP(samplePos, sampleRate);
    clock.publishSampleClock(samplePos, sampleRate, ntp, outputLatencyFrames);
    const uint64_t hostMicros = static_cast<uint64_t>(
        std::max<int64_t>(0, linkAudio.blockHostMicros(samplePos, sampleRate)));
    if (metrics) {
        // One lock-free session capture for the Link readouts, the stream
        // health beside it, then the cross-platform tempo/beat/phase/playing
        // slots from the SAB mirror — live on no-Link builds too.
        clock.publishLinkMetrics(metrics, quantum);
        linkAudio.publishMetrics(metrics);
        clock.publishClockMetrics(metrics, ntp, quantum);
    }
    return BlockTime{ntp, hostMicros};
}

}  // namespace clockwork
