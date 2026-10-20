// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restarts.cpp — docs/RESTARTS.md, pinned at the lanes level.
 *
 * A cold restart is destroy_dsp then rebuild_dsp on the arena that is up:
 * exactly what ClockworkEngine does for a cold device switch, with the
 * egress drained between the two as the engine drains it before the rings
 * go. Each case here is one promise of the document, made against that
 * mechanism, so the promise is about clockwork and not about any one host.
 *
 * The regions the document says are KEPT are checked by writing something
 * only this case could have written, restarting, and reading it back; the
 * regions it says are RESET are checked by dirtying them first, so "was it
 * reset" is a question with a real answer. What the guest keeps in its own
 * regions (config, persist, its memory) is pinned in test_dsp_regions.cpp
 * and not repeated here; the table itself is pinned in test_arena.cpp.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "lanes/lanes.h"
#include "shared_memory.h"
#include "clock/clock_math.h"
#include "clockwork_clock_state.h"
#include "clockwork_client.h"
#include "clockwork_prefix.h"
#include "clockwork_sys.h"
#include "audio_processor.h"
#include "dsp_api.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kClient = 0x4e57;   // this file's asker; no other case uses it

const lanes_test::EgressFrame* findAddress(
    const std::vector<lanes_test::EgressFrame>& frames, const char* address) {
    for (const auto& f : frames) {
        if (osc_test::parseAddress(f.data.data(),
                                   static_cast<uint32_t>(f.data.size())) == address)
            return &f;
    }
    return nullptr;
}

osc_test::ParsedReply ask(const char* verb, const char* replyAddress) {
    const auto q = osc_test::message(verb);
    REQUIRE(lanes_test::ingress(q.ptr(), q.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, replyAddress);
    const auto* reply = findAddress(frames, replyAddress);
    REQUIRE(reply != nullptr);
    REQUIRE(reply->sourceId == kClient);
    return osc_test::parseReply(reply->data.data(),
                                static_cast<uint32_t>(reply->data.size()));
}

uint8_t* arena() {
    auto* base = static_cast<uint8_t*>(clockwork_lanes_base());
    REQUIRE(base != nullptr);
    return base;
}

template <typename T>
T* region(uint32_t offset) { return reinterpret_cast<T*>(arena() + offset); }

// Frames an egress ring has refused for want of room, since the arena was
// built (PerformanceMetrics::egress_frames_refused).
uint32_t metrics_refused() {
    return region<PerformanceMetrics>(METRICS_START)->egress_frames_refused.load();
}

// Nothing left on either egress ring from whatever ran before.
void settle() {
    lanes_test::boot();
    lanes_test::drainRt();
    lanes_test::drainNrt();
}

// The cold restart, in the engine's order: the instance goes, everything the
// engine has to say about that leaves on the rings, then the arena is
// re-initialised and the new instance built. Returns what left.
std::vector<lanes_test::EgressFrame> coldRestart() {
    destroy_dsp();
    auto said = lanes_test::drainRt();
    for (auto& f : lanes_test::drainNrt()) said.push_back(std::move(f));
    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    REQUIRE(g_dsp != nullptr);
    return said;
}

} // namespace

// ── Kept across a cold restart: lifetime cold boot ───────────────────────────

TEST_CASE("restarts: the session clock keeps its tempo across a cold restart",
          "[restarts][regions]") {
    settle();
    auto* state = region<ClockworkClockState>(CLOCK_STATE_START);
    const uint64_t saved = state->bpm.load(std::memory_order_acquire);
    state->bpm.store(clockwork::doubleToBits(132.0), std::memory_order_release);
    REQUIRE(ask("/dummy/region/clock", "/dummy/region/clock-is").argInt(1) == 132000);

    coldRestart();

    // A device change does not change the session: the new instance reads
    // the tempo the old one was playing at.
    CHECK(ask("/dummy/region/clock", "/dummy/region/clock-is").argInt(1) == 132000);
    state->bpm.store(saved, std::memory_order_release);
}

