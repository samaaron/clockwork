// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restart_plan.cpp — the least a restart does (src/restart_plan.h).
 *
 * The rule every host asks when audio comes back: carry on, tell the
 * instance, or build it again. The native engine's device switch and a host
 * coming back over its memory are each pinned where they ask it
 * (test_device_policy.cpp, test_restarts.cpp); this is the rule alone.
 */
#include "restart_plan.h"

#include <catch2/catch_test_macros.hpp>

using clockwork::planRestart;
using clockwork::RestartFacts;
using clockwork::RestartStep;

namespace {

constexpr RestartFacts kBuilt { 48000.0, 128, 2, 2 };

RestartFacts with(double rate, uint32_t block, uint32_t inputs, uint32_t outputs) {
    return RestartFacts { rate, block, inputs, outputs };
}

} // namespace

TEST_CASE("restart plan: nothing changed, the instance carries on", "[restarts][plan]") {
    for (uint32_t survives : { 0u, DSP_SURVIVES_RATE_CHANGE, DSP_SURVIVES_CHANNEL_CHANGE,
                               DSP_SURVIVES_RATE_CHANGE | DSP_SURVIVES_CHANNEL_CHANGE }) {
        const auto plan = planRestart(kBuilt, kBuilt, survives);
        CHECK(plan.step == RestartStep::Resume);
        CHECK_FALSE(plan.rebuildForRate);
        CHECK_FALSE(plan.rebuildForChannels);
        CHECK_FALSE(plan.rebuildForBlock);
    }
}

TEST_CASE("restart plan: a new rate rebuilds an instance that cannot take one, and tells one that can",
          "[restarts][plan]") {
    const auto now = with(44100.0, 128, 2, 2);

    const auto cannot = planRestart(kBuilt, now, DSP_SURVIVES_CHANNEL_CHANGE);
    CHECK(cannot.step == RestartStep::Rebuild);
    CHECK(cannot.rebuildForRate);
    CHECK_FALSE(cannot.rebuildForChannels);

    CHECK(planRestart(kBuilt, now, DSP_SURVIVES_RATE_CHANGE).step == RestartStep::Reconfigure);
}

TEST_CASE("restart plan: new channel ceilings rebuild an instance that cannot take them, and tell one that can",
          "[restarts][plan]") {
    for (const auto& now : { with(48000.0, 128, 2, 4), with(48000.0, 128, 0, 2) }) {
        const auto cannot = planRestart(kBuilt, now, DSP_SURVIVES_RATE_CHANGE);
        CHECK(cannot.step == RestartStep::Rebuild);
        CHECK(cannot.rebuildForChannels);
        CHECK_FALSE(cannot.rebuildForRate);

        CHECK(planRestart(kBuilt, now, DSP_SURVIVES_CHANNEL_CHANGE).step == RestartStep::Reconfigure);
    }
}

TEST_CASE("restart plan: both changing is told only to an instance that takes both", "[restarts][plan]") {
    const auto now = with(44100.0, 128, 2, 4);
    CHECK(planRestart(kBuilt, now, DSP_SURVIVES_RATE_CHANGE).step == RestartStep::Rebuild);
    CHECK(planRestart(kBuilt, now, DSP_SURVIVES_CHANNEL_CHANGE).step == RestartStep::Rebuild);
    CHECK(planRestart(kBuilt, now, DSP_SURVIVES_RATE_CHANGE | DSP_SURVIVES_CHANNEL_CHANGE).step
          == RestartStep::Reconfigure);
}

TEST_CASE("restart plan: a new block rebuilds, whatever the instance survives", "[restarts][plan]") {
    const auto plan = planRestart(kBuilt, with(48000.0, 256, 2, 2),
                                  DSP_SURVIVES_RATE_CHANGE | DSP_SURVIVES_CHANNEL_CHANGE);
    CHECK(plan.step == RestartStep::Rebuild);
    CHECK(plan.rebuildForBlock);
}
