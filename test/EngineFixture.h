// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * EngineFixture.h — a whole ClockworkEngine booted with no audio device.
 *
 * Routes, control thread, scheduler: everything but a device. The test
 * pumps the audio blocks itself (manualAudioPump) and the engine's clock is
 * the sample count (freewheelClock), so a case can render seconds of audio
 * in milliseconds and read the engine's replies back off its egress.
 *
 * A ClockworkEngine owns the process-global audio_processor state, so the cases
 * that use this live in their own executable (clockwork_engine_tests), away from
 * the lanes fixture in clockwork_tests.
 */
#pragma once

#include "ClockworkEngine.h"
#include "OscTestUtils.h"
#include "clockwork_event_sink.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace engine_test {

constexpr int kSampleRate = 48000;

// A host's control thread: what a process that runs the engine's control
// plane itself (Config::hostDrivesControl) has — one thread of its own
// calling controlPass() until told to stop. Started before engine.init()
// (the pass is refused until the door opens, so an early start costs
// nothing) and stopped before engine.shutdown(), as a host must.
class HostControlThread {
public:
    ~HostControlThread() { stop(); }
    void start(ClockworkEngine& engine) {
        if (mThread.joinable()) return;
        mStop.store(false, std::memory_order_release);
        mThread = std::thread([this, &engine] {
            while (!mStop.load(std::memory_order_acquire)) {
                engine.controlPass();
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });
    }
    void stop() {
        mStop.store(true, std::memory_order_release);
        if (mThread.joinable()) mThread.join();
    }
    bool running() const { return mThread.joinable(); }

private:
    std::thread       mThread;
    std::atomic<bool> mStop{false};
};

// Whether every engine these fixtures boot should have the HOST drive its
// control plane, rather than the engine's own gateway thread. Read from the
// environment so the whole suite can be run a second time in that shape:
//
//     CLOCKWORK_TEST_HOST_DRIVES_CONTROL=1 ./build/test/clockwork_engine_tests
//
// Every case then reaches the subsystems through a host's thread, which is
// how a case that quietly depended on the engine's would show itself.
// test_host_control.cpp pins the shape explicitly; this runs everything
// else through it.
bool hostDrivesControlByDefault();

// A booted engine with no device. Replies (onReply) are kept for the MIDI
// case, which asks the engine which ports it has.
struct Engine {
    ClockworkEngine engine;
    std::mutex mu;
    std::vector<std::vector<uint8_t>> replies;
    HostControlThread hostControl;

    Engine() : Engine([](ClockworkEngine::Config&) {}) {}

    // `tweak` edits the config before init — a case that wants one field
    // different from the default boot.
    explicit Engine(const std::function<void(ClockworkEngine::Config&)>& tweak) {
        engine.onDebug = [](const std::string&) {};
        engine.onReply = [this](const uint8_t* d, uint32_t n) {
            std::lock_guard<std::mutex> lock(mu);
            replies.emplace_back(d, d + n);
        };
        ClockworkEngine::Config cfg;
        cfg.sampleRate        = kSampleRate;
        cfg.udpPort           = 0;       // no SHM segment, no cleanup of a real one
        cfg.numOutputChannels = 2;
        cfg.numInputChannels  = 0;
        cfg.headless          = true;
        cfg.manualAudioPump   = true;
        cfg.freewheelClock    = true;
        cfg.hostDrivesControl = hostDrivesControlByDefault();
        tweak(cfg);
        if (cfg.hostDrivesControl) hostControl.start(engine);
        engine.init(cfg);
        engine.pumpAudioBlock();         // anchors the audio-thread clock
    }
    ~Engine() {
        hostControl.stop();              // a host stops its thread first
        engine.shutdown();
        clockwork_sink_close_all();
    }

    // Render `seconds` of audio as fast as the dummy DSP allows.
    void pump(double seconds) {
        const double blocks = seconds * kSampleRate / 32.0;   // 32 = the smallest block
        for (int i = 0; i < static_cast<int>(blocks); ++i) engine.pumpAudioBlock();
    }

