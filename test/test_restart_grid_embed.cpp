// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restart_grid_embed.cpp — the restart grid's embedded host.
 *
 * A host that renders (clockwork_embed_attach) and reads the rings through
 * the handle's client reconfigures it: a plugin deactivated and activated
 * again, a host that changed its rate. That is audio coming back to the
 * engine that is up (docs/RESTARTS.md, "One event, every route"), and it
 * does the least that gets the engine back where it was: at the rate it
 * had, the instance carries on; at another, which the placeholder cannot
 * take, it is replaced. RestartModel.h says what the host must hear either
 * way. Closing the handle instead ends the engine.
 *
 * Here, in clockwork_attach_tests, because each case's handle makes the
 * process's one core (clockwork_core.h), which only this binary arranges.
 * Each case leaves none.
 */
#include "RestartModel.h"
#include "clockwork_arena.h"
#include "clockwork_client.h"
#include "clockwork_embed.h"
#include "lanes/lanes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kBlock  = 128;
constexpr uint32_t kOrigin = 1;

// A host that renders, and its client: the reader.
class Host {
public:
    explicit Host(double rate) { attach(rate); }
    ~Host() { clockwork_embed_close(mH); }

    static ClockworkEmbedConfig config(double rate) {
        ClockworkEmbedConfig cfg {};
        cfg.struct_bytes      = sizeof cfg;
        cfg.sample_rate       = rate;
        cfg.block_size        = kBlock;
        cfg.input_channels    = 0;
        cfg.output_channels   = 2;
        cfg.max_render_frames = kBlock;
        return cfg;
    }
    void attach(double rate) {
        const ClockworkEmbedConfig cfg = config(rate);
        ClockworkStatus st = CLOCKWORK_OK;
        mH = clockwork_embed_attach(&cfg, &st);
        REQUIRE(mH != nullptr);
        mC = clockwork_embed_client(mH);
        REQUIRE(mC != nullptr);
        noteClock();
    }
    // A plugin host's deactivate and activate again: the same handle, with
    // what it renders at now.
    void reconfigure(double rate) {
        const ClockworkEmbedConfig cfg = config(rate);
        REQUIRE(clockwork_embed_reconfigure(mH, &cfg) == CLOCKWORK_OK);
        noteClock();
    }
    void close() { clockwork_embed_close(mH); mH = nullptr; mC = nullptr; }
    // The arena's generation, from the core whose memory the client reads.
    uint32_t generation() const {
        ClockworkRegion r {};
        REQUIRE(clockwork_client_region(mC, CLOCKWORK_REGION_METRICS, &r) == CLOCKWORK_OK);
        return clockwork_arena_generation(clockwork_arena_header(clockwork_core_of_memory(r.base)));
    }

    void render(uint32_t blocks) {
        float* outs[2] = { mL.data(), mR.data() };
        for (uint32_t i = 0; i < blocks; ++i) {
            REQUIRE(clockwork_embed_render(mH, outs, 2, nullptr, 0, kBlock) == kBlock);
            noteClock();
        }
    }
    // The engine's clock as the host's client reads it, after every block and
    // across the attach: the most it went back from one reading to the next.
    // Never, as on native and the web (test_restart_grid.cpp, restart_grid.spec.mjs).
    void noteClock() {
        ClockworkClientSampleClock sc {};
        sc.struct_bytes = sizeof sc;
        if (clockwork_client_sample_clock(mC, &sc) != CLOCKWORK_OK || sc.dac_ntp <= 0) return;
        if (mLastClock > 0 && sc.dac_ntp < mLastClock) mClockBack = std::max(mClockBack, mLastClock - sc.dac_ntp);
        mLastClock = sc.dac_ntp;
    }
    double clockWentBack() const { return mClockBack; }
    // The engine's clock_anchors metric (word 63), read through the host's
    // client. The embedded host uses the engine's clock, so its anchors are
    // counted like every other host's.
    uint32_t anchors() const {
        uint32_t words[128] = {};
        REQUIRE(clockwork_client_metrics(mC, words, 128) > 63);
        return words[63];
    }
    void send(const osc_test::Packet& p) {
        REQUIRE(clockwork_client_send(mC, p.ptr(), p.size(), kOrigin) == CLOCKWORK_OK);
    }
    restart_model::Heard read() {
        restart_model::Heard heard;
        std::array<ClockworkClientMessage, 64> batch {};
        for (;;) {
            const uint32_t n = clockwork_client_poll(mC, batch.data(), static_cast<uint32_t>(batch.size()));
            for (uint32_t i = 0; i < n; ++i) heard.push_back(osc_test::parseReply(batch[i].bytes, batch[i].length));
            if (n < batch.size()) return heard;
        }
    }
    // The 64 bytes the asset's commit names, where the host has an assets region.
    void stageAsset() {
        ClockworkRegion r {};
        if (clockwork_client_region(mC, CLOCKWORK_REGION_ASSETS, &r) == CLOCKWORK_OK && r.bytes >= 64)
            std::memset(r.base, 0x5A, 64);
    }

private:
    ClockworkEmbed*    mH = nullptr;
    ClockworkClient*   mC = nullptr;
    std::vector<float> mL = std::vector<float>(kBlock), mR = std::vector<float>(kBlock);
    double             mLastClock = 0, mClockBack = 0;   // noteClock()
};

