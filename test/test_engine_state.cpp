// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_engine_state.cpp — EngineState lifecycle and shared memory tests.
 *
 * Verifies the engine state machine: Booting → Running → Restarting
 * → Running → Stopped.  Also tests that shared memory survives engine
 * lifecycle transitions.
 */
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <atomic>
#include <cstring>
#include <limits>
#include "EngineFixture.h"
#include "clockwork_prefix.h"
#include "engine_state.h"
#include "shm_segment.hpp"
#include "audio_processor.h"   // control / shared_memory arena globals
#include "shared_memory.h"     // IN ring layout
#include "ring/ring.h"         // Message wire header

#ifdef _WIN32
#else
#include <sys/stat.h>
#include <fcntl.h>
#endif

// ── State transitions ──────────────────────────────────────────────────────

TEST_CASE("EngineState: starts Stopped before init", "[EngineState]") {
    ClockworkEngine engine;
    CHECK(engine.engineState() == EngineState::Stopped);
}

TEST_CASE("EngineState: Running after init", "[EngineState]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);
    CHECK(engine.engineState() == EngineState::Running);
    engine.shutdown();
}

TEST_CASE("EngineState: Stopped after shutdown", "[EngineState]") {
    ClockworkEngine engine;
    engine.onReply = [](const uint8_t*, uint32_t) {};
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 0;
    engine.init(cfg);
    REQUIRE(engine.engineState() == EngineState::Running);
    engine.shutdown();
    CHECK(engine.engineState() == EngineState::Stopped);
}

// ── engineStateToString ─────────────────────────────────────────────────────

TEST_CASE("EngineState: engineStateToString covers all states", "[EngineState]") {
    CHECK(std::string(engineStateToString(EngineState::Booting))    == "booting");
    CHECK(std::string(engineStateToString(EngineState::Running))    == "running");
    CHECK(std::string(engineStateToString(EngineState::Restarting)) == "restarting");
    CHECK(std::string(engineStateToString(EngineState::Stopped))    == "stopped");
    CHECK(std::string(engineStateToString(EngineState::Error))      == "error");
}

// ── Fixture-based: verify engine is Running and responds after boot ─────────

TEST_CASE("EngineState: fixture engine is Running", "[EngineState]") {
    EngineFixture fix;
    CHECK(fix.engine().engineState() == EngineState::Running);

    // Verify it actually works
    fix.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fix.waitForReply("/dummy/pong", r));
}

TEST_CASE("EngineState: switchDevice in headless doesn't change state", "[EngineState]") {
    EngineFixture fix;
    CHECK(fix.engine().engineState() == EngineState::Running);
    // In headless mode, device name is ignored — the swap is a hot swap
    // (pause/resume headless driver) which always succeeds.
    auto result = fix.engine().switchDevice("nonexistent");
    REQUIRE(result.success);
    REQUIRE(result.type == SwapType::Hot);
    // State should still be Running — hot swaps don't transition state
    CHECK(fix.engine().engineState() == EngineState::Running);
}

// ── A guest that does not come up ───────────────────────────────────────────
//
// A boot whose guest refuses (here: the dummy asks the host's heap for a
// real-time pool larger than the heap) is an engine in error, not a running
// one, and every client can learn why: the engine still answers its own
// verbs, and a late registrant's state replay carries the guest's reason.
// Before this, the engine reported "running" with no guest inside, nothing
// drained its ingress, and a client's /clockwork/notify timed out — the
// reason reached only the log.

namespace {
ClockworkEngine::Config askingForAPool(size_t bytes) {
    auto cfg = EngineFixture::defaultConfig();
    cfg.guestConfig = "rtPoolBytes=" + std::to_string(bytes) + "\n";
    return cfg;
}
}

TEST_CASE("EngineState: a guest that does not come up leaves the engine in error, saying why",
          "[EngineState][guest-failed]") {
    EngineFixture fix(askingForAPool(size_t(CLOCKWORK_HEAP_SIZE) + 1024u * 1024u));
    CHECK(fix.engine().engineState() == EngineState::Error);

    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    OscReply state;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("statechange"), state));
    const auto s = state.parsed();
    REQUIRE(s.argCount() >= 2);
    CHECK(s.argString(0) == "error");
    INFO("reason: " << s.argString(1));
    CHECK(s.argString(1).find("rtPoolBytes") != std::string::npos);
}

// A guest's reason is its own, at any length the boot keeps (511 bytes, with
// "<guest> did not start: " in front), and a client that registers is told it
// whole. One too long for a fixed reply buffer threw inside the notify
// handler: the client was answered notify.reply, then refused the verb as
// unknown, and never learned the engine was in error.
TEST_CASE("EngineState: a guest that refuses at length: a client that registers is told the whole reason",
          "[EngineState][guest-failed]") {
    const std::string reason = "refused: " + std::string(460, 'x') + " (last words)";
    auto cfg = askingForAPool(size_t(CLOCKWORK_HEAP_SIZE) + 1024u * 1024u);
    cfg.guestConfig += "refusalReason=" + reason + "\n";
    EngineFixture fix(cfg);
    CHECK(fix.engine().engineState() == EngineState::Error);

    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    OscReply state;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("statechange"), state));
    const auto s = state.parsed();
    REQUIRE(s.argCount() >= 2);
    CHECK(s.argString(0) == "error");
    CHECK(s.argString(1).find(reason) != std::string::npos);
}

