// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_session.cpp — Smoothie's DeviceSession, on the fake drivers.
 *
 * What the engine's device paths stand on: a session opens exactly what it is
 * asked for and nothing it is not — no driver's default opened on the way to
 * another device, no device left open when an open fails — and it runs the
 * processor it is given on whatever device it has open, never after letting
 * it go.
 */
#include <catch2/catch_test_macros.hpp>
#include "FakeAudioDevice.h"
#include "smoothie_devices.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace fake_audio;
using smoothie::DeviceRequest;
using smoothie::DeviceSession;

namespace {

// Two drivers: one with a speaker, an interface (in and out) and a
// microphone, its default the speaker; one with a single device.
std::shared_ptr<FakeSystem> twoDriverSystem() {
    auto sys = makeSimpleSystem();
    auto other = std::make_shared<FakeDeviceSpec>();
    other->name = "Other Out";
    other->maxInputChannels = 0;
    other->sampleRates = { 44100.0, 48000.0, 96000.0 };
    sys->types.push_back({ "OtherDriver", { other }, 0 });
    return sys;
}

int opensOf(FakeSystem& sys) {
    int n = 0;
    for (auto& t : sys.types)
        for (auto& d : t.devices) n += d->opens.load();
    return n;
}

// Counts what it is told, and notes a block that comes when it should not.
struct CountingProcessor final : smoothie::Processor {
    std::atomic<int>  starts { 0 }, stops { 0 }, blocks { 0 };
    std::atomic<bool> letGo { false }, calledAfterLetGo { false };
    smoothie::DeviceInfo lastStart;
    std::mutex lastStartMutex;

    void deviceStarting(const smoothie::DeviceInfo& device) override {
        std::lock_guard<std::mutex> lock(lastStartMutex);
        lastStart = device;
        starts.fetch_add(1);
    }
    void process(const float* const*, int, float* const*, int, int,
                 const smoothie::BlockTime&) override {
        if (letGo.load()) calledAfterLetGo.store(true);
        blocks.fetch_add(1);
    }
    void deviceStopped() override { stops.fetch_add(1); }

    std::string startedOn() {
        std::lock_guard<std::mutex> lock(lastStartMutex);
        return lastStart.name;
    }
    bool waitForBlocks(int atLeast) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (blocks.load() < atLeast) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }
};

DeviceSession::Options quick() {
    DeviceSession::Options o;
    o.driverSettleMs = 0;
    return o;
}

} // namespace

TEST_CASE("device session: lists the drivers it is given and starts on the first with devices",
          "[device-session]") {
    auto sys = twoDriverSystem();
    sys->types[0].devices.clear();   // the first driver lists nothing
    DeviceSession session(quick(), makeDrivers(sys));

    CHECK(session.drivers() == std::vector<std::string>{ "FakeDriver", "OtherDriver" });
    CHECK(session.hasDriver("OtherDriver"));
    CHECK_FALSE(session.hasDriver("NoDriver"));
    CHECK(session.driver() == "OtherDriver");
    CHECK_FALSE(session.isOpen());
    CHECK(opensOf(*sys) == 0);
}

TEST_CASE("device session: opens exactly the request and reports what settled",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    DeviceSession session(quick(), makeDrivers(sys));

    DeviceRequest req;
    req.driver = "FakeDriver";
    req.output = "Fake Interface";
    req.input  = "Fake Interface";
    req.sampleRate   = 44100;
    req.bufferFrames = 256;
    req.outputChannels = 2;
    req.inputChannels  = 1;
    REQUIRE(session.open(req).empty());

    const auto cur = session.current();
    REQUIRE(cur);
    CHECK(cur->driver == "FakeDriver");
    CHECK(cur->name == "Fake Interface");
    CHECK(cur->inputName == "Fake Interface");
    CHECK(cur->sampleRate == 44100);
    CHECK(cur->bufferFrames == 256);
    CHECK(cur->activeOutputs == 2);
    CHECK(cur->activeInputs == 1);
    CHECK(cur->outputs == 2);
    CHECK(cur->inputs == 2);

    const auto settled = session.settled();
    REQUIRE(settled);
    CHECK(settled->output == "Fake Interface");
    CHECK(settled->sampleRate == 44100);
    CHECK(settled->bufferFrames == 256);
    CHECK(settled->inputChannels == 1);
    CHECK(sys->at("Fake Speakers").opens.load() == 0);   // only what was named
}

