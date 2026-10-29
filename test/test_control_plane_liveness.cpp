// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_control_plane_liveness.cpp — registering for notifications must not
 * touch the audio device.
 *
 * The reported failure: the engine boots on Windows, opens a DirectSound
 * device, prints its banner — and then answers nothing. The first client's
 * /clockwork/notify is acked, the second's never is, so that client gives
 * up after 30 s. The reporter could reproduce it at will by launching
 * Voicemeeter first.
 *
 * Registration used to trigger a device report for each newly-seen client. On
 * Windows that report enumerates every device through COM, which is where
 * Voicemeeter's virtual drivers turn a fast probe into a pathological one —
 * putting it squarely in the path of a boot handshake. Clients that want
 * the device list ask for it (/clockwork/devices/report), so the unsolicited
 * probe bought nothing and cost the boot.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "clockwork_prefix.h"
#include "shared_memory.h"
#include "ring/ring.h"
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#ifndef _WIN32
#include "StreamTestClient.h"
#include <poll.h>
#include "comms/StreamOscTransport.h"
#endif

TEST_CASE("Notify registration is device-free", "[control][notify]") {
    EngineFixture fix;

    OscReply r;
    fix.send(osc_test::message("/clockwork/notify"));
    REQUIRE(fix.waitForReply("/clockwork/notify.reply", r));

    // No device traffic may follow a registration — the list only arrives when
    // a client asks for it.
    REQUIRE_FALSE(fix.waitForReply("/clockwork/devices", r, 250));
}

TEST_CASE("Notify registration stays device-free for every new client",
          "[control][notify]") {
    EngineFixture fix;

    // A second client registers after the first, from a different origin. It
    // was that second registration — a fresh client, so a fresh probe — that
    // landed in the boot handshake.
    for (int i = 0; i < 3; ++i) {
        OscReply r;
        fix.send(osc_test::message("/clockwork/notify"));
        REQUIRE(fix.waitForReply("/clockwork/notify.reply", r));
        REQUIRE_FALSE(fix.waitForReply("/clockwork/devices", r, 100));
    }
}

namespace {
// Whether `address` has been answered, waiting up to `ms` of wall clock and
// rendering NO audio block meanwhile: what a client gets while the device is
// stopped, stalled or still coming up.
bool answeredWithoutAudio(EngineFixture& fix, const char* address, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& r : fix.allReplies())
            if (r.address == address) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
} // namespace

TEST_CASE("Clockwork's own control verbs are answered while no audio block renders; the audio thread's wait for one",
          "[control][notify][control-ring]") {
    // The boot handshake is a control verb: Spider registers and waits. A
    // device that stops right after boot, or a reopen that blocks inside the
    // driver, used to leave it unanswered because the answer rode the audio
    // thread (docs/RESTARTS.md, "The control ring"). Here no block renders
    // at all — the pump is the case's, and the case never pumps.
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump = true;
    EngineFixture fix(cfg);

    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    CHECK(answeredWithoutAudio(fix, CLOCKWORK_SYS("notify.reply"), 2000));
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1}));
    CHECK(answeredWithoutAudio(fix, CLOCKWORK_SYS("clock/tempo.reply"), 2000));

    // A verb the audio thread performs — the liveness ping, the ordering
    // barrier — is the block's, as it should be: it says the audio thread is
    // alive, and arrives once a block renders.
    fix.send(osc_test::message(CLOCKWORK_SYS("ping")));
    CHECK_FALSE(answeredWithoutAudio(fix, CLOCKWORK_SYS("pong"), 300));
    fix.pumpBlock(2);
    CHECK(answeredWithoutAudio(fix, CLOCKWORK_SYS("pong"), 2000));
}

// The device list must still be available on request — the fix removes the
// unsolicited probe, not the feature.
TEST_CASE("Device report is still delivered when asked for", "[control][notify]") {
    EngineFixture fix;

    OscReply r;
    fix.send(osc_test::message("/clockwork/notify"));
    REQUIRE(fix.waitForReply("/clockwork/notify.reply", r));

    fix.send(osc_test::message("/clockwork/devices/report"));
    REQUIRE(fix.waitForReply("/clockwork/devices", r, 2000));
}

#ifndef _WIN32
// A UDP client names the port it wants the list on, since it may listen on a
// socket other than the one it sends from. A stream client (TCP, UDS, a pipe,
// shared memory) has no port to name: its connection is what is subscribed.
// One that names a port anyway — a client written for UDP, moved to TCP — was
// subscribed to nothing, and never heard the list.
TEST_CASE("Device report reaches a stream client that names a port",
          "[control][notify][transport]") {
    StreamOscTransport tcp;
    ClockworkEngine engine;
    engine.onDebug = [](const std::string&) {};
    tcp.setIngest([&engine](const uint8_t* d, uint32_t n, uint32_t token) {
        engine.ingest(d, n, token);
    });
    tcp.initialiseTcp(0, "127.0.0.1");
    tcp.setSecret(testSecret());
    engine.setTransport(&tcp);
    auto cfg = EngineFixture::defaultConfig();
    cfg.hostDrivesControl = false;   // the engine runs its own control pass
    engine.init(cfg);
    REQUIRE(tcp.start());

    const int fd = connectTcp(tcp.boundPort());
    writeFramed(fd, osc_test::message(CLOCKWORK_SYS("devices/report"), 57120).data);
    // Pumped: the list is published as the audio thread renders, so a block a
    // poll until the socket has a frame, then the frame (readFramed blocks).
    bool heard = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!heard && std::chrono::steady_clock::now() < deadline) {
        engine.pumpAudioBlock();
        pollfd pfd{ fd, POLLIN, 0 };
        if (::poll(&pfd, 1, 2) <= 0) continue;
        const auto frame = readFramed(fd);
        if (frame.empty()) break;
        heard = osc_test::parseAddress(frame.data(), static_cast<uint32_t>(frame.size()))
             == CLOCKWORK_SYS("devices");
    }
    close(fd);
    engine.shutdown();
    tcp.stop();
    CHECK(heard);
}
#endif

