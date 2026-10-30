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
#include "clockwork_client.h"
#include "metrics_history.h"
#include "OscTestUtils.h"
#include "clockwork_event_sink.h"
#include "clockwork_prefix.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// No case ends with a frame that reached no client — refused by a full
// egress ring, or handed to a transport that could not deliver it — unless it
// says it means to (EngineFixture::expectEgressRefusals,
// expectEgressUndelivered). The line logging a refusal rides the same full
// ring, so the count is the only trace: checked as every fixture's engine goes
// down, so whichever case loses one fails by name.
struct EgressLossExpected {
    bool refused = false;
    bool undelivered = false;
    bool audioAway = false;   // the case stalls the audio on purpose (checkAudioKeptUp)
};
// THE AUDIO KEPT UP, every second of the run. The metrics history keeps, a
// second at a time, how many blocks the audio thread rendered
// (metrics_history.h, process_count): a second that rendered under half of
// what the rate and block size say is a stall — the machine starving the
// audio thread, or the engine's — and a case that did not say it meant one
// (expectAudioAway) is failed naming the second, rather than passing on a
// run that was not real time, or flaking on an answer that came late with
// nothing to say why. Checked for a driver on wall time only: a pumped case
// renders on the engine's own clock, and a second of that is exactly its
// blocks by construction.
inline void checkAudioKeptUp(const ClockworkEngine& engine, ClockworkClient* client, EgressLossExpected expected = {}) {
    if (!engine.isRunning() || !client || expected.audioAway) return;
    const auto& m = engine.getMetrics();
    const uint32_t rate  = m.audio_sample_rate.load(std::memory_order_relaxed);
    const uint32_t block = m.audio_block_size.load(std::memory_order_relaxed);
    if (!rate || !block) return;
    const int64_t expectedBlocks = static_cast<int64_t>(rate / block);
    // Each completed second between the first and the current (both part-way).
    for (uint32_t back = 1; back < clockwork::metrics_history::kBuckets; ++back) {
        ClockworkClientMetricBucket now{}, before{};
        now.struct_bytes = before.struct_bytes = sizeof now;
        if (clockwork_client_metrics_history_bucket(client, 0, back, 0, &now) != CLOCKWORK_OK || !now.recorded) break;
        if (clockwork_client_metrics_history_bucket(client, 0, back + 1, 0, &before) != CLOCKWORK_OK || !before.recorded) break;
        const int64_t rendered = now.last - before.last;
        INFO("second " << back << " before the end: " << rendered << " block(s) rendered of the " << expectedBlocks
             << " a second of audio is; the audio was away, and the case did not say it meant it (expectAudioAway)");
        CHECK(rendered * 2 >= expectedBlocks);
    }
}

inline void checkNoEgressLost(const ClockworkEngine& engine, EgressLossExpected expected = {}) {
    if (!engine.isRunning()) return;   // a case that shut it down, or a boot that failed
    const auto& m = engine.getMetrics();
    if (!expected.refused) {
        const uint32_t refused = m.egress_frames_refused.load(std::memory_order_relaxed);
        INFO(refused << " frame(s) refused by an egress ring: replies, broadcasts or log lines "
             "no client got (PerformanceMetrics::egress_frames_refused)");
        CHECK(refused == 0u);
    }
    if (!expected.undelivered) {
        const uint32_t undelivered = m.egress_frames_undelivered.load(std::memory_order_relaxed);
        INFO(undelivered << " frame(s) the transport could not deliver: a reply to a token no client "
             "answers to, or a subscriber it could not reach (PerformanceMetrics::egress_frames_undelivered)");
        CHECK(undelivered == 0u);
    }
}

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
        cfg.shmSegment           = false;       // no SHM segment, no cleanup of a real one
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
        checkNoEgressLost(engine);
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
// switchDevice() is non-blocking by design: while anything else holds the
// swap gate — a swap's tail, the device lane's own work — it answers "swap
// already in progress", and the engine's own callers retry (the debounced
// switch, for ~3 s). So does a case, until the gate is free: within a bound
// only a gate that is never released reaches.
template <typename Attempt>
SwapResult whenSwapGateFree(Attempt&& attempt) {
    SwapResult r;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        r = attempt();
        if (r.success || r.error != "swap already in progress") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < until);
    return r;
}

template <typename Fix, typename... Args>
SwapResult switchWhenFree(Fix& fix, Args&&... args) {
    return whenSwapGateFree([&] { return fix.engine().switchDevice(args...); });
}

// Booted, and subscribed to the engine's broadcasts as a GUI is: statechange,
// setup, the device reports. A case that waits for any of those needs this.
//
// The engine answers /clockwork/notify with notify.reply and THEN a snapshot
// of its state (snapshotStateTo: reason "snapshot", or an error's own
// reason). Both are the handshake, not anything the case heard happen, so
// both are in before the slate is cleared. Clearing on the reply alone lets
// the snapshot land after the clear, where a case reads its "running" as a
// state change it never heard happen.
template <typename Fix>
void subscribe(Fix& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    REQUIRE(fix.pollUntil([&] {
        for (const auto& r : fix.allReplies()) {
            if (r.address != CLOCKWORK_SYS("statechange")) continue;
            const auto p = r.parsed();
            if (p.argString(1) == "snapshot" || p.argString(0) == "error") return true;
        }
        return false;
    }, 2000));
    fix.clearReplies();
}

