// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * EngineFixture.cpp — see EngineFixture.h.
 *
 * The headless driver ticks process_audio() at audio rate (2.67 ms per
 * 128-frame block at 48 kHz); the engine's own threads drain the rings, as
 * they do under a real device. In manual-pump mode there is no driver
 * thread and the wait primitives pump on the test thread instead.
 */
#include "EngineFixture.h"
#include "DebugTail.h"
#include "ClockworkProcessor.h"
#include "HeadlessDriver.h"
#include "clockwork_event_sink.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>

bool engine_test::hostDrivesControlByDefault() {
    const char* v = std::getenv("CLOCKWORK_TEST_HOST_DRIVES_CONTROL");
    return v && *v && std::strcmp(v, "0") != 0;
}

// Caller holds mReplyMutex.
// The last few held, and how many there are: a case that polls for a reply
// dumps this on every attempt, and a recovery loop can hold a thousand, which
// would bury the engine's own log.
static std::string dumpReplies(const std::vector<OscReply>& replies) {
    if (replies.empty()) return "(none)";
    constexpr size_t kShown = 12;
    std::string out;
    if (replies.size() > kShown)
        out += "\n  | (" + std::to_string(replies.size() - kShown) + " more before these)";
    for (size_t i = replies.size() > kShown ? replies.size() - kShown : 0; i < replies.size(); ++i)
        out += "\n  | " + replies[i].address + " (" + std::to_string(replies[i].raw.size()) + " bytes)";
    return out;
}

ClockworkEngine::Config EngineFixture::defaultConfig() {
    ClockworkEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.bufferSize        = 128;
    cfg.shmSegment           = false;
    cfg.headless          = true;
    // The live engine leaves the counts at kAutoChannelCount so the device's
    // real width is used; without a device that means nothing, and a case
    // wants a predictable 2-in / 2-out world to reason against.
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 2;
    cfg.hostDrivesControl = engine_test::hostDrivesControlByDefault();
    pumpByDefault(cfg);
    return cfg;
}

void EngineFixture::pumpByDefault(ClockworkEngine::Config& cfg) {
    // Pumped: the test thread renders every block (manualAudioPump) on a
    // clock that is the sample count (freewheelClock), so nothing a case
    // asserts can depend on how a real-time driver was scheduled, or on a
    // runner being busy. A case whose claim is about real time asks for the
    // headless driver itself (driverConfig), and is tagged [rt].
    // CLOCKWORK_TEST_REALTIME=1 runs the whole suite on the driver instead.
    const char* realtime = std::getenv("CLOCKWORK_TEST_REALTIME");
    const bool pumped = !(realtime && *realtime == '1');
    cfg.manualAudioPump = pumped;
    cfg.freewheelClock  = pumped;
}

ClockworkEngine::Config EngineFixture::driverConfig() {
    auto cfg = defaultConfig();
    cfg.manualAudioPump = false;
    cfg.freewheelClock  = false;
    return cfg;
}

EngineFixture::EngineFixture() { init(defaultConfig()); }
EngineFixture::EngineFixture(const ClockworkEngine::Config& cfg) { init(cfg); }
EngineFixture::EngineFixture(const ClockworkEngine::Config& cfg, int hostControlStartsAfterMs) {
    init(cfg, hostControlStartsAfterMs);
}
EngineFixture::EngineFixture(const ClockworkEngine::Config& cfg,
                             const std::function<void(ClockworkEngine&)>& beforeInit) {
    init(cfg, 0, beforeInit);
}

void EngineFixture::init(const ClockworkEngine::Config& cfg, int hostControlStartsAfterMs,
                         const std::function<void(ClockworkEngine&)>& beforeInit) {
    mManualPump = cfg.manualAudioPump;
    mAudioCanary = !cfg.manualAudioPump && cfg.headless && !cfg.headlessPacer;   // the headless driver, on wall time
    mSampleRate = cfg.sampleRate;

    mEngine.onReply = [this](const uint8_t* data, uint32_t size) {
        OscReply r;
        r.address = osc_test::parseAddress(data, size);
        r.raw.assign(data, data + size);
        {
            std::lock_guard<std::mutex> lk(mReplyMutex);
            mReplies.push_back(std::move(r));
        }
        mReplyCv.notify_all();
    };
    mEngine.onDebug = [this](const std::string& msg) {
        debug_tail::push(msg);
        std::lock_guard<std::mutex> lk(mDebugMutex);
        mDebugMessages.push_back(msg);
    };

    // The host's control thread, when the config says the host runs the
    // control plane. Started first: it spins until init opens the control
    // ring, so the pass is running from the moment the engine needs one.
    if (cfg.hostDrivesControl && hostControlStartsAfterMs <= 0) mHostControl.start(mEngine);

    if (beforeInit) beforeInit(mEngine);
    mEngine.init(cfg);

    if (cfg.hostDrivesControl && hostControlStartsAfterMs > 0) {
        mLateHostStart = std::thread([this, hostControlStartsAfterMs] {
            std::this_thread::sleep_for(std::chrono::milliseconds(hostControlStartsAfterMs));
            mHostControl.start(mEngine);
        });
    }

    // Boot barrier: a pong means the whole loop is live. The bound is one
    // only a broken boot reaches, and a timeout fails the case outright: a
    // pong given up on still arrives, and a case's own ping would take it
    // for its answer (test_engine_fixture.cpp). Skipped when no answer can
    // reach this fixture: nothing rendering, no guest running to hear the
    // ping, or a host draining the egress itself.
    const bool renders = mManualPump
        || mEngine.audioSource() != ClockworkEngine::AudioSource::None;
    if (renders && mEngine.engineState() == EngineState::Running && !cfg.hostDrainsEgress) {
        OscReply r;
        send(osc_test::message("/dummy/ping"));
        if (!waitForReply("/dummy/pong", r, 30000))
            FAIL("the engine booted but did not answer a ping within 30 s");
    }

    clearReplies();
    clearDebugMessages();
}

