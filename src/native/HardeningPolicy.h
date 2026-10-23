// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * HardeningPolicy.h — pure policy helpers that keep the engine robust
 * against hostile audio-graph conditions (the field failure each one
 * guards is described at its definition below).
 *
 * Header-only and dependency-free so the callback path can use it inline
 * and the rules can be exercised without a running graph.
 */
#pragma once

#include <algorithm>
#include <string>
#include <vector>

// NB: injecting stream.dont-remix via PIPEWIRE_PROPS is deliberately NOT
// one of these helpers. With JUCE's unlabeled channel positions
// WirePlumber links an un-remixed multichannel stream to nothing at all,
// so it starves and the watchdog yanks the user off their chosen device.
// Multichannel routing is the patchbay device's job.

namespace hardening {

// True when the session manager's *configured* default device names a node
// that is absent from the graph while other candidates exist — the stale
// state left behind by unplugging or swapping a sound card. An empty graph
// is not a ghost: during boot the metadata routinely arrives before the
// node registry has populated.
inline bool defaultNodeMissing(const std::string& configuredName,
                               const std::vector<std::string>& presentNodeNames) {
    if (configuredName.empty() || presentNodeNames.empty())
        return false;
    return std::find(presentNodeNames.begin(), presentNodeNames.end(), configuredName)
           == presentNodeNames.end();
}

} // namespace hardening