TEST_CASE("device session: a rate or buffer it doesn't offer settles as JUCE's manager chose",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    DeviceSession session(quick(), makeDrivers(sys));

    DeviceRequest req;
    req.output = "Fake Speakers";
    req.sampleRate   = 22050;   // not offered: the rate it runs at now (a fake runs at 48k)
    req.bufferFrames = 100;     // not offered: the device's default
    REQUIRE(session.open(req).empty());
    CHECK(session.current()->sampleRate == 48000);
    CHECK(session.current()->bufferFrames == 128);
    CHECK(session.current()->inputName.empty());   // no input opened

    // Running at none it offers: the lowest it offers from 44.1 kHz up.
    sys->at("Fake Interface").sampleRates = { 32000.0, 96000.0, 88200.0 };
    DeviceRequest iface;
    iface.output = "Fake Interface";
    iface.sampleRate = 22050;
    REQUIRE(session.open(iface).empty());
    CHECK(session.current()->sampleRate == 88200);
}

TEST_CASE("device session: a failed open leaves nothing open, the device before it included",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    DeviceSession session(quick(), makeDrivers(sys));
    DeviceRequest speakers;
    speakers.output = "Fake Speakers";
    REQUIRE(session.open(speakers).empty());

    SECTION("a device that refuses to open") {
        sys->at("Fake Interface").failOpen = true;
        DeviceRequest req;
        req.output = "Fake Interface";
        CHECK_FALSE(session.open(req).empty());
    }
    SECTION("a device that isn't there") {
        DeviceRequest req;
        req.output = "Unplugged";
        CHECK(session.open(req) == "No such device: Unplugged");
    }
    SECTION("a driver that isn't there") {
        DeviceRequest req;
        req.driver = "NoDriver";
        req.output = "Fake Speakers";
        CHECK(session.open(req) == "No such driver: NoDriver");
    }
    SECTION("a request with nothing to open") {
        DeviceRequest req;
        req.output = "Fake Speakers";
        req.outputChannels = 0;
        req.inputChannels  = 0;
        CHECK_FALSE(session.open(req).empty());
    }
    CHECK_FALSE(session.isOpen());
    CHECK_FALSE(session.current());
    CHECK_FALSE(session.settled());
    CHECK_FALSE(session.liveState().alive);
}

TEST_CASE("device session: the request it settled on leaves the device as it is",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    DeviceSession session(quick(), makeDrivers(sys));
    DeviceRequest req;
    req.output = "Fake Speakers";
    REQUIRE(session.open(req).empty());
    const int opens = sys->at("Fake Speakers").opens.load();

    REQUIRE(session.open(*session.settled()).empty());
    CHECK(sys->at("Fake Speakers").opens.load() == opens);

    // A different rate on the same device opens it again, at that rate.
    auto faster = *session.settled();
    faster.sampleRate = 44100;
    REQUIRE(session.open(faster).empty());
    CHECK(sys->at("Fake Speakers").opens.load() == opens + 1);
    CHECK(session.current()->sampleRate == 44100);
}

TEST_CASE("device session: choosing a driver opens nothing, and opening on another opens only the device named",
          "[device-session]") {
    auto sys = twoDriverSystem();
    DeviceSession session(quick(), makeDrivers(sys));

    REQUIRE(session.selectDriver("OtherDriver"));
    CHECK(session.driver() == "OtherDriver");
    CHECK_FALSE(session.selectDriver("NoDriver"));
    CHECK(opensOf(*sys) == 0);

    // From a device on one driver to a device on the other: JUCE's manager
    // opened the new driver's default on the way, and then the device asked
    // for. The session opens the one asked for.
    REQUIRE(session.selectDriver("FakeDriver"));
    DeviceRequest mic;
    mic.input = "Fake Microphone";
    REQUIRE(session.open(mic).empty());
    // With a device open its driver is the driver: changing it is opening
    // a device on another.
    CHECK_FALSE(session.selectDriver("OtherDriver"));
    CHECK(session.driver() == "FakeDriver");
    DeviceRequest other;
    other.driver = "OtherDriver";
    other.output = "Other Out";
    REQUIRE(session.open(other).empty());
    CHECK(session.driver() == "OtherDriver");
    CHECK(sys->at("Fake Speakers").opens.load() == 0);
    CHECK(sys->at("Other Out").opens.load() == 1);
}

TEST_CASE("device session: a driver change with a device open waits for the devices to settle",
          "[device-session]") {
    auto sys = twoDriverSystem();
    auto options = quick();
    options.driverSettleMs = 60;
    DeviceSession session(options, makeDrivers(sys));
    DeviceRequest speakers;
    speakers.output = "Fake Speakers";
    REQUIRE(session.open(speakers).empty());

    DeviceRequest other;
    other.driver = "OtherDriver";
    other.output = "Other Out";
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(session.open(other).empty());
    CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(60));
}

