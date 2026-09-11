// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_plugin_bridge.cpp — the plugin bridge, as a process.
 *
 * These cases stand where the engine stands: they create the shared segment
 * and the doorbell, open the two ports over it, spawn the REAL bridge binary
 * (built by this tree, found by CLOCKWORK_PLUGIN_BRIDGE_EXE) and drive it exactly
 * as TrackControl does — verbs on the control ring, events on the realtime
 * ring, a block on the send port and a post of the bell — then read what
 * comes back on the return port and the out ring. Nothing here is mocked:
 * a green run means a second process rendered a plugin over shared memory
 * and answered over it, and a bridge killed with SIGKILL came back with its
 * tracks.
 *
 * What is NOT here is the engine's half (TrackControl): the respawn policy,
 * the liveness watch and the port bindings need a running ClockworkEngine, and
 * the standalone host exercises those. This file is the wire contract in
 * src/plugin_bridge.h, from the engine's side of it.
 */
#include "plugin_bridge.h"
#include "shm_segment.hpp"
#include "clockwork_ports.h"
#include "clockwork_sys.h"
#include "shared_memory.h"
#include "lanes/ring_drain.h"
#include "workers/RingBufferWriter.h"
#include "native/clockwork_process.h"
#include "native/TrackControl.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <functional>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

using namespace clockwork_bridge;

