// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_link_audio_drivers.cpp — every driver dates the Link Audio reads.
 *
 * The endpoint writes a peer's audio into a port for a date the audio thread
 * gives it: after each block has read, where the port stands and when the
 * next block reads from there (LinkAudioBridge::dateReads). Three drivers
 * render blocks — the device callback, the headless thread, the manual pump
 * — and each has to date, or the endpoint waits for a date that never comes
 * and every buffer the peer sends is dropped at the door, with nothing in
 * the counters a client reads to say why. The device callback did exactly
 * that once, when the dating moved into a body only the other two ran.
 *
 * No peer here: dating does not need one, and a count of dated blocks
 * against blocks rendered says whether a driver does it.
 */
#include <catch2/catch_test_macros.hpp>

#include "native/LinkAudioBridge.h"   // decides CLOCKWORK_LINK_AUDIO

#if CLOCKWORK_LINK_AUDIO

#include "EngineFixture.h"
#include "FakeAudioDevice.h"

TEST_CASE("link audio drivers: the device callback dates the reads of every block it renders",
          "[link-audio][drivers]") {
    auto sys = fake_audio::makeSimpleSystem();
    EngineFixture fx(fake_audio::fakeEngineConfig(sys, "Fake Speakers"));
    REQUIRE(fx.engine().currentDevice().name == "Fake Speakers");
    const uint64_t before = fx.engine().linkAudio().datedBlocks();
    REQUIRE(fx.waitForBlocks(20));
    CHECK(fx.engine().linkAudio().datedBlocks() - before >= 20);
}

TEST_CASE("link audio drivers: the headless thread dates the reads of every block it renders",
          "[link-audio][drivers]") {
    EngineFixture fx;
    const uint64_t before = fx.engine().linkAudio().datedBlocks();
    REQUIRE(fx.waitForBlocks(20));
    CHECK(fx.engine().linkAudio().datedBlocks() - before >= 20);
}

TEST_CASE("link audio drivers: the manual pump dates the reads of every block it renders",
          "[link-audio][drivers]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fx(cfg);
    const uint64_t before = fx.engine().linkAudio().datedBlocks();
    fx.pumpBlock(20);
    CHECK(fx.engine().linkAudio().datedBlocks() - before >= 20);
}

#endif  // CLOCKWORK_LINK_AUDIO
