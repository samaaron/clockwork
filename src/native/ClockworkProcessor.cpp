// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
#include "ClockworkProcessor.h"
#include "HardeningPolicy.h"
#include "RealtimeThread.h"
#include "clock/clock_math.h"
#include "clock/ClockworkClock.h"
#include "native/AudioBlockClock.h"
#include "native/LinkAudioHost.h"
#include "shared_memory.h"
#include "audio_config.h"
#include "clockwork_config.h"   // clockwork_log
#include "lanes/lanes.h"
#include "dsp_api.h"            // DspFpEnv: what this host declares about its audio thread
#include <cmath>
#include <algorithm>
#include <cstring>

extern "C" void clockwork_publish_audio_load(uint32_t cpuAvgCenti, uint32_t cpuPeakCenti,
                                       uint32_t callbackOverruns);

// Shared per-DSP-block render (see ClockworkProcessor.h). One copy of the
// tick → publish sequence for both HeadlessDriver and the engine's manual
// pump.
void renderAudioBlock(LinkAudioHost& linkAudio,
                      uint32_t blockSize,
                      uint32_t numOutputChannels,
                      uint32_t numInputChannels,
                      uint32_t sampleRate,
                      double   ntp,
                      uint64_t hostMicros,
                      uint32_t publishChannels) {
    if (publishChannels == 0) publishChannels = numOutputChannels;
    clockwork_tick(ntp, numOutputChannels, numInputChannels);

    // This block has read its ports (pull_port_sources, in the tick): what
    // each is at now is read when the next block begins, a block on. Dated
    // after the read, not before, because a block that found its port short
    // reads silence for the rest and leaves the position where it was; dated
    // before, the frames the endpoint then writes would be placed a block
    // early, and the stream heard to skip on its way back in.
    if (hostMicros != 0 && sampleRate != 0)
        linkAudio.dateReads(hostMicros + static_cast<uint64_t>(
            std::llround(1e6 * double(blockSize) / double(sampleRate))));

    // Publish the main sink (stereo when nOut >= 2, mono fallback for 1).
    // No-op when Link Audio is off / no subscriber.
    if (const float* outputBus = clockwork_audio_out()) {
        if (publishChannels >= 2) {
            linkAudio.publishAudioBlock(outputBus, outputBus + blockSize,
                                        static_cast<size_t>(blockSize), sampleRate, hostMicros);
        } else if (publishChannels == 1) {
            linkAudio.publishAudioBlock(outputBus, nullptr,
                                        static_cast<size_t>(blockSize), sampleRate, hostMicros);
        }
    }
}

ClockworkProcessor::ClockworkProcessor() = default;

