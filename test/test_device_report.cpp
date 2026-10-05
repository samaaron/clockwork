// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_report.cpp — the device menus a client is given.
 *
 * A GUI fills its output, input, driver and rate menus from the engine's device
 * report alone (/clockwork/devices, /clockwork/input-devices, /clockwork/info and
 * the per-driver /clockwork/device-table), so what the report offers is what the
 * user can choose. Each rule here came from a menu that misled someone: the same
 * output listed once per driver, a microphone that vanished from the list after
 * failing once, a report sent while the device list was half rebuilt that
 * deselected the user's microphone, rates offered that the microphone could not
 * run at, ALSA devices offered that PipeWire holds and nothing can open, and no
 * way to go back to following the system default.
 */
#include <catch2/catch_test_macros.hpp>
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;
using fake_audio::FakeDeviceSpec;
using fake_audio::FakeSystem;

namespace {

std::shared_ptr<FakeDeviceSpec> device(const std::string& name, int outs, int ins) {
    auto d = std::make_shared<FakeDeviceSpec>();
    d->name = name;
    d->maxOutputChannels = outs;
    d->maxInputChannels  = ins;
    return d;
}

// Booted, and subscribed to the engine's broadcasts as a GUI is.
void subscribe(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("notify")));
    OscReply ack;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("notify.reply"), ack));
    fix.clearReplies();
}

// The user picks an output and an input; what the engine says it did.
osc_test::ParsedReply pick(EngineFixture& fix, const char* output, const char* input) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("devices/switch"))
        << output << 0.0f << static_cast<osc::int32>(0) << input;
    fix.send(b.end());
    OscReply done;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.done"), done, 10000));
    return done.parsed();
}

// Everything the engine has sent so far has arrived: this reply comes back
// through the same ring, after it.
void flushReplies(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1}));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r, 30000));
}

int countOf(EngineFixture& fix, const std::string& address) {
    int n = 0;
    for (auto& r : fix.allReplies())
        if (r.address == address) ++n;
    return n;
}

// The newest message at `address`, which the case has seen arrive.
osc_test::ParsedReply latest(EngineFixture& fix, const std::string& address) {
    osc_test::ParsedReply last;
    for (auto& r : fix.allReplies())
        if (r.address == address) last = r.parsed();
    REQUIRE(!last.address.empty());
    return last;
}

// /clockwork/devices: mode, current, names..., rate, compat..., drivers...
struct Offered { std::string name, driver; };
std::vector<Offered> offeredOutputs(const osc_test::ParsedReply& r) {
    const int n = (r.argCount() - 3) / 3;
    std::vector<Offered> out;
    for (int i = 0; i < n; ++i)
        out.push_back({ r.argString(2 + i), r.argString(3 + 2 * n + i) });
    return out;
}

// /clockwork/input-devices: current, count, names..., drivers...
std::vector<std::string> offeredInputs(const osc_test::ParsedReply& r) {
    std::vector<std::string> names;
    for (int i = 0; i < r.argInt(1); ++i) names.push_back(r.argString(2 + i));
    return names;
}

#ifndef __APPLE__
// /clockwork/info: text, rate, buffer, count, rates...
std::vector<int> offeredRates(const osc_test::ParsedReply& r) {
    std::vector<int> rates;
    for (int i = 0; i < r.argInt(3); ++i) rates.push_back(r.argInt(4 + i));
    return rates;
}
#endif

// One driver's rows in /clockwork/device-table, each a name and its flags.
struct DriverRows {
    std::string driver;
    std::vector<std::pair<std::string, std::string>> outputs, inputs;
};

std::vector<DriverRows> deviceTable(const osc_test::ParsedReply& r) {
    std::vector<DriverRows> table;
    int i = 2;                                    // after current and intended driver
    const int drivers = r.argInt(i++);
    for (int g = 0; g < drivers; ++g) {
        DriverRows rows;
        rows.driver = r.argString(i++);
        for (int n = r.argInt(i++); n > 0; --n, i += 2)
            rows.outputs.emplace_back(r.argString(i), r.argString(i + 1));
        for (int n = r.argInt(i++); n > 0; --n, i += 2)
            rows.inputs.emplace_back(r.argString(i), r.argString(i + 1));
        table.push_back(std::move(rows));
    }
    return table;
}

