// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * FakeAudioDevice.h — scriptable JUCE audio devices for hermetic engine tests.
 *
 * The seam is Config::deviceManagerFactory: the engine constructs its
 * juce::AudioDeviceManager through it (boot AND recovery recreate), so a
 * test can hand the engine a REAL manager whose only device types are
 * fakes. Every line of manager logic — name validation, setup resolution,
 * type switching, change broadcasts — is JUCE's own; only the device edge
 * (open/start/callback) is simulated. That is deliberately the opposite
 * of mocking the manager: the manager's behaviour is part of what the
 * engine tests must exercise.
 *
 * A FakeAudioIODevice delivers real callbacks from its own thread at
 * roughly hardware cadence, so waitForBlocks / the watchdog / first-tick
 * barriers behave as they do against hardware.
 *
 * Hermeticity caveat (macOS): init()'s default-device pre-checks and the
 * aggregate-promotion block still call real CoreAudio when
 * cfg.numInputChannels != 0 and no -H device is given. Hermetic tests boot
 * with cfg.hardwareDevice = "<fake name>" and cfg.numInputChannels = 0,
 * which skips both. fakeEngineConfig() below encodes that recipe.
 */
#pragma once

#include "ClockworkEngine.h"
#include <atomic>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fake_audio {

// One simulated hardware device. Tests mutate these (through the shared
// FakeSystem) to script failures; flags are read at open/start time.
struct FakeDeviceSpec {
    std::string name;
    int maxOutputChannels = 2;
    int maxInputChannels  = 2;
    std::vector<double> sampleRates { 44100.0, 48000.0 };
    std::vector<int>    bufferSizes { 64, 128, 256, 512 };

    bool failOpen      = false;  // open() errors outright
    bool failInputOpen = false;  // open() errors iff input channels requested
    // open() succeeds but callbacks never tick. Atomic: a case may flip it on
    // a running engine — the device comes back — and start() reads it on
    // whichever engine thread is reopening.
    std::atomic<bool> failStart { false };
    // A lying device: the rate its callbacks actually keep time at, whatever
    // rate it was opened at and reports. 0 = honest (ticks at the open rate).
    // sonic-pi#3565's mixer: reports 48000, delivers ~44100.
    double deliveredRate = 0;
    // Unplugged: absent from every device list and refused by name until it
    // comes back. Atomic: a case flips it on a running engine.
    std::atomic<bool> hidden { false };
    // Times the engine has tried to open it, failed or not.
    std::atomic<int> opens { 0 };
    // Gone where it stands: still listed, but its driver says it is no
    // longer alive, and it cannot be opened.
    std::atomic<bool> dead { false };
    // The rate the system now runs the device at, set outside the engine
    // (Windows' sound settings, for a shared-mode device); 0 = unchanged.
    // A stream opened at another rate cannot carry on — its driver says it
    // must be opened again — and this is the only rate it offers.
    std::atomic<double> systemRate { 0 };
};

class FakeAudioIODevice;
class FakeAudioIODeviceType;

// The simulated machine: device types and their devices, shared between
// the test (which scripts it) and every manager the factory builds —
// including managers built later by recovery's recreateDeviceManager.
struct FakeSystem {
    struct TypeSpec {
        std::string typeName;
        std::vector<std::shared_ptr<FakeDeviceSpec>> devices;
        int defaultDeviceIndex = 0;
    };
    std::vector<TypeSpec> types;

    std::shared_ptr<FakeDeviceSpec> device(const std::string& name) {
        for (auto& t : types)
            for (auto& d : t.devices)
                if (d->name == name) return d;
        return nullptr;
    }

    // The device types alive right now. Each manager owns its own, and
    // recovery replaces the manager, so a case delivers an OS notification
    // through whichever exists.
    std::mutex liveTypesMutex;
    std::vector<FakeAudioIODeviceType*> liveTypes;

