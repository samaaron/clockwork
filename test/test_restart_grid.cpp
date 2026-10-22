// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_restart_grid.cpp — docs/RESTARTS.md's model, reader by reader and
 * event by event, on a native engine.
 *
 * "One event, every route": however an instance is replaced, the old
 * generation closes and what was waiting for it is answered once; its last
 * words come before `setup`; `setup` is the new instance's first word; and
 * nothing a reader had not read is lost. A change the instance survives
 * replaces nothing: no refusal, no `setup`, the generation unmoved, and a
 * fact clients compute with that changed is told in a notice naming that
 * generation.
 *
 * Each case is one cell of the grid in "Where it is pinned": one reader —
 * the engine's own, a host reading the rings itself, a tap watching the
 * instance's ring, a peer on the segment's command plane — against one
 * event on fake devices. Every cell does the same thing around its event,
 * and RestartModel.h holds what the model says the reader heard. The
 * messages that wait are sent before the event, or, for a rebuild, between
 * the teardown and the build: sent while there is no instance.
 *
 * The devices keep the case's clock (FakeSystem::useVirtualTime) and the
 * case runs the control plane itself, so nothing renders or is read but
 * when the case says.
 */
#include "CallbackTransport.h"
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include "RestartModel.h"
#include "ShmTransport.h"
#include "clockwork_arena.h"
#include "clockwork_client.h"
#include "lanes/lanes.h"   // clockwork_arena_header: the generation word
#include "lanes/ring_drain.h"
#include "shm_peer_plane.h"
#include "shm_segment.hpp"
#include "workers/RingBufferWriter.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using fake_audio::FakeSystem;
using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;

namespace {

enum class Reader { Engine, HostRings, Tap, ShmPeer };

// When the messages that wait are sent: before the event, or between the
// old instance's teardown and the new one's build.
enum class When { Before, BetweenInstances };

enum class Event {
    RateChange,     // the user picks another rate: the instance does not survive it
    DriverChange,   // the user picks another driver
    OtherDevice,    // another device at the same rate: the instance survives it
    BufferChange,   // the same device with a bigger buffer: survived, and a fact changed
    RolledBack,     // the user picks a device that will not open
    SilentDevice,   // the user picks a device that opens and never calls back
};

const char* readerName(Reader r) {
    switch (r) {
    case Reader::Engine:    return "the engine's own reader";
    case Reader::HostRings: return "a host reading the rings";
    case Reader::Tap:       return "a tap on the instance's ring";
    case Reader::ShmPeer:   return "a peer on the segment's command plane";
    }
    return "?";
}

constexpr int kBiggerBuffer = 512;

// What a host serving a peer on the segment gives the engine: the peer's
// transport for the peer's token, and the engine's own callbacks beside it
// for every other — the fixture's reader among them.
class PeerBeside : public IOscTransport {
public:
    explicit PeerBeside(ClockworkEngine& e) : mOwn(&e.onReply, &e.onReplyRouted) {}
    ShmTransport peer;

