// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//
// Discovery: finding plugins rather than being told where they are.
//
// The test plugin this build produces (ClockworkTestGain.vst3) is the only plugin
// guaranteed to exist on a machine running these, so the directory cases point
// at the build tree rather than at the system folders. What the system folders
// hold is not something a test can assert — but that they are ASKED for, and
// that scanning a folder with nothing in it answers zero rather than failing,
// both are.

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "plugin_discovery.h"
}

namespace fs = std::filesystem;

#ifndef CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE
#define CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE ""
#endif

namespace {
// The directory the test bundle sits IN — scanning is a folder operation.
std::string testPluginDir() {
    const std::string bundle = CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE;
    if (bundle.empty()) return {};
    return fs::path(bundle).parent_path().string();
}
}  // namespace

TEST_CASE("search paths are offered and outlive the call", "[plugin][discovery]") {
    const uint32_t n = plugin_search_paths(nullptr, 0);
    REQUIRE(n > 0);

    std::vector<const char*> paths(n, nullptr);
    REQUIRE(plugin_search_paths(paths.data(), n) == n);

    for (uint32_t i = 0; i < n; ++i) {
        REQUIRE(paths[i] != nullptr);
        REQUIRE(std::strlen(paths[i]) > 0);
    }

    // Static storage: a second call must not move or free the first's strings.
    std::string first = paths[0];
    std::vector<const char*> again(n, nullptr);
    plugin_search_paths(again.data(), n);
    REQUIRE(first == paths[0]);
    REQUIRE(std::string(again[0]) == first);
}

TEST_CASE("search paths are absolute and format-defined", "[plugin][discovery]") {
    const uint32_t n = plugin_search_paths(nullptr, 0);
    std::vector<const char*> paths(n, nullptr);
    plugin_search_paths(paths.data(), n);

    bool sawVst3 = false;
    for (uint32_t i = 0; i < n; ++i) {
        const std::string p = paths[i];
        // A relative search path would resolve against whatever the working
        // directory happened to be, which is not a location.
        REQUIRE(fs::path(p).is_absolute());
        if (p.find("VST3") != std::string::npos || p.find("vst3") != std::string::npos)
            sawVst3 = true;
    }
    REQUIRE(sawVst3);
}

TEST_CASE("a cap of zero still reports the true count", "[plugin][discovery]") {
    // The two-call pattern the header documents: ask for the size, then fill.
    // If cap 0 answered 0 the caller could never size the first allocation.
    const uint32_t n = plugin_search_paths(nullptr, 0);
    const char* one = nullptr;
    REQUIRE(plugin_search_paths(&one, 1) == n);
    if (n > 0) REQUIRE(one != nullptr);
}

TEST_CASE("scanning a missing directory answers zero, not an error", "[plugin][discovery]") {
    REQUIRE(plugin_scan_dir("/definitely/not/a/directory/here", nullptr, 0) == 0);
    REQUIRE(plugin_scan_dir("", nullptr, 0) == 0);
    REQUIRE(plugin_scan_dir(nullptr, nullptr, 0) == 0);
}