void ClockworkProcessor::initialiseDsp(uint8_t* ringBufferStorage,
                                     int sampleRate,
                                     int numOutputChannels,
                                     int numInputChannels,
                                     const void* guestConfig,
                                     uint32_t guestConfigBytes,
                                     void* guestArena,
                                     uint32_t guestArenaBytes,
                                     int bufLen,
                                     const void* inbox,
                                     uint32_t inboxBytes,
                                     void* outbox,
                                     uint32_t outboxBytes)
{
    mRingBufferStorage = ringBufferStorage;
    mSampleRate        = sampleRate;
    mNumOutputChannels = numOutputChannels;
    mNumInputChannels  = numInputChannels;
    mDspInputChannels  = numInputChannels;   // re-synced in resume() after rebuilds
    mDspOutputChannels = numOutputChannels;  // re-synced in resume() after rebuilds

    // Choose the DSP block size. bufLen == 0 means "use platform
    // default" (always 128 on web due to AudioWorklet; on native the caller
    // normally picks one from the opened device's buffer). Clamp
    // into [32, kMaxBlockSize] because static_audio_bus is sized at the
    // max, and too-small blocks waste control-rate density.
    int chosen = (bufLen > 0) ? bufLen : clockwork::kDefaultBlockSize;
    chosen = std::clamp(chosen, 32, clockwork::kMaxBlockSize);
    mBufLen = chosen;

    mPrefetchBuf.assign(static_cast<size_t>(numOutputChannels) * mBufLen, 0.0f);

    // Initial accumulator: 2*mBufLen per channel. deviceStarting will grow
    // it if the HW buffer size exceeds this.
    int inChans = std::max(1, numInputChannels);
    mAccumPerChanCap = mBufLen * 2;
    mInputAccum.assign(static_cast<size_t>(inChans) * mAccumPerChanCap, 0.0f);
    mInputAccumCount = 0;

    // This host arms denormal flush-to-zero on its audio thread every callback
    // (Smoothie's device callback does, before each block), so say so: the
    // guest reads DspConfig::fp_env and must not discover the flag by rendering
    // a different filter tail here than it renders in a browser.
    clockwork_declare_fp_env(DSP_FP_ENV_FLUSH_TO_ZERO);

    // Link Audio's lanes, which the bridge hands to subscriptions: the engine
    // reserves them itself, before the boot that lays them out, so every
    // native host has the same channels.
    LinkAudioBridge::reserveLanes();

    // Boot through the lanes ABI. Clockwork's geometry travels as arguments;
    // the guest's block travels as bytes it copies without reading; the guest's
    // region travels as a base and a length. That region is NULL when there is
    // no public segment — a headless engine has no client to share bulk with,
    // so a guest allocates from the system as it always did.
    clockwork_init(static_cast<double>(sampleRate),
             static_cast<uint32_t>(mBufLen),
             static_cast<uint32_t>(numInputChannels),
             static_cast<uint32_t>(numOutputChannels),
             /*verbosity*/ 0,
             guestArena,
             guestArenaBytes,
             guestConfig,
             guestConfigBytes,
             // No span: this host's allocator is safe to take the guest's
             // real-time pool from, so clockwork::mem stays on the system.
             /*arena_bytes*/ 0,
             inbox, inboxBytes, outbox, outboxBytes);
}

