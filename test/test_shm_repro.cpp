// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
#include "EngineFixture.h"

TEST_CASE("Engine boots with udpPort > 0 (cross-process shm)", "[shm-repro]") {
    ClockworkEngine::Config cfg;
    cfg.sampleRate       = 48000;
    cfg.bufferSize       = 128;
    cfg.udpPort          = 30099;  // non-zero → creates POSIX shm
    cfg.headless         = true;

    EngineFixture fx(cfg);

    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fx.waitForReply("/dummy/pong", r));
}
