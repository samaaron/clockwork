// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_schedule_parse.cpp — pins clockwork contract in
 * src/scheduler/schedule_parse.h.
 *
 * Clockwork does not own the schedule; it only has to agree with whatever is
 * attached about how a time arrives on the wire. schedule_parse.h is that
 * agreement, and it is pure byte work — no guest, no engine globals — so the
 * whole of it is testable here:
 *
 *   clockwork_is_bundle        recognises a timestamped OSC bundle ("#bundle" + an
 *                       8-byte timetag), and rejects anything shorter than the
 *                       16 bytes that pair takes;
 *   clockwork_bundle_timetag   reads that timetag big-endian out of offset 8;
 *   clockwork_ntp_to_timetag   packs NTP seconds into the OSC 32.32 fixed-point form;
 *   clockwork_parse_schedule   parses the flat twin, "/clockwork/schedule <timetag> <blob>", in
 *                       all three timetag spellings — 'h' (int64 OSC timetag,
 *                       full sub-sample resolution), 'd' (double NTP seconds)
 *                       and 'f' (float NTP seconds) — and refuses anything
 *                       malformed or truncated rather than reading past the end.
 *
 * The inner blob is opaque to the parser: it is handed back as a borrowed
 * pointer and length and never inspected. The bytes used for it below are
 * arbitrary filler and mean nothing.
 */
#include "clockwork_prefix.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "OscTestUtils.h"
#include "scheduler/schedule_parse.h"

namespace {

void putPaddedString(std::vector<uint8_t>& v, const char* s) {
    const size_t n = std::strlen(s);
    v.insert(v.end(), s, s + n);
    v.push_back(0);
    while (v.size() % 4u) v.push_back(0);
}

void putBE(std::vector<uint8_t>& v, uint64_t x, int nbytes) {
    for (int i = nbytes - 1; i >= 0; --i)
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFFu));
}

// Byte offsets into the hand-rolled wire form, derived from the reserved
// prefix exactly as the parser derives them. Written out rather than counted so
// that changing the prefix moves the test and the parser together — a test that
// hard-codes 12 and 20 here would keep passing against a parser reading the
// wrong bytes.
constexpr uint32_t kSchedAddr = CLOCKWORK_SYS_LEN("schedule");     // address, NUL excluded
constexpr uint32_t kSchedPad  = CLOCKWORK_SYS_PADDED("schedule");  // ... NUL included, padded to 4
constexpr uint32_t kSchedArgs = kSchedPad + 4u;              // past the type tag

// Hand-roll "/clockwork/schedule <timetag> <blob>" so the wire form under test is
// written out byte for byte rather than borrowed from an encoder. The padded
// address then the padded type tag put the first argument at kSchedArgs —
// exactly where the parser's derived offsets look.
std::vector<uint8_t> scheduleMsg(const char* typetag,
                                 const std::vector<uint8_t>& timeBytes,
                                 const std::vector<uint8_t>& blob) {
    std::vector<uint8_t> v;
    putPaddedString(v, CLOCKWORK_SYS("schedule"));
    putPaddedString(v, typetag);
    v.insert(v.end(), timeBytes.begin(), timeBytes.end());
    putBE(v, blob.size(), 4);
    v.insert(v.end(), blob.begin(), blob.end());
    while (v.size() % 4u) v.push_back(0);
    return v;
}

std::vector<uint8_t> beBytes(uint64_t x, int nbytes) {
    std::vector<uint8_t> v;
    putBE(v, x, nbytes);
    return v;
}

std::vector<uint8_t> doubleBytes(double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, 8);
    return beBytes(bits, 8);
}

std::vector<uint8_t> floatBytes(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    return beBytes(bits, 4);
}

// Arbitrary opaque payload — the parser never looks inside it.
const std::vector<uint8_t> kFiller = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};

}  // namespace

// ── clockwork_ntp_to_timetag ────────────────────────────────────────────────────────

TEST_CASE("schedule_parse - clockwork_ntp_to_timetag packs NTP seconds as 32.32",
          "[schedule_parse][timetag]") {
    CHECK(clockwork_ntp_to_timetag(0.0) == 0);
    CHECK(clockwork_ntp_to_timetag(1.0) == (int64_t{1} << 32));
    CHECK(clockwork_ntp_to_timetag(1.5) == ((int64_t{1} << 32) | int64_t{0x80000000}));
    CHECK(clockwork_ntp_to_timetag(2.25) == ((int64_t{2} << 32) | int64_t{0x40000000}));

    // Whole seconds land in the high word, so ordering is monotonic there.
    CHECK((clockwork_ntp_to_timetag(3600.0) >> 32) == 3600);

    // It is exactly the shared clock formula — one definition, not two.
    CHECK(clockwork_ntp_to_timetag(1234.75) == clockwork::ntpToOscTimetag(1234.75));
}

// ── clockwork_is_bundle / clockwork_bundle_timetag ─────────────────────────────────────────

