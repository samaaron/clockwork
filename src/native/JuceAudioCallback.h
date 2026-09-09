// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * JuceAudioCallback.h — JUCE audio driver bridge
 */
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include "clock/clock_math.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

class ClockworkClock;
class LinkAudioHost;
struct PerformanceMetrics;

extern "C" {
    bool process_audio(double current_time, uint32_t active_output_channels, uint32_t active_input_channels);
    void set_time_offset(double offset);
    uintptr_t get_audio_output_bus();
    uintptr_t get_audio_input_bus();
    int get_audio_buffer_samples();

    int get_audio_num_output_buses();
    int get_audio_num_input_buses();
}

// The input width a Link peer subscription is validated against.
//
// NOT the device's width. A peer lands in a reserved lane
// (clockwork_reserve_lanes), which by construction sits ABOVE every channel the
// device supplies, so validating against the device refuses every subscription
// a client can actually ask for. What matters is the width the DSP allocated:
// device inputs plus the reserved lanes above them, which is what
// get_audio_num_input_buses() answers and what pull_port_sources bounds against.
//
// Falls back to the device width before the DSP exists, when the accessor
// answers 0 and there is nothing allocated to talk about yet.
inline uint32_t clockworkLinkInputWidth(int deviceInputChannels) {
    const int allocated = get_audio_num_input_buses();
    const int width = allocated > deviceInputChannels ? allocated : deviceInputChannels;
    return width > 0 ? static_cast<uint32_t>(width) : 0u;
}

// Shared per-block render, used by both HeadlessDriver and the engine's manual
// pump (ClockworkEngine::pumpAudioBlock): run the tick, then publish the
// rendered block to Link Audio's main sink. The caller owns the NTP/host-time
// derivation, the samplePos advance and the processCount tick — only the block
// body lives here so the two drivers can't drift apart.
//
// The main sink is published here; it reads clockwork_audio_out(), clockwork's
// own staging buffer. Peer inputs are not, and do not need to be — they arrive
// as ordinary input channels through source ports, which pull_port_sources
// reads before dsp_process.
//
// GAP: aux sinks tapping arbitrary buses. LinkAudioBridge::publishAuxSinks
// takes the base of the DSP's signal-bus pool, which dsp_api.h does not
// expose, so nothing calls it.
void renderAudioBlock(LinkAudioHost& linkAudio,
                      uint32_t blockSize,
                      uint32_t numOutputChannels,
                      uint32_t numInputChannels,
                      uint32_t sampleRate,
                      double   ntp,
                      uint64_t hostMicros);

class JuceAudioCallback : public juce::AudioIODeviceCallback {
public:
    JuceAudioCallback();
    ~JuceAudioCallback() override = default;

    // Boot the engine. The geometry here is the Clockwork's — what the device
    // opened — and it is passed as arguments because that is what it is.
    // guestConfig is the block the guest reads and nothing here interprets
    // (GuestConfigBlock.h); clockwork copies it into the region and hands
    // over base and length.
    void initialiseDsp(uint8_t* ringBufferStorage,
                         int sampleRate,
                         int numOutputChannels,
                         int numInputChannels,
                         const void* guestConfig,
                         uint32_t guestConfigBytes,
                         void* guestArena,
                         uint32_t guestArenaBytes,
                         int bufLen = 0,   // 0 = use kDefaultBlockSize
                         // Bulk staging, one writer each. NULL when there is no
                         // segment to carve them from.
                         const void* inbox = nullptr,
                         uint32_t inboxBytes = 0,
                         void* outbox = nullptr,
                         uint32_t outboxBytes = 0);

    // The DSP's audio block size — read at audio-thread frequency.
    int bufferLength() const { return mBufLen; }

    // Wire the engine-owned ClockworkClock. ClockworkClock owns audio-thread NTP
    // derivation (the IIR previously inlined here, now in ClockworkClockNative).
    // Must be called before any audio callback or initialiseDsp.
    void setClockworkClock(ClockworkClock* sc) { mClockworkClock = sc; }
    // Wire the engine-owned Link Audio host: the block stamp in Link's domain,
    // the main-sink publish and the stream-health metrics come from it.
    void setLinkAudio(LinkAudioHost* la) { mLinkAudio = la; }
    // Wire the engine's dashboard block (its segment-resident
    // PerformanceMetrics), which each block's clock readouts are mirrored
    // into. Null is skipped, so a callback before the engine has an arena
    // publishes nothing rather than reading a global that may be stale.
    void setMetrics(PerformanceMetrics* m) { mMetrics = m; }

    // C++20 atomic wait — equivalent of JS Atomics.wait()/notify()
    std::atomic<uint32_t> processCount{0};