    // The OS reporting "the device list changed": on the calling thread, at
    // the caller's moment, through whichever device type is alive. Fenced
    // against that type being destroyed mid-call, as CoreAudio's listener is
    // (LiveInternalRegistry). False when no manager exists.
    bool reportListChanged();
    // The OS reporting a change to an open device (its channels, whether it
    // is alive), the same way.
    bool reportOpenDeviceChanged();

    // ── Time the case keeps ──────────────────────────────────────────────
    // A device keeps wall time until the case takes the clock. From then on
    // each playing device delivers exactly the blocks due by the case's
    // clock, at its delivered rate — the first at once when it starts, as
    // hardware calls back promptly — and advanceTo() returns once every
    // playing device has delivered what is due. Frames and time move
    // together exactly, whatever the machine is doing; give the engine the
    // same clock (Config::watchdogClockMs = nowMs) and what the watchdog
    // measures is exact.
    void useVirtualTime();                  // from now; returns once every device has joined
    void useWallTime();                     // and back: devices keep wall time again
    void advanceTo(int64_t us);
    void advanceBy(int64_t us) { advanceTo(nowUs() + us); }
    int64_t nowUs() const { return mNowUs.load(); }
    int64_t nowMs() const { return mNowUs.load() / 1000; }

    // The device side of the clock (FakeAudioIODevice), under clockMutex.
    std::mutex                      clockMutex;
    std::condition_variable         clockCv;
    bool                            virtualTime = false;
    std::vector<FakeAudioIODevice*> playing;

private:
    std::atomic<int64_t> mNowUs { 0 };
};

class FakeAudioIODevice : public juce::AudioIODevice {
public:
    FakeAudioIODevice(std::shared_ptr<FakeSystem> system,
                      std::shared_ptr<FakeDeviceSpec> outSpec,
                      std::shared_ptr<FakeDeviceSpec> inSpec,
                      const juce::String& typeName)
        : juce::AudioIODevice(outSpec ? juce::String(outSpec->name)
                                      : juce::String(inSpec->name),
                              typeName),
          mSystem(std::move(system)), mOut(std::move(outSpec)), mIn(std::move(inSpec)) {}

    ~FakeAudioIODevice() override { close(); }

    // Open, the channels it was opened with — a driver's are read when it
    // opens, as CoreAudio's are — until it opens again.
    juce::StringArray getOutputChannelNames() override {
        return channelNames(mOpen ? mOpenOuts : liveOuts(), "Out");
    }
    juce::StringArray getInputChannelNames() override {
        return channelNames(mOpen ? mOpenIns : liveIns(), "In");
    }

    // What its driver says now (FakeDeviceSpec::dead, its channels).
    LiveState readLiveState() override {
        LiveState state;
        state.alive = !primary()->dead.load();
        if (state.alive) {
            const double systemRate = primary()->systemRate.load();
            state.mustReopen        = systemRate > 0 && systemRate != mRate;
            state.sampleRate        = systemRate > 0 ? systemRate : mRate;
            state.numOutputChannels = liveOuts();
            state.numInputChannels  = liveIns();
        }
        return state;
    }
    juce::Array<double> getAvailableSampleRates() override {
        juce::Array<double> r;
        for (double sr : offeredRates()) r.add(sr);
        return r;
    }
    juce::Array<int> getAvailableBufferSizes() override {
        juce::Array<int> r;
        for (int b : primary()->bufferSizes) r.add(b);
        return r;
    }
    int getDefaultBufferSize() override { return 128; }