TEST_CASE("schedule_parse - clockwork_is_bundle needs the marker AND a whole timetag",
          "[schedule_parse][bundle]") {
    std::vector<uint8_t> b;
    putPaddedString(b, "#bundle");                  // 8 bytes with the NUL
    const std::vector<uint8_t> tt = beBytes((uint64_t{7} << 32) | 0x80000000u, 8);
    b.insert(b.end(), tt.begin(), tt.end());
    REQUIRE(b.size() == 16u);

    CHECK(clockwork_is_bundle(b.data(), static_cast<uint32_t>(b.size())));

    // 15 bytes cannot hold marker + timetag, so it is not a bundle even though
    // the marker is intact — the size check comes first.
    CHECK_FALSE(clockwork_is_bundle(b.data(), 15u));

    // A near-miss marker is not a bundle.
    std::vector<uint8_t> wrong = b;
    wrong[6] = 'X';                                  // "#bundlX"
    CHECK_FALSE(clockwork_is_bundle(wrong.data(), static_cast<uint32_t>(wrong.size())));
}

TEST_CASE("schedule_parse - clockwork_bundle_timetag reads offset 8 big-endian",
          "[schedule_parse][bundle]") {
    std::vector<uint8_t> b;
    putPaddedString(b, "#bundle");
    const uint64_t when = (uint64_t{0x12345678} << 32) | 0x9ABCDEF0u;
    const std::vector<uint8_t> tt = beBytes(when, 8);
    b.insert(b.end(), tt.begin(), tt.end());

    REQUIRE(clockwork_is_bundle(b.data(), static_cast<uint32_t>(b.size())));
    CHECK(clockwork_bundle_timetag(b.data()) == when);

    // The immediate ("now") timetag, 1, is the common special case.
    std::vector<uint8_t> now;
    putPaddedString(now, "#bundle");
    const std::vector<uint8_t> one = beBytes(1u, 8);
    now.insert(now.end(), one.begin(), one.end());
    CHECK(clockwork_bundle_timetag(now.data()) == 1u);
}

TEST_CASE("schedule_parse - a flat schedule message is not a bundle",
          "[schedule_parse][bundle]") {
    const auto m = scheduleMsg(",hb", beBytes(1u, 8), kFiller);
    CHECK_FALSE(clockwork_is_bundle(m.data(), static_cast<uint32_t>(m.size())));
}

// ── clockwork_parse_schedule: the three timetag spellings ───────────────────────────

TEST_CASE("schedule_parse - 'h' carries the OSC int64 timetag verbatim",
          "[schedule_parse]") {
    const int64_t when = (int64_t{0x00000123} << 32) | int64_t{0x456789ABu};
    const auto m = scheduleMsg(",hb", beBytes(static_cast<uint64_t>(when), 8), kFiller);

    const auto r = clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size()));
    REQUIRE(r.ok);
    CHECK(r.when == when);              // no conversion — the fraction survives
    REQUIRE(r.blobLen == kFiller.size());
    CHECK(std::memcmp(r.blob, kFiller.data(), kFiller.size()) == 0);
    // The blob is borrowed from the caller's buffer, not copied.
    CHECK(r.blob >= m.data());
    CHECK(r.blob + r.blobLen <= m.data() + m.size());
}

TEST_CASE("schedule_parse - 'd' converts double NTP seconds to a timetag",
          "[schedule_parse]") {
    const double ntp = 3600.5;
    const auto m = scheduleMsg(",db", doubleBytes(ntp), kFiller);

    const auto r = clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size()));
    REQUIRE(r.ok);
    CHECK(r.when == clockwork_ntp_to_timetag(ntp));
    CHECK(r.when == ((int64_t{3600} << 32) | int64_t{0x80000000}));
    CHECK(r.blobLen == kFiller.size());
}

TEST_CASE("schedule_parse - 'f' converts float NTP seconds to a timetag",
          "[schedule_parse]") {
    const float ntp = 2.25f;
    const auto m = scheduleMsg(",fb", floatBytes(ntp), kFiller);

    const auto r = clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size()));
    REQUIRE(r.ok);
    CHECK(r.when == clockwork_ntp_to_timetag(static_cast<double>(ntp)));
    CHECK(r.when == ((int64_t{2} << 32) | int64_t{0x40000000}));
    CHECK(r.blobLen == kFiller.size());

    // The float form takes 4 argument bytes, not 8 — the blob still lands.
    CHECK(std::memcmp(r.blob, kFiller.data(), kFiller.size()) == 0);
}

TEST_CASE("schedule_parse - an empty blob parses as an empty blob, not a failure",
          "[schedule_parse]") {
    const auto m = scheduleMsg(",hb", beBytes(42u, 8), {});
    const auto r = clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size()));
    REQUIRE(r.ok);
    CHECK(r.when == 42);
    CHECK(r.blobLen == 0u);
    CHECK(r.blob != nullptr);
}

