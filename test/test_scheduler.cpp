// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_scheduler.cpp — pins clockwork contract in src/scheduler/Scheduler.h.
 *
 * Scheduler<> is clockwork's generic timed-event store: opaque payload bytes
 * keyed by an int64 timetag, released in time order, cancellable by tag. It
 * knows nothing about what an event means or who fires it — that is entirely
 * the attached guest's business. So everything below is a data-structure
 * contract, provable with no audio device and no guest at all:
 *
 *   - sched_tag_hash is FNV-1a over the bytes, constexpr, and NEVER returns 0
 *     (0 is reserved as the flush wildcard);
 *   - the SCHED_TAG_DEFAULT / SCHED_TAG_SYNTH constants have fixed values — a
 *     guest mirrors them bit for bit, so they are frozen here;
 *   - add / popDue / release order events by timetag, FIFO within a timetag;
 *   - flush(tag) cancels exactly one tag, flush(0) cancels everything;
 *   - requestClear / drainPendingClear is the cross-thread clear handshake;
 *   - add() refuses (with no state change) when the slot pool is full, when the
 *     data pool cannot fit the payload, and when the payload can never fit; and
 *   - the data pool is reclaimed by in-place compaction, so continuous churn
 *     against a pinned queue never exhausts it and payload bytes survive it.
 */
#include <catch2/catch_test_macros.hpp>
#include "clockwork_prefix.h"
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstring>

#include "scheduler/Scheduler.h"
#include "scheduler/engine_schedule.h"   // EngineMeta: the Meta clockwork actually schedules with

namespace {
struct TestMeta { uint32_t v = 0; };

const uint32_t TAG_KEEP  = sched_tag_hash("keep", 4);
const uint32_t TAG_FLUSH = sched_tag_hash("flushme", 7);

const uint8_t kData[8] = {1, 2, 3, 4, 5, 6, 7, 8};

// A four-byte input whose raw FNV-1a value is exactly 0 — the one case where
// the "never 0" clamp in sched_tag_hash is observable. Without the clamp this
// tag would silently mean "the wildcard", i.e. one unlucky tag string would
// turn every flush into flush-everything.
constexpr char kZeroPreimage[4] = { '\xCC', '\x24', '\x31', '\xC4' };
}  // namespace

// ── The hash and the tag constants ───────────────────────────────────────────
// Frozen values: a guest computes these on its own side of the C ABI, so they
// are wire constants, not an implementation detail.
static_assert(sched_tag_hash("", 0) == 0x811C9DC5u,
              "empty input is the FNV-1a 32-bit offset basis");
static_assert(sched_tag_hash("keep", 4) == 0xEE7B9448u, "FNV-1a of \"keep\"");
static_assert(sched_tag_hash("flushme", 7) == 0x3071C5EFu, "FNV-1a of \"flushme\"");
static_assert(sched_tag_hash(kZeroPreimage, 4) == 1u,
              "a raw hash of 0 must be clamped to 1, never the flush wildcard");
static_assert(SCHED_TAG_DEFAULT == 0x933B5BDEu, "SCHED_TAG_DEFAULT is frozen");
static_assert(SCHED_TAG_SYNTH   == 0xC0214F59u, "SCHED_TAG_SYNTH is frozen");

// A tag the STORE has never heard of. The store delays OSC and groups it by an
// opaque 32-bit key; which strings exist is the caller's business, so the flush
// cases below key on this rather than on a name the scheduler blesses.
constexpr uint32_t kCallerTag = sched_tag_hash("anything at all", 15);