TEST_CASE("restarts: the node id counter keeps its count across a cold restart",
          "[restarts][regions]") {
    settle();
    auto* counter = region<std::atomic<int32_t>>(NODE_ID_COUNTER_START);
    const int32_t saved = counter->load(std::memory_order_acquire);
    counter->store(0x5EED, std::memory_order_release);

    coldRestart();

    // The ids are the clients', shared between them, and an instance that
    // comes and goes has no say over them.
    CHECK(counter->load(std::memory_order_acquire) == 0x5EED);
    counter->store(saved, std::memory_order_release);
}

TEST_CASE("restarts: the clock anchors are the host's and keep across a cold restart",
          "[restarts][regions]") {
    settle();
    auto* drift  = region<std::atomic<int32_t>>(DRIFT_OFFSET_START);
    auto* global = region<std::atomic<int32_t>>(GLOBAL_OFFSET_START);
    const int32_t savedDrift = drift->load(std::memory_order_acquire);
    const int32_t savedGlobal = global->load(std::memory_order_acquire);
    drift->store(777, std::memory_order_release);
    global->store(-42, std::memory_order_release);

    coldRestart();

    // The region's lifetime is cold boot: what the host measured about this
    // machine's clocks is not a fact about the instance, and a device change
    // must not throw it away.
    CHECK(drift->load(std::memory_order_acquire) == 777);
    CHECK(global->load(std::memory_order_acquire) == -42);
    drift->store(savedDrift, std::memory_order_release);
    global->store(savedGlobal, std::memory_order_release);
}

// ── Reset by a cold restart: lifetime rebuild ────────────────────────────────

TEST_CASE("restarts: the scope is reformatted and its write position restarts",
          "[restarts][regions]") {
    settle();
    auto* out = region<shm_audio_buffer>(SCOPE_START) + CLOCKWORK_TAP_OUT;
    lanes_test::tickUntil(4, nullptr);
    REQUIRE(out->enabled.load(std::memory_order_acquire) == 1u);
    REQUIRE(out->write_position.load(std::memory_order_acquire) > 0u);

    coldRestart();

    // A device switch is a new stream: formatted at the device's channels,
    // and a reader's cursor starts again with it.
    CHECK(out->enabled.load(std::memory_order_acquire) == 1u);
    CHECK(out->channels == lanes_test::kOutChannels);
    CHECK(out->write_position.load(std::memory_order_acquire) == 0u);
    lanes_test::tickUntil(1, nullptr);
    CHECK(out->write_position.load(std::memory_order_acquire) > 0u);
}

TEST_CASE("restarts: the taps start with every slot free",
          "[restarts][regions]") {
    settle();
    coldRestart();
    const auto* taps = arena() + TAPS_START;
    const auto* activeCount = reinterpret_cast<const std::atomic<uint32_t>*>(taps + 4);
    CHECK(activeCount->load(std::memory_order_acquire) == 0u);
    // The first slot, byte for byte: state free, nothing claimed.
    const uint8_t* slot0 = taps + TAPS_HEADER_SIZE;
    bool zero = true;
    for (uint32_t i = 0; i < TAP_SLOT_HEADER_SIZE; ++i) zero = zero && slot0[i] == 0;
    CHECK(zero);
}

TEST_CASE("restarts: the metrics count on across a cold restart, so what a teardown lost is still counted after it",
          "[restarts][regions]") {
    settle();
    auto* metrics = region<PerformanceMetrics>(METRICS_START);
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    lanes_test::tickUntil(4, "/dummy/pong");
    const uint32_t blocks    = metrics->process_count.load(std::memory_order_acquire);
    const uint32_t processed = metrics->messages_processed.load(std::memory_order_acquire);
    REQUIRE(blocks > 0u);
    REQUIRE(processed > 0u);
    metrics->egress_frames_refused.fetch_add(1);   // a loss at the teardown, as a full ring counts one

    coldRestart();

    CHECK(metrics->process_count.load(std::memory_order_acquire) >= blocks);
    CHECK(metrics->messages_processed.load(std::memory_order_acquire) >= processed);
    CHECK(metrics->egress_frames_refused.load(std::memory_order_acquire) >= 1u);
    metrics->egress_frames_refused.fetch_sub(1);
}

