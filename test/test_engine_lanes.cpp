// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_engine_lanes.cpp — the guest's regions, from a real engine boot.
 *
 * WHY A WHOLE ENGINE. LanesFixture hands the guest pointers the test itself
 * allocated, so it can say nothing about where an engine gets them from — and
 * that is the whole question here. EngineFixture boots a ClockworkEngine with
 * `udpPort = 0`: no POSIX segment, because there is no second process to share
 * one with. That is the embedded case, and a plugin is the example — its
 * client is the host application, in this process, reading in place.
 *
 * The guest is asked what it was handed, because that is the only place the
 * answer is observable. Sizes AND usability: a NULL region and a region that
 * is a bad pointer are different faults and only the first shows in a size.
 */
#include "EngineFixture.h"
#include "memory_profile.h"

#include <catch2/catch_test_macros.hpp>

using engine_test::Engine;

namespace {

// Ask the guest, and pump until it answers. The engine's control thread wakes
// per block, so a reply needs blocks to ride out on.
osc_test::ParsedReply askGuest(Engine& e, const char* verb, const char* replyAddr) {
    const auto msg = osc_test::message(verb);
    e.engine.ingest(msg.ptr(), msg.size(), 0);
    osc_test::ParsedReply r;
    REQUIRE(e.reply(replyAddr, r));
    return r;
}

} // namespace

TEST_CASE("engine: the guest gets both bulk lanes with no shared segment",
          "[boundary][regions][engine]") {
    Engine e;   // udpPort = 0 — the embedded case, no segment

    const auto r = askGuest(e, "/dummy/region/lanes", "/dummy/region/lanes-is");

    // NOT NULL, and the size the profile asks for. Reverting to
    // `mShmemCreator ? get_inbox() : nullptr` fails here, which is the point
    // of the case: the previous shape was silent, and a guest with no outbox
    // simply published nothing.
    CHECK(r.argInt(0) == static_cast<int32_t>(CLOCKWORK_INBOX_BYTES));
    CHECK(r.argInt(1) == static_cast<int32_t>(CLOCKWORK_OUTBOX_BYTES));

    // And real memory of that length: the guest read the first, middle and
    // last byte of the inbox, and wrote then read back the same three of the
    // outbox. A pointer that is merely non-NULL passes the checks above and
    // fails these.
    CHECK(r.argInt(2) == 1);
    CHECK(r.argInt(3) == 1);
}

TEST_CASE("engine: the arena is there with no shared segment too",
          "[boundary][regions][engine]") {
    Engine e;

    // The same fix, made earlier and for the same reason — the arena used to
    // be carved from the segment, so an embedded engine handed its guest no
    // working memory at all. Asserted here rather than only in the lanes
    // fixture because this is the boot path that had the bug.
    const auto r = askGuest(e, "/dummy/region/memory", "/dummy/region/memory-is");

    CHECK(r.argInt(0) == static_cast<int32_t>(CLOCKWORK_ARENA_BYTES));
    CHECK(r.argInt(1) == 1);   // zeroed at dsp_new
    CHECK(r.argInt(2) == 1);   // and writable to its stated length
}