// Everything the engine's device lane was given before this has run: it
// runs its tasks in order.
inline void deviceLaneDone(ClockworkEngine& engine) {
    std::promise<void> done;
    auto finished = done.get_future();
    engine.postDeviceTask([&] { done.set_value(); });
    REQUIRE(finished.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
}

inline int32_t lastInt(const OscReply& r) {
    const auto p = r.parsed();
    return p.argCount() > 0 ? p.argInt(p.argCount() - 1) : -1;
}

class EngineFixture {
public:
    EngineFixture();
    explicit EngineFixture(const ClockworkEngine::Config& cfg);
    // A host whose control thread comes up late: started this long after
    // init returns rather than before it (Config::hostDrivesControl only).
    // What a slow host looks like to the boot barrier.
    EngineFixture(const ClockworkEngine::Config& cfg, int hostControlStartsAfterMs);
    // Given the engine before it boots, to set the hooks a case drives boot
    // through (testInitFailure and the like).
    EngineFixture(const ClockworkEngine::Config& cfg,
                  const std::function<void(ClockworkEngine&)>& beforeInit);
    ~EngineFixture();

    // OSC in, in process — no socket.
    // A case that overflows an egress ring on purpose, or sends to a client
    // that has gone, says so, and is not failed for it at teardown
    // (checkNoEgressLost).
    void expectEgressRefusals() { mExpectedLoss.refused = true; }
    void expectEgressUndelivered() { mExpectedLoss.undelivered = true; }
    // The case stalls the audio on purpose: a second with few blocks is not failed (checkAudioKeptUp).
    void expectAudioAway() { mExpectedLoss.audioAway = true; }

    void send(const osc_test::Packet& pkt);
    void send(const uint8_t* data, uint32_t size);

    // Wait for a reply whose address is `addr`; it is removed from the
    // collection and written to `out`.
    bool waitForReply(const std::string& addr, OscReply& out, int timeoutMs = 2000);

    // Wait until the audio thread has rendered `n` more blocks, or time out.
    // Anchors a read of live state to DSP progress rather than wall clock,
    // which a loaded runner cannot honour.
    // n more blocks rendered: by the driver, within timeoutMs; or, under the
    // manual pump, rendered here and now (the timeout is not used).
    bool waitForBlocks(uint32_t n, int timeoutMs = 2000);

    // Poll `pred()` every few ms until true or timeout. In manual-pump mode
    // nothing advances the engine unless this thread does, so each poll
    // pumps a block first.
    //
    // Pumped, the wait has two budgets, and gives up only once both are
    // spent: timeoutMs of audio, pumped a block a turn (what the audio thread
    // answers after so many blocks arrives on audio time, however slow the
    // machine), and timeoutMs of wall clock (what one of the engine's threads
    // delivers once woken needs real time to run). Either budget alone fails
    // on a loaded machine, where each turn's sleep stretches and the wall
    // clock runs out before the audio does.
    template <typename Pred>
    bool pollUntil(Pred pred, int timeoutMs = 2000) {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(timeoutMs);
        uint32_t owed = mManualPump ? blocksOfAudio(timeoutMs) : 0;
        while (true) {
            if (owed) { pumpBlock(); --owed; }
            if (pred()) return true;
            if (!owed && std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(mManualPump ? 3 : 2));
        }
    }

    // The blocks the engine renders in `ms` of audio, at its rate and block size.
    uint32_t blocksOfAudio(int ms) const;

    std::vector<OscReply> allReplies() const;
    void clearReplies();
    // waitForReply's account of a timeout: the held list once per address, a line each time after.
    void sayTimedOut(const std::string& addr, int timeoutMs, const std::string& held);

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
    // What defaultConfig() does to a config, for a case that builds its own
    // from scratch: pumped (manualAudioPump + freewheelClock), unless
    // CLOCKWORK_TEST_REALTIME=1 asks for the driver everywhere.
    static void pumpByDefault(ClockworkEngine::Config& cfg);
    // The same, rendered by the headless driver on a thread of its own rather
    // than pumped: for a case whose claim is about a live audio thread (a
    // stalled one, a second renderer refused), or about traffic arriving
    // while nobody pumps. Such a case is tagged [rt] and run on its own.
    static ClockworkEngine::Config driverConfig();

    // The host's control thread, when the config asked for one.
    bool hostDrivesControl() const { return mHostControl.running(); }
    // Stop it, so the case is the host: no control pass runs but the ones it
    // runs itself (engine().controlPass()).
    void stopHostControl() { mHostControl.stop(); }

    // Stop the headless driver so the caller owns process_audio exclusively.
    void stopHeadlessDriver();
    // Start it again after stopHeadlessDriver: the audio back after a stall.
    void startHeadlessDriver();

    // Render `n` blocks on the calling thread via pumpAudioBlock().
    void pumpBlock(uint32_t n = 1);

    bool manualPump() const { return mManualPump; }

private:
    void init(const ClockworkEngine::Config& cfg, int hostControlStartsAfterMs = 0,
              const std::function<void(ClockworkEngine&)>& beforeInit = {});
    ClockworkEngine mEngine;
    engine_test::HostControlThread mHostControl;
    std::thread                    mLateHostStart;
    EgressLossExpected             mExpectedLoss;
    bool            mAudioCanary = false;   // a driver on wall time: checkAudioKeptUp at teardown
    bool            mManualPump = false;
    int             mSampleRate = 48000;

    mutable std::mutex       mReplyMutex;
    std::condition_variable  mReplyCv;
    std::vector<OscReply>    mReplies;
    std::map<std::string, int> mTimeouts;   // waitForReply timeouts so far, by address
    // Replies the client parser (clockwork_osc_decodes) would not decode:
    // the case fails at teardown naming each one.
    std::vector<std::string> mUndecodable;

    mutable std::mutex       mDebugMutex;
    std::vector<std::string> mDebugMessages;
};