TEST_CASE("restarts: the in ring and the instance's ring keep their place across a cold restart, and setup follows the old instance's words",
          "[restarts][regions]") {
    settle();
    const auto* cursors = region<RingCursors>(RING_CURSORS_START);
    // The old instance answers, and nobody reads it before the rebuild.
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    lanes_test::tick();
    const int32_t inHead = cursors->in_head.load(), inSequence = cursors->in_sequence.load();
    destroy_dsp();
    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    REQUIRE(g_dsp != nullptr);

    // The in ring: where it was, its sequence going on — nothing in it was
    // the rebuild's to throw away.
    CHECK(cursors->in_head.load() == inHead);
    CHECK(cursors->in_tail.load() == inHead);
    CHECK(cursors->in_sequence.load() == inSequence);
    CHECK(cursors->status_flags.load() == STATUS_OK);
    // The instance's ring: the old instance's answer, then the new instance's
    // setup, naming the rate, the device's callback and the generation opened.
    const auto said = lanes_test::drainRt();
    const auto* pong  = findAddress(said, "/dummy/pong");
    const auto* setup = findAddress(said, CLOCKWORK_SYS("setup"));
    REQUIRE(pong != nullptr);
    REQUIRE(setup != nullptr);
    CHECK(pong < setup);
    const auto p = osc_test::parseReply(setup->data.data(), static_cast<uint32_t>(setup->data.size()));
    REQUIRE(p.argCount() == 3);
    CHECK(p.argInt(0) == static_cast<int32_t>(lanes_test::kSampleRate));
    CHECK(p.argInt(1) == static_cast<int32_t>(lanes_test::kBufLength));
    CHECK(p.argInt(2) == static_cast<int32_t>(clockwork_arena_generation(arenaHeader(arena()))));
    // And the new instance answers through it, after.
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, "/dummy/pong");
    CHECK(findAddress(frames, "/dummy/pong") != nullptr);
}

TEST_CASE("restarts: the guest window is zeroed until the new instance lays it out",
          "[restarts][regions]") {
    settle();
    struct Window { uint32_t magic, seq, value, writes; };
    constexpr uint32_t kWindowMagic = 0x57494e44u;   // 'WIND', the placeholder's
    const auto* w = region<const Window>(GUEST_WINDOW_START);
    const auto pub = osc_test::message("/dummy/region/window", 0x7e57, 0);
    REQUIRE(lanes_test::ingress(pub.ptr(), pub.size(), kClient));
    lanes_test::tickUntil(4, nullptr);
    REQUIRE(w->magic == kWindowMagic);

    coldRestart();

    // A reader polling by hand sees zeros, meaning "not yet", rather than the
    // old instance's record wearing the new one's identity.
    CHECK(w->magic == 0u);
    CHECK(w->value == 0u);
}

// ── Every region, by the table ───────────────────────────────────────────────

namespace {
// The regions a cold restart is judged on by their bytes: everything in the
// table but the transport area. The rings are a protocol between threads,
// not bytes — a producer may be mid-write through a rebuild — so their
// reset is their cursors (a fresh epoch), pinned by the rings case above,
// and scribbling over them would not be dirtying but corrupting.
std::vector<const ClockworkArenaEntry*> byteRegions() {
    const auto* h = arenaHeader(arena());
    std::vector<const ClockworkArenaEntry*> out;
    for (uint32_t i = 0; i < h->entry_count; ++i) {
        const auto* e = &h->entries[i];
        if (e->audience & CLOCKWORK_AUDIENCE_TRANSPORT) continue;
        if (e->lifetime == CLOCKWORK_LIFETIME_ARENA) continue;
        out.push_back(e);
    }
    return out;
}
std::vector<uint8_t> bytesOf(const ClockworkArenaEntry* e) {
    return std::vector<uint8_t>(arena() + e->offset, arena() + e->offset + e->bytes);
}
void scribble(const ClockworkArenaEntry* e, uint8_t pattern) {
    std::memset(arena() + e->offset, pattern, e->bytes);
}
} // namespace