TEST_CASE("Scheduler - sched_tag_hash is FNV-1a and never returns the wildcard",
          "[scheduler][tag]") {
    CHECK(sched_tag_hash("", 0) == 0x811C9DC5u);
    CHECK(sched_tag_hash("keep", 4) == 0xEE7B9448u);
    CHECK(sched_tag_hash("flushme", 7) == 0x3071C5EFu);

    // The clamp: this input hashes to 0 and must come back as 1.
    CHECK(sched_tag_hash(kZeroPreimage, 4) == 1u);

    // Length is explicit — the hash covers exactly n bytes, not to a NUL.
    CHECK(sched_tag_hash("keepXX", 4) == sched_tag_hash("keep", 4));
    CHECK(sched_tag_hash("keep", 3) != sched_tag_hash("keep", 4));
}

TEST_CASE("Scheduler - the shared tag constants are distinct and non-wildcard",
          "[scheduler][tag]") {
    CHECK(SCHED_TAG_DEFAULT == 0x933B5BDEu);
    CHECK(SCHED_TAG_SYNTH   == 0xC0214F59u);

    CHECK(SCHED_TAG_DEFAULT != 0u);
    CHECK(SCHED_TAG_SYNTH   != 0u);
    CHECK(kCallerTag        != 0u);
    CHECK(SCHED_TAG_DEFAULT != SCHED_TAG_SYNTH);
    CHECK(SCHED_TAG_DEFAULT != kCallerTag);
    CHECK(SCHED_TAG_SYNTH   != kCallerTag);
}

TEST_CASE("Scheduler - a default-tag flush leaves the other tags alone",
          "[scheduler][tag][flush]") {
    Scheduler<TestMeta, 16, 8192> s;
    REQUIRE(s.add(100, SCHED_TAG_DEFAULT, {}, kData, 4));
    REQUIRE(s.add(200, kCallerTag,        {}, kData, 4));
    REQUIRE(s.add(300, SCHED_TAG_SYNTH,   {}, kData, 4));

    s.flush(SCHED_TAG_DEFAULT);
    CHECK(s.size() == 2);
    CHECK(s.nextTime() == 200);   // the caller-tagged event survives

    s.flush(0);                   // only the wildcard takes the rest
    CHECK(s.size() == 0);
}

// ── Ordering ─────────────────────────────────────────────────────────────────

TEST_CASE("Scheduler - pops due events in time order (FIFO for equal times)",
          "[scheduler]") {
    Scheduler<TestMeta, 16, 8192> s;
    s.add(300, TAG_KEEP, {3}, kData, 4);
    s.add(100, TAG_KEEP, {1}, kData, 4);
    s.add(100, TAG_KEEP, {2}, kData, 4);   // same time as {1}, added later → after it
    s.add(200, TAG_KEEP, {4}, kData, 4);

    int64_t last = -1;
    int order[4]; int n = 0;
    for (;;) {
        auto e = s.popDue(INT64_MAX);
        if (!e.valid()) break;
        CHECK(e.when >= last); last = e.when;
        order[n++] = static_cast<int>(e.meta->v);
        s.release(e);
    }
    REQUIRE(n == 4);
    CHECK(order[0] == 1);   // t=100 first-added
    CHECK(order[1] == 2);   // t=100 second-added (FIFO)
    CHECK(order[2] == 4);   // t=200
    CHECK(order[3] == 3);   // t=300
}

TEST_CASE("Scheduler - popDue releases nothing before its timetag", "[scheduler]") {
    Scheduler<TestMeta, 16, 8192> s;
    CHECK(s.nextTime() == INT64_MAX);      // empty
    REQUIRE(s.add(500, TAG_KEEP, {7}, kData, 4));
    CHECK(s.nextTime() == 500);

    CHECK_FALSE(s.popDue(499).valid());    // not yet due
    CHECK(s.size() == 1);                  // and still live

    auto e = s.popDue(500);                // due AT the timetag, not after it
    REQUIRE(e.valid());
    CHECK(e.when == 500);
    CHECK(e.tag == TAG_KEEP);
    CHECK(e.meta->v == 7u);
    s.release(e);
    CHECK(s.size() == 0);
    CHECK(s.nextTime() == INT64_MAX);
}

// ── Cancellation ─────────────────────────────────────────────────────────────

