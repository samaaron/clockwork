// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * ClockworkProcessor.h — clockwork's processor on Smoothie's native worklet.
 *
 * What ClockworkProcessor in js/workers/clockwork_audio_worklet.js is to a
 * browser's AudioWorklet, this is to Smoothie's (smoothie_worklet.h): the
 * platform owns the device and hands this the hardware's buffers once per
 * callback; this turns them into the engine's blocks. It sees no JUCE —
 * the device reaches it as plain facts and plain buffers.
 */
#pragma once

#include "smoothie_worklet.h"
#include "lanes/lanes.h"   // ClockworkBlockTime
#include "audio_processor.h"   // ClockworkDeviceTime
#include "workers/RingReader.h"
#include "workers/wait_word.h"
#include "clock/clock_math.h"
#include "log_limit.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

class ClockworkClock;
class LinkAudioHost;
struct PerformanceMetrics;

// The input width a Link peer subscription is validated against.
//
// NOT the device's width. A peer lands in a reserved lane
// (clockwork_reserve_lanes), which by construction sits ABOVE every channel the
// device supplies, so validating against the device refuses every subscription
// a client can actually ask for. What matters is the width the DSP allocated:
// device inputs plus the reserved lanes above them, which is what
// clockwork_in_channels answers and what pull_port_sources bounds against.
//
// Falls back to the device width before the DSP exists, when the accessor
// answers 0 and there is nothing allocated to talk about yet.
inline uint32_t clockworkLinkInputWidth(ClockworkCore* core, int deviceInputChannels) {
    const uint32_t allocated = clockwork_in_channels(core);
    const uint32_t device = deviceInputChannels > 0 ? static_cast<uint32_t>(deviceInputChannels) : 0u;
    return allocated > device ? allocated : device;
}

// Shared per-block render, used by both HeadlessDriver and the engine's manual
// pump (ClockworkEngine::pumpAudioBlock): run the tick, then publish the
// rendered block to Link Audio's main sink. The caller owns the NTP/host-time
// derivation, the samplePos advance and the processCount tick — only the block
// body lives here so the two drivers can't drift apart.
//
// The main sink is published here; it reads clockwork_audio_out(core), clockwork's
// own staging buffer. Peer inputs are not, and do not need to be — they arrive
// as ordinary input channels through source ports, which pull_port_sources
// reads before dsp_process.
//
// GAP: aux sinks tapping arbitrary buses. LinkAudioBridge::publishAuxSinks
// takes the base of the DSP's signal-bus pool, which dsp_api.h does not
// expose, so nothing calls it.
//
// `publishChannels` is how many output channels the main sink publishes
// from: the channels the DSP rendered this block. The device path clamps it
// below numOutputChannels while a hot swap onto a wider device leaves the
// DSP rendering fewer than the staging buffer holds; the others render what
// they allocated. 0 means numOutputChannels.
//
// Returns the tick's answer: only CLOCKWORK_TICK_RENDERED leaves a block in
// clockwork_audio_out for the caller to read (lanes.h).
ClockworkTick renderAudioBlock(ClockworkCore* core,
                      LinkAudioHost& linkAudio,
                      uint32_t blockSize,
                      uint32_t numOutputChannels,
                      uint32_t numInputChannels,
                      uint32_t sampleRate,
                      ClockworkBlockTime& time,   // in: the host's frame count, clocks, latency, Link stamp; out: the block's time
                      uint32_t publishChannels = 0);

class ClockworkProcessor final : public smoothie::Processor {
public:
    ClockworkProcessor();

    // Boot the engine. The geometry here is the Clockwork's — what the device
    // opened — and it is passed as arguments because that is what it is.
    // guestConfig is the block the guest reads and nothing here interprets
    // (ClockworkEngine::Config::guestConfig); clockwork copies it into the
    // region and hands over base and length.
    void initialiseDsp(uint8_t* ringBufferStorage,
                         int sampleRate,
                         int numOutputChannels,
                         int numInputChannels,
                         const void* guestConfig,
                         uint32_t guestConfigBytes,
                         void* guestMemory,
                         uint32_t guestMemoryBytes,
                         int bufLen = 0,   // 0 = use kDefaultBlockSize
                         // Bulk staging, one writer each. NULL when there is no
                         // segment to carve them from.
                         const void* assets = nullptr,
                         uint32_t assetsBytes = 0,
                         void* returns = nullptr,
                         uint32_t returnsBytes = 0);