    // Nominal rate of the currently-open device (set in
    // audioDeviceAboutToStart; 0 before the first device). Atomic because the
    // watchdog's rate-skew check reads it off the control thread.
    int nominalSampleRate() const {
        return mNominalRate.load(std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;

    // --- Pause/resume for device swap ---
    void pause();
    void resume();
    bool isPaused() const;

    // --- Cached channel widths ---
    // The widths the IO callback clamps render/input copies against. Exposed
    // for tests, though nothing in this repository reads them.
    int dspOutputChannels() const { return mDspOutputChannels; }
    int dspInputChannels() const  { return mDspInputChannels; }

    // --- Gap detector state (for testing) ---
    // Returns true if the inter-callback gap detector has a baseline timestamp,
    // meaning the next callback will measure the gap since that timestamp.
    bool gapDetectorArmed() const { return mLastCbTime.time_since_epoch().count() != 0; }
    // Arm the gap detector by recording the current time as baseline.
    void armGapDetector() { mLastCbTime = std::chrono::high_resolution_clock::now(); }

    // Read wall clock as NTP seconds (sub-millisecond precision).
    // Public + static so HeadlessDriver can reuse the same implementation.
    static double wallClockNTP() { return ::wallClockNTP(); }

    // --- Pre-tick hook (for clockwork integration) ---
    // Called before each DSP block (mBufLen frames, not necessarily 128).
    // samplePosition: cumulative samples processed (for beat calculation)
    // wallMs: wall clock time in milliseconds
    std::function<void(double samplePosition, double wallMs)> preTick;

    // --- Wake hook ---
    // Called when a sleep/wake cycle is detected (callback gap > 2 seconds).
    // Used to purge stale messages from the ring buffer and scheduler.
    std::function<void()> onWake;


private:
    // The DSP's audio block size — the number of samples it
    // processes per tick. Matches the hardware callback size when set at
    // init so the process loop is 1:1 (no accumulator / prefetch dance).
    // Capped at clockwork::kMaxBlockSize because static_audio_bus in
    // audio_processor.cpp is sized at that max. Initialised in
    // initialiseDsp from the caller's chosen value (typically HW buffer
    // size) and honoured thereafter.
    int mBufLen = 128;

    ClockworkClock*   mClockworkClock   = nullptr;
    LinkAudioHost* mLinkAudio = nullptr;
    PerformanceMetrics* mMetrics = nullptr;
    uint8_t*   mRingBufferStorage  = nullptr;
    int        mSampleRate         = 48000;
    int        mNumOutputChannels  = 2;
    int        mNumInputChannels   = 2;
    // Input channel width of the current DSP. mNumInputChannels tracks the
    // live device and can exceed it; the input-feed loop clamps to this.
    // Set at initialiseDsp and re-synced from the live DSP in resume(),
    // because a cold swap rebuilds the DSP at the new device's width.
    int        mDspInputChannels = 0;
    // Output channel width of the current DSP. mNumOutputChannels tracks the
    // live device and can exceed it after a hot swap to a wider device; the
    // render loop clamps to this so channels the DSP never renders emit
    // silence instead of stale bus contents. Set at initialiseDsp and
    // re-synced from the live DSP in resume() — without the re-sync, a
    // boot on a narrow device pins the clamp at the boot width and the
    // DSP's writes to higher output buses never reach the hardware after
    // a cold swap onto a wider device.
    int        mDspOutputChannels = 0;
    double     mSamplePosition     = 0.0;   // cumulative samples (increments by mBufLen)
    int        mOutputLatencySamples = 0;   // device DSP→DAC latency, captured at start

    // Prefetch buffer: channel-major, mBufLen samples per channel. Only
    // populated when the HW callback wants fewer samples than a DSP
    // block (rare — happens if HW buffer shrinks after DSP init).
    std::vector<float> mPrefetchBuf;
    int   mPrefetchCount = 0;

    // Input accumulator: needed only when HW callback size < mBufLen.
    // In the typical case (HW buffer == block size) mInputAccumCount is
    // always == mBufLen after one callback and the accumulator is a
    // no-op copy. Layout: channel-major, capacity sized to hold at
    // least HW + block.
    std::vector<float> mInputAccum;
    int   mInputAccumCount = 0;
    int   mAccumPerChanCap = 0;

    std::atomic<bool> mPaused{false};
    std::atomic<int>  mNominalRate{0};   // see nominalSampleRate()

    // One-shot guard for promoting the audio thread to realtime. Reset in
    // audioDeviceAboutToStart (control thread) so each device (re)start
    // re-promotes the possibly-new audio thread; acted on in the first
    // audioDeviceIOCallbackWithContext, which runs on the audio thread itself.
    std::atomic<bool> mRealtimeElevated{false};

    // One-shot per device start (same reset pattern as mRealtimeElevated):
    // logs the moment the device's IO thread first reaches our callback.
    // Boot-diagnostic: a process death between "audio callback attached"
    // and this line means the driver/HAL never delivered a callback.
    std::atomic<bool> mFirstCallbackLogged{false};

    // Audio thread timing stats (accessed only from audio thread, no atomics needed)
    uint32_t mCallbackCount = 0;
    uint32_t mOverrunCount  = 0;
    double   mTotalUs       = 0.0;
    double   mMaxUs         = 0.0;
    double   mLastOverrunLogSec = 0.0;  // rate-limits [overrun] log lines

    // DSP load published to native-stats: a smoothed average (EMA) and a decaying
    // peak of per-callback load (callback time / time budget), in percent.
    double   mLoadAvgPct    = 0.0;
    double   mLoadPeakPct   = 0.0;

    // Inter-callback gap detector baseline (used by sleep/wake recovery)
    std::chrono::high_resolution_clock::time_point mLastCbTime{};

};
