// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restart_grid_embed.cpp — the restart grid's embedded host.
 *
 * A host that renders (clockwork_embed_attach) and reads the rings through
 * the handle's client closes its handle and attaches again: a plugin
 * reloaded, a host that changed its rate. With no other host holding the
 * engine, the attach is audio coming back to the arena that is up
 * (docs/RESTARTS.md, "One event, every route"), and it does the least that
 * gets the engine back where it was: at the rate it had, the instance
 * carries on; at another, which the placeholder cannot take, it is replaced.
 * RestartModel.h says what the host must hear either way.
 *
 * Here, in clockwork_attach_tests, because the event needs an attach count
 * that is back at zero, which only this binary walks. Each case leaves it
 * there.
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

    void attach(double rate) {
        ClockworkEmbedConfig cfg {};
        cfg.struct_bytes      = sizeof cfg;
        cfg.sample_rate       = rate;
        cfg.block_size        = kBlock;
        cfg.input_channels    = 0;
        cfg.output_channels   = 2;
        cfg.max_render_frames = kBlock;
        ClockworkStatus st = CLOCKWORK_OK;
        mH = clockwork_embed_attach(&cfg, &st);
        REQUIRE(mH != nullptr);
        mC = clockwork_embed_client(mH);
        REQUIRE(mC != nullptr);
        noteClock();
    }
    void close() { clockwork_embed_close(mH); mH = nullptr; mC = nullptr; }

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

uint32_t generation() { return clockwork_arena_generation(clockwork_arena_header()); }

void attachingAgain(double rate) {
    REQUIRE(clockwork_attached() == 0);
    Host host(48000.0);
    host.render(8);
    host.read();   // what the attach said

    host.send(restart_model::oldAsk());
    host.render(2);   // answered by the old instance, and not read

    host.stageAsset();
    for (const auto& ask : restart_model::waitingAsks()) host.send(ask);

    const uint32_t before = generation();
    host.close();
    host.attach(rate);
    const uint32_t after = generation();

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

TEST_CASE("restart grid: an embedded host attaching again", "[restarts][grid][embed]") {
    attachingAgain(48000.0);
}

TEST_CASE("restart grid: an embedded host attaching again at another rate", "[restarts][grid][embed]") {
    attachingAgain(44100.0);
}

TEST_CASE("restart grid: an embedded host attaching again after the audio was away hears what waited refused as stale",
          "[restarts][grid][embed]") {
    // The same attach, with the audio away longer than kAudioAwaySeconds
    // between the close and the attach: the instance carries on, and what
    // waited for it through the gap is stale — refused once as such, never
    // performed late, by the one rule every target keeps (process_audio); what
    // is sent after is answered. And the clock never went back.
    REQUIRE(clockwork_attached() == 0);
    Host host(48000.0);
    host.render(8);
    host.read();

    for (const auto& ask : restart_model::waitingAsks()) host.send(ask);
    const uint32_t before = generation();
    host.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(2300));
    host.attach(48000.0);
    CHECK(generation() == before);

    // A block first: what is in the ring as the first block back begins is
    // what waited, and is refused; what a host sends once it is rendering
    // again is taken.
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
    CHECK(stale == 1);
    CHECK(delivered == 0);
    CHECK(answered == 1);
    INFO("the engine's clock, as the host's client reads it, went back by " << host.clockWentBack() << " s");
    CHECK(host.clockWentBack() == 0.0);
}