const DriverRows* rowsFor(const std::vector<DriverRows>& table, const std::string& driver) {
    for (auto& g : table)
        if (g.driver == driver) return &g;
    return nullptr;
}

bool lists(const std::vector<std::pair<std::string, std::string>>& rows,
           const std::string& name) {
    for (auto& r : rows)
        if (r.first == name) return true;
    return false;
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

}  // namespace

TEST_CASE("DeviceReport: every driver but ASIO offers a System Default row, and "
          "picking it follows the default", "[DeviceReport]") {
    auto sys = makeSimpleSystem();                // the default is Fake Speakers
    sys->types.push_back({ "ASIO", { device("Fake ASIO Box", 2, 2) }, 0 });
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));

    fix.send(osc_test::message(CLOCKWORK_SYS("devices/report")));
    OscReply reply;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("device-table"), reply, 10000));
    const auto table = deviceTable(reply.parsed());
    const auto* fake = rowsFor(table, "FakeDriver");
    const auto* asio = rowsFor(table, "ASIO");
    REQUIRE(fake);
    REQUIRE(asio);
    REQUIRE(!fake->outputs.empty());
    CHECK(fake->outputs.front().first == "System Default");
    CHECK(fake->outputs.front().second == "follows-default,synthetic");
    // An ASIO driver is its device: there is no default for it to follow.
    CHECK(lists(asio->outputs, "Fake ASIO Box"));
    CHECK_FALSE(lists(asio->outputs, "System Default"));

    // Picked by its name, as a GUI sends the table's rows.
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("devices/switch"))
        << "System Default" << 0.0f << static_cast<osc::int32>(0) << "";
    fix.send(b.end());
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.done"), reply, 10000));
    INFO(fix.debugMessagesDump());
    CHECK(reply.parsed().argInt(0) == 1);
    CHECK(reply.parsed().argString(3) == "Fake Speakers");
    CHECK(fix.engine().deviceMode().empty());
}

TEST_CASE("DeviceReport: a microphone that failed to open is still offered, so the "
          "user can try it again", "[DeviceReport]") {
    auto sys = makeSimpleSystem();
    sys->device("Fake Microphone")->failInputOpen = true;
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    const auto done = pick(fix, "", "Fake Microphone");
    INFO(fix.debugMessagesDump());
    CHECK(done.argInt(0) == 1);                   // the output plays on
    CHECK(done.argInt(6) == 1);                   // the microphone did not open
    CHECK_FALSE(done.argString(7).empty());       // and why
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    // The report the switch sent, which arrived before its switch.done.
    CHECK(contains(offeredInputs(latest(fix, CLOCKWORK_SYS("input-devices"))),
                   "Fake Microphone"));
}

TEST_CASE("DeviceReport: an output every driver lists appears once, as the driver in "
          "use has it, while the per-driver table keeps every driver's row",
          "[DeviceReport]") {
    auto sys = std::make_shared<FakeSystem>();
    sys->types.push_back({ "Windows Audio", { device("Speakers (X)", 2, 0),
                                              device("Only WASAPI Out", 2, 0) }, 0 });
    sys->types.push_back({ "DirectSound", { device("Primary Sound Driver", 2, 0),
                                            device("Speakers (X)", 2, 0) }, 0 });
    // Playing on another device: on macOS the open device's channels are read
    // from CoreAudio by name, which knows nothing of a fake.
    auto cfg = fakeEngineConfig(sys, "Primary Sound Driver");
    cfg.audioDriver = "DirectSound";
    EngineFixture fix(cfg);

    fix.send(osc_test::message(CLOCKWORK_SYS("devices/report")));
    OscReply devices, table;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices"), devices, 10000));
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("device-table"), table, 10000));

    int speakers = 0;
    bool wasapiOnly = false;
    for (auto& o : offeredOutputs(devices.parsed())) {
        if (o.name == "Speakers (X)") {
            ++speakers;
            CHECK(o.driver == "DirectSound");
        }
        if (o.name == "Only WASAPI Out") wasapiOnly = true;
    }
    CHECK(speakers == 1);
    CHECK(wasapiOnly);

    const auto rows = deviceTable(table.parsed());
    const auto* wasapi = rowsFor(rows, "Windows Audio");
    const auto* ds     = rowsFor(rows, "DirectSound");
    REQUIRE(wasapi);
    REQUIRE(ds);
    CHECK(lists(wasapi->outputs, "Speakers (X)"));
    CHECK(lists(wasapi->outputs, "Only WASAPI Out"));
    CHECK(lists(ds->outputs, "Speakers (X)"));
}