TEST_CASE("device session: the attached processor runs on every device it opens, and never after it is let go",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    DeviceSession session(quick(), makeDrivers(sys));
    CountingProcessor p;

    DeviceRequest speakers;
    speakers.output = "Fake Speakers";
    REQUIRE(session.open(speakers).empty());
    session.attach(p);
    CHECK(session.attached());
    CHECK(p.starts.load() == 1);
    CHECK(p.startedOn() == "Fake Speakers");
    REQUIRE(p.waitForBlocks(3));

    // Another device under it: told the one stopped and the next started.
    DeviceRequest iface;
    iface.output = "Fake Interface";
    REQUIRE(session.open(iface).empty());
    CHECK(p.stops.load() >= 1);
    CHECK(p.starts.load() == 2);
    CHECK(p.startedOn() == "Fake Interface");
    const int before = p.blocks.load();
    REQUIRE(p.waitForBlocks(before + 3));

    const int stopsBefore = p.stops.load();
    session.detach();
    p.letGo.store(true);
    CHECK_FALSE(session.attached());
    CHECK(p.stops.load() == stopsBefore + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK_FALSE(p.calledAfterLetGo.load());
}

TEST_CASE("device session: a processor attached with nothing open starts on the next device",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    DeviceSession session(quick(), makeDrivers(sys));
    CountingProcessor p;
    session.attach(p);
    CHECK(p.starts.load() == 0);

    DeviceRequest speakers;
    speakers.output = "Fake Speakers";
    REQUIRE(session.open(speakers).empty());
    CHECK(p.starts.load() == 1);
    REQUIRE(p.waitForBlocks(2));
    session.detach();
}

TEST_CASE("device session: what a driver says without opening, and what a device created to be asked says",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    sys->at("Fake Microphone").wireless = true;
    sys->at("Fake Microphone").kind = "blue";
    DeviceSession session(quick(), makeDrivers(sys));

    CHECK(session.deviceNames("FakeDriver", false)
          == std::vector<std::string>{ "Fake Speakers", "Fake Interface" });
    CHECK(session.deviceNames("FakeDriver", true)
          == std::vector<std::string>{ "Fake Interface", "Fake Microphone" });
    CHECK(session.deviceNames("NoDriver", false).empty());
    CHECK(session.defaultDevice("FakeDriver", false) == "Fake Speakers");

    const auto mic = session.traits("FakeDriver", "Fake Microphone");
    CHECK(mic.wireless);
    CHECK_FALSE(mic.pairs);
    CHECK(mic.kind == "blue");
    CHECK(mic.inputChannels == 2);

    const auto probe = session.probe("FakeDriver", "Fake Interface", true);
    REQUIRE(probe);
    CHECK(probe->inputChannels == 2);
    CHECK(probe->sampleRates == std::vector<double>{ 44100.0, 48000.0 });
    CHECK_FALSE(session.probe("FakeDriver", "Unplugged", false));
    CHECK_FALSE(session.isOpen());   // asking opened nothing to play

    // Unplugged, gone from the lists once the driver looks again.
    sys->at("Fake Speakers").hidden = true;
    session.scanAll();
    CHECK(session.deviceNames("FakeDriver", false) == std::vector<std::string>{ "Fake Interface" });
}

TEST_CASE("device session: a driver's changes reach the session's owner, and stop when it goes",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    std::atomic<int> lists { 0 }, defaults { 0 }, opens { 0 };
    {
        auto options = quick();
        options.onChange = [&](smoothie::DeviceChange c) {
            if (c == smoothie::DeviceChange::list) lists.fetch_add(1);
            if (c == smoothie::DeviceChange::systemDefault) defaults.fetch_add(1);
            if (c == smoothie::DeviceChange::openDevice) opens.fetch_add(1);
        };
        DeviceSession session(options, makeDrivers(sys));
        CHECK(sys->reportListChanged());
        CHECK(sys->reportDefaultChanged());
        CHECK(sys->reportOpenDeviceChanged());
        CHECK(lists.load() == 1);
        CHECK(defaults.load() == 1);
        CHECK(opens.load() == 1);
    }
    CHECK_FALSE(sys->reportListChanged());   // no driver left to hear it
    CHECK(lists.load() == 1);
}

TEST_CASE("device session: the open device as its driver reports it now",
          "[device-session]") {
    auto sys = makeSimpleSystem();
    DeviceSession session(quick(), makeDrivers(sys));
    DeviceRequest speakers;
    speakers.output = "Fake Speakers";
    REQUIRE(session.open(speakers).empty());
    CHECK(session.liveState().alive);

    sys->at("Fake Speakers").heldRate = 44100;
    CHECK(session.liveState().sampleRate == 44100);
    sys->at("Fake Speakers").dead = true;
    CHECK_FALSE(session.liveState().alive);
}