    // Wait for a reply with this address, or give up.
    bool reply(const char* address, osc_test::ParsedReply& out, double timeoutSec = 2.0) {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(static_cast<int>(timeoutSec * 1000));
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mu);
                for (const auto& r : replies) {
                    if (osc_test::parseAddress(r.data(), static_cast<uint32_t>(r.size())) == address) {
                        out = osc_test::parseReply(r.data(), static_cast<uint32_t>(r.size()));
                        return true;
                    }
                }
            }
            engine.pumpAudioBlock();     // the control thread wakes per block
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
};

}  // namespace engine_test

// ── EngineFixture — the full harness ─────────────────────────────────────────
//
// engine_test::Engine above is the minimal form: booted, manual pump, replies
// kept. EngineFixture is the same engine with what a longer case needs: a
// free-running headless driver by default (cfg.manualAudioPump makes the test
// thread the only audio-thread writer instead), waitForReply / pollUntil /
// waitForBlocks, and the debug stream kept for CAPTURE.
//
// Boot barrier: the dummy DSP answers /dummy/ping on the audio thread, so a
// /dummy/pong proves ingress, audio thread, DSP and egress are all live.

struct OscReply {
    std::string          address;
    std::vector<uint8_t> raw;

    osc_test::ParsedReply parsed() const {
        return osc_test::parseReply(raw.data(), static_cast<uint32_t>(raw.size()));
    }
};

// The correlation token a clockwork reply echoes as its LAST argument
// (see EngineClock.cpp): the id a request carried, back on its reply.
inline int32_t lastInt(const OscReply& r) {
    const auto p = r.parsed();
    return p.argCount() > 0 ? p.argInt(p.argCount() - 1) : -1;
}

class EngineFixture {
public:
    EngineFixture();
    explicit EngineFixture(const ClockworkEngine::Config& cfg);
    ~EngineFixture();

    // OSC in, in process — no socket.
    void send(const osc_test::Packet& pkt);
    void send(const uint8_t* data, uint32_t size);

    // Wait for a reply whose address is `addr`; it is removed from the
    // collection and written to `out`.
    bool waitForReply(const std::string& addr, OscReply& out, int timeoutMs = 2000);

    // Wait until the audio thread has rendered `n` more blocks, or time out.
    // Anchors a read of live state to DSP progress rather than wall clock,
    // which a loaded runner cannot honour.
    bool waitForBlocks(uint32_t n, int timeoutMs = 2000);

    // Poll `pred()` every few ms until true or timeout. In manual-pump mode
    // nothing advances the engine unless this thread does, so each poll
    // pumps a block first.
    template <typename Pred>
    bool pollUntil(Pred pred, int timeoutMs = 2000) {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(timeoutMs);
        while (true) {
            if (mManualPump) pumpBlock();
            if (pred()) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(mManualPump ? 3 : 2));
        }
    }

    std::vector<OscReply> allReplies() const;
    void clearReplies();

    std::vector<std::string> debugMessages() const;
    void clearDebugMessages();

    // One-per-line renderings for INFO/CAPTURE: an assertion on a count
    // expands to a bare number, so the contents have to be logged beside it.
    std::string debugMessagesDump() const;
    std::string repliesDump() const;

    ClockworkEngine& engine() { return mEngine; }

    // The headless config the default constructor boots with, so a case can
    // change one field and pass it to the Config constructor. Its
    // hostDrivesControl follows engine_test::hostDrivesControlByDefault();
    // a config that sets it has the fixture run the host's thread
    // (HostControlThread), started before init and stopped before shutdown.
    static ClockworkEngine::Config defaultConfig();

    // The host's control thread, when the config asked for one.
    bool hostDrivesControl() const { return mHostControl.running(); }

    // Stop the headless driver so the caller owns process_audio exclusively.
    void stopHeadlessDriver();

    // Render `n` blocks on the calling thread via pumpAudioBlock().
    void pumpBlock(uint32_t n = 1);

    bool manualPump() const { return mManualPump; }

private:
    void init(const ClockworkEngine::Config& cfg);
    ClockworkEngine mEngine;
    engine_test::HostControlThread mHostControl;
    bool            mManualPump = false;

    mutable std::mutex       mReplyMutex;
    std::condition_variable  mReplyCv;
    std::vector<OscReply>    mReplies;

    mutable std::mutex       mDebugMutex;
    std::vector<std::string> mDebugMessages;
};