namespace {

constexpr uint32_t kBlock = 128;
constexpr double   kRate  = 48000.0;
constexpr uint32_t kSlack = 2;

int ownPid() {
#if defined(_WIN32)
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

// Waits up to `ms` for `pred`, polling gently. False on timeout.
bool waitFor(const std::function<bool()>& pred, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

// A received OSC message, decoded enough to assert on.
struct Msg {
    uint32_t route = 0;
    uint32_t token = 0;
    uint32_t len = 0;      // the whole frame, [route][osc], as the out ring held it
    std::string addr;
    std::vector<std::string> strs;
    std::vector<int32_t> ints;
    std::vector<float> floats;
};

// The engine's side of the segment, in one object. Mirrors what
// TrackControl::init does, minus the engine it does it for.
struct Engine {
    detail_shm_segment::shm_handle seg{};
    Header*  h = nullptr;
    Doorbell bell;
    ClockworkPort  send = CLOCKWORK_PORT_NONE;
    ClockworkPort  ret  = CLOCKWORK_PORT_NONE;
    ClockworkProcess proc;
    ClockworkDrainState outDrain;
    std::vector<Msg> inbox;
    // Planar scratch for a block each way.
    std::vector<float> sendBuf, retBuf;
    std::vector<float*> sendPtrs, retPtrs;

    // The slack the header is made with and the return is primed to.
    uint32_t slack = kSlack;
    explicit Engine(uint32_t slack_ = kSlack) : slack(slack_) {
        const std::string name = segment_name(ownPid());
        detail_shm_segment::shm_remove(name);
        seg = detail_shm_segment::shm_create(name, SEGMENT_SIZE);
        h = header(seg.ptr);
        init_header(h, kRate, kBlock, 0, ownPid(), slack);
        REQUIRE(bell.create(doorbell_name(ownPid())));
        openPorts(true);
        sendBuf.assign(size_t(LANES) * MAX_BLOCK, 0.0f);
        retBuf.assign(size_t(LANES) * MAX_BLOCK, 0.0f);
        for (uint32_t l = 0; l < LANES; ++l) {
            sendPtrs.push_back(sendBuf.data() + size_t(l) * MAX_BLOCK);
            retPtrs.push_back(retBuf.data() + size_t(l) * MAX_BLOCK);
        }
    }
    ~Engine() {
        stop();
        closePorts();
        bell.close();
        detail_shm_segment::shm_close(seg);
        detail_shm_segment::shm_remove(segment_name(ownPid()));
    }

    void openPorts(bool reset) {
        closePorts();
        send = clockwork_port_open_shared("test/send", kClockworkPortSink, LANES, AUDIO_RING_FRAMES,
                                    send_ring(seg.ptr), AUDIO_RING_BYTES, reset ? 1 : 0);
        ret = clockwork_port_open_shared("test/return", kClockworkPortSource, LANES, AUDIO_RING_FRAMES,
                                   return_ring(seg.ptr), AUDIO_RING_BYTES, reset ? 1 : 0);
        REQUIRE(send != CLOCKWORK_PORT_NONE);
        REQUIRE(ret != CLOCKWORK_PORT_NONE);
        // slack blocks of silence, as the engine primes it
        std::vector<float> zeros(size_t(kBlock) * LANES, 0.0f);
        for (uint32_t i = 0; i < slack; ++i) clockwork_port_produce(ret, zeros.data(), kBlock);
    }
    void closePorts() {
        if (send != CLOCKWORK_PORT_NONE) { clockwork_port_close(send); send = CLOCKWORK_PORT_NONE; }
        if (ret != CLOCKWORK_PORT_NONE)  { clockwork_port_close(ret);  ret = CLOCKWORK_PORT_NONE; }
    }

    // Spawn, and wait for the join.
    bool spawn(bool restore) {
        h->bridge_ready.store(0, std::memory_order_release);
        h->bridge_pid.store(0, std::memory_order_release);
        h->quit.store(0, std::memory_order_release);
        h->restore.store(restore ? 1 : 0, std::memory_order_release);
        h->generation.fetch_add(1, std::memory_order_acq_rel);
        if (!proc.spawn(CLOCKWORK_PLUGIN_BRIDGE_EXE, {std::to_string(ownPid())})) return false;
        return waitFor([&] { return h->bridge_ready.load(std::memory_order_acquire) != 0; }, 15000);
    }
    void stop() {
        if (!proc.running()) return;
        h->quit.store(1, std::memory_order_release);
        bell.post();
        waitFor([&] { return !proc.alive(); }, 3000);
        if (proc.alive()) proc.kill();
        proc.wait();
    }

    // A verb on the control ring, with a token to find the reply by.
    void control(uint32_t token, const std::function<void(osc::OutboundPacketStream&)>& fill) {
        char buf[2048];
        osc::OutboundPacketStream s(buf, sizeof buf);
        fill(s);
        MsgRing& r = h->ctl;
        REQUIRE(RingBufferWriter::write(ctl_ring(seg.ptr), CTL_RING_SIZE, &r.head, &r.tail,
                                        &r.sequence, &r.write_lock,
                                        reinterpret_cast<const uint8_t*>(s.Data()),
                                        static_cast<uint32_t>(s.Size()), token));
    }
    // An event on the realtime ring: [frame offset][osc].
    void realtime(uint32_t frameOffset, const std::function<void(osc::OutboundPacketStream&)>& fill) {
        char buf[2048];
        std::memcpy(buf, &frameOffset, sizeof frameOffset);
        osc::OutboundPacketStream s(buf + sizeof(uint32_t), sizeof buf - sizeof(uint32_t));
        fill(s);
        MsgRing& r = h->rt;
        REQUIRE(RingBufferWriter::write(rt_ring(seg.ptr), RT_RING_SIZE, &r.head, &r.tail,
                                        &r.sequence, &r.write_lock,
                                        reinterpret_cast<const uint8_t*>(buf),
                                        static_cast<uint32_t>(sizeof(uint32_t) + s.Size()), 1));
    }

    // Everything the bridge has put on the out ring, decoded into the inbox.
    void drain() {
        MsgRing& r = h->out;
        clockwork_drain_ring(out_ring(seg.ptr), OUT_RING_SIZE, &r.head, &r.tail, outDrain, ClockworkDrainMetrics{}, 0,
            [&](uint32_t token, const uint8_t* payload, uint32_t len, uint32_t) {
                if (len <= sizeof(uint32_t)) return ClockworkDrainVerdict::Consume;
                Msg m;
                m.token = token;
                m.len = len;
                std::memcpy(&m.route, payload, sizeof m.route);
                try {
                    osc::ReceivedMessage msg(osc::ReceivedPacket(
                        reinterpret_cast<const char*>(payload + 4),
                        static_cast<osc::osc_bundle_element_size_t>(len - 4)));
                    m.addr = msg.AddressPattern();
                    for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
                        if (it->IsString()) m.strs.push_back(it->AsStringUnchecked());
                        else if (it->IsInt32()) m.ints.push_back(it->AsInt32Unchecked());
                        else if (it->IsFloat()) m.floats.push_back(it->AsFloatUnchecked());
                    }
                } catch (...) {}
                inbox.push_back(std::move(m));
                return ClockworkDrainVerdict::Consume;
            });
    }
    // The first message at `addr` with `token`, waiting for it.
    bool await(const char* addr, uint32_t token, Msg& out, int ms = 10000) {
        return waitFor([&] {
            drain();
            for (auto& m : inbox)
                if (m.addr == addr && m.token == token) { out = m; return true; }
            return false;
        }, ms);
    }

    // One block round trip, as the audio thread does it: the send lanes
    // filled with `v`, a post, the return read back. Returns lane 0 of the
    // return's first frame.
    void pushBlock(float v) {
        for (uint32_t l = 0; l < LANES; ++l)
            std::fill(sendPtrs[l], sendPtrs[l] + kBlock, v);
        clockwork_port_write(send, sendPtrs.data(), LANES, kBlock);
        h->block_time.store(h->block_time.load(std::memory_order_relaxed) + kBlock,
                            std::memory_order_relaxed);
        h->engine_blocks.fetch_add(1, std::memory_order_relaxed);
        bell.post();
    }
    // Wait for the bridge to have rendered `n` more blocks than it had.
    bool rendered(uint64_t upTo, int ms = 5000) {
        return waitFor([&] { return h->bridge_heartbeat.load(std::memory_order_acquire) >= upTo; }, ms);
    }
    void pullBlock() {
        clockwork_port_read(ret, retPtrs.data(), LANES, kBlock);
    }
    float returned(uint32_t lane, uint32_t frame = 0) const { return retPtrs[lane][frame]; }
    // What TrackControl does after every pull: keep the return at slack - 1
    // blocks, plus the rest of a callback the bridge may already have
    // answered, dropping anything older as arrived-too-late.
    uint32_t perCallback = 1;
    void trimReturn() {
        const uint32_t keep = (slack - 1 + perCallback - 1) * kBlock;
        uint32_t readable = clockwork_port_readable(ret);
        while (readable > keep) {
            const uint32_t n = std::min<uint32_t>(readable - keep, kBlock);
            clockwork_port_read(ret, retPtrs.data(), LANES, n);
            readable -= n;
        }
    }

    // The usual studio: one track with the test gain plugin on it.
    int trackWithGain() {
        Msg m;
        control(11, [](osc::OutboundPacketStream& s) {
            s << osc::BeginMessage(CLOCKWORK_SYS("track/create")) << "g" << osc::EndMessage;
        });
        REQUIRE(await(CLOCKWORK_SYS("track/create.reply"), 11, m));
        REQUIRE(m.ints.size() == 2);
        REQUIRE(m.ints[1] == 1);
        const int track = m.ints[0];
        control(12, [&](osc::OutboundPacketStream& s) {
            s << osc::BeginMessage(CLOCKWORK_SYS("track/plugin/add")) << track
              << CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE << 0 << osc::EndMessage;
        });
        REQUIRE(await(CLOCKWORK_SYS("track/plugin/add.reply"), 12, m));
        REQUIRE(m.ints.size() == 2);
        REQUIRE(m.ints[1] == 1);
        REQUIRE(m.strs[0] == "ClockworkTestGain");
        return track;
    }
};

}  // namespace

TEST_CASE("the bridge joins the segment and renders what it is sent", "[plugin][bridge]") {
    Engine e;
    REQUIRE(e.spawn(false));
    REQUIRE(e.h->bridge_pid.load() == e.proc.pid());

    // The join announces what it has: nothing.
    Msg m;
    REQUIRE(e.await(CLOCKWORK_SYS("track/list"), BRIDGE_ORIGIN_TOKEN, m));
    REQUIRE(m.route == EGRESS_BROADCAST_NOTIFY);

    e.trackWithGain();

    // The test plugin's default gain is 0.5. Track 1 is slot 0: lanes 0 and
    // 1 both ways. With slack 2, block k comes back at pull k + 2.
    const uint64_t before = e.h->bridge_heartbeat.load();
    for (int k = 0; k < 4; ++k) {
        e.pushBlock(1.0f);
        REQUIRE(e.rendered(before + k + 1));
        e.pullBlock();
    }
    REQUIRE(e.returned(0) == Catch::Approx(0.5f).margin(1e-5));
    REQUIRE(e.returned(1) == Catch::Approx(0.5f).margin(1e-5));
    // A lane no track owns is silent, whatever was sent on it.
    REQUIRE(e.returned(2) == 0.0f);
    // And the bridge says so each block: slot 0 carries a track, no other
    // does. This is what the engine taps its scopes by.
    REQUIRE(e.h->slots_live.load() == 1u);
}

TEST_CASE("a track is bound to a timeline over the control ring", "[plugin][bridge][clock]") {
    Engine e;
    REQUIRE(e.spawn(false));
    const int track = e.trackWithGain();
    Msg m;

    // The query: the default.
    e.control(21, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/timeline")) << track << osc::EndMessage;
    });
    REQUIRE(e.await(CLOCKWORK_SYS("track/timeline.reply"), 21, m));
    REQUIRE(m.ints.size() == 2);
    CHECK(m.ints[0] == track);
    CHECK(m.ints[1] == 1);
    REQUIRE(m.strs.size() == 1);
    CHECK(m.strs[0] == "link");