void ClockworkProcessor::deviceStarting(const smoothie::DeviceInfo& device) {
    mSampleRate     = static_cast<int>(device.sampleRate);
    mNominalRate.store(mSampleRate, std::memory_order_relaxed);
    mDeviceBufferSize.store(device.bufferFrames, std::memory_order_relaxed);
    mSamplePosition = 0.0;
    mPrefetchCount  = 0;
    mInputAccumCount = 0;
    clockwork::resetAudioBlockClock(*mClockworkClock, *mLinkAudio, mSamplePosition, mSampleRate);
    // A device restart can change the rate under a running engine, so the
    // endpoint is told again here as well as at initialiseDsp.
    mLinkAudio->setAudioFormat(static_cast<uint32_t>(mSampleRate),
                               clockworkLinkInputWidth(mNumInputChannels));
    std::fill(mPrefetchBuf.begin(), mPrefetchBuf.end(), 0.0f);

    // A device (re)start can hand the callback to a brand-new audio thread, so
    // re-arm realtime promotion; the first IO callback below performs it on the
    // audio thread itself (this function runs on the control thread).
    mRealtimeElevated.store(false, std::memory_order_relaxed);
    mFirstCallbackLogged.store(false, std::memory_order_relaxed);

    // Sync our internal channel counts from what the device actually opened.
    // mNumInput/OutputChannels are what we tell the DSP (via clockwork_tick's
    // active-channel args) and what we clamp the device's channel arrays to.
    // Without this, initialiseDsp()'s boot-time values stick forever even
    // after cold swaps that change the channel count (e.g. re-enabling inputs
    // after a mic-permission grant). Result: hw mic delivers samples but
    // the DSP never sees them.
    int activeIn  = device.inputChannels;
    int activeOutCount = device.outputChannels;
    if (activeIn > 0)       mNumInputChannels  = activeIn;
    if (activeOutCount > 0) mNumOutputChannels = activeOutCount;

    // Resize mPrefetchBuf if the new device exposes more output channels
    // than we allocated at initialiseDsp. mPrefetchBuf is indexed per
    // channel as prefBase + static_cast<size_t>(ch) * mBufLen — writing to a ch beyond the
    // original allocation corrupts memory after the HW-buffer < mBufLen
    // prefetch path runs. Example: boot on a 2-ch device (buffer sized
    // for 2), cold-swap to 4-ch Loopback, prefetch writes at ch=2 / ch=3
    // fall off the end of the buffer.
    {
        size_t needed = static_cast<size_t>(mNumOutputChannels) * mBufLen;
        if (mPrefetchBuf.size() < needed)
            mPrefetchBuf.assign(needed, 0.0f);
    }

    // Size the input accumulator to hold at least one full HW callback's
    // worth of samples plus a DSP block. HW buffers > 2*mBufLen (e.g.
    // 512 or 1024) would otherwise overflow the default 256-sample
    // accumulator and corrupt memory in the overflow branch.
    //
    // Two independent growth dimensions: per-channel capacity AND
    // channel count. The accumulator is channel-major — the callback
    // addresses it as `accumBase + ch * mAccumPerChanCap` — so the
    // vector must hold mAccumPerChanCap * chans floats. A cold-swap
    // from a 2-ch device to an 8-ch device at the same hardware
    // buffer size leaves mAccumPerChanCap unchanged but doubles the
    // required total size; without a separate channel-count check,
    // the next callback's input memcpy would walk past the vector
    // end at ch >= old chans.
    int hwBufSize = device.bufferFrames;
    int perChanNeeded = std::max(hwBufSize + mBufLen, 2 * mBufLen);
    int chans = std::max(1, std::max(activeIn, mNumInputChannels));
    if (perChanNeeded > mAccumPerChanCap)
        mAccumPerChanCap = perChanNeeded;
    size_t neededSize = static_cast<size_t>(mAccumPerChanCap) * chans;
    if (mInputAccum.size() < neededSize) {
        clockwork_log("[juce] resizing input accum: %zu -> %zu floats "
                "(perChanCap=%d chans=%d hwBuf=%d activeIn=%d mNumIn=%d)",
                mInputAccum.size(), neededSize, mAccumPerChanCap, chans,
                hwBufSize, activeIn, mNumInputChannels);
        mInputAccum.assign(neededSize, 0.0f);
    }

    // Log the full output channel layout so we can verify which physical
    // channels the aggregate is exposing and which are actually active.
    // Ordering matters: for an aggregate combining e.g. MBP Speakers +
    // MOTU, channels 0-1 might be MBP and 2-3 might be MOTU (or vice
    // versa, depending on sub-device list order). We write to
    // whatever channels the device marks active — usually the first two.
    // Channel-layout dump on every device open — kept always-on because
    // it's the one-shot output we need to diagnose "my MOTU output 5 has
    // the wrong thing" / aggregate-ordering issues in user bug reports.
    // One per device start, so the volume is bounded.
    {
        clockwork_log("[juce] output channels (%d total):", static_cast<int>(device.outputs.size()));
        for (size_t i = 0; i < device.outputs.size(); ++i) {
            clockwork_log("[juce]   [%d] %s%s", static_cast<int>(i),
                    device.outputs[i].name.c_str(), device.outputs[i].active ? " (active)" : "");
        }
    }
    clockwork_log("[juce] aboutToStart: device='%s' type='%s' sr=%d bs=%d activeOut=%d activeIn=%d outLat=%d inLat=%d",
            device.name.c_str(),
            device.driver.c_str(),
            mSampleRate,
            hwBufSize,
            activeOutCount, activeIn,
            device.outputLatencyFrames,
            device.inputLatencyFrames);

    mOutputLatencySamples = device.outputLatencyFrames;

    // Native timing: set ntp_start and drift to 0. NTP is derived from sample
    // position with slow drift correction (see ClockworkClock::updateAudioThreadNTP),
    // so these offsets are unused.
    if (mRingBufferStorage) {
        double* ntpStartPtr = reinterpret_cast<double*>(
            mRingBufferStorage + NTP_START_TIME_START);
        *ntpStartPtr = 0.0;

        auto* driftPtr = reinterpret_cast<std::atomic<int32_t>*>(
            mRingBufferStorage + DRIFT_OFFSET_START);
        driftPtr->store(0, std::memory_order_relaxed);
    }

}

void ClockworkProcessor::deviceStopped() {
    clockwork_log("[juce] audioDeviceStopped (callbackCount=%u)", mCallbackCount);
    mSamplePosition = 0.0;
    mPrefetchCount  = 0;
}

// --- Pause/resume ---

void ClockworkProcessor::pause() {
    mPaused.store(true, std::memory_order_release);
}

