// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_fuzzy_match.cpp — pins clockwork contract for fuzzyMatch(), the
 * multi-token device-name matcher behind the -H hardware flag.
 *
 * Contract pinned (FuzzyMatch.h):
 *   - the pattern is split on whitespace into tokens;
 *   - a candidate matches only when EVERY token appears in it as a
 *     case-insensitive substring (token order is irrelevant);
 *   - among matching candidates the SHORTEST wins (most specific);
 *   - empty / whitespace-only patterns, an empty candidate list, and a
 *     no-match pattern all return the empty string.
 *
 * Pure string work: no audio device, no engine, no host process.
 */
#include <catch2/catch_test_macros.hpp>
#include "FuzzyMatch.h"

#include <string>
#include <vector>

// A realistic multi-driver device list (a Windows box enumerating the same
// physical outputs under three WASAPI modes plus DirectSound and ASIO).
static const std::vector<std::string> kDevices = {
    "Windows Audio : Speakers (Qualcomm Aqstic Audio Adapter Device)",
    "Windows Audio (Exclusive Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)",
    "Windows Audio (Low Latency Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)",
    "DirectSound : Primary Sound Driver",
    "DirectSound : Speakers (Qualcomm Aqstic Audio Adapter Device)",
    "DirectSound : Headphones (USB Audio Device)",
    "ASIO : Focusrite USB ASIO",
};

TEST_CASE("fuzzyMatch: empty pattern returns empty", "[fuzzy]") {
    CHECK(fuzzyMatch("", kDevices).empty());
}

TEST_CASE("fuzzyMatch: whitespace-only pattern returns empty", "[fuzzy]") {
    CHECK(fuzzyMatch("   ", kDevices).empty());
}

TEST_CASE("fuzzyMatch: empty candidates returns empty", "[fuzzy]") {
    CHECK(fuzzyMatch("speakers", {}).empty());
}

TEST_CASE("fuzzyMatch: no match returns empty", "[fuzzy]") {
    CHECK(fuzzyMatch("bluetooth", kDevices).empty());
}

TEST_CASE("fuzzyMatch: single token matches driver name", "[fuzzy]") {
    // "asio" appears in one entry only.
    CHECK(fuzzyMatch("asio", kDevices) == "ASIO : Focusrite USB ASIO");
}

TEST_CASE("fuzzyMatch: single token matches device name", "[fuzzy]") {
    // "headphones" only appears in one entry.
    CHECK(fuzzyMatch("headphones", kDevices) == "DirectSound : Headphones (USB Audio Device)");
}

TEST_CASE("fuzzyMatch: single token matching one entry resolves it", "[fuzzy]") {
    CHECK(fuzzyMatch("primary", kDevices) == "DirectSound : Primary Sound Driver");
}

TEST_CASE("fuzzyMatch: two tokens narrow driver + device", "[fuzzy]") {
    CHECK(fuzzyMatch("direct headphones", kDevices) ==
          "DirectSound : Headphones (USB Audio Device)");
}

TEST_CASE("fuzzyMatch: two tokens in reverse order", "[fuzzy]") {
    // Token order must not matter.
    CHECK(fuzzyMatch("headphones direct", kDevices) ==
          "DirectSound : Headphones (USB Audio Device)");
}

TEST_CASE("fuzzyMatch: case insensitive", "[fuzzy]") {
    CHECK(fuzzyMatch("DIRECTSOUND", kDevices) == "DirectSound : Primary Sound Driver");
    CHECK(fuzzyMatch("Speakers DIRECT", kDevices) ==
          "DirectSound : Speakers (Qualcomm Aqstic Audio Adapter Device)");
}

TEST_CASE("fuzzyMatch: 'exclusive' selects the exclusive-mode entry", "[fuzzy]") {
    CHECK(fuzzyMatch("exclusive", kDevices) ==
          "Windows Audio (Exclusive Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)");
}

TEST_CASE("fuzzyMatch: 'low latency' selects the low-latency entry", "[fuzzy]") {
    CHECK(fuzzyMatch("low latency", kDevices) ==
          "Windows Audio (Low Latency Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)");
}

TEST_CASE("fuzzyMatch: 'focusrite' matches the ASIO device", "[fuzzy]") {
    CHECK(fuzzyMatch("focusrite", kDevices) == "ASIO : Focusrite USB ASIO");
}

TEST_CASE("fuzzyMatch: 'usb' matches shortest USB entry", "[fuzzy]") {
    // Both the ASIO entry and the DirectSound headphones contain "usb";
    // "ASIO : Focusrite USB ASIO" is the shorter string, so it wins.
    CHECK(fuzzyMatch("usb", kDevices) == "ASIO : Focusrite USB ASIO");
}

TEST_CASE("fuzzyMatch: 'usb direct' narrows to the DirectSound USB device", "[fuzzy]") {
    CHECK(fuzzyMatch("usb direct", kDevices) ==
          "DirectSound : Headphones (USB Audio Device)");
}

TEST_CASE("fuzzyMatch: three tokens work", "[fuzzy]") {
    CHECK(fuzzyMatch("windows exclusive qualcomm", kDevices) ==
          "Windows Audio (Exclusive Mode) : Speakers (Qualcomm Aqstic Audio Adapter Device)");
}

TEST_CASE("fuzzyMatch: partial token match", "[fuzzy]") {
    // "qual" is a substring of "Qualcomm"; several entries contain it, so
    // the shortest of those wins — whichever it is, it must be a Qualcomm one.
    auto result = fuzzyMatch("qual", kDevices);
    CHECK_FALSE(result.empty());
    CHECK(result.find("Qualcomm") != std::string::npos);
}

TEST_CASE("fuzzyMatch: all tokens must match", "[fuzzy]") {
    // No single entry contains both "asio" and "headphones".
    CHECK(fuzzyMatch("asio headphones", kDevices).empty());
}

TEST_CASE("fuzzyMatch: extra whitespace is ignored", "[fuzzy]") {
    CHECK(fuzzyMatch("  direct   headphones  ", kDevices) ==
          "DirectSound : Headphones (USB Audio Device)");
}
