// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_clock_snapshot.cpp — /clockwork/clock/state/get on the native engine.
 *
 * The whole clock in one reply, answered on the audio thread from the mirror
 * the engine publishes (clockwork_sys.h). The web has always answered it
 * (test_dsp_boundary.cpp); natively the verb went to the control thread's
 * clock handler, which does not know it, and the client was told the verb is
 * unsupported.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_prefix.h"

TEST_CASE("clock/state/get is answered natively, as on the web", "[clock][control]") {
    EngineFixture fx;

    fx.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), 4241));
    OscReply tempo;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), tempo));

    fx.send(osc_test::message(CLOCKWORK_SYS("clock/state/get"), 4242));
    OscReply state;
    INFO(fx.repliesDump());
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("clock/state.reply"), state));

    const auto p = state.parsed();
    REQUIRE(p.argCount() == 8);
    CHECK(p.argDouble(0) == tempo.parsed().argDouble(0));   // bpm, as the clock says
    CHECK(p.argInt(5) > 0);                                  // meter_num
    CHECK(p.argInt(6) > 0);                                  // meter_den
    CHECK(lastInt(state) == 4242);                           // the caller's token
}
