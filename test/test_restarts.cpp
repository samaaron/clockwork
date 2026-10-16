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
#include "clockwork_prefix.h"
#include "audio_processor.h"
#include "dsp_api.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
extern struct Dsp* g_dsp;
void destroy_dsp();
void rebuild_dsp(double sample_rate);
}

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
    rebuild_dsp(lanes_test::kSampleRate);
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

TEST_CASE("restarts: the metrics counters start from zero",
          "[restarts][regions]") {
    settle();
    auto* metrics = region<PerformanceMetrics>(METRICS_START);
    lanes_test::tickUntil(4, nullptr);
    REQUIRE(metrics->process_count.load(std::memory_order_acquire) > 0u);

    coldRestart();

    CHECK(metrics->process_count.load(std::memory_order_acquire) == 0u);
    CHECK(metrics->messages_processed.load(std::memory_order_acquire) == 0u);
    CHECK(metrics->messages_dropped.load(std::memory_order_acquire) == 0u);
}

TEST_CASE("restarts: the rings begin a fresh epoch, every one of them empty",
          "[restarts][regions]") {
    settle();
    coldRestart();
    const auto* cursors = region<RingCursors>(RING_CURSORS_START);
    // The in ring: empty, its sequence from the start.
    CHECK(cursors->in_head.load() == cursors->in_tail.load());
    CHECK(cursors->in_sequence.load() == 0);
    CHECK(cursors->status_flags.load() == STATUS_OK);
    // The out rings: whatever is on them was said in the new epoch (the
    // rebuild's own account of itself), numbered from the start again, and
    // nothing of the old instance's is among it.
    auto fresh = lanes_test::drainRt();
    for (auto& f : lanes_test::drainNrt()) fresh.push_back(std::move(f));
    for (const auto& f : fresh) {
        const auto addr = osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size()));
        INFO("on the rings after the rebuild: " << addr << " seq " << f.seq);
        CHECK(f.seq < 16u);
        CHECK(addr != "/dummy/pong");
    }
    // And the new instance answers through them.
    const auto ping = osc_test::message("/dummy/ping");
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

// ── The transport area: nothing lost without a word ──────────────────────────

TEST_CASE("restarts: a message in the in ring at a cold restart is refused to its sender, not dropped",
          "[restarts][transport]") {
    settle();
    // In the ring, not yet drained: what a client had sent in the moment the
    // instance went, which is any message a client sends between the device
    // stopping and the rebuild.
    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));

    const auto said = coldRestart();

    // Refused to the one who sent it, naming the message and why: there is
    // no instance to answer it, and clockwork has the address and the token,
    // which is all a refusal needs.
    const auto* refusal = findAddress(said, CLOCKWORK_SYS("error"));
    REQUIRE(refusal != nullptr);
    CHECK(refusal->sourceId == kClient);
    const auto r = osc_test::parseReply(refusal->data.data(),
                                        static_cast<uint32_t>(refusal->data.size()));
    REQUIRE(r.argCount() >= 2);
    CHECK(r.argString(0) == "/dummy/ping");
    CHECK(r.argString(1).find("rebuild") != std::string::npos);

    // And the new instance never saw it: a message addressed to the old one
    // is not the new one's to answer.
    const auto after = lanes_test::tickUntil(4, "/dummy/pong");
    CHECK(findAddress(after, "/dummy/pong") == nullptr);
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

    rebuild_dsp(lanes_test::kSampleRate);
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
    rebuild_dsp(lanes_test::kSampleRate);
    const auto frames = lanes_test::tickUntil(4, CLOCKWORK_SYS("pong"));
    const auto* pong = findAddress(frames, CLOCKWORK_SYS("pong"));
    CHECK(pong != nullptr);
    CHECK(findAddress(frames, CLOCKWORK_SYS("error")) == nullptr);
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