    bool send(uint32_t t, const uint8_t* d, uint32_t n, bool net) override {
        return t == SHM_PEER_ORIGIN_TOKEN ? peer.send(t, d, n, net) : mOwn.send(t, d, n, net);
    }
    uint32_t broadcastNotify(const uint8_t* d, uint32_t n) override { return peer.broadcastNotify(d, n) + mOwn.broadcastNotify(d, n); }
    uint32_t broadcastLink(const uint8_t* d, uint32_t n) override { return peer.broadcastLink(d, n); }
    bool hasNotifySubscribers() const override { return peer.hasNotifySubscribers() || mOwn.hasNotifySubscribers(); }
    bool subscribeNotify(uint32_t t) override { return of(t).subscribeNotify(t); }
    bool subscribeNotifyPort(int) override { return false; }
    void unsubscribeNotify(uint32_t t) override { of(t).unsubscribeNotify(t); }
    void clearNotify() override { peer.clearNotify(); mOwn.clearNotify(); }
    bool subscribeLink(uint32_t t) override { return of(t).subscribeLink(t); }
    void unsubscribeLink(uint32_t t) override { of(t).unsubscribeLink(t); }
    uint32_t broadcastMidi(const uint8_t* d, uint32_t n) override { return peer.broadcastMidi(d, n) + mOwn.broadcastMidi(d, n); }
    bool subscribeMidi(uint32_t t) override { return of(t).subscribeMidi(t); }
    void unsubscribeMidi(uint32_t t) override { of(t).unsubscribeMidi(t); }
    uint32_t broadcastGamepad(const uint8_t* d, uint32_t n) override { return peer.broadcastGamepad(d, n) + mOwn.broadcastGamepad(d, n); }
    bool subscribeGamepad(uint32_t t) override { return of(t).subscribeGamepad(t); }
    void unsubscribeGamepad(uint32_t t) override { of(t).unsubscribeGamepad(t); }

private:
    IOscTransport& of(uint32_t t) { return t == SHM_PEER_ORIGIN_TOKEN ? static_cast<IOscTransport&>(peer) : mOwn; }
    CallbackTransport mOwn;
};

// A cell: an engine on fake devices, the reader, and what the reader heard.
class Cell {
public:
    Cell(Reader reader, Event event, unsigned port = 0,
         const std::function<void(ClockworkEngine::Config&)>& tweak = {})
        : mReader(reader), mEvent(event), mSys(makeSimpleSystem()) {
        if (event == Event::DriverChange) {
            auto other = std::make_shared<fake_audio::FakeDeviceSpec>();
            other->name = "Other Card";
            other->maxInputChannels = 0;
            mSys->types.push_back({ "OtherDriver", { other }, 0 });
        }
        auto cfg = fakeEngineConfig(mSys, "Fake Speakers");
        cfg.hostDrivesControl = true;
        cfg.hostDrainsEgress  = reader == Reader::HostRings;
        if (tweak) tweak(cfg);
        if (reader == Reader::ShmPeer) {
            REQUIRE(port != 0);
            cfg.udpPort     = port;   // the public segment, and its plane
            cfg.shmCommands = true;
            mFix = std::make_unique<EngineFixture>(cfg, [this](ClockworkEngine& e) {
                mTransport = std::make_unique<PeerBeside>(e);
                e.setTransport(mTransport.get());
            });
        } else {
            mFix = std::make_unique<EngineFixture>(cfg);
        }
        if (reader == Reader::Engine || reader == Reader::Tap) subscribe(*mFix);
        mFix->stopHostControl();
        if (reader == Reader::ShmPeer) attachPeer();
        mSys->useVirtualTime();

        // Everything the boot said is read and set aside.
        advance(20);
        if (reader == Reader::HostRings) readRings();
        mFix->clearReplies();
        if (reader == Reader::Tap) {
            ClockworkStatus st = CLOCKWORK_OK;
            mTap = clockwork_client_tap_open(engine().egressClient(), CLOCKWORK_REGION_EGRESS, &st);
            REQUIRE(mTap != nullptr);
        }
        mHeard.clear();
    }

    ~Cell() {
        if (mTap) clockwork_client_tap_close(mTap);
        mSys->useWallTime();
    }

    ClockworkEngine& engine() { return mFix->engine(); }
    EngineFixture& fix() { return *mFix; }
    FakeSystem& sys() { return *mSys; }
    Reader reader() const { return mReader; }

    // `ms` of the case's clock, a few milliseconds at a time, the control
    // plane passing after each step as a host's does.
    void advance(int ms) {
        for (int t = 0; t < ms; t += 5) {
            mSys->advanceBy(5000);
            engine().controlPass();
        }
    }