void ClockworkProcessor::resume() {
    mSamplePosition = 0.0;
    mPrefetchCount  = 0;
    mInputAccumCount = 0;      // discard stale mic samples from before pause
    clockwork::resetAudioBlockClock(*mClockworkClock, *mLinkAudio, mSamplePosition, mSampleRate);
    mCallbackCount  = 0;       // re-arm warmup for new device
    mLastCbTime     = {};      // clear gap detector baseline
    mOverrunCount   = 0;
    mTotalUs        = 0.0;
    mMaxUs          = 0.0;

    // Re-sync the cached DSP channel widths: a cold swap rebuilds the DSP
    // at the new device's channel counts, and the IO-callback clamps must
    // follow or writes to channels beyond the boot-time width never reach the
    // hardware (and re-enabled inputs never reach the DSP). Safe here:
    // resume() runs on the control thread while the callback is still
    // gated on mPaused, and the release store below publishes the update.
    // A non-zero output width doubles as a DSP-alive check — a failed rebuild
    // leaves no instance and these answer 0, in which case keep the previous
    // widths.
    if (get_audio_num_output_buses() > 0) {
        mDspOutputChannels = get_audio_num_output_buses();
        mDspInputChannels  = get_audio_num_input_buses();
    }

    mPaused.store(false, std::memory_order_release);
}

bool ClockworkProcessor::isPaused() const {
    return mPaused.load(std::memory_order_acquire);
}