void reconfiguring(double rate) {
    Host host(48000.0);
    host.render(8);
    host.read();   // what the attach said

    host.send(restart_model::oldAsk());
    host.render(2);   // answered by the old instance, and not read

    host.stageAsset();
    for (const auto& ask : restart_model::waitingAsks()) host.send(ask);

    const uint32_t before = host.generation();
    host.reconfigure(rate);
    const uint32_t after = host.generation();

    host.send(restart_model::newAsk());
    host.render(8);
    if (rate == 48000.0)
        restart_model::checkSurvived(host.read(), "an embedded host", before, after, 48000, /*changedBuffer*/ 0);
    else
        restart_model::checkReplaced(host.read(), "an embedded host", /*seesRefusals*/ true, before, after);
    INFO("the engine's clock, as the host's client reads it, went back by " << host.clockWentBack() << " s");
    CHECK(host.clockWentBack() == 0.0);
}

} // namespace

TEST_CASE("restart grid: an embedded host reconfiguring", "[restarts][grid][embed]") {
    reconfiguring(48000.0);
}

TEST_CASE("restart grid: an embedded host reconfiguring at another rate", "[restarts][grid][embed]") {
    reconfiguring(44100.0);
}

TEST_CASE("restart grid: an embedded host that closes and attaches again gets an engine of its own",
          "[restarts][grid][embed]") {
    // Closing a handle ends its engine (docs/ENGINES.md): what waited
    // for it goes with it, and the next attach is a fresh engine on fresh
    // memory, which numbers its own generations from the first
    // (docs/RESTARTS.md, "The generation"): no client of the old one is left
    // to hear a number twice, its client having closed with its handle.
    Host host(48000.0);
    host.render(8);
    host.read();
    for (const auto& ask : restart_model::waitingAsks()) host.send(ask);
    host.close();
    host.attach(48000.0);
    CHECK(host.generation() == 1);
    host.render(8);
    const restart_model::Heard heard = host.read();
    INFO(restart_model::dump(heard));
    for (const auto& m : heard) CHECK(m.address != "/dummy/events");
}

TEST_CASE("restart grid: an embedded host reconfiguring after the audio was away hears what waited answered, late",
          "[restarts][grid][embed]") {
    // The same reconfigure, with the audio away for over two seconds before
    // it: the instance carries on, and what waited for it through the gap is
    // delivered when it is back — the engine drops nothing for a gap, on any
    // target; what is sent after is answered. And the clock never went back.
    Host host(48000.0);
    host.render(8);
    host.read();

    for (const auto& ask : restart_model::waitingAsks()) host.send(ask);
    const uint32_t before   = host.generation();
    const uint32_t anchored = host.anchors();
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    host.reconfigure(48000.0);
    CHECK(host.generation() == before);

    // A block first: what is in the ring as the first block back begins is
    // what waited, and is taken; so is what a host sends once it is
    // rendering again.
    host.render(1);
    host.send(restart_model::newAsk());
    host.render(8);
    const restart_model::Heard heard = host.read();
    INFO(restart_model::dump(heard));
    int stale = 0, delivered = 0, answered = 0;
    for (const auto& m : heard) {
        if (m.address == restart_model::kError && m.argString(0) == "/dummy/events/get") {
            ++stale;
            CHECK(m.argString(1) == CLOCKWORK_REFUSED_STALLED);
        }
        if (m.address == "/dummy/events") ++delivered;
        if (m.address == "/dummy/reconfigured.reply") ++answered;
    }
    CHECK(stale == 0);
    CHECK(delivered == 1);
    CHECK(answered == 1);
    INFO("the engine's clock, as the host's client reads it, went back by " << host.clockWentBack() << " s");
    CHECK(host.clockWentBack() == 0.0);
    // The clock re-anchored exactly once, at the attach; the gap before it
    // did not add a second anchor.
    CHECK(host.anchors() == anchored + 1);
}