TEST_CASE("restarts: every region is reset or kept across a cold restart as the table says, and nothing else decides",
          "[restarts][regions][table]") {
    settle();
    // Two restarts, each from a different scribble. A region the table says
    // is RESET comes out the same both times: what it held before has no say.
    // One it says is KEPT comes out different: the scribble is still there.
    // Judged from the table alone, so a region added without a reset, or
    // labelled wrong, fails here by name rather than by someone noticing.
    const auto regions = byteRegions();
    REQUIRE(regions.size() >= 8);   // the table is not empty, and most of it is here
    std::vector<std::vector<uint8_t>> original;
    for (const auto* e : regions) original.push_back(bytesOf(e));

    for (const auto* e : regions) scribble(e, 0xA5);
    coldRestart();
    std::vector<std::vector<uint8_t>> first;
    for (const auto* e : regions) first.push_back(bytesOf(e));

    for (const auto* e : regions) scribble(e, 0x5A);
    coldRestart();
    for (size_t i = 0; i < regions.size(); ++i) {
        const auto* e = regions[i];
        const auto second = bytesOf(e);
        INFO("region id " << e->id << " (lifetime " << e->lifetime << ")");
        if (e->lifetime == CLOCKWORK_LIFETIME_REBUILD)
            CHECK(second == first[i]);            // reset: independent of what it held
        else
            CHECK(second != first[i]);            // kept: the scribble survived
    }

    // The suite shares this arena: what was there goes back, and one more
    // build lays the reset regions out from it again.
    for (size_t i = 0; i < regions.size(); ++i)
        std::memcpy(arena() + regions[i]->offset, original[i].data(), original[i].size());
    coldRestart();
}

TEST_CASE("restarts: every region in the table has a reset, and only the table says when it runs",
          "[restarts][regions][table]") {
    settle();
    const auto* h = arenaHeader(arena());
    for (uint32_t i = 0; i < h->entry_count; ++i) {
        const auto* e = &h->entries[i];
        INFO("region id " << e->id);
        // A region that lives as long as the arena is never reset; every
        // other one has its reset in the lanes, run at the event its
        // lifetime names (docs/RESTARTS.md).
        CHECK(clockwork_region_has_reset(e->id) == (e->lifetime != CLOCKWORK_LIFETIME_ARENA));
    }
    CHECK_FALSE(clockwork_region_has_reset(999u));   // not a region
}

// ── The transport area: nothing lost without a word ──────────────────────────

TEST_CASE("restarts: a message in the in ring at a cold restart is refused to its sender, naming the generation that closed",
          "[restarts][transport]") {
    settle();
    // In the ring, not yet drained: what a client had sent in the moment the
    // instance went, which is any message a client sends between the device
    // stopping and the rebuild.
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    const uint32_t closing = clockwork_arena_generation(arenaHeader(arena()));
    coldRestart();

    // Refused to the one who sent it by the next drain, naming the message,
    // why, and the generation it was written for: clockwork has the address
    // and the token, which is all a refusal needs.
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("error"));
    const auto* refusal = findAddress(frames, CLOCKWORK_SYS("error"));
    REQUIRE(refusal != nullptr);
    CHECK(refusal->sourceId == kClient);
    const auto r = osc_test::parseReply(refusal->data.data(),
                                        static_cast<uint32_t>(refusal->data.size()));
    REQUIRE(r.argCount() == 3);
    CHECK(r.argString(0) == "/dummy/ping");
    CHECK(r.argString(1) == CLOCKWORK_REFUSED_REBUILDING);
    CHECK(static_cast<uint32_t>(r.argInt(2)) == closing);

    // And the new instance never saw it: a message addressed to the old one
    // is not the new one's to answer.
    CHECK(findAddress(frames, "/dummy/pong") == nullptr);
    const auto after = lanes_test::tickUntil(4, "/dummy/pong");
    CHECK(findAddress(after, "/dummy/pong") == nullptr);
}