void ClockworkProcessor::process(
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const smoothie::BlockTime& time)
{
    // One-shot per device start: proves the device's IO thread actually
    // reached us. Deliberately the first statement — if the process dies
    // later in this callback, the line is already on the ring for the gateway
    // thread to surface. Warmup-phase only, so the log never lands on a
    // steady-state block.
    if (!mFirstCallbackLogged.exchange(true, std::memory_order_relaxed)) {
        clockwork_log("[juce] first audio callback: numSamples=%d out=%d in=%d",
                numSamples, numOutputChannels, numInputChannels);
    }

    // Promote the audio thread to realtime once per device start. Done here
    // rather than in deviceStarting (control thread) because this
    // callback runs on the audio thread. A denied request (no rtprio
    // permission) leaves the thread unchanged; see RealtimeThread.h.
    if (!mRealtimeElevated.exchange(true, std::memory_order_relaxed)) {
        const auto rt = clockwork::elevateCurrentThreadToRealtime();
        clockwork_log("[juce] audio thread realtime: status=%d policy=%d prio=%d err=%d",
               static_cast<int>(rt.status), rt.policy, rt.priority, rt.error);
    }

    // ── If paused, output silence and touch nothing else ──────────────────────
    // Placed before the warmup counter, mSamplePosition and mLastCbTime reads
    // below: during a device swap/recovery the control thread runs resume(),
    // which resets exactly those fields, and a freshly-opened device can already
    // be delivering callbacks on its own IO thread before resume() clears
    // mPaused. Gating here keeps that callback off every resume-mutated field
    // (the acquire pairs with resume()'s release, publishing the reset), so
    // there's no data race — and no need to make those hot-path fields atomic.
    if (mPaused.load(std::memory_order_acquire)) {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0,
                            static_cast<size_t>(numSamples) * sizeof(float));
        processCount.fetch_add(1, std::memory_order_release);
        processCount.notify_all();
        return;
    }

    int nIn  = std::min(numInputChannels,  mNumInputChannels);
    // Clamp to the DSP's output channel count as well as the live device count:
    // after a hot swap onto a wider device mNumOutputChannels exceeds what the
    // DSP renders, and the copy loops below would read staging rows the DSP
    // never writes (mirrors the mDspInputChannels clamp on the input side).
    int nOut = std::min(numOutputChannels,
                        std::min(mNumOutputChannels, mDspOutputChannels));

    const uint64_t engineBlockMicros = mSampleRate > 0
        ? static_cast<uint64_t>((static_cast<double>(mBufLen) * 1e6) / mSampleRate)
        : 0ULL;

    // Zero any extra output channels that the DSP won't fill
    for (int ch = nOut; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch])
            std::memset(outputChannelData[ch], 0,
                        static_cast<size_t>(numSamples) * sizeof(float));

    // ── Warmup: output silence for the first few callbacks to absorb page faults
    if (mCallbackCount < 4) {
        for (int ch = 0; ch < nOut; ++ch)
            if (outputChannelData[ch])
                std::memset(outputChannelData[ch], 0,
                            static_cast<size_t>(numSamples) * sizeof(float));
        mCallbackCount++;
        processCount.fetch_add(1, std::memory_order_release);
        processCount.notify_all();
        return;
    }

    // ── Timing measurement ────────────────────────────────────────────────────
    auto cbStart = std::chrono::high_resolution_clock::now();

    // Sleep/wake recovery: if gap > 2 seconds, re-anchor NTP timing
    // and purge stale messages so the engine doesn't try to catch up
    if (mLastCbTime.time_since_epoch().count() != 0) {
        double gapUs = std::chrono::duration<double, std::micro>(cbStart - mLastCbTime).count();
        if (gapUs > 2'000'000.0) {
            clockwork_log("  [wake] gap=%.0fs — re-anchoring NTP, purging stale messages",
                    gapUs / 1e6);
            clockwork::resetAudioBlockClock(*mClockworkClock, *mLinkAudio, mSamplePosition, mSampleRate);
            if (onWake) onWake();
        } else if (gapUs > hardening::stallThresholdUs(numSamples, mSampleRate)) {
            // Sub-wake stall: long enough to snap the timeline and push
            // scheduled client threads past their sched-ahead window (a client
            // raises TimingError on the affected live loops), too short for
            // the wake path above. Log its size — without this, such a stall
            // surfaces only as unexplained LATEs downstream. The threshold
            // scales with the callback period so a graph running us at a
            // huge quantum isn't misread as one stall per cycle.
            clockwork_log("  [gap] audio callback stalled %.0fms", gapUs / 1000.0);
        }
    }
    mLastCbTime = cbStart;

    int outputFilled  = 0;
    float* prefBase   = mPrefetchBuf.data();
    float* accumBase  = mInputAccum.data();
    const int accumPerChanCap = mAccumPerChanCap;

    // Accumulate available hardware input samples. Decouples HW buffer size
    // from the DSP's mBufLen block: we feed the DSP a full mBufLen only
    // when we have one, so no zero-padding inside a block.
    if (nIn > 0) {
        // Clamp incoming samples to our capacity. If the HW buffer is larger
        // than the accumulator (shouldn't happen — aboutToStart sized us to
        // fit), keep the newest samples only.
        int inSamples = std::min(numSamples, accumPerChanCap);
        int roomLeft  = accumPerChanCap - mInputAccumCount;
        if (inSamples > roomLeft) {
            // Drop oldest samples from accumulator to make room.
            int drop = inSamples - roomLeft;
            int keep = std::max(0, mInputAccumCount - drop);
            if (keep > 0) {
                for (int ch = 0; ch < nIn; ++ch)
                    std::memmove(accumBase + static_cast<size_t>(ch) * accumPerChanCap,
                                 accumBase + static_cast<size_t>(ch) * accumPerChanCap + drop,
                                 static_cast<size_t>(keep) * sizeof(float));
            }
            mInputAccumCount = keep;
        }
        // Copy new samples (or the tail if HW buffer > capacity)
        int srcOffset = numSamples - inSamples;
        for (int ch = 0; ch < nIn; ++ch) {
            if (inputChannelData[ch])
                std::memcpy(accumBase + static_cast<size_t>(ch) * accumPerChanCap + mInputAccumCount,
                            inputChannelData[ch] + srcOffset,
                            static_cast<size_t>(inSamples) * sizeof(float));
            else
                std::memset(accumBase + static_cast<size_t>(ch) * accumPerChanCap + mInputAccumCount,
                            0, static_cast<size_t>(inSamples) * sizeof(float));
        }
        mInputAccumCount += inSamples;
    }

    // ── 1. Drain leftover samples from the previous callback ─────────────────
    if (mPrefetchCount > 0) {
        int toDrain = std::min(mPrefetchCount, numSamples);
        for (int ch = 0; ch < nOut; ++ch)
            if (outputChannelData[ch])
                std::memcpy(outputChannelData[ch],
                            prefBase + static_cast<size_t>(ch) * mBufLen,
                            static_cast<size_t>(toDrain) * sizeof(float));
        if (toDrain < mPrefetchCount) {
            int remaining = mPrefetchCount - toDrain;
            for (int ch = 0; ch < nOut; ++ch)
                std::memmove(prefBase + static_cast<size_t>(ch) * mBufLen,
                             prefBase + static_cast<size_t>(ch) * mBufLen + toDrain,
                             static_cast<size_t>(remaining) * sizeof(float));
        }
        mPrefetchCount -= toDrain;
        outputFilled    = toDrain;
    }

    // ── 2. Generate mBufLen-sized DSP blocks until the device's buffer is full ───
    // One clock step per hardware callback: the sample-clock line is linear
    // across the sub-blocks, so one anchor serves them all (per-block cursor
    // advances happen in the loop). Negative latency reports from flaky
    // drivers clamp to 0 — a raw cast would push the anchor ~a day ahead.
    const clockwork::BlockTime bt = clockwork::beginAudioBlock(
        *mClockworkClock, *mLinkAudio, mSamplePosition, static_cast<double>(mSampleRate),
        static_cast<uint32_t>(std::max(0, mOutputLatencySamples)), mMetrics);
    double wallNTP = bt.ntp;         // advanced per sub-block below

    // hostTimeNs is the driver's "this buffer plays at T" timestamp (only the
    // CoreAudio backend supplies it; the others, our PipeWire backend among
    // them, give none). When absent, the block
    // keeps ClockworkClock's jitter-free stamp rather than a jittery link.clock()
    // read here.
    uint64_t linkAudioBlockHostMicros =
        time.hostTimeNs != 0 ? time.hostTimeNs / 1000ULL : bt.hostMicros;


    while (outputFilled < numSamples) {
        // Feed the DSP one full mBufLen block of input from the accumulator.
        // If the accumulator doesn't have a full block yet (common at startup
        // when HW buffer < mBufLen), fall back to zero-padding the rest —
        // but this is now a rare edge case, not the common path.
        float* inputBus = clockwork_audio_in();
        if (inputBus && nIn > 0) {
            int usable = std::min(mInputAccumCount, mBufLen);
            // Clamp to the DSP's input channel count: mNumInputChannels tracks
            // the live device and can exceed it after a device swap, which would
            // write into staging channels the DSP is never handed. Matches the
            // width process_audio passes to dsp_process.
            int inCh = std::min(nIn, mDspInputChannels);
            for (int ch = 0; ch < inCh; ++ch) {
                if (usable > 0)
                    std::memcpy(inputBus + static_cast<size_t>(ch) * mBufLen,
                                accumBase + static_cast<size_t>(ch) * accumPerChanCap,
                                static_cast<size_t>(usable) * sizeof(float));
                if (usable < mBufLen)
                    std::memset(inputBus + static_cast<size_t>(ch) * mBufLen + usable, 0,
                                static_cast<size_t>(mBufLen - usable) * sizeof(float));
            }
            if (usable > 0) {
                int remaining = mInputAccumCount - usable;
                if (remaining > 0) {
                    for (int ch = 0; ch < nIn; ++ch)
                        std::memmove(accumBase + static_cast<size_t>(ch) * accumPerChanCap,
                                     accumBase + static_cast<size_t>(ch) * accumPerChanCap + usable,
                                     static_cast<size_t>(remaining) * sizeof(float));
                }
                mInputAccumCount = remaining;
            }
        }

        if (preTick)
            preTick(mSamplePosition, wallNTP * 1000.0 - clockwork::kNtpEpochOffset * 1000.0);

        // The one per-block body every driver runs (renderAudioBlock): the
        // tick, dating the Link Audio reads it made, publishing the block.
        // Native timing: wall-clock NTP as-is (only the WASM build converts
        // its argument, from AudioContext time), advanced a block per
        // sub-block, as the Link Audio stamp is: the audio framework's
        // playback timestamp for THIS sub-block.
        renderAudioBlock(*mLinkAudio,
                         static_cast<uint32_t>(mBufLen),
                         static_cast<uint32_t>(mNumOutputChannels),
                         static_cast<uint32_t>(mNumInputChannels),
                         static_cast<uint32_t>(mSampleRate),
                         wallNTP, linkAudioBlockHostMicros,
                         static_cast<uint32_t>(nOut));
        wallNTP += static_cast<double>(mBufLen) / mSampleRate;
        linkAudioBlockHostMicros += engineBlockMicros;
        mSamplePosition += mBufLen;
        // Keep scope-stream writes anchored to the block being rendered.
        mClockworkClock->advanceEngineFrames(mSamplePosition);

        const float* outputBus = clockwork_audio_out();
        if (outputBus) {
            int needed  = numSamples - outputFilled;
            int toCopy  = std::min(needed, mBufLen);

            for (int ch = 0; ch < nOut; ++ch)
                if (outputChannelData[ch])
                    std::memcpy(outputChannelData[ch] + outputFilled,
                                outputBus + static_cast<size_t>(ch) * mBufLen,
                                static_cast<size_t>(toCopy) * sizeof(float));
            outputFilled += toCopy;

            // Save any leftover samples for the next device callback
            int leftover = mBufLen - toCopy;
            if (leftover > 0) {
                for (int ch = 0; ch < nOut; ++ch)
                    std::memcpy(prefBase + static_cast<size_t>(ch) * mBufLen,
                                outputBus + static_cast<size_t>(ch) * mBufLen + toCopy,
                                static_cast<size_t>(leftover) * sizeof(float));
                mPrefetchCount = leftover;
            }
        } else {
            // Engine not ready yet — init_memory hasn't run so the output
            // bus is null. Emit silence for the rest of this callback
            // rather than spinning in the while loop. This can happen
            // briefly at boot between aboutToStart and initialiseDsp().
            for (int ch = 0; ch < nOut; ++ch)
                if (outputChannelData[ch])
                    std::memset(outputChannelData[ch] + outputFilled, 0,
                                static_cast<size_t>(numSamples - outputFilled) * sizeof(float));
            outputFilled = numSamples;
        }
    }

    // (No recording tap here: the master mix leaves through the audio taps
    // a client reads — shm_audio_buffer slot 0 — and a client records it.)

    // ── 4. Timing stats (no I/O on audio thread — store atomically for external query) ──
    auto cbEnd = std::chrono::high_resolution_clock::now();
    double cbUs = std::chrono::duration<double, std::micro>(cbEnd - cbStart).count();
    double budgetUs = (static_cast<double>(numSamples) / mSampleRate) * 1e6;

    mCallbackCount++;
    mTotalUs += cbUs;
    if (cbUs > mMaxUs) mMaxUs = cbUs;
    if (cbUs > budgetUs) {
        mOverrunCount++;
        // An overrun (render missed its budget) is audible as a stutter but
        // produces no callback gap and no clock drift, so it needs its own
        // log line to be diagnosable. Rate-limited to one line per second.
        const double nowSec =
            std::chrono::duration<double>(cbEnd.time_since_epoch()).count();
        if (nowSec - mLastOverrunLogSec >= 1.0) {
            mLastOverrunLogSec = nowSec;
            clockwork_log("[overrun] render took %.1fms of %.1fms budget "
                   "(total overruns: %u)",
                   cbUs / 1000.0, budgetUs / 1000.0, mOverrunCount);
        }
    }

    // DSP load = how much of the callback's time budget the render consumed.
    // Smooth the average (EMA) and let the peak decay, so both track recent
    // behaviour rather than pinning to a one-off lifetime spike. Published to
    // native-stats as percent * 100 for the GUI dashboard.
    if (budgetUs > 0.0) {
        const double instPct = (cbUs / budgetUs) * 100.0;
        mLoadAvgPct  += (instPct - mLoadAvgPct) * 0.1;            // ~10-callback EMA
        mLoadPeakPct = std::max(instPct, mLoadPeakPct * 0.95);   // decaying peak
        clockwork_publish_audio_load(static_cast<uint32_t>(std::lround(mLoadAvgPct * 100.0)),
                               static_cast<uint32_t>(std::lround(mLoadPeakPct * 100.0)),
                               mOverrunCount);
    }

    // ── 5. Notify worker threads (one tick per device callback) ─────────────────
    processCount.fetch_add(1, std::memory_order_release);
    processCount.notify_all();
}