TEST_CASE("Scheduler - flush cancels only the matching tag; flush(0) clears all",
          "[scheduler][flush]") {
    Scheduler<TestMeta, 16, 8192> s;
    s.add(100, TAG_KEEP, {}, kData, 4);
    s.add(200, TAG_FLUSH, {}, kData, 4);
    s.add(300, TAG_KEEP, {}, kData, 4);
    REQUIRE(s.size() == 3);

    s.flush(TAG_FLUSH);
    CHECK(s.size() == 2);
    CHECK(s.nextTime() == 100);          // earliest survivor
    // The flushed event must never surface.
    int popped = 0;
    for (;;) { auto e = s.popDue(INT64_MAX); if (!e.valid()) break; ++popped; s.release(e); }
    CHECK(popped == 2);

    s.add(100, TAG_KEEP, {}, kData, 4);
    s.add(200, TAG_FLUSH, {}, kData, 4);
    s.flush(0);                          // wildcard
    CHECK(s.size() == 0);
    CHECK(s.nextTime() == INT64_MAX);
}

// Regression: flushing buried (not-yet-due) events must not leak heap capacity.
// A flushed event whose slot is freed but whose time is later than a live event
// never sits at the heap top, so a tombstone-only design would never reclaim it
// and add() would falsely report the pool full.
TEST_CASE("Scheduler - flush of buried future events does not leak capacity",
          "[scheduler][flush]") {
    Scheduler<TestMeta, 4, 8192> s;

    // One early, live event pins the heap top so flushed later events are buried.
    REQUIRE(s.add(/*when*/ 10, TAG_KEEP, {}, kData, sizeof kData));

    for (int i = 0; i < 1000; ++i) {
        REQUIRE(s.add(/*when*/ 1000 + i, TAG_FLUSH, {}, kData, sizeof kData));
        s.flush(TAG_FLUSH);
        REQUIRE(s.size() == 1);   // only the keeper remains live
    }

    // Capacity must still be available — the buried flushes did not leak.
    REQUIRE(s.add(/*when*/ 20, TAG_KEEP, {}, kData, sizeof kData));
    CHECK(s.size() == 2);
}

TEST_CASE("Scheduler - requestClear is drained on the consumer side",
          "[scheduler][flush]") {
    Scheduler<TestMeta, 16, 8192> s;
    REQUIRE(s.add(100, TAG_KEEP, {}, kData, 8));
    REQUIRE(s.add(200, TAG_FLUSH, {}, kData, 8));
    CHECK(s.size() == 2);

    // Nothing requested yet: the drain is a no-op and leaves the queue intact.
    CHECK_FALSE(s.drainPendingClear());
    CHECK(s.size() == 2);

    s.requestClear();
    CHECK(s.size() == 2);          // the request alone changes nothing
    CHECK(s.drainPendingClear());  // the consumer performs it
    CHECK(s.size() == 0);
    CHECK(s.nextTime() == INT64_MAX);
    CHECK(s.dataUsed() == 0);

    // The flag is consumed, not sticky.
    CHECK_FALSE(s.drainPendingClear());
}

// ── Refusals ─────────────────────────────────────────────────────────────────

TEST_CASE("Scheduler - a full slot pool refuses further adds", "[scheduler]") {
    Scheduler<TestMeta, 4, 8192> s;   // 4 slots, a pool far larger than needed
    const uint8_t one[1] = {0};
    for (int i = 0; i < 4; ++i) REQUIRE(s.add(100 + i, TAG_KEEP, {}, one, sizeof one));

    CHECK(s.full());
    CHECK(s.size() == 4);
    CHECK_FALSE(s.add(200, TAG_KEEP, {}, one, sizeof one));
    CHECK(s.size() == 4);   // a refused add changes nothing

    // Freeing one slot re-opens exactly one add.
    auto e = s.popDue(INT64_MAX);
    REQUIRE(e.valid());
    s.release(e);
    CHECK_FALSE(s.full());
    CHECK(s.add(200, TAG_KEEP, {}, one, sizeof one));
}