    // The DSP's audio block size — read at audio-thread frequency.
    int bufferLength() const { return mBufLen; }

    // Wire the engine-owned ClockworkClock. ClockworkClock owns audio-thread NTP
    // derivation (the IIR in ClockworkClockNative).
    // Must be called before any audio callback or initialiseDsp.
    void setClockworkClock(ClockworkClock* sc) { mClockworkClock = sc; }
    // Wire the engine-owned Link Audio host: the block stamp in Link's domain,
    // the main-sink publish and the stream-health metrics come from it.
    void setLinkAudio(LinkAudioHost* la) { mLinkAudio = la; }
    // Wire the engine's core: what initialiseDsp boots and every block ticks.
    // Must be called before initialiseDsp.
    void setCore(ClockworkCore* core) { mCore = core; }
    // Wire the engine's dashboard block (its segment-resident
    // PerformanceMetrics), which each block's clock readouts are mirrored
    // into. Null is skipped, so a callback before the engine has an arena
    // publishes nothing rather than reading a global that may be stale.
    void setMetrics(PerformanceMetrics* m) { mMetrics = m; }

    // Blocks rendered, from any driver. A thread that waits for a block (the
    // first one at boot, a fade-out's end) blocks on this word
    // (src/workers/wait_word.h); the watchdog reads it.
    ClockworkWaitWord processCount;

    // The end of a block, from every driver: count it, wake whoever waits for
    // one, and wake the control pass, which drains what the block wrote. Two
    // atomic operations each when nobody waits; never blocks.
    void blockDone() noexcept {
        clockwork::bump(processCount);
        if (RingReader* r = mControlPass.load(std::memory_order_acquire)) r->wake();
    }
    // The control pass the engine runs on a thread of its own; null while it
    // has none (a host runs the pass, or the engine is down).
    void setControlPass(RingReader* r) { mControlPass.store(r, std::memory_order_release); }

    // Nominal rate and buffer size of the currently-open device (set in
    // deviceStarting; 0 before the first device). Atomic because the
    // watchdog's rate-skew check and the control pass read them, and the
    // control pass must never wait for the swap gate to ask the device itself
    // (a cold swap holding the gate waits for the pass to park).
    int nominalSampleRate() const {
        return mNominalRate.load(std::memory_order_relaxed);
    }
    int deviceBufferSize() const {
        return mDeviceBufferSize.load(std::memory_order_relaxed);
    }

    // The worklet's calls (smoothie_worklet.h): a device starting, each of
    // its blocks on the real-time thread, and its stopping.
    void deviceStarting(const smoothie::DeviceInfo& device) override;
    void process(const float* const* inputs, int numInputs,
                 float* const* outputs, int numOutputs,
                 int frames, const smoothie::BlockTime& time) override;
    void deviceStopped() override;

    // A device whose thread is its caller's (ClockworkEngine::pumpDevice):
    // after deviceStarting, its first callback leaves the thread as it is.
    void leaveThreadPriority() { mRealtimeElevated.store(true, std::memory_order_relaxed); }

    // --- Pause/resume for device swap ---
    // A restart is heard as a dip, not as a click: the output fades to
    // silence over kRestartFadeSeconds before the device stops, and back from
    // silence over the same once the restart resumes it — the web's resume
    // fade (clockwork_audio_worklet.js), natively. A device's first start,
    // before any restart, is not faded.
    static constexpr double kRestartFadeSeconds = 0.02;
    // From the next block, fade the output to silence, and pause there
    // (isPaused). Control thread.
    void fadeOut();
    void pause();
    void resume();   // fades in what follows
    bool isPaused() const;

