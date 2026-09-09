// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_engine_control.cpp — the engine's own /clockwork-sys surface, end to end.
 *
 * These cases go in at ingest() and come out on the reply egress, through
 * the route table and the control thread — the path a client's packet
 * takes. The unit cases in test_clockwork_sys.cpp cover the core handler alone;
 * what is pinned here is what the engine as a whole answers.
 */
#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_prefix.h"

#include <catch2/catch_test_macros.hpp>

using engine_test::Engine;

// "/clockwork/clock/offset" was the web build's global-offset knob, forwarded
// by the native engine to a state field the native audio thread never reads.
// A verb the native engine does not implement is refused, like any other
// unknown /clock verb — not silently swallowed.
TEST_CASE("clock/offset is not a native verb: refused as unsupported",
          "[engine][clock][clockwork_sys]") {
    Engine e;

    const auto pkt = osc_test::message(CLOCKWORK_SYS("clock/offset"), 0.5f);
    e.engine.ingest(pkt.ptr(), pkt.size(), 0);

    osc_test::ParsedReply r;
    REQUIRE(e.reply(CLOCKWORK_SYS("clock/unsupported"), r));
    CHECK(r.argString(0) == CLOCKWORK_SYS("clock/offset"));
}

// The meter goes in through the route table and comes back out of the
// same clock state the audio thread reads: the whole path, with a token.
TEST_CASE("clock/meter sets the session meter and the query reads it back",
          "[engine][clock][clockwork_sys]") {
    Engine e;

    auto set = osc_test::message(CLOCKWORK_SYS("clock/meter"), 7, 8);
    e.engine.ingest(set.ptr(), set.size(), 0);
    auto get = osc_test::message(CLOCKWORK_SYS("clock/meter"), 42);
    e.engine.ingest(get.ptr(), get.size(), 0);

    osc_test::ParsedReply r;
    REQUIRE(e.reply(CLOCKWORK_SYS("clock/meter.reply"), r));
    CHECK(r.argInt(0) == 7);
    CHECK(r.argInt(1) == 8);
    CHECK(r.argInt(2) == 42);

    auto bar = osc_test::message(CLOCKWORK_SYS("clock/bar"), 43);
    e.engine.ingest(bar.ptr(), bar.size(), 0);
    REQUIRE(e.reply(CLOCKWORK_SYS("clock/bar.reply"), r));
    CHECK(r.argDouble(1) >= 0.0);
    CHECK(r.argDouble(1) < 3.5);
    CHECK(r.argInt(2) == 7);
    CHECK(r.argInt(3) == 8);
    CHECK(r.argInt(4) == 43);
}