    // A midi timeline, with the id the engine resolved (TrackControl puts it
    // there; this test stands where the engine stands).
    e.inbox.clear();
    e.control(22, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/timeline")) << track << "midi:iac-1" << 1 << osc::EndMessage;
    });
    REQUIRE(e.await(CLOCKWORK_SYS("track/timeline.reply"), 22, m));
    CHECK(m.ints[1] == 1);
    CHECK(m.strs[0] == "midi:iac-1");
    // ...and the studio was rebroadcast, with the timeline trailing the list.
    REQUIRE(e.await(CLOCKWORK_SYS("track/list"), BRIDGE_ORIGIN_TOKEN, m));
    REQUIRE(!m.strs.empty());
    CHECK(m.strs.back() == "midi:iac-1");
    e.control(23, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/timeline")) << track << osc::EndMessage;
    });
    REQUIRE(e.await(CLOCKWORK_SYS("track/timeline.reply"), 23, m));
    CHECK(m.strs[0] == "midi:iac-1");

    // A name with no id is one the engine could not resolve: refused, and
    // the binding stands.
    e.control(24, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/timeline")) << track << "midi:nope" << osc::EndMessage;
    });
    REQUIRE(e.await(CLOCKWORK_SYS("track/timeline.reply"), 24, m));
    CHECK(m.ints[1] == 0);
    CHECK(m.strs[0] == "no such timeline");
    REQUIRE(e.await(CLOCKWORK_SYS("track/error"), BRIDGE_ORIGIN_TOKEN, m));
    CHECK(m.strs[0] == "timeline");

    // Back to the clock, by name and with no id: the bridge knows this one.
    e.control(25, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/timeline")) << track << "link" << osc::EndMessage;
    });
    REQUIRE(e.await(CLOCKWORK_SYS("track/timeline.reply"), 25, m));
    CHECK(m.ints[1] == 1);
    CHECK(m.strs[0] == "link");

    // A track that is not there.
    e.control(26, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/timeline")) << 999 << "link" << osc::EndMessage;
    });
    REQUIRE(e.await(CLOCKWORK_SYS("track/timeline.reply"), 26, m));
    CHECK(m.ints[1] == 0);
    CHECK(m.strs[0] == "no such track");
}

