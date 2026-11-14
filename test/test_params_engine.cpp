// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_params_engine.cpp — the parameter verbs on a full native engine, which
 * registers its own routes rather than the lanes host's (test_params.cpp).
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cstdio>
#include <vector>

#include "EngineFixture.h"
#include "clockwork_prefix.h"
#include "params/clockwork_params.h"

using Catch::Approx;

TEST_CASE("params: a native engine answers the verbs, and a change is in the table and announced to nobody", "[params][engine]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.guestConfig = "paramsAtBoot=1\n";
    EngineFixture fx(cfg);
    fx.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));

    ClockworkParamEntry e{};
    e.id = 6000; e.kind = CLOCKWORK_PARAM_CONTINUOUS; e.min = 0.f; e.max = 2.f; e.def = 1.f;
    e.flags = CLOCKWORK_PARAM_AUTOMATABLE;
    std::snprintf(e.name, sizeof e.name, "drive");
    fx.send(osc_test::messageWithBlob(CLOCKWORK_SYS("param/define"), &e, sizeof e));
    OscReply reply;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("param/define.reply"), reply));

    // The guest's two from its boot, and this one.
    fx.clearReplies();
    fx.send(osc_test::message(CLOCKWORK_SYS("param/list")));
    OscReply head;
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("param/list.reply"), head));
    CHECK(head.parsed().argInt(1) == 3);

    osc_test::Builder set;
    set.begin(CLOCKWORK_SYS("param/set")) << osc::int32(6000) << 5.f;
    fx.send(set.end());
    // A set has no reply. Messages are handled in the order sent, so the answer to a
    // list sent after it says the set has landed.
    fx.clearReplies();
    fx.send(osc_test::message(CLOCKWORK_SYS("param/list")));
    REQUIRE(fx.waitForReply(CLOCKWORK_SYS("param/list.reply"), head));
    // This client asked for the engine's notices, and heard nothing of the define or the set.
    for (const OscReply& heard : fx.allReplies()) {
        CHECK(heard.address != CLOCKWORK_SYS("param/changed"));
        CHECK(heard.address != CLOCKWORK_SYS("param/defined"));
    }

    // The region a reader maps holds the value as held, not as asked.
    const ClockworkParamsHeader* t = clockwork_params_table(fx.engine().core());
    REQUIRE(t != nullptr);
    bool found = false;
    for (uint32_t s = 0; s < t->capacity; ++s) {
        ClockworkParamEntry got{};
        if (clockwork_params_read_entry(t, s, &got) == 1 && got.id == 6000) {
            found = true;
            CHECK(clockwork_params_value(t, s) == Approx(2.f));
        }
    }
    CHECK(found);
}
