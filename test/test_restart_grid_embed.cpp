// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restart_grid_embed.cpp — the restart grid's embedded host.
 *
 * A host that renders (clockwork_embed_attach) and reads the rings through
 * the handle's client closes its handle and attaches again: a plugin
 * reloaded, a host that changed its rate. With no other host holding the
 * engine, the attach lays the engine out again on the arena that is up, and
 * that replaces the instance (docs/RESTARTS.md, "One event, every route"):
 * RestartModel.h says what the host must hear.
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

#include <array>
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
    }
    void close() { clockwork_embed_close(mH); mH = nullptr; mC = nullptr; }

    void render(uint32_t blocks) {
        float* outs[2] = { mL.data(), mR.data() };
        for (uint32_t i = 0; i < blocks; ++i)
            REQUIRE(clockwork_embed_render(mH, outs, 2, nullptr, 0, kBlock) == kBlock);
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
    restart_model::checkReplaced(host.read(), "an embedded host", /*seesRefusals*/ true, before, after);
}

} // namespace

// Red until docs/RESTARTS.md's "Not yet" row 5 is done: [!shouldfail] passes
// while it fails, and fails the day it passes, so the tag goes with the fix.
TEST_CASE("restart grid: an embedded host attaching again", "[restarts][grid][embed][not-yet-5][!shouldfail]") {
    attachingAgain(48000.0);
}

TEST_CASE("restart grid: an embedded host attaching again at another rate", "[restarts][grid][embed][not-yet-5][!shouldfail]") {
    attachingAgain(44100.0);
}
