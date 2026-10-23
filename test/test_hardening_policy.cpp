// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_hardening_policy.cpp — Graph-robustness policy helpers.
 *
 * Guards a failure mode seen in the field (PipeWire): stale session-manager
 * state can leave the *configured* default
 *     device pointing at hardware that is no longer present (user swapped
 *     sound cards), silently changing how streams negotiate. The registry
 *     mirror detects the ghost so it can be surfaced as a diagnostic.
 */
#include <catch2/catch_test_macros.hpp>
#include "HardeningPolicy.h"

using namespace hardening;

// ── Ghost default-device detection ────────────────────────────────────────

TEST_CASE("No ghost when nothing is configured", "[Hardening]") {
    REQUIRE_FALSE(defaultNodeMissing("", { "alsa_output.ssl2" }));
}

TEST_CASE("No ghost when the configured default is present", "[Hardening]") {
    REQUIRE_FALSE(defaultNodeMissing("alsa_output.ssl2",
                                     { "alsa_output.hdmi", "alsa_output.ssl2" }));
}

TEST_CASE("Ghost detected when the configured default is absent", "[Hardening]") {
    // The #3553 case: default still points at an unplugged Scarlett while
    // other sinks are present.
    REQUIRE(defaultNodeMissing("alsa_output.usb-Focusrite_Scarlett_18i20",
                               { "alsa_output.ssl2" }));
}

TEST_CASE("No ghost verdict while the graph has no candidates yet", "[Hardening]") {
    // During boot the metadata can arrive before any nodes have been
    // registered; an empty graph must not read as a ghost.
    REQUIRE_FALSE(defaultNodeMissing("alsa_output.ssl2", {}));
}
