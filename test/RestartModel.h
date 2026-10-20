// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * RestartModel.h — what docs/RESTARTS.md's model says a reader heard.
 *
 * Every cell of the restart grid ("Where it is pinned") does the same thing
 * around its event, through whatever door and reader it is about:
 *
 *   - the old instance answers oldAsk(), and the reader has not read the
 *     answer when the event comes;
 *   - waitingAsks() are in the in ring when it comes, sent but not yet
 *     taken: one of the guest's, one of clockwork's that is not the guest's
 *     business (a sync), and one of clockwork's that hands the guest
 *     something (an asset's commit, of 64 bytes the cell puts at the start
 *     of the assets region);
 *   - after it, newAsk().
 *
 * and then hands what the reader heard, in order, to one of the two checks
 * below. A reader that sees only the instance's ring (a tap) sees no
 * refusals, which go on clockwork's.
 */
#pragma once

#include "OscTestUtils.h"
#include "clockwork_prefix.h"
#include "clockwork_sys.h"
#include "dsp_api.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace restart_model {

using Heard = std::vector<osc_test::ParsedReply>;

constexpr int32_t kSyncId = 7;

inline const std::string kSetup   = CLOCKWORK_SYS("setup");
inline const std::string kChanged = CLOCKWORK_SYS("setup/changed");
inline const std::string kError   = CLOCKWORK_SYS("error");
inline const std::string kSynced  = CLOCKWORK_SYS("synced");
inline const std::string kCommit  = CLOCKWORK_SYS("asset/commit");

inline osc_test::Packet oldAsk() { return osc_test::message("/dummy/ping"); }
inline osc_test::Packet newAsk() { return osc_test::message("/dummy/reconfigured"); }

inline std::vector<osc_test::Packet> waitingAsks() {
    osc_test::Builder b;
    b.begin(kCommit.c_str()) << int32_t{5} << int32_t{CLOCKWORK_ASSET_RAW} << int32_t{0}
                             << int32_t{64} << int32_t{0} << int32_t{0} << 0.f;
    return { osc_test::message("/dummy/events/get"),
             osc_test::message(CLOCKWORK_SYS("sync"), kSyncId),
             b.end() };
}

inline std::string dump(const Heard& heard) {
    std::string s;
    for (const auto& w : heard) {
        s += "\n  | " + w.address;
        if (w.address == kError || w.address == kSetup || w.address == kChanged
            || w.address == CLOCKWORK_SYS("statechange"))
            for (int i = 0; i < w.argCount(); ++i) {
                const auto str = w.argString(i);
                s += " " + (str.empty() ? std::to_string(w.argInt(i)) : str);
            }
    }
    return s;
}

inline std::vector<long> where(const Heard& heard, const std::string& address) {
    std::vector<long> at;
    for (size_t i = 0; i < heard.size(); ++i)
        if (heard[i].address == address) at.push_back(static_cast<long>(i));
    return at;
}

// The refusals of messages sent to `address`.
inline Heard refusalsOf(const Heard& heard, const std::string& address) {
    Heard out;
    for (const auto& w : heard)
        if (w.address == kError && w.argString(0) == address) out.push_back(w);
    return out;
}

// The instance was replaced, once or more, between generations `before` and
// `after`: every setup names a generation above the last, the last the one
// open now; the old instance's answer comes before the first, everything
// the new one says after the last; what was waiting for the guest is
// refused once, for `reason`, naming the generation that closed; clockwork's
// own is answered once, and a sync after everything sent before it — when
// is clockwork's, since its answers belong to no instance. `reason` is
// CLOCKWORK_REFUSED_REBUILDING for what was in the in ring at the teardown,
// CLOCKWORK_REFUSED_NO_INSTANCE for what was sent between instances.
inline void checkReplaced(const Heard& heard, const char* reader, bool seesRefusals,
                          uint32_t before, uint32_t after,
                          const char* reason = CLOCKWORK_REFUSED_REBUILDING) {
    INFO(reader << " heard:" << dump(heard));
    REQUIRE(after > before);

    const auto setups = where(heard, kSetup);
    REQUIRE_FALSE(setups.empty());
    uint32_t named = before;
    for (long at : setups) {
        const auto g = static_cast<uint32_t>(heard[static_cast<size_t>(at)].argInt(2));
        CHECK(g > named);
        named = g;
    }
    CHECK(named == after);
    const long first = setups.front(), last = setups.back();

    const auto pong = where(heard, "/dummy/pong");
    REQUIRE(pong.size() == 1);
    CHECK(pong[0] < first);

    const auto reply = where(heard, "/dummy/reconfigured.reply");
    REQUIRE(reply.size() == 1);
    CHECK(reply[0] > last);

    const auto synced = where(heard, kSynced);
    REQUIRE(synced.size() == 1);
    CHECK(heard[static_cast<size_t>(synced[0])].argInt(0) == kSyncId);

    // The guest's answer to a message refused is never heard, and neither is
    // the commit of an asset the old instance never took.
    CHECK(where(heard, "/dummy/events").empty());
    CHECK(where(heard, CLOCKWORK_SYS("asset/committed")).empty());

    if (!seesRefusals) return;
    for (const auto& addr : { std::string("/dummy/events/get"), kCommit }) {
        INFO("refusals of " << addr);
        const auto refused = refusalsOf(heard, addr);
        REQUIRE(refused.size() == 1);
        CHECK(refused[0].argString(1) == reason);
        CHECK(static_cast<uint32_t>(refused[0].argInt(2)) == before);
    }
    // The sync was in the in ring after the guest's message: its answer
    // comes after that message's refusal, both said by the drain on the
    // instance's ring. (A message refused at the door was never in the ring,
    // and its refusal rides clockwork's, which has no place against it.)
    if (std::string(reason) == CLOCKWORK_REFUSED_REBUILDING)
        for (size_t i = 0; i < heard.size(); ++i)
            if (heard[i].address == kError && heard[i].argString(0) == "/dummy/events/get")
                CHECK(static_cast<long>(i) < synced[0]);
}

// The instance survived: nothing refused, no setup, the generation unmoved,
// and everything asked of it answered by it, on its ring. When a fact
// clients compute with changed, a notice names the new facts (rate, buffer)
// and the same generation; `changedBuffer` 0 says none did.
inline void checkSurvived(const Heard& heard, const char* reader,
                          uint32_t before, uint32_t after, int rate, int changedBuffer) {
    INFO(reader << " heard:" << dump(heard));
    CHECK(after == before);
    CHECK(where(heard, kSetup).empty());
    CHECK(where(heard, kError).empty());
    for (const char* answer : { "/dummy/pong", "/dummy/events", "/dummy/reconfigured.reply",
                                CLOCKWORK_SYS("synced"), CLOCKWORK_SYS("asset/committed") }) {
        INFO("answers heard at " << answer);
        CHECK(where(heard, answer).size() == 1);
    }

    const auto changed = where(heard, kChanged);
    if (changedBuffer == 0) {
        CHECK(changed.empty());
        return;
    }
    REQUIRE(changed.size() == 1);
    const auto& notice = heard[static_cast<size_t>(changed[0])];
    REQUIRE(notice.argCount() == 3);
    CHECK(notice.argInt(0) == rate);
    CHECK(notice.argInt(1) == changedBuffer);
    CHECK(static_cast<uint32_t>(notice.argInt(2)) == before);
}

} // namespace restart_model