TEST_CASE("Scheduler - a payload larger than the whole pool is refused outright",
          "[scheduler]") {
    Scheduler<TestMeta, 16, 256> s;   // 256-byte data pool
    CHECK(s.dataCapacity() == 256u);

    uint8_t oversize[300] = {};
    CHECK_FALSE(s.add(100, TAG_KEEP, {}, oversize, sizeof oversize));
    CHECK(s.size() == 0);
    CHECK(s.dataUsed() == 0);   // refused before anything was reserved

    // Exactly the capacity still fits.
    uint8_t exact[256] = {};
    CHECK(s.add(100, TAG_KEEP, {}, exact, sizeof exact));
    CHECK(s.dataUsed() == 256u);
}

// The data pool can be exhausted while slots remain free (a lean profile sizes
// the pool well below slots*maxPayload). add() then fails even though full() —
// which reports slot-count only — is false. Callers that back-pressure on
// full() must also treat an add() failure as backpressure, not a silent drop.
TEST_CASE("Scheduler - data pool fills while slots stay free; full() misses it",
          "[scheduler]") {
    Scheduler<TestMeta, 64, 256> s;   // 64 slots, 256-byte pool
    const uint8_t blob[64] = {0};

    int added = 0;
    while (s.add(/*when*/ 1000 + added, TAG_KEEP, {}, blob, sizeof blob)) ++added;

    CHECK(added < 64);        // ran out of DATA pool (256/64 = 4 chunks), not slots
    CHECK_FALSE(s.full());    // full() is slot-count only — slots are still free
    const uint8_t tiny[1] = {0};
    CHECK_FALSE(s.add(2000, TAG_KEEP, {}, tiny, sizeof tiny));  // even 1 byte won't fit
}

// ── The data pool ────────────────────────────────────────────────────────────

TEST_CASE("Scheduler - data pool reclaims released bytes under continuous use",
          "[scheduler]") {
    Scheduler<TestMeta, 512, 524288> s;
    // Pin the queue at >=1 so the pool never fully drains (the only free path is
    // then compaction).
    uint8_t keeper[64] = {};
    REQUIRE(s.add(INT64_MAX, TAG_KEEP, {}, keeper, sizeof keeper));

    uint8_t busy[152];
    std::memset(busy, 0xAB, sizeof busy);
    int failedAt = -1;
    for (int i = 0; i < 10000; ++i) {
        if (!s.add(i, TAG_KEEP, {}, busy, sizeof busy)) { failedAt = i; break; }
        auto e = s.popDue(INT64_MAX);   // earliest is busy(i), well before INT64_MAX
        REQUIRE(e.valid());
        s.release(e);
    }
    CHECK(failedAt == -1);
}

TEST_CASE("Scheduler - drained queue resets the data-pool head", "[scheduler]") {
    Scheduler<TestMeta, 16, 8192> s;
    for (int i = 0; i < 5; ++i) REQUIRE(s.add(i, TAG_KEEP, {}, kData, 8));
    CHECK(s.dataUsed() > 0);
    while (s.size() > 0) { auto e = s.popDue(INT64_MAX); REQUIRE(e.valid()); s.release(e); }
    CHECK(s.dataUsed() == 0);
}

TEST_CASE("Scheduler - payload bytes survive pool churn intact", "[scheduler]") {
    Scheduler<TestMeta, 16, 8192> s;
    uint8_t keeper[32] = {};
    REQUIRE(s.add(INT64_MAX, TAG_KEEP, {}, keeper, sizeof keeper));
    for (int i = 0; i < 100; ++i) {
        uint8_t marker[64];
        for (int b = 0; b < 64; ++b) marker[b] = static_cast<uint8_t>((i * 7 + b) & 0xFF);
        REQUIRE(s.add(i, TAG_KEEP, {}, marker, sizeof marker));
        auto e = s.popDue(INT64_MAX);
        REQUIRE(e.valid());
        REQUIRE(e.size == sizeof marker);
        CHECK(std::memcmp(e.data, marker, sizeof marker) == 0);
        s.release(e);
    }
}