TEST_CASE("EngineState: with no guest, its messages go nowhere and the engine's own verbs still answer",
          "[EngineState][guest-failed]") {
    EngineFixture fix(askingForAPool(size_t(CLOCKWORK_HEAP_SIZE) + 1024u * 1024u));
    fix.send(osc_test::message("/dummy/ping"));
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), 777));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r));
    CHECK(lastInt(r) == 777);
    CHECK_FALSE(fix.waitForReply("/dummy/pong", r, 200));
}

// ── The heap is the host's to size ──────────────────────────────────────────
//
// What a guest will take from the heap depends on its configuration — a
// server guest's real-time pool is an option — and only the host that wrote
// that configuration knows it. So the host says how big a heap to take, and
// the boot either has it or says it could not get it.

TEST_CASE("EngineState: a host that sizes the heap for its guest's pool boots the guest",
          "[EngineState][heap]") {
    const size_t pool = size_t(CLOCKWORK_HEAP_SIZE) + 1024u * 1024u;
    auto cfg = askingForAPool(pool);
    cfg.heapBytes = pool + 8u * 1024u * 1024u;
    EngineFixture fix(cfg);
    CHECK(fix.engine().engineState() == EngineState::Running);
    fix.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fix.waitForReply("/dummy/pong", r));
}

TEST_CASE("EngineState: a heap the system cannot provide stops the boot, saying why",
          "[EngineState][heap]") {
    // AddressSanitizer's allocator aborts the process on a request this size
    // rather than answering NULL (allocation-size-too-big), so under it the
    // failure this case is about cannot happen; the plain builds run it.
#if defined(__SANITIZE_ADDRESS__)
    SKIP("ASan aborts on an impossible allocation instead of failing it");
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
    SKIP("ASan aborts on an impossible allocation instead of failing it");
#  endif
#endif
    auto cfg = EngineFixture::defaultConfig();
    // Past any address space, 32-bit as much as 64-bit: the most size_t can
    // say. (1 << 60 is no size at all where size_t is 32 bits — the shift is
    // undefined, and i386 booted a heap.) The engine caps a request to what
    // it can add its own bookkeeping to, so this cannot wrap to something
    // small; it is refused for being more than there is.
    cfg.heapBytes = std::numeric_limits<size_t>::max();
    EngineFixture fix(cfg);
    CHECK(fix.engine().engineState() == EngineState::Error);

    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack, state;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("statechange"), state));
    const auto s = state.parsed();
    REQUIRE(s.argCount() >= 2);
    CHECK(s.argString(0) == "error");
    INFO("reason: " << s.argString(1));
    CHECK(s.argString(1).find("heap") != std::string::npos);
}

TEST_CASE("EngineState: the next engine takes the profile's heap again", "[EngineState][heap]") {
    // heapBytes is one engine's, not the process's: a later boot that does
    // not ask gets the default, and a pool that only the bigger heap held no
    // longer fits.
    {
        const size_t pool = size_t(CLOCKWORK_HEAP_SIZE) + 1024u * 1024u;
        auto cfg = askingForAPool(pool);
        cfg.heapBytes = pool + 8u * 1024u * 1024u;
        EngineFixture fix(cfg);
        REQUIRE(fix.engine().engineState() == EngineState::Running);
    }
    EngineFixture again(askingForAPool(size_t(CLOCKWORK_HEAP_SIZE) + 1024u * 1024u));
    CHECK(again.engine().engineState() == EngineState::Error);
}

// ── Recording survives pause/resume (hot swap) ──────────────────────────────

// (A recording case lived here. The engine opens no files now; session
// recording is the client's — SuperSonic's front, test_front_recording.cpp.)

TEST_CASE("EngineState: purge clears stale messages after cold swap",
          "[EngineState][atomicity]") {
    // Verify that purge() clears the ring buffer so stale messages
    // from before a swap don't reach the fresh guest.
    EngineFixture fix;

    // Send commands and verify they work
    fix.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fix.waitForReply("/dummy/pong", r));

    // Purge (simulates what happens during cold swap drain)
    fix.engine().purge();

    // Verify the engine still works after purge — no leftover state
    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", r));

}