TEST_CASE("an event on the realtime ring lands in the next block", "[plugin][bridge]") {
    Engine e;
    REQUIRE(e.spawn(false));
    const int track = e.trackWithGain();

    // By name, as code does: `gain` to 1.0 (the plugin's plain range is 0..2).
    e.realtime(0, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/param")) << track << "gain" << 1.0f << osc::EndMessage;
    });
    const uint64_t before = e.h->bridge_heartbeat.load();
    for (int k = 0; k < 4; ++k) {
        e.pushBlock(1.0f);
        REQUIRE(e.rendered(before + k + 1));
        e.pullBlock();
    }
    REQUIRE(e.returned(0) == Catch::Approx(1.0f).margin(1e-5));

    // By id, as a panel does, at a frame offset: the first half of the block
    // is still at 1.0, the second at the new value.
    e.realtime(kBlock / 2, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/plugin/param")) << 1 << 0 << 0.25f << osc::EndMessage;
    });
    const uint64_t again = e.h->bridge_heartbeat.load();
    e.pushBlock(1.0f);
    REQUIRE(e.rendered(again + 1));
    for (int k = 0; k < 3; ++k) {
        e.pushBlock(1.0f);
        REQUIRE(e.rendered(again + k + 2));
        e.pullBlock();
    }
    REQUIRE(e.returned(0, 0) == Catch::Approx(1.0f).margin(1e-5));
    REQUIRE(e.returned(0, kBlock - 1) == Catch::Approx(0.25f).margin(1e-5));
}