// Device commands now run on the device worker rather than the gateway, so the
// thing to pin is that their replies still arrive — an offloaded command that
// silently stops answering is a worse bug than the one being fixed.
TEST_CASE("Offloaded device commands still reply", "[control][notify]") {
    EngineFixture fix;
    OscReply r;

    // The report only goes to notify subscribers, so register first — same as
    // a real client does before asking for device state.
    fix.send(osc_test::message("/clockwork/notify"));
    REQUIRE(fix.waitForReply("/clockwork/notify.reply", r));

    fix.send(osc_test::message("/clockwork/devices/mode", "system"));
    REQUIRE(fix.waitForReply("/clockwork/devices/mode.reply", r, 2000));

    fix.send(osc_test::message("/clockwork/devices/report"));
    REQUIRE(fix.waitForReply("/clockwork/devices", r, 2000));
}

// The control thread is the sole non-RT consumer: whatever it does while
// handling one client's registration is time every other client spends
// unanswered. What parked it was device work done inline — a probe of every
// device — and device work takes the swap gate. So the registrations are made
// while a swap holds it: each is answered all the same, where inline device
// work would wait for the swap and never answer. Structural, not a duration a
// loaded machine can overrun (test_gateway_swap_gate.cpp holds the same line
// for every device question).
TEST_CASE("Registering clients does not park the control thread",
          "[control][blocking][notify]") {
    EngineFixture fix;
    auto swap = fix.engine().testHoldSwapGate();

    // Five origins register during a real boot — the second of them was what
    // parked the thread behind the first one's device probe.
    for (int i = 0; i < 5; ++i) {
        OscReply r;
        fix.send(osc_test::message("/clockwork/notify"));
        REQUIRE(fix.waitForReply("/clockwork/notify.reply", r, 30000));
    }
}

TEST_CASE("control ring: a pass with more answers than the egress ring holds loses none — the rest wait for the next pass",
          "[control][control-ring]") {
    // The control pass answers every verb in the control ring in one pass,
    // and a verb's replies go to the engine's egress ring, which is finite
    // (NRT_OUT_RING_SIZE) and refuses a reply with no room. A ring of verbs
    // whose replies are larger than the verbs themselves overflowed it: the
    // surplus was refused, and the line logging the refusal rode the same
    // full ring, so nothing said so (a peer lost 296 replies of 6000 on CI,
    // 2026-10-16). The pass now leaves a verb in the control ring while the
    // egress ring lacks room for its answer, and answers it on the next pass,
    // once the egress has been drained.
    ClockworkEngine engine;
    std::mutex mu;
    std::vector<std::vector<uint8_t>> replies;
    engine.onDebug = [](const std::string&) {};
    engine.onReply = [&](const uint8_t* d, uint32_t n) {
        std::lock_guard<std::mutex> lock(mu);
        replies.emplace_back(d, d + n);
    };
    auto cfg = EngineFixture::defaultConfig();
    cfg.manualAudioPump   = true;
    cfg.hostDrivesControl = true;   // no pass runs until this case runs one
    engine.init(cfg);
    engine.pumpAudioBlock();

    // The time verb's answer is larger than its question (an int64 and a
    // longer address): a full control ring of them answers with more bytes
    // than the egress ring holds.
    const auto ask = osc_test::message(CLOCKWORK_SYS("clock/time/now/get"));
    int sent = 0;
    while (engine.ingest(ask.ptr(), ask.size(), 0) == CLOCKWORK_OK) ++sent;
    REQUIRE(sent > 0);

    auto answered = [&] {
        std::lock_guard<std::mutex> lock(mu);
        int n = 0;
        for (const auto& r : replies)
            if (osc_test::parseAddress(r.data(), static_cast<uint32_t>(r.size()))
                == CLOCKWORK_SYS("clock/time/now.reply")) ++n;
        return n;
    };
    // As many passes as it takes — a desktop's ring takes hundreds of answers
    // a pass, the smallest profile's a few — while each pass answers more:
    // a pass that answered nothing, fifty times running, is a verb held for
    // good.
    for (int idle = 0; answered() < sent && idle < 50;) {
        const int before = answered();
        REQUIRE(engine.controlPass());
        idle = answered() > before ? 0 : idle + 1;
    }
    CHECK(answered() == sent);

    // The case bites only while the answers outgrow the egress ring: each
    // takes its bytes, the route word and the ring's frame header (ring.h).
    size_t answerBytes = 0;
    { std::lock_guard<std::mutex> lock(mu); if (!replies.empty()) answerBytes = replies.front().size(); }
    const uint64_t footprint = (sizeof(Message) + EGRESS_ROUTE_SIZE + answerBytes + 3u) & ~uint64_t{3};
    CHECK(static_cast<uint64_t>(sent) * footprint > NRT_OUT_RING_SIZE);
    checkNoEgressLost(engine);   // the pass waited for room: nothing was refused
    engine.shutdown();
}
