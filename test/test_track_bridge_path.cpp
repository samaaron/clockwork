// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_track_bridge_path.cpp — where the engine finds its plugin bridge.
 *
 * TrackControl::resolveBridge is the whole of the search, pure: the path the
 * host configured (ClockworkEngine::Config::pluginBridgePath), the
 * CLOCKWORK_PLUGIN_BRIDGE override, then beside the executable. These cases
 * hand it files that are and are not there, so an embedder that names the
 * bridge gets the one it named, and a stale name falls back rather than
 * leaving the engine without plugins.
 */
#include "native/TrackControl.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>

#include <juce_core/juce_core.h>

namespace {

struct TempFile {
    juce::File file;
    explicit TempFile(const char* name)
        : file(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("clockwork-bridge-path-" + juce::String(name) + "-"
                                 + juce::String(juce::Random::getSystemRandom().nextInt64())))
    {
        file.replaceWithText("not a bridge, but a file that is there");
    }
    ~TempFile() { file.deleteFile(); }
    std::string path() const { return file.getFullPathName().toStdString(); }
};

} // namespace

TEST_CASE("resolveBridge: the configured path wins when it names a file", "[tracks][bridge]") {
    TempFile configured("configured");
    TempFile env("env");
    CHECK(TrackControl::resolveBridge(configured.path(), env.path(), "/nowhere") == configured.path());
}

TEST_CASE("resolveBridge: a configured path that is not there falls back to the override", "[tracks][bridge]") {
    TempFile env("env");
    CHECK(TrackControl::resolveBridge("/no/such/bridge", env.path(), "/nowhere") == env.path());
}

TEST_CASE("resolveBridge: with neither, the search beside the executable decides", "[tracks][bridge]") {
    // nothing beside /nowhere, and no installed layout: empty, as findBridgeBeside says
    CHECK(TrackControl::resolveBridge("", "", "/nowhere") == TrackControl::findBridgeBeside("/nowhere"));
}