    // --- Cached channel widths ---
    // The widths the IO callback clamps render/input copies against. Exposed
    // for tests, though nothing in this repository reads them.
    int dspOutputChannels() const { return mDspOutputChannels; }
    int dspInputChannels() const  { return mDspInputChannels; }

    // Read wall clock as NTP seconds (sub-millisecond precision).
    // Public + static so HeadlessDriver can reuse the same implementation.
    static double wallClockNTP() { return ::wallClockNTP(); }

private:
    std::atomic<RingReader*> mControlPass{nullptr};
    // The DSP's audio block size — the number of samples it
    // processes per tick. Matches the hardware callback size when set at
    // init so the process loop is 1:1 (no accumulator / prefetch dance).
    // Capped at clockwork::kMaxBlockSize, the profile's largest block.
    // Initialised in
    // initialiseDsp from the caller's chosen value (typically HW buffer
    // size) and honoured thereafter.
    int mBufLen = 128;

    ClockworkClock*   mClockworkClock   = nullptr;
    LinkAudioHost* mLinkAudio = nullptr;
    ClockworkCore*    mCore = nullptr;
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

    // The restart fades (kRestartFadeSeconds), in samples at the device's
    // rate. The counts are the audio thread's, reset only while the callback
    // is gated or not yet running (deviceStarting, resume); the request
    // crosses from the control thread, and the arrival at silence is mPaused.
    int mFadeLength  = 0;
    int mFadeInDone  = 0;   // samples of the fade-in rendered; mFadeLength: none due
    int mFadeOutDone = 0;   // samples of the fade-out rendered
    std::atomic<bool> mFadeOutRequested{false};
    // The fades on this callback's output, as far as they have got.
    void applyFades(float* const* outputs, int numOutputs, int frames);

    std::atomic<int>  mNominalRate{0};        // see nominalSampleRate()
    std::atomic<int>  mDeviceBufferSize{0};   // see deviceBufferSize()

    // One-shot guard for promoting the audio thread to realtime. Reset in
    // deviceStarting (control thread) so each device (re)start re-promotes
    // the possibly-new audio thread; acted on in the first process call,
    // which runs on the audio thread itself.
    std::atomic<bool> mRealtimeElevated{false};

    // One-shot per device start (same reset pattern as mRealtimeElevated):
    // logs the moment the device's IO thread first reaches our callback.
    // Boot-diagnostic: a process death between "audio callback attached"
    // and this line means the driver/HAL never delivered a callback.
    clockwork::LogLimit mFirstCallbackLog = clockwork::LogLimit::first(1);

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

    // How the device keeps time, as each callback's BlockTime says: what can
    // be heard as a dropout while every block rendered in time. Checked on
    // every callback, paused or not (noteDeviceTime); the audio thread's.
    // What one device run's next callback is measured against: reset as
    // each device starts (deviceStarting), before its first callback.
    double   mLastDeviceFrame  = -1.0;   // the device's frame count at the last callback; -1: none
    int      mLastFrames       = 0;      // and the frames that callback played
    int64_t  mLastDriverMisses = -1;     // the driver's count of missed cycles then; -1: none
    double   mLastCallbackSec  = 0.0;    // the last callback's start, monotonic; 0: none
    // Since the engine started: never reset, so the metric words they are
    // published to (clockwork_publish_device_time) count every device run.
    ClockworkDeviceTime mDeviceTime{};
    bool     mRendered = false;          // the engine has rendered a block: silence from here is counted
    double   mGapThisMinuteUs = 0.0;     // the longest gap since mGapMinuteStart
    double   mGapLastMinuteUs = 0.0;     // and in the minute before
    double   mGapMinuteStart  = 0.0;
    // One line a second at most for each, by the monotonic clock in ns.
    clockwork::LogSpacing mMissedLog{1000000000ULL};
    clockwork::LogSpacing mJumpLog{1000000000ULL};
    clockwork::LogSpacing mSilentLog{1000000000ULL};
    void noteDeviceTime(int frames, const smoothie::BlockTime& time);
    void noteSilentCallback(int frames, bool busy);
};
