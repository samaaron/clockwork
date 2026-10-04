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
#include "clockwork_prefix.h"

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