TEST_CASE("scanning a directory with no plugins answers zero", "[plugin][discovery]") {
    const fs::path dir = fs::temp_directory_path() / "clockwork_discovery_empty";
    fs::create_directories(dir);
    // Something that is emphatically not a plugin, to prove the walk does not
    // try to open every file it meets.
    {
        std::vector<char> junk(64, 'x');
        FILE* f = std::fopen((dir / "notes.txt").string().c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fwrite(junk.data(), 1, junk.size(), f);
        std::fclose(f);
    }
    REQUIRE(plugin_scan_dir(dir.string().c_str(), nullptr, 0) == 0);
    fs::remove_all(dir);
}

TEST_CASE("the build's own test plugin is discovered by scanning its folder",
          "[plugin][discovery]") {
    const std::string dir = testPluginDir();
    if (dir.empty()) return;   // a build without VST3 hosting has nothing to find

    const uint32_t n = plugin_scan_dir(dir.c_str(), nullptr, 0);
    REQUIRE(n >= 1);

    std::vector<PluginEntry> entries(n);
    REQUIRE(plugin_scan_dir(dir.c_str(), entries.data(), n) == n);

    bool found = false;
    for (const PluginEntry& e : entries) {
        if (std::string(e.name).find("ClockworkTestGain") == std::string::npos) continue;
        found = true;
        // The path must be openable as it stands — a discovery API that hands
        // back something plugin_open refuses has discovered nothing.
        REQUIRE(fs::exists(e.path));
        REQUIRE(std::strlen(e.id) > 0);
        REQUIRE(e.format == kPluginFormatVst3);
    }
    REQUIRE(found);
}

TEST_CASE("entries own their strings", "[plugin][discovery]") {
    const std::string dir = testPluginDir();
    if (dir.empty()) return;

    const uint32_t n = plugin_scan_dir(dir.c_str(), nullptr, 0);
    if (n == 0) return;
    std::vector<PluginEntry> first(n);
    plugin_scan_dir(dir.c_str(), first.data(), n);
    const std::string name = first[0].name;
    const std::string path = first[0].path;

    // PluginDesc borrows from the scanner's cache, so a rescan of the same
    // path rewrites it. PluginEntry must survive that — this is the whole
    // reason it copies rather than borrows.
    std::vector<PluginEntry> second(n);
    plugin_scan_dir(dir.c_str(), second.data(), n);

    REQUIRE(std::string(first[0].name) == name);
    REQUIRE(std::string(first[0].path) == path);
}

TEST_CASE("a short cap fills what it can and still reports the total",
          "[plugin][discovery]") {
    const std::string dir = testPluginDir();
    if (dir.empty()) return;
    const uint32_t n = plugin_scan_dir(dir.c_str(), nullptr, 0);
    if (n == 0) return;

    PluginEntry one {};
    REQUIRE(plugin_scan_dir(dir.c_str(), &one, 1) == n);
    REQUIRE(std::strlen(one.path) > 0);
}

TEST_CASE("scanning every standard folder does not crash and de-duplicates",
          "[plugin][discovery]") {
    // What is installed on the machine running this is unknown and unassertable.
    // That the call completes, and that whatever it returns carries no duplicate
    // identity, is not.
    const uint32_t n = plugin_scan_all(nullptr, 0);
    if (n == 0) return;   // a machine with no plugins installed is a valid one

    std::vector<PluginEntry> all(n);
    REQUIRE(plugin_scan_all(all.data(), n) == n);

    std::vector<std::string> keys;
    for (const PluginEntry& e : all) {
        keys.push_back(std::to_string(static_cast<int>(e.format)) + ":" + e.id);
        REQUIRE(std::strlen(e.path) > 0);
    }
    const size_t before = keys.size();
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    REQUIRE(keys.size() == before);
}

TEST_CASE("scan_all is ordered stably", "[plugin][discovery]") {
    const uint32_t n = plugin_scan_all(nullptr, 0);
    if (n < 2) return;

    std::vector<PluginEntry> a(n), b(n);
    plugin_scan_all(a.data(), n);
    plugin_scan_all(b.data(), n);

    for (uint32_t i = 0; i < n; ++i) {
        REQUIRE(std::string(a[i].id) == std::string(b[i].id));
        REQUIRE(std::string(a[i].path) == std::string(b[i].path));
    }
    // Sorted by vendor then name, so a browser can render the list as it comes.
    for (uint32_t i = 1; i < n; ++i) {
        const int v = std::strcmp(a[i - 1].vendor, a[i].vendor);
        REQUIRE(v <= 0);
        if (v == 0) REQUIRE(std::strcmp(a[i - 1].name, a[i].name) <= 0);
    }
}