    juce::String open(const juce::BigInteger& inputChannels,
                      const juce::BigInteger& outputChannels,
                      double sampleRate, int bufferSizeSamples) override {
        std::lock_guard<std::recursive_mutex> lk(mLifecycle);
        primary()->opens.fetch_add(1);
        // Error strings mimic real JUCE drivers, which name the device.
        // Nothing reads the wording: the engine attributes input-side
        // failures by retrying output-only, not by parsing these.
        if (primary()->failOpen || primary()->dead.load())
            return "Failed to open device: " + juce::String(primary()->name);
        if (mIn && mIn->failInputOpen && inputChannels.countNumberOfSetBits() > 0)
            return "Failed to open input device: " + juce::String(mIn->name);

        // Clamp requested bits to capacity — CoreAudio semantics.
        mOpenOuts = liveOuts();
        mOpenIns  = liveIns();
        mActiveOut = outputChannels;
        mActiveOut.setRange(mOpenOuts, 256, false);
        mActiveIn = inputChannels;
        mActiveIn.setRange(mOpenIns, 256, false);

        mRate = pickNearest(offeredRates(), sampleRate);
        mBufferSize = pickNearest(primary()->bufferSizes,
                                  bufferSizeSamples > 0 ? bufferSizeSamples
                                                        : getDefaultBufferSize());
        mOpen = true;
        return {};
    }

    // A real driver serialises its own open/close/start/stop, so the fake does
    // too: a race that remains is the caller's, not the harness's.
    void close() override {
        std::lock_guard<std::recursive_mutex> lk(mLifecycle);
        stop();
        mOpen = false;
    }
    bool isOpen() override { return mOpen; }

    void start(juce::AudioIODeviceCallback* callback) override {
        std::lock_guard<std::recursive_mutex> lk(mLifecycle);
        if (!mOpen || mPlaying.load()) return;
        mCallback = callback;
        if (callback) callback->audioDeviceAboutToStart(this);
        mPlaying.store(true);
        if (!primary()->failStart) {
            {
                std::lock_guard<std::mutex> clk(mSystem->clockMutex);
                mJoinedVirtual = false;
                mSystem->playing.push_back(this);
            }
            mThread = std::thread([this] { tickLoop(); });
        }
    }

    void stop() override {
        std::lock_guard<std::recursive_mutex> lk(mLifecycle);
        {
            std::lock_guard<std::mutex> clk(mSystem->clockMutex);
            if (!mPlaying.exchange(false)) return;
        }
        mSystem->clockCv.notify_all();
        if (mThread.joinable()) mThread.join();
        {
            std::lock_guard<std::mutex> clk(mSystem->clockMutex);
            auto& v = mSystem->playing;
            v.erase(std::remove(v.begin(), v.end(), this), v.end());
        }
        mSystem->clockCv.notify_all();
        if (mCallback) mCallback->audioDeviceStopped();
        mCallback = nullptr;
    }

    // Under the system's clockMutex: delivered everything due by `us`, or
    // not playing.
    bool caughtUpTo(int64_t us) const {
        return !mPlaying.load() || (mJoinedVirtual && mNextUs > static_cast<double>(us));
    }
    bool joinedVirtualTime() const { return !mPlaying.load() || mJoinedVirtual; }