TEST_CASE("DeviceReport: a device list caught mid-change, missing the microphone in "
          "use, is not sent, so the user's microphone stays selected",
          "[DeviceReport]") {
    auto sys = std::make_shared<FakeSystem>();
    auto mic = device("Fake Microphone", 0, 2);   // the only input there is
    sys->types.push_back({ "FakeDriver", { device("Fake Speakers", 2, 0), mic }, 0 });
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);
    REQUIRE(pick(fix, "", "Fake Microphone").argInt(0) == 1);
    fix.clearReplies();

    {   // the device layer's list, read halfway through a rebuild
        auto hold = fix.engine().testHoldSwapGate();
        mic->hidden = true;
    }
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    flushReplies(fix);
    INFO(fix.debugMessagesDump());
    CHECK(countOf(fix, CLOCKWORK_SYS("input-devices")) == 0);
    CHECK(countOf(fix, CLOCKWORK_SYS("devices")) == 0);
    CHECK(fix.engine().currentDevice().activeInputChannels == 2);

    mic->hidden = false;
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    OscReply inputs;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("input-devices"), inputs, 10000));
    CHECK(inputs.parsed().argString(0) == "Fake Microphone");
    CHECK(offeredInputs(inputs.parsed()) == std::vector<std::string>{ "Fake Microphone" });
}

// macOS pairs a separate microphone through an aggregate device, which the
// fakes cannot build yet; the rates on offer there are the aggregate's.
#ifndef __APPLE__
TEST_CASE("DeviceReport: with an output and a separate microphone, the rates offered "
          "are the ones both can play, or the output's when they share none",
          "[DeviceReport]") {
    auto sys = std::make_shared<FakeSystem>();
    auto out    = device("Fake Wide Out", 2, 0);
    auto narrow = device("Fake Narrow Mic", 0, 2);
    auto hfp    = device("Fake HFP Mic", 0, 1);  // a Bluetooth headset's microphone
    out->sampleRates    = { 44100.0, 48000.0, 88200.0, 96000.0 };
    narrow->sampleRates = { 44100.0, 48000.0 };
    hfp->sampleRates    = { 16000.0 };
    sys->types.push_back({ "FakeDriver", { out, narrow, hfp }, 0 });
    EngineFixture fix(fakeEngineConfig(sys, "Fake Wide Out"));
    subscribe(fix);

    REQUIRE(pick(fix, "Fake Wide Out", "Fake Narrow Mic").argInt(0) == 1);
    CHECK(offeredRates(latest(fix, CLOCKWORK_SYS("info")))
          == std::vector<int>{ 44100, 48000 });
    fix.clearReplies();

    // Nothing in common: the output is what is heard, so its rates stand.
    REQUIRE(pick(fix, "", "Fake HFP Mic").argInt(0) == 1);
    CHECK(offeredRates(latest(fix, CLOCKWORK_SYS("info")))
          == std::vector<int>{ 44100, 48000, 88200, 96000 });
}
#endif

