// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_assets.cpp — assets: bulk that enters the guest through the inbox lane.
 *
 * A sample, a wavetable, a synthdef bundle: a large blob the guest needs in
 * memory and must never receive through the message ring. The client owns
 * the lane and a pool over it; it writes the bytes into a slot and sends ONE
 * message naming the slot — /clockwork/asset/commit — and the audio thread
 * hands the guest a pointer (dsp_asset). When the guest is done it says so
 * through one host callback, and the client hears /clockwork/asset/released
 * so its pool can reclaim. Nothing here knows what a sample is; the guest
 * gives the bytes a meaning when it gets the pointer.
 *
 * These cases drive that loop against the dummy DSP, which records what it was
 * handed and releases on request, so every leg is observable from the client's
 * side: the pointer landed on the right bytes, the audio kind carried its
 * geometry, a bad range was refused before the guest saw it, a release came
 * back as a notification, and the audio thread allocated nothing for any of it.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "clockwork_client.h"
#include "dsp_api.h"
#include "rt_alloc.h"
#include "shm_segment.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__APPLE__)
#  include <mach/mach.h>
#elif !defined(_WIN32)
#  include <cstdio>
#  include <unistd.h>
#endif

namespace {

// The resident set as it stands, in bytes: what the process has touched and
// still holds. A lane that is only reserved does not move it; one that was
// cleared at boot does. 0 where it cannot be read (Windows), which the case
// below takes as "nothing to measure here".
//
// NOT THE PEAK. This read ru_maxrss, the process's high-water mark, which
// only moves when the process goes higher than it has ever been — so in a
// suite run in one process, where earlier cases had already been higher, a
// lane committed whole at boot would not have moved it at all, and the case
// passed without measuring anything. Alone, it measured the boot and failed.
uint64_t residentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<uint64_t>(info.resident_size);
#elif defined(_WIN32)
    return 0;
#else
    unsigned long total = 0, resident = 0;
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    const int got = std::fscanf(f, "%lu %lu", &total, &resident);
    std::fclose(f);
    if (got != 2) return 0;
    return static_cast<uint64_t>(resident) * static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

uint64_t grownSince(uint64_t before) {
    const uint64_t now = residentBytes();
    return now > before ? now - before : 0;
}

// The lane, as the client writes it. The engine's view is const (the guest
// reads it); the client is the one writer, which is exactly this side.
uint8_t* inboxOf(EngineFixture& fx) {
    return const_cast<uint8_t*>(fx.engine().guestInbox());
}

uint32_t checksum(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// /clockwork/asset/commit ,iiiiiif id kind offset bytes channels frames rate
osc_test::Packet commit(int32_t id, int32_t kind, int32_t offset, int32_t bytes,
                        int32_t channels = 0, int32_t frames = 0, float rate = 0.f) {
    osc_test::Builder b;
    auto& s = b.begin("/clockwork/asset/commit");
    s << id << kind << offset << bytes << channels << frames << rate;
    return b.end();
}

// What the dummy DSP recorded for its most recent asset:
// /dummy/asset/last.reply ,iiiiiii id kind bytes channels frames rate_x1000 checksum
struct Last { int32_t id, kind, bytes, channels, frames, rateMilli; uint32_t sum; };
Last lastAsset(EngineFixture& fx) {
    fx.send(osc_test::message("/dummy/asset/last"));
    OscReply r;
    REQUIRE(fx.waitForReply("/dummy/asset/last.reply", r));
    const auto p = r.parsed();
    REQUIRE(p.argCount() == 7);
    return { p.argInt(0), p.argInt(1), p.argInt(2), p.argInt(3), p.argInt(4), p.argInt(5),
             static_cast<uint32_t>(p.argInt(6)) };
}

} // namespace

TEST_CASE("asset: a committed slot reaches the guest as a pointer into the inbox", "[asset]") {
    EngineFixture fx;
    uint8_t* inbox = inboxOf(fx);
    REQUIRE(inbox != nullptr);
    REQUIRE(fx.engine().guestInboxBytes() >= 65536);

    // The client's bytes, at an offset of its choosing. The engine copies
    // nothing: the guest reads these very bytes, so a pattern written here is
    // the pattern the guest checksums.
    const uint32_t off = 4096, n = 1000;
    for (uint32_t i = 0; i < n; ++i) inbox[off + i] = static_cast<uint8_t>((i * 7) ^ 0x5A);
    const uint32_t expect = checksum(inbox + off, n);

    fx.send(commit(7, CLOCKWORK_ASSET_RAW, static_cast<int32_t>(off), static_cast<int32_t>(n)));
    OscReply ok;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", ok));
    CHECK(ok.parsed().argInt(0) == 7);

    const Last l = lastAsset(fx);
    CHECK(l.id == 7);
    CHECK(l.kind == CLOCKWORK_ASSET_RAW);
    CHECK(l.bytes == static_cast<int32_t>(n));
    CHECK(l.sum == expect);
}

TEST_CASE("asset: the audio kind carries channels, frames and rate, and must add up", "[asset]") {
    EngineFixture fx;
    uint8_t* inbox = inboxOf(fx);
    const uint32_t off = 8192, channels = 2, frames = 250;
    const uint32_t bytes = frames * channels * sizeof(float);
    std::memset(inbox + off, 0, bytes);

    fx.send(commit(3, CLOCKWORK_ASSET_AUDIO_F32, static_cast<int32_t>(off), static_cast<int32_t>(bytes),
                   channels, frames, 48000.f));
    OscReply ok;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", ok));
    const Last l = lastAsset(fx);
    CHECK(l.id == 3);
    CHECK(l.kind == CLOCKWORK_ASSET_AUDIO_F32);
    CHECK(l.channels == 2);
    CHECK(l.frames == 250);
    CHECK(l.rateMilli == 48000000);

    // Geometry that does not describe the byte count is refused before the
    // guest sees it, and says which asset it refused.
    fx.clearReplies();
    fx.send(commit(4, CLOCKWORK_ASSET_AUDIO_F32, static_cast<int32_t>(off), static_cast<int32_t>(bytes),
                   channels, frames + 1, 48000.f));
    OscReply no;
    REQUIRE(fx.waitForReply("/clockwork/asset/refused", no));
    CHECK(no.parsed().argInt(0) == 4);
    CHECK(lastAsset(fx).id == 3);
}

TEST_CASE("asset: a range outside the inbox is refused before the guest sees it", "[asset]") {
    EngineFixture fx;
    const uint32_t size = fx.engine().guestInboxBytes();
    // Establish a known last asset first.
    fx.send(commit(1, CLOCKWORK_ASSET_RAW, 0, 16));
    OscReply ok;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", ok));

    auto refused = [&](int32_t id, int32_t offset, int32_t bytes) {
        fx.clearReplies();
        fx.send(commit(id, CLOCKWORK_ASSET_RAW, offset, bytes));
        OscReply no;
        REQUIRE(fx.waitForReply("/clockwork/asset/refused", no));
        CHECK(no.parsed().argInt(0) == id);
        CHECK(no.parsed().argCount() >= 2);   // and a reason
    };
    refused(20, static_cast<int32_t>(size), 16);                  // starts past the end
    refused(21, static_cast<int32_t>(size - 8), 16);              // runs past the end
    refused(22, -4, 16);                                          // negative offset
    refused(23, 0, 0);                                            // empty
    refused(24, 0x7FFFFFF0, 0x7FFFFFF0);                          // would overflow 32 bits
    CHECK(lastAsset(fx).id == 1);
}

TEST_CASE("asset: a release comes back to the client as a notification", "[asset]") {
    EngineFixture fx;
    fx.send(commit(9, CLOCKWORK_ASSET_RAW, 1024, 64));
    OscReply ok;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", ok));

    // The guest lets go (here: on request); the client hears it by id, which
    // is what lets its pool reclaim the slot.
    fx.clearReplies();
    fx.send(osc_test::message("/dummy/asset/release", int32_t{9}));
    OscReply rel;
    REQUIRE(fx.waitForReply("/clockwork/asset/released", rel));
    CHECK(rel.parsed().argInt(0) == 9);

    // The id is free to be reused, and a second release of it is nothing.
    fx.clearReplies();
    fx.send(commit(9, CLOCKWORK_ASSET_RAW, 2048, 64));
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", ok));
    fx.clearReplies();
    fx.send(osc_test::message("/dummy/asset/release", int32_t{9}));
    REQUIRE(fx.waitForReply("/clockwork/asset/released", rel));
    fx.clearReplies();
    fx.send(osc_test::message("/dummy/asset/release", int32_t{9}));
    OscReply none;
    CHECK_FALSE(fx.waitForReply("/clockwork/asset/released", none, 300));
}

TEST_CASE("asset: the audio thread allocates nothing for a commit or a release", "[asset]") {
    EngineFixture fx;
    // Warm up: the first messages may touch paths that allocate once.
    fx.send(commit(30, CLOCKWORK_ASSET_RAW, 0, 64));
    OscReply ok;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", ok));
    fx.send(osc_test::message("/dummy/asset/release", int32_t{30}));
    OscReply rel;
    REQUIRE(fx.waitForReply("/clockwork/asset/released", rel));

    const int64_t before = rt_alloc::g_allocs.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < 50; ++i) {
        fx.send(commit(100 + i, CLOCKWORK_ASSET_RAW, 4096 + 128 * i, 64));
        REQUIRE(fx.waitForReply("/clockwork/asset/committed", ok));
        fx.send(osc_test::message("/dummy/asset/release", int32_t{100 + i}));
        REQUIRE(fx.waitForReply("/clockwork/asset/released", rel));
    }
    CHECK(rt_alloc::g_allocs.load(std::memory_order_relaxed) == before);
}

