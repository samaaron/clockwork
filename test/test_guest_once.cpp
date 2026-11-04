// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_guest_once.cpp — a guest that leaves DspInfo::instances unset, as a
 * zeroed DspInfo does, is one a process holds once (docs/BOUNDARY.md, "How
 * many in a process"): a second core's build is refused with the reason, as
 * any build is refused; the first core's own rebuild is not, and its going
 * lets another in.
 *
 * Its own executable, clockwork_guest_once_tests, linked with the dummy
 * built to leave the count unset: a process links one guest.
 */
#include "BootedCore.h"
#include "audio_processor.h"   // destroy_dsp, rebuild_dsp
#include "clockwork_embed.h"
#include "dsp_api.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string_view>

using namespace core_test;

TEST_CASE("guest: one that leaves its count unset is held once, and a second core is refused with the reason",
          "[attach][guest]") {
    REQUIRE(dsp_describe()->instances == 0);

    std::optional<Core> first(std::in_place);
    CHECK(clockwork_boot_error(first->core) == nullptr);
    {
        Core second(nullptr, /*guest*/ false);
        const char* why = clockwork_boot_error(second.core);
        REQUIRE(why != nullptr);
        INFO(why);
        CHECK(std::string_view(why).find("only one can run in a process") != std::string_view::npos);
        CHECK(tickHere(first->core) == CLOCKWORK_TICK_RENDERED);
    }
    destroy_dsp(first->core);
    rebuild_dsp(first->core, kRate, kBlock);
    CHECK(clockwork_boot_error(first->core) == nullptr);

    first.reset();
    Core third;
    CHECK(clockwork_boot_error(third.core) == nullptr);
}

// The place is the core's through its own instance's free and rebuild: a
// core that comes between — booting while the first is between instances,
// as a cold restart leaves it — is the one refused, and the first's rebuild
// never is.
TEST_CASE("guest: a core keeps its place between its instances, and a core booting meanwhile is refused",
          "[attach][guest]") {
    Core first;
    REQUIRE(clockwork_boot_error(first.core) == nullptr);
    destroy_dsp(first.core);
    {
        Core meanwhile(nullptr, /*guest*/ false);
        const char* why = clockwork_boot_error(meanwhile.core);
        REQUIRE(why != nullptr);
        INFO(why);
        CHECK(std::string_view(why).find("only one can run in a process") != std::string_view::npos);
    }
    rebuild_dsp(first.core, kRate, kBlock);
    CHECK(clockwork_boot_error(first.core) == nullptr);
    CHECK(tickHere(first.core) == CLOCKWORK_TICK_RENDERED);
}

// A plugin's door: the second instance in the process is refused at attach,
// with the status, rather than handed an engine that renders silence.
TEST_CASE("guest: embed attach refuses the instance the guest cannot hold", "[attach][guest][embed]") {
    ClockworkEmbedConfig cfg {};
    cfg.struct_bytes    = sizeof cfg;
    cfg.sample_rate     = 48000;
    cfg.output_channels = 2;
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkEmbed* one = clockwork_embed_attach(&cfg, &st);
    REQUIRE(one != nullptr);
    CHECK(st == CLOCKWORK_OK);
    st = CLOCKWORK_OK;
    ClockworkEmbed* two = clockwork_embed_attach(&cfg, &st);
    CHECK(two == nullptr);
    CHECK(st == CLOCKWORK_E_NO_INSTANCE);
    clockwork_embed_close(two);
    clockwork_embed_close(one);
    // And once the first has gone, the place is free again.
    ClockworkEmbed* again = clockwork_embed_attach(&cfg, &st);
    CHECK(again != nullptr);
    clockwork_embed_close(again);
}