TEST_CASE("restarts: more waiting at a cold restart than the instance's ring can hold refusals for are all refused, as it makes room",
          "[restarts][transport]") {
    settle();
    // Far more refusals than the instance's ring holds at once: four
    // thousand, or as many as the in ring holds (the smallest profile's
    // holds about a thousand).
    int waiting = 0;
    const auto ping = osc_test::message("/dummy/ping");
    while (waiting < 4000 && lanes_test::ingress(ping.ptr(), ping.size(), kClient)) ++waiting;
    REQUIRE(waiting > 500);
    coldRestart();
    const uint32_t refusedBefore = metrics_refused();

    // Nobody reads for a while: the refusals stop when the ring is full, and
    // none is lost to it.
    for (int i = 0; i < 400; ++i) lanes_test::tick();
    CHECK(metrics_refused() == refusedBefore);

    // Read, and the rest follow: every one, once.
    int refused = 0, answered = 0;
    for (int i = 0; i < 4000 && refused + answered < waiting; ++i) {
        lanes_test::tick();
        for (const auto& f : lanes_test::drainRt()) {
            const auto addr = osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size()));
            if (addr == CLOCKWORK_SYS("error")) ++refused;
            if (addr == "/dummy/pong") ++answered;
        }
    }
    CHECK(refused == waiting);
    CHECK(answered == 0);
    CHECK(metrics_refused() == refusedBefore);
}

TEST_CASE("restarts: a message sent while there is no instance is refused at once",
          "[restarts][transport]") {
    settle();
    destroy_dsp();
    lanes_test::drainRt();
    lanes_test::drainNrt();

    const auto ping = osc_test::message("/dummy/ping");
    // Not written: the boundary says so to its caller, and says why on the
    // egress for the source that sent it, with no block to wait for. Here
    // it is on the ring before the rebuild.
    CHECK_FALSE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    auto said = lanes_test::drainRt();
    for (auto& f : lanes_test::drainNrt()) said.push_back(std::move(f));
    const auto* refusal = findAddress(said, CLOCKWORK_SYS("error"));
    CHECK(refusal != nullptr);
    if (refusal) {
        CHECK(refusal->sourceId == kClient);
        const auto r = osc_test::parseReply(refusal->data.data(),
                                            static_cast<uint32_t>(refusal->data.size()));
        REQUIRE(r.argCount() >= 2);
        CHECK(r.argString(0) == "/dummy/ping");
        CHECK(r.argString(1) == "no instance");
    }

    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    REQUIRE(g_dsp != nullptr);
    const auto after = lanes_test::tickUntil(4, "/dummy/pong");
    CHECK(findAddress(after, "/dummy/pong") == nullptr);
}

TEST_CASE("restarts: a message written in place while there is no instance is refused, as one sent is",
          "[restarts][transport]") {
    settle();
    destroy_dsp();
    lanes_test::drainRt();
    lanes_test::drainNrt();

    // A client building its message in the ring, where it will live
    // (clockwork_client_send_begin / _commit): the same door as a send, and
    // the same refusal, whether at the reservation or at the commit.
    ClockworkStatus st = CLOCKWORK_OK;
    uint8_t* base = static_cast<uint8_t*>(clockwork_lanes_base());
    ClockworkClient* c = clockwork_client_open_memory(base, arenaHeader(base)->arena_bytes, &st);
    REQUIRE(c != nullptr);
    const auto ping = osc_test::message("/dummy/ping");
    uint8_t* at = clockwork_client_send_begin(c, ping.size(), &st);
    if (at) {
        std::memcpy(at, ping.ptr(), ping.size());
        st = clockwork_client_send_commit(c, ping.size(), kClient);
    }
    CHECK(st == CLOCKWORK_E_NO_INSTANCE);
    clockwork_client_close(c);

    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    REQUIRE(g_dsp != nullptr);
    const auto after = lanes_test::tickUntil(4, "/dummy/pong");
    CHECK(findAddress(after, "/dummy/pong") == nullptr);
}