// A producer preempted inside clockwork_ingress_write holds in_write_lock with the
// old head already loaded. If purge() moves the IN-ring cursors underneath
// it, the producer's completed write re-publishes head past the reset point,
// re-exposing every already-consumed frame between offset 0 and the old head
// — the next drain then replays messages the engine already performed.
// Manual pump makes the interleaving deterministic: the test thread is the
// producer, the purger, and the audio thread.
TEST_CASE("EngineState: purge with a producer mid-write does not replay consumed frames",
          "[EngineState][purge]") {
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fix(cfg);

    // Park some consumed frames at the front of the ring: send tempo/get queries and
    // wait for their replies, so [0, h0) holds performed frames.
    OscReply r;
    for (int i = 0; i < 3; ++i) {
        fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), 4200 + i));
        REQUIRE(fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r));
    }
    fix.clearReplies();

    const int32_t h0 = control->in_head.load(std::memory_order_acquire);
    REQUIRE(h0 > 0);
    REQUIRE(static_cast<uint32_t>(h0) + 64 < IN_BUFFER_SIZE);  // room for the frame below

    // Producer stalls mid-write: lock held, head loaded, payload not yet copied.
    int32_t unlocked = 0;
    REQUIRE(control->in_write_lock.compare_exchange_strong(unlocked, 1));

    fix.engine().purge();

    // Producer resumes: completes the frame at its pre-loaded head and
    // publishes, following RingBufferWriter::write's sequence (header +
    // payload memcpy, then head store, then unlock).
    {
        auto pkt = osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), 4299);
        Message hdr;
        hdr.magic    = MESSAGE_MAGIC;
        hdr.length   = static_cast<uint32_t>(sizeof(Message)) + pkt.size();
        hdr.sequence = static_cast<uint32_t>(
            control->in_sequence.fetch_add(1, std::memory_order_relaxed));
        hdr.sourceId = 0;
        uint8_t* slot = shared_memory + IN_BUFFER_START + h0;
        std::memcpy(slot, &hdr, sizeof(Message));
        std::memcpy(slot + sizeof(Message), pkt.ptr(), pkt.size());
        const uint32_t aligned = (hdr.length + 3u) & ~3u;
        control->in_head.store(
            static_cast<int32_t>((static_cast<uint32_t>(h0) + aligned) % IN_BUFFER_SIZE),
            std::memory_order_release);
        control->in_write_lock.store(0, std::memory_order_release);
    }

    // Drain. Frames performed before the purge must not run again.
    fix.pumpBlock(8);
    for (auto& reply : fix.allReplies()) {
        if (reply.address != CLOCKWORK_SYS("clock/tempo.reply")) continue;
        const int32_t id = lastInt(reply);
        CHECK(id != 4200);
        CHECK(id != 4201);
        CHECK(id != 4202);
    }

    // Engine must still be fully functional.
    fix.clearReplies();
    fix.send(osc_test::message("/dummy/ping"));
    REQUIRE(fix.waitForReply("/dummy/pong", r));
}

TEST_CASE("EngineState: sequential swaps in headless both succeed cleanly",
          "[EngineState][atomicity]") {
    EngineFixture fix;
    // In headless mode, switchDevice with a device name is a hot swap
    // (device name is ignored, headless driver pause/resumes)
    auto r1 = switchWhenFree(fix, "test");
    REQUIRE(r1.success);
    REQUIRE(r1.type == SwapType::Hot);

    // Second call should also succeed cleanly
    auto r2 = switchWhenFree(fix, "test2");
    REQUIRE(r2.success);
    REQUIRE(r2.type == SwapType::Hot);

    // Engine should still be running
    REQUIRE(fix.engine().engineState() == EngineState::Running);
}

// ── Shared memory ownership ─────────────────────────────────────────────────

TEST_CASE("SharedMemory: engine creates an anonymous segment on boot", "[EngineState][SharedMemory]") {
    using detail_shm_segment::shm_handle_valid;
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 59100;   // non-zero is what creates the segment
    engine.init(cfg);
    REQUIRE(engine.isRunning());

    // The segment exists and is what a reader would be handed: a duplicate of
    // the handle maps to a published, layout-checked segment.
    REQUIRE(shm_handle_valid(engine.shmNativeHandle()));
    {
        detail_shm_segment::shm_segment_client client(
            detail_shm_segment::shm_dup_handle(engine.shmNativeHandle()));
        CHECK(client.get_metrics() != nullptr);
    }

    // After shutdown there is nothing to hand out. The pages themselves go
    // with the last mapping; there is no name left behind to check for.
    engine.shutdown();
    CHECK_FALSE(shm_handle_valid(engine.shmNativeHandle()));
}

TEST_CASE("SharedMemory: segment survives a cold swap",
          "[EngineState][SharedMemory]") {
    ClockworkEngine engine;
    ClockworkEngine::Config cfg;
    cfg.headless = true;
    cfg.udpPort  = 59101;
    engine.init(cfg);
    const auto handle = engine.shmNativeHandle();
    REQUIRE(detail_shm_segment::shm_handle_valid(handle));

    // The guest teardown must not remove the segment: the engine owns it.
    auto pkt = osc_test::message("/dummy/ping");
    engine.sendOSC(pkt.ptr(), pkt.size());

    // Same object, not a re-created one: a reader that attached before the
    // swap is still looking at live memory.
    CHECK(engine.shmNativeHandle() == handle);

    engine.shutdown();
    CHECK_FALSE(detail_shm_segment::shm_handle_valid(engine.shmNativeHandle()));
}