// ── Coverage the C++ scheduler had and the Rust one had lost ─────────────────
//
// The store was rewritten in Rust and the suite grew — 14 cases here and 35 in
// the crate against upstream's 9. But three things upstream asserted had no
// equivalent on either side, and more cases is not the same as more coverage.
// A rewrite is exactly where a specific guarantee goes missing under a larger
// number.

TEST_CASE("Scheduler - the scheduling caller's origin survives to fire time",
          "[scheduler]") {
    Scheduler<EngineMeta, 16, 8192> es;
    const uint8_t osc[] = { 0x90, 60, 100 };
    REQUIRE(es.add(/*when*/ 1000, SCHED_TAG_DEFAULT, EngineMeta{/*origin*/ 4242},
                   osc, sizeof osc));

    // A scheduled message is answered long after the caller's request returned,
    // so the token has to be stored with the event rather than read from
    // whatever is being dispatched when it fires. Without it the reply routes
    // on origin 0, which is the notify audience and not the caller.
    auto e = es.popDue(2000);
    REQUIRE(e.valid());
    REQUIRE(e.meta != nullptr);
    CHECK(e.meta->origin == 4242u);
    es.release(e);
}

TEST_CASE("Scheduler - a due event is handed over exactly once",
          "[scheduler][regression]") {
    // Upstream: "EngineScheduler emits each due event once". Nothing here
    // asserted that popDue does not hand the SAME event back on the next call —
    // which, for a scheduler, is a doubled note rather than a late one.
    Scheduler<TestMeta, 16, 8192> s;
    REQUIRE(s.add(100, SCHED_TAG_DEFAULT, {7}, kData, 4));

    auto first = s.popDue(1000);
    REQUIRE(first.valid());
    CHECK(first.meta->v == 7);
    s.release(first);

    // Nothing is left, however many times it is asked.
    CHECK_FALSE(s.popDue(1000).valid());
    CHECK_FALSE(s.popDue(INT64_MAX).valid());
    CHECK(s.size() == 0);
}