EngineFixture::~EngineFixture() {
    checkNoEgressLost(mEngine, mExpectedLoss);
    if (mAudioCanary) checkAudioKeptUp(mEngine, mEngine.egressClient(), mExpectedLoss);
    if (mLateHostStart.joinable()) mLateHostStart.join();
    mHostControl.stop();   // a host stops its thread before the engine
    mEngine.shutdown();
    clockwork_sink_close_all();
}

void EngineFixture::send(const osc_test::Packet& pkt) { send(pkt.ptr(), pkt.size()); }
void EngineFixture::send(const uint8_t* data, uint32_t size) { mEngine.sendOSC(data, size); }

bool EngineFixture::waitForReply(const std::string& addr, OscReply& out, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);

    auto take = [&]() -> bool {
        std::lock_guard<std::mutex> lk(mReplyMutex);
        for (auto it = mReplies.begin(); it != mReplies.end(); ++it) {
            if (it->address == addr) {
                out = *it;
                mReplies.erase(it);
                return true;
            }
        }
        return false;
    };

    // Manual pump: replies are delivered after the audio thread ticks, and
    // nothing ticks unless this thread does. Two budgets, audio and wall
    // clock, both spent before giving up (pollUntil says why).
    if (mManualPump) {
        uint32_t owed = blocksOfAudio(timeoutMs);
        while (true) {
            if (owed) { pumpBlock(); --owed; }
            if (take()) return true;
            if (!owed && std::chrono::steady_clock::now() >= deadline) {
                sayTimedOut(addr, timeoutMs, repliesDump());
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    }

    std::unique_lock<std::mutex> lk(mReplyMutex);
    while (true) {
        for (auto it = mReplies.begin(); it != mReplies.end(); ++it) {
            if (it->address == addr) {
                out = *it;
                mReplies.erase(it);
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            sayTimedOut(addr, timeoutMs, dumpReplies(mReplies));
            return false;
        }
        mReplyCv.wait_until(lk, deadline);
    }
}

uint32_t EngineFixture::blocksOfAudio(int ms) const {
    const double frames = double(ms) * double(mSampleRate) / 1000.0;
    return static_cast<uint32_t>(std::ceil(frames / double(get_audio_buffer_samples())));
}

bool EngineFixture::waitForBlocks(uint32_t n, int timeoutMs) {
    const uint32_t start =
        mEngine.processor().processCount.load(std::memory_order_acquire);
    // Unsigned wrap is fine: (now - start) counts blocks since the snapshot.
    auto rendered = [&] {
        return mEngine.processor().processCount.load(std::memory_order_acquire) - start;
    };
    // Manual pump: the case is the clock. The blocks are rendered here, so
    // there is no deadline to miss however busy the machine is.
    if (mManualPump) {
        pumpBlock(n);
        return rendered() >= n;
    }
    return pollUntil([&] { return rendered() >= n; }, timeoutMs);
}

std::vector<OscReply> EngineFixture::allReplies() const {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    return mReplies;
}

void EngineFixture::clearReplies() {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    mReplies.clear();
}

std::vector<std::string> EngineFixture::debugMessages() const {
    std::lock_guard<std::mutex> lk(mDebugMutex);
    return mDebugMessages;
}

void EngineFixture::clearDebugMessages() {
    std::lock_guard<std::mutex> lk(mDebugMutex);
    mDebugMessages.clear();
}

std::string EngineFixture::debugMessagesDump() const {
    std::lock_guard<std::mutex> lk(mDebugMutex);
    if (mDebugMessages.empty()) return "(none)";
    std::string out;
    for (auto& m : mDebugMessages) out += "\n  | " + m;
    return out;
}

// A case that polls for a reply times out on every attempt: the held list
// is said the first time for an address, and a line each time after.
void EngineFixture::sayTimedOut(const std::string& addr, int timeoutMs, const std::string& held) {
    const int n = ++mTimeouts[addr];
    if (n == 1)
        UNSCOPED_INFO("waitForReply(\"" << addr << "\") timed out after " << timeoutMs
                      << "ms; replies held:" << held
                      << "\nengine debug lines:" << debugMessagesDump());
    else
        UNSCOPED_INFO("waitForReply(\"" << addr << "\") timed out again (" << n << " times)");
}

std::string EngineFixture::repliesDump() const {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    return dumpReplies(mReplies);
}

void EngineFixture::startHeadlessDriver() {
    if (mManualPump) return;
    mEngine.mHeadlessDriver->start();
}

void EngineFixture::stopHeadlessDriver() {
    if (mManualPump) return;   // pumped: there is no driver to stop
    mEngine.mHeadlessDriver->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void EngineFixture::pumpBlock(uint32_t n) {
    if (mManualPump) {
        for (uint32_t i = 0; i < n; ++i) mEngine.pumpAudioBlock();
        return;
    }
    // A driver thread is rendering: a block pumped here would be a second
    // renderer, which the engine refuses. With a driver running, "pump n"
    // means "let n blocks pass", so wait for the driver's.
    if (!waitForBlocks(n))
        WARN("pumpBlock(" << n << "): the driver rendered no block within the wait");
}
