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
#ifndef _WIN32
#include "StreamTestClient.h"
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
    engine.setTransport(&tcp);
    auto cfg = EngineFixture::defaultConfig();
    cfg.hostDrivesControl = false;   // the engine runs its own control pass
    engine.init(cfg);
    REQUIRE(tcp.start());

    const int fd = connectTcp(tcp.boundPort());
    writeFramed(fd, osc_test::message(CLOCKWORK_SYS("devices/report"), 57120).data);
    bool heard = false;
    while (!heard) {
        const auto frame = readFramed(fd);   // empty after 2 s of nothing
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