TEST_CASE("restarts: clockwork's own verbs are answered while there is no instance",
          "[restarts][transport]") {
    settle();
    destroy_dsp();
    lanes_test::drainRt();
    lanes_test::drainNrt();

    // A verb of clockwork's, not the guest's: the engine answers it itself,
    // instance or no instance. /clockwork/ping is the liveness verb every
    // build answers.
    const auto ping = osc_test::message(CLOCKWORK_SYS("ping"));
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("pong"));
    const auto* pong = findAddress(frames, CLOCKWORK_SYS("pong"));
    CHECK(pong != nullptr);
    CHECK(findAddress(frames, CLOCKWORK_SYS("error")) == nullptr);
}

TEST_CASE("restarts: what hands the guest something was written for its instance, refused with the guest's own at a cold restart and at the door between instances",
          "[restarts][transport]") {
    settle();
    const uint32_t closing = clockwork_arena_generation(arenaHeader(arena()));
    // An asset's commit, of 64 bytes at the start of the assets area, and a
    // sync: one hands the guest something, the other is clockwork's alone.
    std::memset(lanes_test::assets(), 0x5A, 64);
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("asset/commit")) << int32_t{5} << int32_t{CLOCKWORK_ASSET_RAW} << int32_t{0}
                                           << int32_t{64} << int32_t{0} << int32_t{0} << 0.f;
    const auto commit = b.end();
    const auto sync = osc_test::message(CLOCKWORK_SYS("sync"), int32_t{7});
    const auto refusalOf = [](const std::vector<lanes_test::EgressFrame>& frames, const char* reason) {
        for (const auto& f : frames) {
            const auto r = osc_test::parseReply(f.data.data(), static_cast<uint32_t>(f.data.size()));
            if (r.address == CLOCKWORK_SYS("error") && r.argString(0) == CLOCKWORK_SYS("asset/commit")
                && r.argString(1) == reason)
                return r.argInt(2);
        }
        return -1;
    };

    // Waiting when the instance goes.
    REQUIRE(lanes_test::ingress(commit.ptr(), commit.size(), kClient));
    REQUIRE(lanes_test::ingress(sync.ptr(), sync.size(), kClient));
    destroy_dsp();
    lanes_test::drainRt();
    lanes_test::drainNrt();

    // Sent between instances: refused at the door.
    CHECK_FALSE(lanes_test::ingress(commit.ptr(), commit.size(), kClient));
    auto said = lanes_test::drainRt();
    for (auto& f : lanes_test::drainNrt()) said.push_back(std::move(f));
    CHECK(refusalOf(said, CLOCKWORK_REFUSED_NO_INSTANCE) == static_cast<int32_t>(closing));

    // The next instance: the waiting commit refused as the closing
    // generation's, the sync beside it answered, and nothing committed.
    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    REQUIRE(g_dsp != nullptr);
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("synced"));
    CHECK(refusalOf(frames, CLOCKWORK_REFUSED_REBUILDING) == static_cast<int32_t>(closing));
    CHECK(findAddress(frames, CLOCKWORK_SYS("synced")) != nullptr);
    CHECK(findAddress(frames, CLOCKWORK_SYS("asset/committed")) == nullptr);
}

// ── The generation ───────────────────────────────────────────────────────────

namespace {
const ClockworkArenaHeader* header() { return arenaHeader(arena()); }
}

