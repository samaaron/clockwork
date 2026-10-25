// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_engine_fixture.cpp — the fixture every engine case boots through.
 *
 * Its boot barrier pings and waits for the pong, so a case starts with the
 * whole loop live. A pong the barrier stops waiting for still comes, later,
 * and a /dummy/pong carries nothing to say which ping it answers: a case that
 * then pings takes the boot's pong for its own. On a loaded machine that is a
 * case passing or failing on timing alone.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include <chrono>
#include <thread>
#include "clockwork_client.h"
#include "clockwork_prefix.h"
#include "lanes/lanes_internal.h"

#include <vector>

namespace {

// A round trip with an answer of its own, tagged.
bool tempoRoundTrip(EngineFixture& fix, int32_t tag) {
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), tag));
    OscReply reply;
    return fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), reply, 30000)
        && lastInt(reply) == tag;
}

}  // namespace

TEST_CASE("Fixture: the boot barrier waits for its own pong, however late it comes",
          "[Fixture]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.hostDrivesControl = true;
    // Nothing reaches the fixture until the host passes, 2.5 s after boot.
    EngineFixture fix(cfg, 2500);

    // Two round trips: the first pass drains whatever was waiting, the second
    // answer comes from a later pass, so every reply from boot is in by then.
    REQUIRE(tempoRoundTrip(fix, 4747));
    REQUIRE(tempoRoundTrip(fix, 4848));

    int pongs = 0;
    for (const auto& r : fix.allReplies())
        if (r.address == "/dummy/pong") ++pongs;
    CHECK(pongs == 0);   // the barrier took its own
}

// THE TEARDOWN CHECK LOOKS (checkNoEgressLost). Every case passes with
// nothing lost, which is also what a check that stopped looking would say;
// these lose a frame each way and pass only by failing ([!shouldfail]), so a
// check that went blind turns them red. Their twins say they mean to.
namespace {
void loseOneUndelivered(EngineFixture& fix) {
    clockwork_client_count_undelivered(fix.engine().egressClient(), 1);   // as a host whose peer had gone
}
void loseOneRefused() {
    std::vector<uint8_t> tooBig(clockwork_egress_nrt_max_frame() + 1, 0);
    tooBig[0] = '/';
    REQUIRE_FALSE(clockwork_egress_nrt_write(EGRESS_REPLY, 1, tooBig.data(), static_cast<uint32_t>(tooBig.size())));
}
}  // namespace

TEST_CASE("Fixture: a case that ends with a frame no transport could deliver fails",
          "[Fixture][egress][!shouldfail]") {
    EngineFixture fix;
    loseOneUndelivered(fix);
}

TEST_CASE("Fixture: a case that ends with a frame an egress ring refused fails",
          "[Fixture][egress][!shouldfail]") {
    EngineFixture fix;
    loseOneRefused();
}

// THE TEARDOWN CHECK LOOKS AT THE AUDIO (checkAudioKeptUp): a case whose audio
// stalled for a second without saying so fails; its twin says it means to.
TEST_CASE("Fixture: a case whose audio stalled without saying so fails", "[Fixture][audio][rt][!shouldfail]") {
    EngineFixture fix(EngineFixture::driverConfig());   // the headless driver, on wall time
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // a full second rendered
    fix.stopHeadlessDriver();
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));   // a full second with no block
    fix.startHeadlessDriver();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // and a full second after, so the stall is a completed second
}

TEST_CASE("Fixture: a case that says its audio may be away is not failed for it", "[Fixture][audio][rt]") {
    EngineFixture fix(EngineFixture::driverConfig());
    fix.expectAudioAway();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    fix.stopHeadlessDriver();
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));
    fix.startHeadlessDriver();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
}

TEST_CASE("Fixture: a case that says it loses frames is not failed for them",
          "[Fixture][egress]") {
    EngineFixture fix;
    fix.expectEgressUndelivered();
    fix.expectEgressRefusals();
    loseOneUndelivered(fix);
    loseOneRefused();
}