// A genuine encoder must produce something this parser accepts — otherwise the
// hand-rolled layout above is only self-consistent. The address is "/clockwork/schedule"
// and the arguments are an int64 timetag plus an opaque blob.
TEST_CASE("schedule_parse - accepts a message from a real OSC encoder",
          "[schedule_parse]") {
    const int64_t when = (int64_t{4242} << 32) | int64_t{0x00001000};
    osc_test::Builder b;
    auto& s = b.begin(CLOCKWORK_SYS("schedule"));
    s << static_cast<osc::int64>(when)
      << osc::Blob(kFiller.data(),
                   static_cast<osc::osc_bundle_element_size_t>(kFiller.size()));
    const auto pkt = b.end();

    const auto r = clockwork_parse_schedule(pkt.ptr(), pkt.size());
    REQUIRE(r.ok);
    CHECK(r.when == when);
    REQUIRE(r.blobLen == kFiller.size());
    CHECK(std::memcmp(r.blob, kFiller.data(), kFiller.size()) == 0);
}

// ── clockwork_parse_schedule: refusals ──────────────────────────────────────────────

TEST_CASE("schedule_parse - malformed headers are refused", "[schedule_parse]") {
    const auto good = scheduleMsg(",hb", beBytes(1u, 8), kFiller);
    REQUIRE(clockwork_parse_schedule(good.data(), static_cast<uint32_t>(good.size())).ok);

    SECTION("too short to hold address + tag + timetag") {
        // kSchedPad + 8 is the floor; anything under it is refused before any read.
        for (uint32_t n = 0; n < kSchedPad + 8u; ++n)
            CHECK_FALSE(clockwork_parse_schedule(good.data(), n).ok);
    }

    SECTION("a different address") {
        auto m = good;
        m[kSchedAddr - 1] = 'X';           // last character differs — the memcmp fails
        CHECK_FALSE(clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size())).ok);
    }

    SECTION("a longer address sharing the /clockwork/schedule prefix") {
        // The NUL is part of the check, so "/clockwork/scheduled" is not
        // "/clockwork/schedule".
        auto m = good;
        m[kSchedAddr] = 'd';
        CHECK_FALSE(clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size())).ok);
    }

    SECTION("a type tag not starting with ','") {
        auto m = good;
        m[kSchedPad] = ';';
        CHECK_FALSE(clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size())).ok);
    }

    SECTION("an unsupported timetag type") {
        for (char t : {'i', 's', 't', 'b', '\0'}) {
            auto m = good;
            m[kSchedPad + 1u] = t;
            CHECK_FALSE(clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size())).ok);
        }
    }

    SECTION("a second argument that is not a blob") {
        for (char t : {'i', 's', 'f', '\0'}) {
            auto m = good;
            m[kSchedPad + 2u] = t;
            CHECK_FALSE(clockwork_parse_schedule(m.data(), static_cast<uint32_t>(m.size())).ok);
        }
    }
}

TEST_CASE("schedule_parse - truncated arguments are refused, never read past",
          "[schedule_parse]") {
    const auto h = scheduleMsg(",hb", beBytes(1u, 8), kFiller);

    // Every truncation of a well-formed message must be refused. The only
    // lengths that may succeed are those carrying the whole declared blob.
    const uint32_t full = static_cast<uint32_t>(h.size());
    for (uint32_t n = 0; n < full; ++n) {
        const auto r = clockwork_parse_schedule(h.data(), n);
        CHECK_FALSE(r.ok);
    }
    CHECK(clockwork_parse_schedule(h.data(), full).ok);

    // A blob length longer than the bytes that follow is refused rather than
    // handed back as an over-long borrowed range.
    auto lying = h;
    // The blob-length word follows the 8-byte int64 timetag; byte 2 of its
    // big-endian four turns 8 into 264, past the end.
    lying[kSchedArgs + 8u + 2u] = 0x01;
    CHECK_FALSE(clockwork_parse_schedule(lying.data(), static_cast<uint32_t>(lying.size())).ok);

    // Same for the float form, whose arguments start 4 bytes earlier.
    const auto f = scheduleMsg(",fb", floatBytes(1.0f), kFiller);
    for (uint32_t n = 0; n < f.size(); ++n)
        CHECK_FALSE(clockwork_parse_schedule(f.data(), n).ok);
    CHECK(clockwork_parse_schedule(f.data(), static_cast<uint32_t>(f.size())).ok);
}

TEST_CASE("schedule_parse - a failed parse yields a wholly inert result",
          "[schedule_parse]") {
    const uint8_t junk[24] = {0};
    const auto r = clockwork_parse_schedule(junk, sizeof junk);
    CHECK_FALSE(r.ok);
    CHECK(r.when == 0);
    CHECK(r.blob == nullptr);
    CHECK(r.blobLen == 0u);
}