TEST_CASE("restarts: the generation is in the arena header, even with an instance, odd between, and moves on at every build",
          "[restarts][generation]") {
    settle();
    const auto* h = header();
    REQUIRE(clockwork_arena_has_instance(h));
    const uint32_t gen = clockwork_arena_generation(h);
    CHECK(gen >= 1);

    // Torn down: the word goes odd FIRST, before anything else of the
    // teardown, and names the generation that is closing. A reader in
    // another process knows "between instances" from the bytes alone.
    destroy_dsp();
    CHECK_FALSE(clockwork_arena_has_instance(h));
    CHECK(clockwork_arena_generation(h) == gen);
    lanes_test::drainRt();
    lanes_test::drainNrt();

    // Built: even again, one on. The header is the region a rebuild keeps,
    // so the count is continuous across every build on this arena.
    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    REQUIRE(g_dsp != nullptr);
    CHECK(clockwork_arena_has_instance(h));
    CHECK(clockwork_arena_generation(h) == gen + 1);
}

TEST_CASE("restarts: a refusal names the generation that refused",
          "[restarts][generation][transport]") {
    settle();
    const uint32_t gen = clockwork_arena_generation(header());

    // In the ring at the teardown.
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    destroy_dsp();
    lanes_test::drainRt();
    lanes_test::drainNrt();

    // Between instances: refused at the door, the generation still the
    // closing one, since none has opened.
    CHECK_FALSE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
    auto said = lanes_test::drainRt();
    for (auto& f : lanes_test::drainNrt()) said.push_back(std::move(f));
    const auto* between = findAddress(said, CLOCKWORK_SYS("error"));
    REQUIRE(between != nullptr);
    auto r = osc_test::parseReply(between->data.data(),
                                  static_cast<uint32_t>(between->data.size()));
    REQUIRE(r.argCount() == 3);
    CHECK(r.argString(1) == CLOCKWORK_REFUSED_NO_INSTANCE);
    CHECK(r.argInt(2) == static_cast<int32_t>(gen));

    // Built again. What waited at the teardown is refused by the next drain,
    // naming the generation that was closing, the one its sender had been
    // talking to.
    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    REQUIRE(g_dsp != nullptr);
    const auto waited = lanes_test::tickUntil(4, CLOCKWORK_SYS("error"));
    const auto* atTeardown = findAddress(waited, CLOCKWORK_SYS("error"));
    REQUIRE(atTeardown != nullptr);
    r = osc_test::parseReply(atTeardown->data.data(),
                             static_cast<uint32_t>(atTeardown->data.size()));
    REQUIRE(r.argCount() == 3);
    CHECK(r.argString(1) == CLOCKWORK_REFUSED_REBUILDING);
    CHECK(r.argInt(2) == static_cast<int32_t>(gen));

    // And every refusal clockwork makes from now on, of anything, carries
    // the generation that opened. One shape for all of them.
    const auto unknown = osc_test::message(CLOCKWORK_SYS("no-such-verb"));
    REQUIRE(lanes_test::ingress(unknown.ptr(), unknown.size(), kClient));
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("error"));
    const auto* refused = findAddress(frames, CLOCKWORK_SYS("error"));
    REQUIRE(refused != nullptr);
    r = osc_test::parseReply(refused->data.data(),
                             static_cast<uint32_t>(refused->data.size()));
    REQUIRE(r.argCount() == 3);
    CHECK(r.argString(0) == CLOCKWORK_SYS("no-such-verb"));
    CHECK(r.argInt(2) == static_cast<int32_t>(gen + 1));
}

// ── The control ring ─────────────────────────────────────────────────────────

namespace {
const ControlRingCursors* controlRing() { return region<const ControlRingCursors>(CONTROL_RING_START); }
int32_t controlHead() { return controlRing()->head.load(std::memory_order_acquire); }
int32_t inHead() { return region<RingCursors>(RING_CURSORS_START)->in_head.load(std::memory_order_acquire); }
} // namespace