    // A message from the reader's client: through the engine's door, or
    // written into the plane's command ring, as a peer writes one.
    void send(const osc_test::Packet& p) {
        if (mReader != Reader::ShmPeer) { mFix->send(p); return; }
        REQUIRE(RingBufferWriter::write(shm_peer_cmd_ring(mPlane), SHM_PEER_CMD_RING_SIZE,
                                        &mPlane->cmd_head, &mPlane->cmd_tail,
                                        &mPlane->cmd_sequence, &mPlane->cmd_write_lock,
                                        p.ptr(), p.size(), 0));
    }
    // A peer's message reaches the engine when the control pass takes it
    // from the plane, as a host's pass does soon after it is written. Every
    // other door is the engine's at once.
    void delivered() {
        if (mReader == Reader::ShmPeer) engine().controlPass();
    }

    // What the reader has been given since the last read, appended in order.
    void read() {
        switch (mReader) {
        case Reader::ShmPeer:
            clockwork_drain_ring(
                shm_peer_rep_ring(mPlane), SHM_PEER_REP_RING_SIZE,
                &mPlane->rep_head, &mPlane->rep_tail, mRepState, ClockworkDrainMetrics{}, 0,
                [&](uint32_t, const uint8_t* d, uint32_t n, uint32_t) {
                    mHeard.push_back(osc_test::parseReply(d, n));
                    return ClockworkDrainVerdict::Consume;
                });
            break;
        case Reader::Engine:
            for (const auto& r : mFix->allReplies()) mHeard.push_back(osc_test::parseReply(r.raw.data(), static_cast<uint32_t>(r.raw.size())));
            mFix->clearReplies();
            break;
        case Reader::HostRings:
            readRings();
            break;
        case Reader::Tap: {
            std::array<ClockworkClientMessage, 64> batch{};
            for (;;) {
                const uint32_t n = clockwork_client_tap_poll(mTap, batch.data(), static_cast<uint32_t>(batch.size()));
                for (uint32_t i = 0; i < n; ++i) mHeard.push_back(osc_test::parseReply(batch[i].bytes, batch[i].length));
                if (n < batch.size()) break;
            }
            mFix->clearReplies();   // the engine's reader took them too
            break;
        }
        }
    }

    const std::vector<osc_test::ParsedReply>& heard() const { return mHeard; }
    ShmPeerPlaneHeader* plane() { return mPlane; }
    uint64_t tapMissed() const { return mTap ? clockwork_client_tap_missed(mTap) : 0; }

    // Something to do between the old instance's teardown and the new one's
    // build, on the swap's thread: as the rebuild opens its device.
    void betweenInstances(std::function<void()> act) {
        REQUIRE(mEvent == Event::RateChange);
        auto fired = std::make_shared<bool>(false);
        mSys->at("Fake Speakers").onOpen = [act, fired] {
            if (*fired) return;   // the first open: the rebuild's
            *fired = true;
            REQUIRE_FALSE(clockwork_arena_has_instance(clockwork_arena_header()));
            act();
        };
    }

    // The event itself.
    SwapResult happen() {
        switch (mEvent) {
        case Event::RateChange:
            return swap([&] { return engine().switchDevice("Fake Speakers", 44100); });
        case Event::DriverChange:
            return swap([&] { return engine().switchDriver("OtherDriver"); });
        case Event::OtherDevice:
            return swap([&] { return engine().switchDevice("Fake Interface", 48000); });
        case Event::BufferChange:
            return swap([&] { return engine().switchDevice("Fake Speakers", 48000, kBiggerBuffer); });
        case Event::RolledBack:
            mSys->at("Fake Interface").failOpen = true;
            return swap([&] { return engine().switchDevice("Fake Interface", 44100); });
        case Event::SilentDevice:
            // The swap waits for its first block (five seconds of the wall
            // clock), and goes back to the device that was playing.
            mSys->at("Fake Interface").failStart = true;
            return swap([&] { return engine().switchDevice("Fake Interface", 44100); });
        }
        return {};
    }

private:
    // Every open of every device so far, failed or not.
    int opens() {
        int n = 0;
        for (const auto& t : mSys->types)
            for (const auto& d : t.devices) n += d->opens.load();
        return n;
    }