TEST_CASE("Scheduler - flush cancels the pending, never the already-fired",
          "[scheduler][regression]") {
    // Upstream: "flush only affects pending events, not fired ones". A flush
    // arriving between popDue and release must not disturb the event the caller
    // is holding — it is already on its way out, and cancelling it there would
    // be a use-after-free of a payload the caller is still reading.
    Scheduler<TestMeta, 16, 8192> s;
    REQUIRE(s.add(100, SCHED_TAG_DEFAULT, {1}, kData, 4));
    REQUIRE(s.add(200, SCHED_TAG_DEFAULT, {2}, kData, 4));

    auto fired = s.popDue(150);          // the first is out and in hand
    REQUIRE(fired.valid());
    CHECK(fired.meta->v == 1);

    s.flush(SCHED_TAG_DEFAULT);          // cancel everything pending

    // The event in hand is untouched, payload included. Reading it straight
    // after the flush is not enough — freed-but-untouched bytes still read
    // correctly — so force the store to REUSE the space first. If the flush
    // released this slot, the new payload lands on top of the old one.
    static const uint8_t kOther[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    for (int i = 0; i < 8; ++i)
        (void)s.add(300 + i, SCHED_TAG_DEFAULT, {99}, kOther, 4);

    CHECK(fired.meta->v == 1);
    REQUIRE(fired.data != nullptr);
    CHECK(std::memcmp(fired.data, kData, 4) == 0);
    s.release(fired);
    s.flush(SCHED_TAG_DEFAULT);

    // And the pending one really did go.
    CHECK(s.size() == 0);
    CHECK_FALSE(s.popDue(INT64_MAX).valid());
}

TEST_CASE("Scheduler - events fire in the first block at or after their time, "
          "never early and without drift", "[scheduler][accuracy][regression]") {
    // Upstream's "scheduled events fire block-accurately, never early, no
    // drift" — the only case that walked the clock a block at a time instead of
    // asking the store a question. It is the one that would catch a store that
    // is internally consistent and still fires on the wrong block, and it had
    // no equivalent here at all.
    Scheduler<TestMeta, 512, 65536> s;

    constexpr int64_t kBlock = 128;
    constexpr int     kN     = 12;
    // Neither is a multiple of the block, so every event lands off the grid and
    // the quantisation is exercised rather than dodged.
    constexpr int64_t kInterval = kBlock * 5 + 37;
    constexpr int64_t kBase     = kBlock * 3 + 11;

    for (int i = 0; i < kN; ++i)
        REQUIRE(s.add(kBase + i * kInterval, SCHED_TAG_DEFAULT,
                      {static_cast<uint32_t>(i)}, kData, 4));

    // Walk one block at a time, recording the block each event fired in.
    std::vector<int64_t> firedAt;
    std::vector<uint32_t> firedOrder;
    int64_t t = 0;
    const int64_t last = kBase + (kN - 1) * kInterval;
    while (t < last + kBlock * 2 && firedAt.size() < kN) {
        const int64_t blockEnd = t + kBlock;
        int inThisBlock = 0;
        for (;;) {
            auto e = s.popDue(blockEnd);
            if (!e.valid()) break;
            ++inThisBlock;
            firedAt.push_back(blockEnd);
            firedOrder.push_back(e.meta->v);
            s.release(e);
        }
        // kInterval > kBlock, so at most one can come due per block. More than
        // one means the store released something before its time.
        CHECK(inThisBlock <= 1);
        t = blockEnd;
    }
    REQUIRE(firedAt.size() == kN);

    int64_t worstLatency = 0;
    for (int i = 0; i < kN; ++i) {
        const int64_t target = kBase + i * kInterval;
        INFO("event " << i << " target " << target << " fired at " << firedAt[i]);
        // In order.
        CHECK(firedOrder[i] == static_cast<uint32_t>(i));
        // NEVER EARLY. A tick ahead of its beat cannot be corrected downstream.
        CHECK(firedAt[i] >= target);
        // And in the FIRST block at or after it, not some later one.
        CHECK(firedAt[i] - target < kBlock);
        worstLatency = std::max(worstLatency, firedAt[i] - target);
    }
    // NO DRIFT: the last event is no worse than the first. A store that leaked
    // a block per event would pass every check above and fail this one.
    CHECK(worstLatency < kBlock);
    CHECK(firedAt[kN - 1] - (kBase + (kN - 1) * kInterval) < kBlock);
}

// ── Bundles and the prefix ───────────────────────────────────────────────────

TEST_CASE("a /clockwork/ verb inside a bundle is not claimed by clockwork",
          "[scheduler][prefix][bundle]") {
    // A bundle begins "#bundle" and has no single address, so clockwork_sys_claims
    // cannot classify it and does not try: the whole bundle goes to the DSP.
    // That means a clockwork verb travelling inside one is NOT answered here —
    // a sharp edge worth pinning, because the obvious expectation is that an
    // address clockwork owns is answered by clockwork wherever it appears.
    const uint8_t bundle[] = {
        '#','b','u','n','d','l','e','\0',      // marker
        0,0,0,0, 0,0,0,1,                      // timetag: immediately
        0,0,0,16,                              // element size
        '/','c','l','o','c','k','w','o','r','k','/','m','i','d','i','\0',
    };
    CHECK_FALSE(clockwork_sys_claims(bundle, sizeof bundle));

    // The same address on its own IS claimed. The difference is the envelope,
    // not the address.
    const uint8_t bare[] = { '/','c','l','o','c','k','w','o','r','k','/','m','i','d','i','\0' };
    CHECK(clockwork_sys_claims(bare, sizeof bare));
}