TEST_CASE("the bridge scans the folders it is given and says what it found", "[plugin][bridge]") {
    // The test bundle's folder is not a platform folder, so this exercises
    // folders/add too; the platform folders may hold anything, so the
    // assertion is that the test plugin is IN the answer, not the answer.
    Engine e;
    REQUIRE(e.spawn(false));
    const std::string dir = std::filesystem::path(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE).parent_path().string();
    e.control(21, [&](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/folders/add")) << dir.c_str() << osc::EndMessage;
    });
    e.control(22, [](osc::OutboundPacketStream& s) {
        s << osc::BeginMessage(CLOCKWORK_SYS("track/scan")) << osc::EndMessage;
    });
    Msg m;
    REQUIRE(e.await(CLOCKWORK_SYS("track/plugins.reply"), 22, m, 60000));
    // <total> <offset> <count>. The platform folders may hold any number of
    // plugins, so the answer may be several pages; the first says how many
    // there are, and the rest arrive on the same token until the offsets
    // cover the total.
    REQUIRE(m.ints.size() >= 3);
    const int total = m.ints[0];
    REQUIRE(total >= 1);
    REQUIRE(m.ints[1] == 0);
    REQUIRE(waitFor([&] {
        e.drain();
        int covered = 0;
        for (auto& p : e.inbox)
            if (p.addr == CLOCKWORK_SYS("track/plugins.reply") && p.token == 22 && p.ints.size() >= 3)
                covered = std::max(covered, p.ints[1] + p.ints[2]);
        return covered >= total;
    }, 5000));
    bool found = false;
    for (auto& p : e.inbox) {
        if (p.addr != CLOCKWORK_SYS("track/plugins.reply") || p.token != 22) continue;
        // Every page is one frame on the engine's NRT egress lane, which
        // refuses anything over 8 KB: a page that does not fit is a page
        // nobody hears. That bound is the reason pages are byte-capped.
        REQUIRE(p.len <= 8192);
        const int n = p.ints[2];
        // Six fields a plugin, four of them strings.
        REQUIRE(p.strs.size() == static_cast<size_t>(4 * n));
        REQUIRE(p.ints.size() == static_cast<size_t>(3 + 2 * n));
        for (int i = 0; i < n; ++i) {
            if (p.strs[4 * i] != "ClockworkTestGain") continue;
            found = true;
            REQUIRE(p.strs[4 * i + 1] == "Clockwork");
            REQUIRE(p.strs[4 * i + 2] == "vst3");
            // The scan joins directory and entry with the platform's own
            // separator, so on Windows the reply comes back with a backslash
            // where the define has a slash — the same file, spelled two ways.
            // Compare as paths rather than as text.
            REQUIRE(std::filesystem::path(p.strs[4 * i + 3])
                    == std::filesystem::path(CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE));
            REQUIRE(p.ints[2 + 2 * i] == 0);   // an effect
        }
    }
    REQUIRE(found);
    // And the broadcast, the copy the GUI hears.
    REQUIRE(e.await(CLOCKWORK_SYS("track/plugins"), BRIDGE_ORIGIN_TOKEN, m, 5000));
    e.stop();
}

TEST_CASE("the bridge leaves when asked and when its engine is gone", "[plugin][bridge]") {
    Engine e;
    REQUIRE(e.spawn(false));
    e.h->quit.store(1, std::memory_order_release);
    e.bell.post();
    REQUIRE(waitFor([&] { return !e.proc.alive(); }, 5000));
    REQUIRE(e.proc.exitCode() == 0);
    REQUIRE(e.h->bridge_pid.load() == 0);
    REQUIRE(e.h->bridge_ready.load() == 0);
}