    // A swap on a thread of its own, as the device lane runs one, with the
    // case's clock held until the swap opens a device — by then the old one
    // has stopped feeding the engine, so nothing waiting in the in ring is
    // taken by a block of the old instance's on the way — and moved on from
    // then, so the devices the swap opens deliver as it waits for them.
    template <typename Attempt>
    SwapResult swap(Attempt attempt) {
        const int before = opens();
        auto done = std::async(std::launch::async, [&] { return whenSwapGateFree(attempt); });
        const auto opening = [&] {
            return opens() > before
                || done.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        };
        while (!opening()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        while (done.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
            mSys->advanceBy(1000);
        return done.get();
    }

    // The peer attaches to the plane and subscribes, as a client does, and
    // hears its subscription answered.
    void attachPeer() {
        mTransport->peer.bindPlaneSlot(engine().peerPlaneSlot());
        REQUIRE(mTransport->peer.ready());
        mSegment = std::make_unique<shm_segment_client>(
            detail_shm_segment::shm_dup_handle(engine().shmNativeHandle()));
        mPlane = mSegment->get_peer_plane();
        REQUIRE(mPlane != nullptr);
        shm_peer_attach(mPlane, 1);
        send(osc_test::message(CLOCKWORK_SYS("notify")));
        for (int i = 0; i < 4; ++i) engine().controlPass();
        read();
        REQUIRE(restart_model::where(mHeard, CLOCKWORK_SYS("notify.reply")).size() == 1);
    }

    void readRings() {
        std::array<ClockworkClientMessage, 64> batch{};
        for (;;) {
            const uint32_t n = clockwork_client_poll(engine().egressClient(), batch.data(), static_cast<uint32_t>(batch.size()));
            for (uint32_t i = 0; i < n; ++i) mHeard.push_back(osc_test::parseReply(batch[i].bytes, batch[i].length));
            if (n < batch.size()) return;
        }
    }

    Reader                              mReader;
    Event                               mEvent;
    std::shared_ptr<FakeSystem>         mSys;
    std::unique_ptr<PeerBeside>         mTransport;   // outlives the engine that sends through it
    std::unique_ptr<EngineFixture>      mFix;
    std::unique_ptr<shm_segment_client> mSegment;
    ShmPeerPlaneHeader*                 mPlane = nullptr;
    ClockworkDrainState                 mRepState;
    ClockworkClientTap*                 mTap = nullptr;
    std::vector<osc_test::ParsedReply>  mHeard;
};

// ── The cell's run ──────────────────────────────────────────────────────────

struct Run {
    uint32_t   before = 0, after = 0;   // the generation either side
    SwapResult swap;
};

Run runCell(Cell& cell, When when) {
    cell.send(restart_model::oldAsk());
    cell.delivered();
    cell.sys().advanceBy(20000);   // answered by the old instance, and not read

    std::memset(const_cast<uint8_t*>(cell.engine().guestAssets()), 0x5A, 64);
    const auto sendWaiting = [&cell] {
        for (const auto& ask : restart_model::waitingAsks()) cell.send(ask);
        cell.delivered();
    };
    if (when == When::Before) sendWaiting();
    else                      cell.betweenInstances(sendWaiting);

    Run run;
    run.before = cell.engine().generation();
    run.swap   = cell.happen();
    run.after  = cell.engine().generation();

    cell.send(restart_model::newAsk());
    cell.delivered();
    cell.advance(100);
    cell.read();
    return run;
}

void checkCell(Reader reader, Event event, When when, unsigned port) {
    Cell cell(reader, event, port);
    const Run run = runCell(cell, when);
    INFO(cell.fix().debugMessagesDump());
    const bool seesRefusals = reader != Reader::Tap;   // they go on clockwork's ring
    const auto replaced = [&] {
        CHECK(clockwork_arena_has_instance(clockwork_arena_header()));
        restart_model::checkReplaced(cell.heard(), readerName(reader), seesRefusals, run.before, run.after,
                                     when == When::Before ? CLOCKWORK_REFUSED_REBUILDING
                                                          : CLOCKWORK_REFUSED_NO_INSTANCE);
    };
    const auto survived = [&](int changedBuffer) {
        restart_model::checkSurvived(cell.heard(), readerName(reader), run.before, run.after, 48000, changedBuffer);
    };
    switch (event) {
    case Event::RateChange:
    case Event::DriverChange:
        CHECK(run.swap.success);
        CHECK(run.swap.type == SwapType::Cold);
        replaced();
        break;
    case Event::OtherDevice:
        REQUIRE(run.swap.success);
        REQUIRE(run.swap.type == SwapType::Hot);
        survived(0);
        break;
    case Event::BufferChange:
        REQUIRE(run.swap.success);
        REQUIRE(run.swap.type == SwapType::Hot);
        survived(kBiggerBuffer);
        break;
    case Event::RolledBack:
    case Event::SilentDevice:
        // Whether a swap that fails replaced the instance on its way back is
        // the engine's; what it said is the model's either way.
        if (run.after == run.before) survived(0);
        else                         replaced();
        break;
    }
    // A tap is never lapped here: nothing a reader had not read is lost.
    CHECK(cell.tapMissed() == 0u);
}

} // namespace

#define GRID_CELL(reader, event, when, port, title)                                              \
    TEST_CASE("restart grid: " title, "[restarts][grid]") { checkCell(reader, event, when, port); }

GRID_CELL(Reader::Engine,    Event::RateChange,   When::Before, 0, "the engine's own reader at a rate change")
GRID_CELL(Reader::HostRings, Event::RateChange,   When::Before, 0, "a host reading the rings at a rate change")
GRID_CELL(Reader::Tap,       Event::RateChange,   When::Before, 0, "a tap at a rate change")
GRID_CELL(Reader::Engine,    Event::DriverChange, When::Before, 0, "the engine's own reader at a driver change")
GRID_CELL(Reader::HostRings, Event::DriverChange, When::Before, 0, "a host reading the rings at a driver change")
GRID_CELL(Reader::Tap,       Event::DriverChange, When::Before, 0, "a tap at a driver change")
GRID_CELL(Reader::Engine,    Event::OtherDevice,  When::Before, 0, "the engine's own reader at a device the instance survives")
GRID_CELL(Reader::HostRings, Event::OtherDevice,  When::Before, 0, "a host reading the rings at a device the instance survives")
GRID_CELL(Reader::Tap,       Event::OtherDevice,  When::Before, 0, "a tap at a device the instance survives")
GRID_CELL(Reader::Engine,    Event::BufferChange, When::Before, 0, "the engine's own reader at a bigger buffer")
GRID_CELL(Reader::HostRings, Event::BufferChange, When::Before, 0, "a host reading the rings at a bigger buffer")
GRID_CELL(Reader::Tap,       Event::BufferChange, When::Before, 0, "a tap at a bigger buffer")
GRID_CELL(Reader::Engine,    Event::RolledBack,   When::Before, 0, "the engine's own reader at a device that will not open")
GRID_CELL(Reader::HostRings, Event::RolledBack,   When::Before, 0, "a host reading the rings at a device that will not open")
GRID_CELL(Reader::Tap,       Event::RolledBack,   When::Before, 0, "a tap at a device that will not open")
GRID_CELL(Reader::Engine,    Event::SilentDevice, When::Before, 0, "the engine's own reader at a device that never calls back")
GRID_CELL(Reader::HostRings, Event::SilentDevice, When::Before, 0, "a host reading the rings at a device that never calls back")
GRID_CELL(Reader::Tap,       Event::SilentDevice, When::Before, 0, "a tap at a device that never calls back")

// The segment's peer, and what was sent between instances (a rebuild only:
// the instance is down between its teardown and the next build).
GRID_CELL(Reader::ShmPeer,   Event::RateChange,  When::Before,           57250, "a peer on the plane at a rate change")
GRID_CELL(Reader::ShmPeer,   Event::OtherDevice, When::Before,           57251, "a peer on the plane at a device the instance survives")
GRID_CELL(Reader::Engine,    Event::RateChange,  When::BetweenInstances, 0,     "the engine's own reader sending between instances")
GRID_CELL(Reader::HostRings, Event::RateChange,  When::BetweenInstances, 0,     "a host reading the rings sending between instances")
GRID_CELL(Reader::ShmPeer,   Event::RateChange,  When::BetweenInstances, 57252, "a peer on the plane sending between instances")

// A peer that is behind: its reply ring all but full of the old instance's
// answers when the rebuild comes. It loses none of them, and hears setup
// after them once it reads.
TEST_CASE("restart grid: a peer whose reply ring is nearly full when the rebuild comes", "[restarts][grid]") {
    Cell cell(Reader::ShmPeer, Event::RateChange, 57253);
    ShmPeerPlaneHeader* plane = cell.plane();
    const auto used = [plane] {
        const int32_t head = plane->rep_head.load(), tail = plane->rep_tail.load();
        return static_cast<uint32_t>((head - tail + static_cast<int32_t>(SHM_PEER_REP_RING_SIZE))
                                     % static_cast<int32_t>(SHM_PEER_REP_RING_SIZE));
    };
    // Pings, answered and not read, a few at a time near the top, until
    // fewer than a kilobyte is free.
    int pings = 0;
    while (used() + 1024 < SHM_PEER_REP_RING_SIZE) {
        // Fewer than a pass takes from the plane (256 frames).
        const uint32_t batch = std::clamp<uint32_t>((SHM_PEER_REP_RING_SIZE - 1024 - used()) / 64, 1, 128);
        for (uint32_t i = 0; i < batch; ++i, ++pings) cell.send(restart_model::oldAsk());
        cell.delivered();
        cell.sys().advanceBy(5000);
        cell.engine().controlPass();
        REQUIRE(plane->rep_dropped.load() == 0);
    }

    const uint32_t before = cell.engine().generation();
    const auto swap = cell.happen();
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);

