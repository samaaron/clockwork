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
#include "JuceAudioCallback.h"
#include "clockwork_event_sink.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

bool engine_test::hostDrivesControlByDefault() {
    const char* v = std::getenv("CLOCKWORK_TEST_HOST_DRIVES_CONTROL");
    return v && *v && std::strcmp(v, "0") != 0;
}

// Caller holds mReplyMutex.
static std::string dumpReplies(const std::vector<OscReply>& replies) {
    if (replies.empty()) return "(none)";
    std::string out;
    for (auto& r : replies)
        out += "\n  | " + r.address + " (" + std::to_string(r.raw.size()) + " bytes)";
    return out;
}

ClockworkEngine::Config EngineFixture::defaultConfig() {
    ClockworkEngine::Config cfg;
    cfg.sampleRate        = 48000;
    cfg.bufferSize        = 128;
    cfg.udpPort           = 0;
    cfg.headless          = true;
    // The live engine leaves the counts at kAutoChannelCount so the device's
    // real width is used; without a device that means nothing, and a case
    // wants a predictable 2-in / 2-out world to reason against.
    cfg.numOutputChannels = 2;
    cfg.numInputChannels  = 2;
    cfg.hostDrivesControl = engine_test::hostDrivesControlByDefault();
    return cfg;
}

EngineFixture::EngineFixture() { init(defaultConfig()); }
EngineFixture::EngineFixture(const ClockworkEngine::Config& cfg) { init(cfg); }

void EngineFixture::init(const ClockworkEngine::Config& cfg) {
    mManualPump = cfg.manualAudioPump;

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

    // A host's thread, when the config says the host runs the control plane.
    // Started first: it spins on a closed door until init opens it, so the
    // pass is running from the moment the engine needs one.
    if (cfg.hostDrivesControl) mHostControl.start(mEngine);

    mEngine.init(cfg);

    // Boot barrier: a pong means the whole loop is live.
    OscReply r;
    send(osc_test::message("/dummy/ping"));
    waitForReply("/dummy/pong", r);

    clearReplies();
    clearDebugMessages();
}

EngineFixture::~EngineFixture() {
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
    // nothing ticks unless this thread does.
    if (mManualPump) {
        while (true) {
            pumpBlock();
            if (take()) return true;
            if (std::chrono::steady_clock::now() >= deadline) {
                UNSCOPED_INFO("waitForReply(\"" << addr << "\") timed out after "
                              << timeoutMs << "ms; replies held:" << repliesDump());
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
            UNSCOPED_INFO("waitForReply(\"" << addr << "\") timed out after "
                          << timeoutMs << "ms; replies held:" << dumpReplies(mReplies));
            return false;
        }
        mReplyCv.wait_until(lk, deadline);
    }
}

bool EngineFixture::waitForBlocks(uint32_t n, int timeoutMs) {
    const uint32_t start =
        mEngine.audioCallback().processCount.load(std::memory_order_acquire);
    // Unsigned wrap is fine: (now - start) counts blocks since the snapshot.
    return pollUntil([&] {
        return mEngine.audioCallback().processCount.load(std::memory_order_acquire) - start >= n;
    }, timeoutMs);
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

std::string EngineFixture::repliesDump() const {
    std::lock_guard<std::mutex> lk(mReplyMutex);
    return dumpReplies(mReplies);
}

void EngineFixture::stopHeadlessDriver() {
    mEngine.mHeadlessDriver.signalThreadShouldExit();
    mEngine.mHeadlessDriver.stopThread(2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void EngineFixture::pumpBlock(uint32_t n) {
    if (mManualPump) {
        for (uint32_t i = 0; i < n; ++i) mEngine.pumpAudioBlock();
        return;
    }
    // A driver thread is rendering: a block pumped here would be a second
    // renderer, which the engine refuses. What a test means by "pump n" with
    // a driver running is "let n blocks pass" — so wait for the driver's.
    if (!waitForBlocks(n))
        WARN("pumpBlock(" << n << "): the driver rendered no block within the wait");
}