TEST_CASE("restarts: with the control ring served, a control verb goes there and not through the audio thread; without, everything rides the in ring",
          "[restarts][control-ring][transport]") {
    settle();
    const auto notify = osc_test::message(CLOCKWORK_SYS("notify"));
    const auto ping   = osc_test::message(CLOCKWORK_SYS("ping"));
    const auto guest  = osc_test::message("/dummy/ping");

    // Nobody drains the control ring in this binary (the lanes have no
    // control pass), so the word is off: a control verb rides the in ring as
    // it always did, and the audio thread forwards it along its chain.
    REQUIRE_FALSE(clockwork_control_served());
    {
        const int32_t in0 = inHead(), ctl0 = controlHead();
        REQUIRE(lanes_test::ingress(notify.ptr(), notify.size(), kClient));
        CHECK(inHead() != in0);
        CHECK(controlHead() == ctl0);
        lanes_test::tickUntil(2, nullptr);
    }

    // The engine says it serves the ring (ClockworkEngine does, once its
    // control pass drains it): from then on a control verb is written there,
    // where no audio block is between it and its answer, while a verb the
    // audio thread performs and the guest's traffic ride the in ring.
    clockwork_control_serve(1);
    REQUIRE(clockwork_control_served());
    {
        const int32_t in0 = inHead(), ctl0 = controlHead();
        REQUIRE(lanes_test::ingress(notify.ptr(), notify.size(), kClient));
        CHECK(inHead() == in0);
        CHECK(controlHead() != ctl0);
        const int32_t ctl1 = controlHead();
        REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));
        REQUIRE(lanes_test::ingress(guest.ptr(), guest.size(), kClient));
        CHECK(inHead() != in0);
        CHECK(controlHead() == ctl1);
        lanes_test::tickUntil(4, "/dummy/pong");
    }

    // Between instances the control ring takes a control verb as before: it
    // is clockwork's, answered with or without a guest, and never refused.
    destroy_dsp();
    lanes_test::drainRt();
    lanes_test::drainNrt();
    {
        const int32_t ctl0 = controlHead();
        REQUIRE(lanes_test::ingress(notify.ptr(), notify.size(), kClient));
        CHECK(controlHead() != ctl0);
        auto said = lanes_test::drainRt();
        for (auto& f : lanes_test::drainNrt()) said.push_back(std::move(f));
        CHECK(findAddress(said, CLOCKWORK_SYS("error")) == nullptr);
    }
    rebuild_dsp(lanes_test::kSampleRate, lanes_test::kBufLength);
    REQUIRE(g_dsp != nullptr);
    // And a rebuild leaves the ring alone: lifetime arena.
    CHECK(controlHead() != 0);
    clockwork_control_serve(0);
    // What this case queued is nobody's to drain here: the ring is left as a
    // control pass would find it, and the next case does not read it.
}

#if CLOCKWORK_SCHEDULER
TEST_CASE("restarts: what was scheduled for the old instance is cleared, not delivered to the new one",
          "[restarts][transport][scheduler]") {
    settle();
    // Forty blocks out: held in the store, not performed at once.
    const uint64_t dueFrame = lanes_test::position() + 40ull * lanes_test::kBufLength;
    const int64_t  due = clockwork::ntpToOscTimetag(lanes_test::ntpAtFrame(dueFrame));
    const auto inner = osc_test::message("/dummy/when");
    const auto pkt   = osc_test::bundle(static_cast<uint64_t>(due), { inner });
    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));
    lanes_test::tickUntil(2, nullptr);   // into the store

    coldRestart();

    // The schedule is empty from the restart: a client that halted on
    // `restarting` re-sends its future after `setup`.
    const auto frames = lanes_test::tickUntil(60, "/dummy/when-was");
    CHECK(findAddress(frames, "/dummy/when-was") == nullptr);
}
#endif