#if !defined(_WIN32)
TEST_CASE("a bridge killed outright comes back with its tracks", "[plugin][bridge]") {
    Engine e;
    REQUIRE(e.spawn(false));
    e.trackWithGain();
    REQUIRE(waitFor([&] { return e.h->mirror_len.load(std::memory_order_acquire) > 0; }, 5000));
    const uint32_t gen = e.h->generation.load();

    ::kill(e.proc.pid(), SIGKILL);
    REQUIRE(waitFor([&] { return !e.proc.alive(); }, 5000));
    REQUIRE(e.proc.exitCode() < 0);   // a signal, not an exit
    e.proc.wait();

    // As the engine does: fresh audio rings, the rig from the mirror.
    e.openPorts(true);
    e.inbox.clear();
    REQUIRE(e.spawn(true));
    REQUIRE(e.h->generation.load() == gen + 1);
    REQUIRE(e.h->restore.load() == 0);

    Msg m;
    REQUIRE(e.await(CLOCKWORK_SYS("track/list"), BRIDGE_ORIGIN_TOKEN, m));
    // The list carries the track, its name and the plugin that came back.
    bool sawTrack = false, sawPlugin = false;
    for (auto& s : m.strs) { if (s == "g") sawTrack = true; if (s == "ClockworkTestGain") sawPlugin = true; }
    REQUIRE(sawTrack);
    REQUIRE(sawPlugin);

    // And it renders again, through the restored plugin.
    const uint64_t before = e.h->bridge_heartbeat.load();
    for (int k = 0; k < 4; ++k) {
        e.pushBlock(1.0f);
        REQUIRE(e.rendered(before + k + 1));
        e.pullBlock();
    }
    REQUIRE(e.returned(0) == Catch::Approx(0.5f).margin(1e-5));
}
#endif

TEST_CASE("a bridge from another generation does not stay", "[plugin][bridge]") {
    Engine e;
    REQUIRE(e.spawn(false));
    // The engine moves on without telling this bridge: it must notice.
    e.h->generation.fetch_add(1, std::memory_order_acq_rel);
    REQUIRE(waitFor([&] { return !e.proc.alive(); }, 5000));
    e.proc.wait();
}

TEST_CASE("the slack a device callback implies", "[plugin][bridge]") {
    // One or two blocks per callback: the floor, which is where this began.
    REQUIRE(slack_for(0, kBlock) == DEFAULT_SLACK);
    REQUIRE(slack_for(128, kBlock) == DEFAULT_SLACK);
    REQUIRE(slack_for(256, kBlock) == 4);
    // Eight blocks per callback — a 1024-frame WASAPI buffer: eight, plus two.
    REQUIRE(slack_for(1024, kBlock) == 10);
    // A round-up, not a truncation: 1000 frames is still eight blocks.
    REQUIRE(slack_for(1000, kBlock) == 10);
    // Bounded by the ring, one block kept free.
    REQUIRE(slack_for(4096, kBlock) == AUDIO_RING_FRAMES / kBlock - 1);
    // An override wins, within the same bound.
    REQUIRE(slack_for(1024, kBlock, 3) == 3);
    REQUIRE(slack_for(1024, kBlock, 99) == AUDIO_RING_FRAMES / kBlock - 1);
}