// Only Linux has ALSA, and only there does PipeWire hold the card.
#ifdef __linux__
TEST_CASE("DeviceReport: on a PipeWire desktop, ALSA's direct-hardware devices that "
          "cannot open are not offered", "[DeviceReport]") {
    const std::string hw = "Direct hardware device without any conversions (hw:0,0)";
    auto offered = [&](bool pipewire) {
        auto sys = std::make_shared<FakeSystem>();
        std::vector<std::shared_ptr<FakeDeviceSpec>> alsa;
        if (pipewire) alsa.push_back(device("PipeWire Sound Server", 2, 2));
        alsa.push_back(device(hw, 2, 2));
        sys->types.push_back({ "ALSA", alsa, 0 });
        EngineFixture fix(fakeEngineConfig(sys, pipewire ? "PipeWire Sound Server" : hw));
        fix.send(osc_test::message(CLOCKWORK_SYS("devices/report")));
        OscReply devices;
        REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices"), devices, 10000));
        std::vector<std::string> names;
        for (auto& o : offeredOutputs(devices.parsed())) names.push_back(o.name);
        return names;
    };
    CHECK(offered(true) == std::vector<std::string>{ "PipeWire Sound Server" });
    // Without PipeWire it is the card's way in, and is offered.
    CHECK(contains(offered(false), hw));
}
#endif

#ifdef __APPLE__
// CoreAudio opens a wireless device reliably only as the default, and can't
// run one inside an aggregate: wireless outputs are not offered by name,
// and while one is playing no microphone can join it.
TEST_CASE("DeviceReport: wireless devices are not offered, and while one is "
          "playing no microphone is offered or added", "[DeviceReport]") {
    auto airpods = device("Fake AirPods", 2, 1);
    airpods->kind = "blue";
    airpods->wireless = true;
    auto airplay = device("Fake AirPlay", 2, 0);
    airplay->kind = "airp";
    airplay->wireless = true;
    auto blackhole = device("Fake BlackHole", 2, 2);
    blackhole->kind = "virt";
    blackhole->isVirtual = true;
    auto sys = std::make_shared<FakeSystem>();
    sys->types.push_back({ "FakeDriver", { device("Fake Speakers", 2, 0), airpods, airplay,
                                           blackhole, device("Fake Microphone", 0, 2) }, 0 });
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    // A fresh report: it is built on the device lane, so wait for it, not
    // for a round trip through the control path.
    auto report = [&] {
        fix.clearReplies();
        fix.send(osc_test::message(CLOCKWORK_SYS("devices/report")));
        REQUIRE(fix.pollUntil([&] {
            return countOf(fix, CLOCKWORK_SYS("devices")) > 0
                && countOf(fix, CLOCKWORK_SYS("input-devices")) > 0;
        }, 30000));
    };
    auto offersOutput = [&](const std::string& name) {
        for (auto& o : offeredOutputs(latest(fix, CLOCKWORK_SYS("devices"))))
            if (o.name == name) return true;
        return false;
    };
    report();
    CHECK_FALSE(offersOutput("Fake AirPods"));
    CHECK_FALSE(offersOutput("Fake AirPlay"));
    CHECK(offersOutput("Fake Speakers"));
    CHECK(offersOutput("Fake BlackHole"));
    const auto inputs = offeredInputs(latest(fix, CLOCKWORK_SYS("input-devices")));
    CHECK_FALSE(contains(inputs, "Fake AirPods"));
    CHECK(contains(inputs, "Fake Microphone"));

    // Playing on AirPlay (an embedder can still ask for it by name).
    REQUIRE(switchWhenFree(fix, "Fake AirPlay").success);
    report();
    INFO(fix.debugMessagesDump());
    CHECK(offeredInputs(latest(fix, CLOCKWORK_SYS("input-devices"))).empty());
    const auto done = pick(fix, "", "Fake Microphone");
    CHECK(done.argInt(0) == 0);
    CHECK(done.argString(5).find("Fake Microphone") != std::string::npos);
    CHECK(done.argString(5).find("wireless") != std::string::npos);

    // Off the wireless output, microphones are offered again.
    REQUIRE(switchWhenFree(fix, "Fake BlackHole").success);
    report();
    CHECK(contains(offeredInputs(latest(fix, CLOCKWORK_SYS("input-devices"))),
                   "Fake Microphone"));
}
#endif