    // The peer catches up, until the plane has nothing more for it.
    for (size_t was = SIZE_MAX; was != cell.heard().size()
         || plane->cmd_head.load() != plane->cmd_tail.load();) {
        was = cell.heard().size();
        cell.read();
        cell.advance(20);
        cell.read();
    }
    const auto& heard = cell.heard();
    // Every ping answered once: by the old instance, or refused as it went.
    const size_t answered = restart_model::where(heard, "/dummy/pong").size();
    const size_t refused  = restart_model::refusalsOf(heard, "/dummy/ping").size();
    INFO(answered << " answers and " << refused << " refusals heard of " << pings << " pings;"
         << " the plane dropped " << plane->rep_dropped.load());
    CHECK(plane->rep_dropped.load() == 0);
    CHECK(answered + refused == static_cast<size_t>(pings));
    const auto setups = restart_model::where(heard, restart_model::kSetup);
    REQUIRE(setups.size() == 1);
    CHECK(static_cast<uint32_t>(heard[static_cast<size_t>(setups[0])].argInt(2)) == before + 1);
    CHECK(restart_model::where(heard, "/dummy/pong").back() < setups[0]);
}


// A peer that writes while no instance comes up — a rebuild whose guest would
// not start, so the engine waits in error: each message for the guest is
// refused once, and the plane moves on past it to what clockwork answers.
TEST_CASE("restart grid: a peer writing while no instance comes up is refused once a message, and the plane moves on",
          "[restarts][grid]") {
    // A guest that builds now and not after the rebuild: its pool asks for
    // more than the bulk memory the rebuild hands it (test_cold_swap_recovery.cpp).
    const size_t pool = size_t(CLOCKWORK_GUEST_MEMORY_BULK_BYTES) + 1024u * 1024u;
    clockwork_set_guest_bulk_bytes(pool + (8u << 20));
    Cell cell(Reader::ShmPeer, Event::RateChange, 57254, [pool](ClockworkEngine::Config& cfg) {
        cfg.guestConfig = "rtPoolBytes=" + std::to_string(pool) + "\ndeclareBulk=0\n";
    });
    clockwork_set_guest_bulk_bytes(0);
    cell.happen();
    REQUIRE(cell.engine().engineState() == EngineState::Error);
    REQUIRE_FALSE(clockwork_arena_has_instance(clockwork_arena_header()));
    cell.read();   // what the failed rebuild said

    constexpr int kAsks = 3;
    for (int i = 0; i < kAsks; ++i) cell.send(restart_model::oldAsk());
    cell.send(osc_test::message(CLOCKWORK_SYS("sync"), restart_model::kSyncId));
    for (int i = 0; i < 8; ++i) cell.advance(5);
    cell.read();

    const auto& heard = cell.heard();
    INFO("the peer heard:" << restart_model::dump(heard));
    const auto refused = restart_model::refusalsOf(heard, "/dummy/ping");
    CHECK(refused.size() == static_cast<size_t>(kAsks));
    for (const auto& r : refused) CHECK(r.argString(1) == CLOCKWORK_REFUSED_NO_INSTANCE);
    CHECK(restart_model::where(heard, restart_model::kSynced).size() == 1);
    ShmPeerPlaneHeader* plane = cell.plane();
    CHECK(plane->cmd_head.load() == plane->cmd_tail.load());
}

// More waiting at the teardown than the refusals of it fill on clockwork's
// ring: every one is still refused, once.
void manyWaiting(Reader reader) {
    Cell cell(reader, Event::RateChange);
    // Four thousand, or as many as the in ring holds (the smallest profile's
    // holds about a thousand): far more than the refusals of them fill on a
    // reply ring either way.
    int waiting = 0;
    const auto ping = restart_model::oldAsk();
    while (waiting < 4000 && cell.engine().ingest(ping.ptr(), ping.size(), 0) == CLOCKWORK_OK) ++waiting;
    REQUIRE(waiting > 500);

    const uint32_t before = cell.engine().generation();
    const auto swap = cell.happen();
    REQUIRE(swap.success);
    REQUIRE(swap.type == SwapType::Cold);
    // A block refuses a few dozen; as long as it takes, within a bound only
    // a refusal lost reaches.
    for (int ms = 0; ms < 2000
         && restart_model::refusalsOf(cell.heard(), "/dummy/ping").size() < static_cast<size_t>(waiting);
         ms += 20) {
        cell.advance(20);
        cell.read();
    }

    const auto& heard = cell.heard();
    const auto refused = restart_model::refusalsOf(heard, "/dummy/ping");
    INFO(refused.size() << " of " << waiting << " refused; "
         << restart_model::where(heard, "/dummy/pong").size() << " answered");
    CHECK(refused.size() == static_cast<size_t>(waiting));
    for (const auto& r : refused) {
        CHECK(r.argString(1) == CLOCKWORK_REFUSED_REBUILDING);
        CHECK(static_cast<uint32_t>(r.argInt(2)) == before);
        if (r.argString(1) != CLOCKWORK_REFUSED_REBUILDING) break;
    }
}

TEST_CASE("restart grid: the engine's own reader with thousands waiting at a rebuild", "[restarts][grid]") {
    manyWaiting(Reader::Engine);
}

TEST_CASE("restart grid: a host reading the rings with thousands waiting at a rebuild", "[restarts][grid]") {
    manyWaiting(Reader::HostRings);
}