    bool isPlaying() override { return mPlaying.load(); }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return mBufferSize; }
    double getCurrentSampleRate() override { return mRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return mActiveOut; }
    juce::BigInteger getActiveInputChannels() const override { return mActiveIn; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    // The spec that answers rate/buffer/failure questions: the output side
    // when present (it is the clock master everywhere in the engine), else
    // the input side.
    const std::shared_ptr<FakeDeviceSpec>& primary() const {
        return mOut ? mOut : mIn;
    }

    std::vector<double> offeredRates() const {
        const double systemRate = primary()->systemRate.load();
        return systemRate > 0 ? std::vector<double>{ systemRate } : primary()->sampleRates;
    }
    int liveOuts() const { return mOut ? mOut->maxOutputChannels : 0; }
    int liveIns()  const { return mIn  ? mIn->maxInputChannels   : 0; }

    static juce::StringArray channelNames(int count, const char* stem) {
        juce::StringArray names;
        for (int i = 0; i < count; ++i)
            names.add(juce::String(stem) + " " + juce::String(i + 1));
        return names;
    }

    static double pickNearest(const std::vector<double>& xs, double want) {
        double best = xs.empty() ? want : xs.front();
        for (double x : xs)
            if (std::abs(x - want) < std::abs(best - want)) best = x;
        return best;
    }
    static int pickNearest(const std::vector<int>& xs, int want) {
        int best = xs.empty() ? want : xs.front();
        for (int x : xs)
            if (std::abs(x - want) < std::abs(best - want)) best = x;
        return best;
    }

    void tickLoop() {
        const int nOut = mActiveOut.countNumberOfSetBits();
        const int nIn  = mActiveIn.countNumberOfSetBits();
        std::vector<std::vector<float>> outBufs(
            static_cast<size_t>(std::max(nOut, 1)),
            std::vector<float>(static_cast<size_t>(mBufferSize), 0.0f));
        std::vector<std::vector<float>> inBufs(
            static_cast<size_t>(std::max(nIn, 1)),
            std::vector<float>(static_cast<size_t>(mBufferSize), 0.0f));
        std::vector<float*>       outPtrs;
        std::vector<const float*> inPtrs;
        for (auto& b : outBufs) outPtrs.push_back(b.data());
        for (auto& b : inBufs)  inPtrs.push_back(b.data());

        // Tick on an absolute schedule, as hardware does: sleeping a fixed
        // period AFTER each callback would add the callback's own duration
        // to every period, and the watchdog's rate-skew check (5% tolerance
        // by default) would read an honest device as slow.
        const double tickRate = primary()->deliveredRate > 0 ? primary()->deliveredRate
                                                              : mRate;
        const double periodUs = 1e6 * mBufferSize / tickRate;
        const auto period = std::chrono::microseconds(static_cast<int64_t>(periodUs));
        auto next = std::chrono::steady_clock::now();
        FakeSystem& sys = *mSystem;
        while (mPlaying.load()) {
            // On the case's clock: wait for this block to fall due (the first
            // is due as the device joins).
            bool onCaseClock = false;
            {
                std::unique_lock<std::mutex> clk(sys.clockMutex);
                if (sys.virtualTime) {
                    onCaseClock = true;
                    if (!mJoinedVirtual) {
                        mJoinedVirtual = true;
                        mNextUs = static_cast<double>(sys.nowUs());
                        sys.clockCv.notify_all();
                    }
                    sys.clockCv.wait(clk, [&] {
                        return !mPlaying.load() || !sys.virtualTime
                            || static_cast<double>(sys.nowUs()) >= mNextUs;
                    });
                    if (!mPlaying.load()) break;
                    if (!sys.virtualTime) {     // the case handed the clock back
                        onCaseClock = false;
                        mJoinedVirtual = false;
                        next = std::chrono::steady_clock::now();
                    }
                }
            }
            if (mCallback)
                mCallback->audioDeviceIOCallbackWithContext(
                    nIn > 0 ? inPtrs.data() : nullptr, nIn,
                    outPtrs.data(), nOut, mBufferSize, {});
            if (onCaseClock) {
                {
                    std::lock_guard<std::mutex> clk(sys.clockMutex);
                    mNextUs += periodUs;
                }
                sys.clockCv.notify_all();
            } else {
                next += period;
                std::this_thread::sleep_until(next);
            }
        }
    }

    std::shared_ptr<FakeSystem> mSystem;
    std::shared_ptr<FakeDeviceSpec> mOut, mIn;
    juce::AudioIODeviceCallback* mCallback = nullptr;
    // On the case's clock (FakeSystem::useVirtualTime), under clockMutex:
    // whether this device has joined it, and when its next block falls due.
    bool   mJoinedVirtual = false;
    double mNextUs = 0;
    juce::BigInteger mActiveOut, mActiveIn;
    int mOpenOuts = 0, mOpenIns = 0;
    std::thread mThread;
    std::recursive_mutex mLifecycle;
    std::atomic<bool> mPlaying { false };
    bool mOpen = false;
    double mRate = 48000.0;
    int mBufferSize = 128;
};