// A 1024-frame device callback ticks eight blocks back to back — each pushing
// a send block, pulling a return block and trimming — then the callback ends.
// The probe is the block index itself, through the test plugin's 0.5 gain:
// a drop, a repeat or a silence is a pull that does not carry the block due.
//
// NON-REALTIME BY CONSTRUCTION. The gap between callbacks is a barrier on the
// bridge's own heartbeat, not a sleep. What a device buys the bridge by
// spending 21 ms there is the chance to render what it was just handed, so
// waiting for exactly that tests the invariant and nothing else: a lead of
// kPer + 2 serves a whole callback. That is a fact about the ring, and it is
// true at any clock speed.
//
// Pacing this against a wall clock instead measured the machine. On a runner
// preempted for 119 ms the absolute deadlines had all passed, five callbacks
// fired back to back with no gap, and 275 of 320 pulls came back offset — with
// the bridge having rendered 96% of blocks throughout. That run said nothing
// about the bridge. The wall clock lives in the hidden [.cadence] case below.
static int callbacksOfEight(Engine& e, int callbacks, std::vector<float>& got) {
    constexpr uint32_t kPer = 8;
    e.perCallback = kPer;
    const uint64_t before = e.h->bridge_heartbeat.load();
    for (int c = 0; c < callbacks; ++c) {
        // Inside a callback: no waiting between the eight. This is the burst a
        // too-small slack cannot serve, and it stays exactly as a device
        // presents it — pulls k=1..7 ask for blocks pushed microseconds ago.
        for (uint32_t k = 0; k < kPer; ++k) {
            e.pushBlock(float(c * kPer + k + 1));
            e.pullBlock();
            got.push_back(e.returned(0));
            e.trimReturn();
        }
        // Between callbacks: the bridge catches up, as a device's idle period
        // lets it. A timeout here is itself the finding, and the assertion in
        // the caller reports it.
        e.rendered(before + uint64_t(c + 1) * kPer);
    }
    const uint64_t renderedBlocks = e.h->bridge_heartbeat.load() - before;
    // Block n comes back at pull n + slack; before that, the prime's silence.
    int wrong = 0, silent = 0, firstWrong = -1;
    for (size_t i = 0; i < got.size(); ++i) {
        const float want = i < e.slack ? 0.0f : 0.5f * float(i + 1 - e.slack);
        if (std::fabs(got[i] - want) > 1e-4f) {
            if (wrong < 3) UNSCOPED_INFO("pull " << i << " carried block " << std::lround(got[i] * 2.0f) << ", due " << std::lround(want * 2.0f));
            if (firstWrong < 0) firstWrong = int(i);
            // Silence means the bridge had nothing ready; any other value
            // means it answered with the wrong block, which is a different
            // fault entirely — ordering or trim, not lateness.
            if (std::fabs(got[i]) <= 1e-4f) ++silent;
            ++wrong;
        }
    }
    UNSCOPED_INFO("the bridge rendered " << renderedBlocks << " of " << got.size() << " blocks sent");
    if (wrong)
        UNSCOPED_INFO("first wrong pull " << firstWrong << " of " << got.size() << "; "
                      << silent << " of the " << wrong << " wrong were silence (nothing ready), "
                      << (wrong - silent) << " carried some other block (ordering, not lateness)");
    {
        std::string seq;
        for (size_t i = 0; i < std::min<size_t>(got.size(), 40); ++i) seq += std::to_string((int) std::lround(got[i] * 2.0f)) + " ";
        UNSCOPED_INFO("blocks carried by the first pulls (0 = prime silence): " << seq);
    }
    return wrong;
}

TEST_CASE("a callback of eight blocks is served whole at the slack the device implies", "[plugin][bridge]") {
    Engine e(slack_for(8 * kBlock, kBlock));
    REQUIRE(e.spawn(false));
    e.trackWithGain();
    std::vector<float> got;
    const int wrong = callbacksOfEight(e, 40, got);
    INFO("slack " << e.slack << ": of " << got.size() << " pulls, " << wrong << " did not carry the block due");
    REQUIRE(wrong == 0);
}

TEST_CASE("a callback of eight blocks at the old slack of two is mostly silence", "[plugin][bridge]") {
    // The failure this guards against, kept as a fact rather than a memory:
    // the bridge cannot answer six pulls that are microseconds apart, so at
    // slack two most of every callback is the prime's silence and the late
    // render is trimmed away — heard as a 375 Hz buzz over the music.
    Engine e(DEFAULT_SLACK);
    REQUIRE(e.spawn(false));
    e.trackWithGain();
    std::vector<float> got;
    const int wrong = callbacksOfEight(e, 40, got);
    INFO("slack " << e.slack << ": of " << got.size() << " pulls, " << wrong << " did not carry the block due");
    REQUIRE(wrong > int(got.size() / 2));
}