TEST_CASE("asset: the lane is the size the host asks for, and costs nothing until written",
          "[asset][lane]") {
    // A lane sized for real samples — a minute of stereo at 48 kHz is 23 MB
    // of frames, and a set holds many — is address space, not memory: the
    // pages are committed as the client writes them, never cleared at boot.
    // With the segment (a public reader may map it) and without (the lanes
    // are the engine's own allocation), the same.
    const uint32_t want = 256u * 1024u * 1024u;
    const bool segment = GENERATE(false, true);
    INFO((segment ? "public segment" : "process-local lanes"));

    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();

    // WHAT A BOOT COSTS ANYWAY, measured rather than assumed: the heap, the
    // rings and the DSP are memory an engine touches whatever its lane is,
    // and how much that is belongs to the build (a sanitizer's is several
    // times a release's). An engine with the default lane says what it is
    // here, in this process, and the big lane is held to that plus a margin
    // far smaller than itself.
    uint64_t ordinary = 0;
    {
        ClockworkEngine::Config plain = cfg;
        plain.udpPort = segment ? 57342 : 0;
        const uint64_t before = residentBytes();
        EngineFixture small(plain);
        ordinary = grownSince(before);
    }

    cfg.inboxBytes = want;
    cfg.udpPort    = segment ? 57341 : 0;
    const uint64_t before = residentBytes();
    EngineFixture fx(cfg);
    REQUIRE(fx.engine().guestInboxBytes() == want);
    const uint64_t grew = grownSince(before);
    INFO("an ordinary boot grew the resident set by " << (ordinary >> 20)
         << " MB, this one by " << (grew >> 20) << " MB");
    CHECK(grew < ordinary + 64u * 1024u * 1024u);

    // The far end is addressable: an asset at the last page of the lane
    // reaches the guest, which checksums the very bytes written there.
    uint8_t* inbox = inboxOf(fx);
    const uint32_t off = want - 4096, n = 512;
    for (uint32_t i = 0; i < n; ++i) inbox[off + i] = static_cast<uint8_t>(i * 3 + 1);
    fx.send(commit(11, CLOCKWORK_ASSET_RAW, static_cast<int32_t>(off), static_cast<int32_t>(n)));
    OscReply ok;
    REQUIRE(fx.waitForReply("/clockwork/asset/committed", ok));
    const Last l = lastAsset(fx);
    CHECK(l.id == 11);
    CHECK(l.sum == checksum(inbox + off, n));

    // AND THE MEASUREMENT CAN SEE A LANE THAT IS WRITTEN. Half of it, touched
    // a page at a time, must show up in the resident set — otherwise "it did
    // not grow" above is what a ruler that reads nothing would say as well.
    if (before != 0) {
        const uint64_t half = want / 2;
        const uint64_t untouched = residentBytes();
        for (uint64_t at = 0; at < half; at += 4096) inbox[at] = 1;
        const uint64_t touched = grownSince(untouched);
        INFO("writing " << (half >> 20) << " MB of the lane grew the resident set by "
             << (touched >> 20) << " MB");
        CHECK(touched > half / 2);
    }

    if (segment) {
        // A reader takes the lane's size from the header, not from a constant
        // of its own build: this client sees the whole of it.
        CHECK(fx.engine().shmSegmentSize() > want);
        ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
        ClockworkClient* c = clockwork_client_open_shm_handle(
            detail_shm_segment::shm_handle_to_intptr(
                detail_shm_segment::shm_dup_handle(fx.engine().shmNativeHandle())), &st);
        REQUIRE(c != nullptr);
        ClockworkRegion region {};
        REQUIRE(clockwork_client_region(c, CLOCKWORK_REGION_INBOX, &region) == CLOCKWORK_OK);
        CHECK(region.bytes == want);
        CHECK(static_cast<uint8_t*>(region.base)[off] == inbox[off]);
        clockwork_client_close(c);
    }
}

TEST_CASE("asset: a client over the segment sees the same inbox", "[asset][shm]") {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.udpPort = 57340;   // non-zero: the engine carves the lanes from a public segment
    EngineFixture fx(cfg);

    ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
    ClockworkClient* c = clockwork_client_open_shm_handle(
        detail_shm_segment::shm_handle_to_intptr(
            detail_shm_segment::shm_dup_handle(fx.engine().shmNativeHandle())), &st);
    REQUIRE(c != nullptr);

    ClockworkRegion inbox {};
    REQUIRE(clockwork_client_region(c, CLOCKWORK_REGION_INBOX, &inbox) == CLOCKWORK_OK);
    REQUIRE(inbox.base != nullptr);
    CHECK(inbox.bytes == fx.engine().guestInboxBytes());
    CHECK(inbox.writable == 1);

    // Two mappings of one lane: what the client writes, the guest reads.
    static_cast<uint8_t*>(inbox.base)[300] = 0xC3;
    CHECK(fx.engine().guestInbox()[300] == 0xC3);
    clockwork_client_close(c);
}