class FakeAudioIODeviceType : public juce::AudioIODeviceType {
public:
    FakeAudioIODeviceType(std::shared_ptr<FakeSystem> system, size_t typeIndex)
        : juce::AudioIODeviceType(
              juce::String(system->types[typeIndex].typeName)),
          mSystem(std::move(system)), mTypeIndex(typeIndex) {
        std::lock_guard<std::mutex> lk(mSystem->liveTypesMutex);
        mSystem->liveTypes.push_back(this);
    }

    ~FakeAudioIODeviceType() override {
        std::lock_guard<std::mutex> lk(mSystem->liveTypesMutex);
        auto& v = mSystem->liveTypes;
        v.erase(std::remove(v.begin(), v.end(), this), v.end());
    }

    // What a real type does when the OS tells it the list changed, or that
    // an open device changed where it stands.
    void reportListChanged() { reportDeviceChange(DeviceChange::list); }
    void reportOpenDeviceChanged() { reportDeviceChange(DeviceChange::openDevice); }

    void scanForDevices() override { mScanned = true; }

    juce::StringArray getDeviceNames(bool wantInputNames) const override {
        juce::StringArray names;
        for (auto& d : spec().devices) {
            if (d->hidden.load()) continue;
            if (wantInputNames ? d->maxInputChannels > 0
                               : d->maxOutputChannels > 0)
                names.add(juce::String(d->name));
        }
        return names;
    }

    int getDefaultDeviceIndex(bool) const override {
        return spec().defaultDeviceIndex;
    }

    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override {
        if (!device) return -1;
        return getDeviceNames(asInput).indexOf(device->getName());
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName,
                                      const juce::String& inputDeviceName) override {
        auto out = find(outputDeviceName.toStdString());
        auto in  = find(inputDeviceName.toStdString());
        if (!out && !in) return nullptr;
        return new FakeAudioIODevice(mSystem, out, in, getTypeName());
    }

private:
    const FakeSystem::TypeSpec& spec() const {
        return mSystem->types[mTypeIndex];
    }
    std::shared_ptr<FakeDeviceSpec> find(const std::string& name) const {
        if (name.empty()) return nullptr;
        for (auto& d : spec().devices)
            if (d->name == name && !d->hidden.load()) return d;
        return nullptr;
    }

    std::shared_ptr<FakeSystem> mSystem;
    size_t mTypeIndex;
    bool mScanned = false;
};

inline void FakeSystem::useVirtualTime() {
    std::unique_lock<std::mutex> lk(clockMutex);
    virtualTime = true;
    clockCv.notify_all();
    // A device mid-way through a wall-clock period joins when it wakes.
    clockCv.wait(lk, [&] {
        for (auto* d : playing) if (!d->joinedVirtualTime()) return false;
        return true;
    });
}

inline void FakeSystem::useWallTime() {
    {
        std::lock_guard<std::mutex> lk(clockMutex);
        virtualTime = false;
    }
    clockCv.notify_all();
}

inline void FakeSystem::advanceTo(int64_t us) {
    std::unique_lock<std::mutex> lk(clockMutex);
    if (us > mNowUs.load()) mNowUs.store(us);
    clockCv.notify_all();
    clockCv.wait(lk, [&] {
        for (auto* d : playing) if (!d->caughtUpTo(us)) return false;
        return true;
    });
}

inline bool FakeSystem::reportListChanged() {
    std::lock_guard<std::mutex> lk(liveTypesMutex);
    if (liveTypes.empty()) return false;
    liveTypes.back()->reportListChanged();
    return true;
}

inline bool FakeSystem::reportOpenDeviceChanged() {
    std::lock_guard<std::mutex> lk(liveTypesMutex);
    if (liveTypes.empty()) return false;
    liveTypes.back()->reportOpenDeviceChanged();
    return true;
}