// The wall clock, as a measurement rather than a gate.
//
// HIDDEN ([.]), so catch_discover_tests never registers it and CI never runs
// it. What it reports is a property of the MACHINE — whether this host can
// present a device's cadence to a separate process — and a shared runner
// cannot. A GitHub macOS runner held the aggregate (864 ms against 853 ms of
// device time) while stalling 119 ms on a single callback, which is five
// periods; the absolute deadlines had then all passed, five callbacks fired
// back to back, and the stream came back offset. None of that is a fact about
// the bridge, so none of it should fail a build. Run it by name for the
// numbers:
//
//     clockwork_tests "the cadence this machine can hold"
//
TEST_CASE("the cadence this machine can hold", "[.cadence][plugin][bridge]") {
    constexpr uint32_t kPer = 8;
    constexpr int kCallbacks = 40;
    Engine e(slack_for(kPer * kBlock, kBlock));
    REQUIRE(e.spawn(false));
    e.trackWithGain();
    e.perCallback = kPer;

    const uint64_t before = e.h->bridge_heartbeat.load();
    const auto t0 = std::chrono::steady_clock::now();
    int64_t worstLateUs = 0;
    std::vector<float> got;
    for (int c = 0; c < kCallbacks; ++c) {
        const auto due = t0 + std::chrono::microseconds(
            (int64_t) (c * 1e6 * kPer * kBlock / kRate));
        std::this_thread::sleep_until(due);
        const int64_t lateUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - due).count();
        if (lateUs > worstLateUs) worstLateUs = lateUs;
        for (uint32_t k = 0; k < kPer; ++k) {
            e.pushBlock(float(c * kPer + k + 1));
            e.pullBlock();
            got.push_back(e.returned(0));
            e.trimReturn();
        }
    }
    const int64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count();
    e.rendered(before + uint64_t(kCallbacks) * kPer, 500);

    int wrong = 0;
    for (size_t i = 0; i < got.size(); ++i) {
        const float want = i < e.slack ? 0.0f : 0.5f * float(i + 1 - e.slack);
        if (std::fabs(got[i] - want) > 1e-4f) ++wrong;
    }
    const int64_t dueUs = (int64_t) (kCallbacks * 1e6 * kPer * kBlock / kRate);
    WARN("cadence held to within " << worstLateUs << " us at worst; "
         << kCallbacks << " callbacks took " << elapsedUs / 1000 << " ms against "
         << dueUs / 1000 << " ms of device time; the bridge rendered "
         << (e.h->bridge_heartbeat.load() - before) << " of " << got.size()
         << " blocks, and " << wrong << " pulls did not carry the block due");

    // The only claim: the run completed. Everything above is a number, not a
    // verdict — the verdict lives in the deterministic case at the top.
    CHECK(got.size() == size_t(kCallbacks) * kPer);
}

// ── Where the engine looks for the bridge ────────────────────────────────────
// Beside its own executable first — a build tree, an app bundle — and only
// then in CLOCKWORK_PLUGIN_BRIDGE_DIR, the directory an installed layout names
// at configure time. A package that keeps helper programs out of PATH puts the
// bridge there and the engine in /usr/bin, and the two still find each other.
TEST_CASE("bridge lookup: beside the engine first, then the installed directory",
          "[bridge][lookup]") {
    namespace fs = std::filesystem;
    const fs::path bridge = CLOCKWORK_PLUGIN_BRIDGE_EXE;
    const std::string name = bridge.filename().string();   // the built name, .exe and all
    const fs::path beside = fs::temp_directory_path() / ("clockwork-bridge-lookup-" + std::to_string(ownPid()));
    fs::remove_all(beside);
    fs::create_directories(beside);

    // An empty directory beside the engine and nothing installed: not found.
#ifndef CLOCKWORK_PLUGIN_BRIDGE_DIR
    CHECK(TrackControl::findBridgeBeside(beside.string()).empty());
#else
    const fs::path installedDir = CLOCKWORK_PLUGIN_BRIDGE_DIR;
    fs::create_directories(installedDir);
    const fs::path installed = installedDir / name;
    fs::copy_file(bridge, installed, fs::copy_options::overwrite_existing);
    // Nothing beside the engine: the installed one.
    CHECK(fs::path(TrackControl::findBridgeBeside(beside.string())) == installed);
#endif

    // A bridge beside the engine wins over the installed one.
    fs::copy_file(bridge, beside / name, fs::copy_options::overwrite_existing);
    CHECK(fs::path(TrackControl::findBridgeBeside(beside.string())) == beside / name);

    fs::remove_all(beside);
#ifdef CLOCKWORK_PLUGIN_BRIDGE_DIR
    fs::remove(installed);
#endif
}