class FakeDeviceManager : public juce::AudioDeviceManager {
public:
    explicit FakeDeviceManager(std::shared_ptr<FakeSystem> system)
        : mSystem(std::move(system)) {}

    void createAudioDeviceTypes(
        juce::OwnedArray<juce::AudioIODeviceType>& types) override {
        for (size_t i = 0; i < mSystem->types.size(); ++i)
            types.add(new FakeAudioIODeviceType(mSystem, i));
    }

private:
    std::shared_ptr<FakeSystem> mSystem;
};

// The engine's watchdog on the case's clock (FakeSystem::useVirtualTime,
// Config::watchdogClockMs = the system's nowMs): polled every `pollMs` of it,
// while every playing device delivers what falls due, until `done` or `forMs`
// of it have passed. A device change in flight is not polled through (the
// watchdog does not measure during one): the clock creeps a millisecond per
// millisecond of wall time, so the devices the change opens deliver as it
// waits for them. How much of the case's clock a change takes then varies
// with the machine; what the watchdog measures never does — frames and time
// still move together, and nothing it judges spans a change. False if a
// change never finished (30 s of wall time), or `done` never came.
inline bool runWatchdogUntil(ClockworkEngine& engine, FakeSystem& sys, int pollMs,
                             int64_t forMs, const std::function<bool()>& done) {
    const int64_t end = sys.nowMs() + forMs;
    for (;;) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (engine.devicePhase() != ClockworkEngine::DevicePhase::Idle
               || engine.recoveryInFlight()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            sys.advanceBy(1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (done && done()) return true;
        if (sys.nowMs() >= end) return !done;
        sys.advanceBy(int64_t(pollMs) * 1000);
        engine.watchdogPoll();
    }
}

inline bool runWatchdogFor(ClockworkEngine& engine, FakeSystem& sys, int pollMs, int64_t forMs) {
    return runWatchdogUntil(engine, sys, pollMs, forMs, nullptr);
}

// A one-output-one-input machine on a single driver — the common case.
inline std::shared_ptr<FakeSystem> makeSimpleSystem() {
    auto sys = std::make_shared<FakeSystem>();
    auto out = std::make_shared<FakeDeviceSpec>();
    out->name = "Fake Speakers";
    out->maxInputChannels = 0;
    auto duplex = std::make_shared<FakeDeviceSpec>();
    duplex->name = "Fake Interface";
    duplex->maxOutputChannels = 2;
    duplex->maxInputChannels  = 2;
    auto mic = std::make_shared<FakeDeviceSpec>();
    mic->name = "Fake Microphone";
    mic->maxOutputChannels = 0;
    sys->types.push_back({ "FakeDriver", { out, duplex, mic }, 0 });
    return sys;
}

inline std::function<std::unique_ptr<juce::AudioDeviceManager>()>
makeFactory(std::shared_ptr<FakeSystem> system) {
    return [system] {
        return std::make_unique<FakeDeviceManager>(system);
    };
}

// The hermetic boot recipe (see header comment): a named -H open of a
// fake device with inputs disabled, real device paths active, watchdog
// off unless the test opts in.
inline ClockworkEngine::Config fakeEngineConfig(
        std::shared_ptr<FakeSystem> system,
        const std::string& bootOutput) {
    ClockworkEngine::Config cfg;
    cfg.sampleRate           = 48000;
    cfg.udpPort              = 0;
    cfg.headless             = false;
    cfg.numOutputChannels    = 2;
    cfg.numInputChannels     = 0;   // skip mac aggregate-promotion CoreAudio reads
    cfg.hardwareDevice       = bootOutput;  // skip mac default-output CoreAudio reads
    cfg.deviceManagerFactory = makeFactory(std::move(system));
    return cfg;
}

} // namespace fake_audio
