// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_events.cpp — device events arriving whenever the OS sends them.
 *
 * An OS tells the device layer about a device coming or going on a thread of
 * its own, at a moment of its own; the engine must be safe whatever it is in
 * the middle of. A device manager that closes and reopens devices by itself
 * on the thread the change arrives on races the engine's device lane
 * rebuilding that same manager: a use of freed memory and a dead engine
 * process.
 *
 * The device layer hands every change to the engine, and the device lane
 * decides what it means: the device that was playing has gone, the user's
 * device is back, or only the list to report has changed. Run the race case
 * under AddressSanitizer (build-asan) too: a use of freed memory shows there
 * with both threads' stacks even when the timing does not happen to crash.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include "DeviceInvariants.h"
#include "EngineFixture.h"
#include "FakeAudioDevice.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Catch::Generators::from_range;
using fake_audio::Driver;
using fake_audio::FakeSystem;
using fake_audio::Says;
using fake_audio::fakeEngineConfig;
using fake_audio::makeSimpleSystem;
using fake_audio::runWatchdogUntil;

namespace {

// Whether the engine has told its clients it is playing on `name`: the
// device report a GUI fills its output menu from.
bool reported(EngineFixture& fix, const std::string& name) {
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("devices") && r.parsed().argString(1) == name)
            return true;
    return false;
}

// Whether the last device report says `name` plays at the engine's rate,
// as the GUI's output menu shows it: -1 if no report lists it. The report is
// mode, current device, the outputs' names, the rate, then one flag per
// output in the same order.
int playsAtTheRate(EngineFixture& fix, const std::string& name) {
    int result = -1;
    for (auto& r : fix.allReplies()) {
        if (r.address != CLOCKWORK_SYS("devices")) continue;
        const auto p = r.parsed();
        int outputs = 0;
        while (2 + outputs < p.argCount() && !p.argString(2 + outputs).empty()) ++outputs;
        for (int i = 0; i < outputs; ++i)
            if (p.argString(2 + i) == name) result = p.argInt(2 + outputs + 1 + i);
    }
    return result;
}


// Whether the engine has told its clients it records from `name`: the report
// a GUI fills its input menu from.
bool reportedInput(EngineFixture& fix, const std::string& name) {
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("input-devices") && r.parsed().argString(0) == name)
            return true;
    return false;
}

bool recordingFrom(EngineFixture& fix, const std::string& output,
                   const std::string& input, int timeoutMs = 10000) {
    return fix.pollUntil([&] {
        const auto cur = fix.engine().currentDevice();
        return cur.name == output && cur.inputDeviceName == input
            && cur.activeInputChannels > 0;
    }, timeoutMs);
}

// Everything the engine has sent so far has arrived: this reply comes back
// through the same ring, after it.
void flushReplies(EngineFixture& fix) {
    fix.send(osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1}));
    OscReply r;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("clock/tempo.reply"), r, 30000));
}

std::shared_ptr<fake_audio::FakeDeviceSpec> device(const std::string& name, int outs, int ins) {
    auto d = std::make_shared<fake_audio::FakeDeviceSpec>();
    d->name = name;
    d->maxOutputChannels = outs;
    d->maxInputChannels  = ins;
    return d;
}

// The simple machine (makeSimpleSystem), on `driver`.
std::shared_ptr<FakeSystem> systemOn(Driver driver) {
    auto sys = makeSimpleSystem();
    fake_audio::behaveLike(sys->types[0], driver);
    return sys;
}

}  // namespace

TEST_CASE("DeviceEvents: unplugging the device the engine plays on moves it to "
          "the default at once, and its clients are told", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();                // the default is Fake Speakers
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);

    sys->at("Fake Interface").hidden = true;  // unplugged
    REQUIRE(sys->reportListChanged());

    checkPlayingOn(fix, "Fake Speakers");
    CHECK(fix.pollUntil([&] { return reported(fix, "Fake Speakers"); }, 10000));
    // Still the user's device: it is where the engine goes when it is back.
    CHECK(fix.engine().preferredOutputDevice() == "Fake Interface");
}

TEST_CASE("DeviceEvents: the user's device plugged back in is played on again, "
          "and its clients are told", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);
    auto interface = sys->device("Fake Interface");

    interface->hidden = true;
    REQUIRE(sys->reportListChanged());
    checkPlayingOn(fix, "Fake Speakers");
    fix.clearReplies();

    interface->hidden = false;                     // plugged back in
    REQUIRE(sys->reportListChanged());

    checkPlayingOn(fix, "Fake Interface");
    CHECK(fix.pollUntil([&] { return reported(fix, "Fake Interface"); }, 10000));
}

TEST_CASE("DeviceEvents: a device the user picks while theirs is unplugged is kept "
          "when theirs comes back", "[DeviceEvents]") {
    // The interface drops out, the engine falls back to the speakers, and the
    // user picks the speakers in the menu. A pick of the device already
    // playing is a pick all the same: the interface returning must not take
    // the engine back with it.
    auto sys = makeSimpleSystem();                // the default is Fake Speakers
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);
    auto interface = sys->device("Fake Interface");

    interface->hidden = true;
    REQUIRE(sys->reportListChanged());
    checkPlayingOn(fix, "Fake Speakers");

    CHECK(pick(fix, "Fake Speakers").argInt(0) == 1);
    CHECK(fix.engine().preferredOutputDevice() == "Fake Speakers");

    interface->hidden = false;                     // plugged back in
    REQUIRE(sys->reportListChanged());
    deviceWorkDone(fix);
    INFO(fix.debugMessagesDump());
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
    checkCoherent(fix);
}

// -H matches loosely ("motu" finds "motu-xaero"), and what it found is the
// device: the server goes back to it after an unplug, by its name.
TEST_CASE("DeviceEvents: a device -H matched loosely is remembered by its name and played on again",
          "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "interface"));
    checkPlayingOn(fix, "Fake Interface");
    CHECK(fix.engine().preferredOutputDevice() == "Fake Interface");
    auto interface = sys->device("Fake Interface");

    interface->hidden = true;
    REQUIRE(sys->reportListChanged());
    checkPlayingOn(fix, "Fake Speakers");

    interface->hidden = false;                     // plugged back in
    REQUIRE(sys->reportListChanged());
    checkPlayingOn(fix, "Fake Interface");
}

// The same -H with the device not there at boot: the default plays until a
// device the words match appears, and then that device does.
TEST_CASE("DeviceEvents: a device -H names loosely that is absent at boot is played on when it appears",
          "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    auto interface = sys->device("Fake Interface");
    interface->hidden = true;                      // not plugged in at boot
    EngineFixture fix(fakeEngineConfig(sys, "interface"));
    checkPlayingOn(fix, "Fake Speakers");

    interface->hidden = false;                     // plugged in
    REQUIRE(sys->reportListChanged());
    checkPlayingOn(fix, "Fake Interface");
    CHECK(fix.engine().preferredOutputDevice() == "Fake Interface");
}

TEST_CASE("DeviceEvents: the session's buffer size is kept through an unplug and back",
          "[DeviceEvents]") {
    // The session asks for 64 frames. The fallback to the default device and
    // the return to the interface both keep that size rather than adopting
    // the device's own.
    auto sys = makeSimpleSystem();                // the fakes' own buffer is 128
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.bufferSize = 64;
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().activeBufferSize == 64);
    auto interface = sys->device("Fake Interface");

    interface->hidden = true;
    REQUIRE(sys->reportListChanged());
    checkPlayingOn(fix, "Fake Speakers");
    CHECK(fix.engine().currentDevice().activeBufferSize == 64);

    interface->hidden = false;                     // plugged back in
    REQUIRE(sys->reportListChanged());
    checkPlayingOn(fix, "Fake Interface");
    CHECK(fix.engine().currentDevice().activeBufferSize == 64);
}

TEST_CASE("DeviceEvents: the user's microphone plugged back in is recorded from again, "
          "and one already recording is left alone", "[DeviceEvents]") {
    auto sys = std::make_shared<fake_audio::FakeSystem>();
    auto speakers = device("Fake Speakers", 2, 0);
    auto mic = device("Fake Microphone", 0, 2);   // no other input to stand in for it
    mic->hidden = true;
    sys->types.push_back({ "FakeDriver", { speakers, mic }, 0 });
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.numInputChannels = 2;
    cfg.inputDevice = "Fake Microphone";          // saved, and not plugged in
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().activeInputChannels == 0);
    subscribe(fix);

    mic->hidden = false;
    REQUIRE(sys->reportListChanged());
    const bool recording = recordingFrom(fix, "Fake Speakers", "Fake Microphone");
    INFO(fix.debugMessagesDump());
    REQUIRE(recording);
    CHECK(fix.engine().currentDevice().activeInputChannels == 2);
    CHECK(fix.pollUntil([&] { return reportedInput(fix, "Fake Microphone"); }, 10000));

    // The list changing again while it records costs it nothing: the devices
    // are not opened again (a pair opens through its output) and nothing is
    // rebuilt.
    deviceLaneDone(fix.engine());
    flushReplies(fix);
    fix.clearReplies();
    const int opened = speakers->opens.load();
    REQUIRE(sys->reportListChanged());
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    flushReplies(fix);
    int rebuilds = 0;
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("setup")) ++rebuilds;
    CHECK(rebuilds == 0);
    CHECK(speakers->opens.load() == opened);
    checkCoherent(fix);
}

TEST_CASE("DeviceEvents: the user's microphone is recorded from again after the "
          "watchdog's recovery of the output",
          "[DeviceEvents][Watchdog][RateSkew]") {
    // A rate-skew recovery rebuilds the device manager on the system defaults
    // and reopens the pinned output from there. It must pass the pinned input
    // name too, or the fresh manager's default input stands. The pinned input
    // is the user's choice as much as the output is: the same recovery keeps
    // both.
    auto sys = makeSimpleSystem();
    // A virtual input ahead of every other in the driver's list (a webcam's
    // microphone, NDI) is where a reopen that names no input ends up:
    // recovery's fresh manager stands on the system defaults, and the switch
    // falls back to the first input the driver lists.
    {
        auto webcam = std::make_shared<fake_audio::FakeDeviceSpec>();
        webcam->name = "Fake Webcam Microphone";
        webcam->maxOutputChannels = 0;
        webcam->maxInputChannels  = 1;
        webcam->isVirtual = true;
        auto& devices = sys->types[0].devices;
        devices.insert(devices.begin(), webcam);
        sys->types[0].defaultDeviceIndex = 1;   // the speakers out; the interface in
    }
    // A microphone paired with the speakers, and an interface played and
    // recorded on as one device.
    const auto [output, input] = GENERATE(
        std::pair<std::string, std::string>{ "Fake Speakers",  "Fake Microphone" },
        std::pair<std::string, std::string>{ "Fake Interface", "Fake Interface" });
    INFO("out " << output << ", in " << input);
    // The output keeps time at 44.1k while reporting 48k: one recovery, the
    // watchdog's, which reopens it at the measured rate.
    sys->at(output).deliveredRate = 44100.0;
    auto cfg = fakeEngineConfig(sys, output);
    cfg.numInputChannels           = 2;
    cfg.inputDevice                = input;
    cfg.callbackWatchdog           = true;
    cfg.watchdogPollMs             = 50;
    cfg.watchdogStallMs            = 1000;
    cfg.watchdogRateWindowMs       = 300;
    cfg.watchdogRateTolerance      = 0.05;
    cfg.watchdogRateBadWindows     = 2;
    cfg.watchdogRateMaxRecoveries  = 1;
    cfg.watchdogRecoveryCooldownMs = 200;
    cfg.watchdogClockMs            = [sys] { return sys->nowMs(); };
    cfg.hostDrivesControl          = true;
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().name == output);
    REQUIRE(fix.engine().currentDevice().inputDeviceName == input);

    sys->useVirtualTime();
    const bool recovered = runWatchdogUntil(fix.engine(), *sys, 50, 20000, [&] {
        return fix.engine().rateSkewRecoveryCount() >= 1
            && static_cast<int>(fix.engine().currentDevice().activeSampleRate) == 44100
            && fix.engine().audioSource() == ClockworkEngine::AudioSource::RealCallback;
    });
    sys->useWallTime();
    INFO(fix.debugMessagesDump());
    REQUIRE(recovered);

    const auto cur = fix.engine().currentDevice();
    CHECK(cur.name == output);
    CHECK(cur.inputDeviceName == input);
    CHECK(cur.activeInputChannels == 2);
}

TEST_CASE("DeviceEvents: the device list changing while recovery rebuilds the "
          "device manager is safe", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.watchdogRecoveryCooldownMs = 0;   // recover as fast as asked
    EngineFixture fix(cfg);
    auto speakers = sys->device("Fake Speakers");

    // The OS: the open device goes and comes back (headphones out, in), and
    // each time it says so, whenever it likes.
    std::atomic<bool> stop { false };
    std::atomic<int> reports { 0 };
    std::thread os([&] {
        std::mt19937 rng(20261003);
        while (!stop.load()) {
            if (rng() % 3 == 0) speakers->hidden = !speakers->hidden.load();
            if (sys->reportListChanged()) reports.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 3000));
        }
    });

    // Meanwhile recovery rebuilds the manager, as Reset and the watchdog do.
    std::mt19937 rng(42);
    int recoveries = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < until) {
        std::string reason;
        if (fix.engine().requestAudioRecovery(reason)) ++recoveries;
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + rng() % 40));
    }
    stop = true;
    os.join();
    // The device is back, and the OS says so. The OS thread's last report
    // may have said "gone" and still be in flight; a reopen on it finds no
    // device and goes idle, and with no watchdog in this config only the
    // next OS report brings the engine back. So report once more here.
    speakers->hidden = false;
    sys->reportListChanged();

    INFO("reports=" << reports.load() << " recoveries=" << recoveries);
    CHECK(reports.load() > 0);
    CHECK(recoveries > 0);

    // And the engine is still answering.
    OscReply reply;
    const bool answered = fix.pollUntil([&] {
        fix.send(osc_test::message("/dummy/ping"));
        return fix.waitForReply("/dummy/pong", reply, 200);
    }, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(answered);
}

TEST_CASE("DeviceEvents: a device that goes while a recovery runs and comes back is reopened "
          "from idle when the OS says so, with no watchdog", "[DeviceEvents]") {
    // The engine with no device is idle: it renders nothing and the guest's
    // messages wait. Without the callback watchdog (off in this config) only
    // the OS reporting a change brings it back, which it does for every
    // device that comes back.
    //
    // One output only: with a second one the recovery lands on it and the
    // engine is never idle (the simple system's Fake Interface would show
    // "no source" only for the instant between the detach and the reopen).
    auto sys = std::make_shared<FakeSystem>();
    auto speakers = device("Fake Speakers", 2, 0);
    sys->types.push_back({ "FakeDriver", { speakers }, 0 });
    auto cfg = fakeEngineConfig(sys, "Fake Speakers");
    cfg.watchdogRecoveryCooldownMs = 0;
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().audioSource() == ClockworkEngine::AudioSource::RealCallback);

    // Gone, and the OS says so: the recovery finds no device and finishes
    // with the engine idle. The device comes back after that, not during it.
    speakers->hidden = true;
    REQUIRE(sys->reportListChanged());
    REQUIRE(fix.pollUntil([&] { return fix.engine().audioSource() == ClockworkEngine::AudioSource::None; }, 5000));
    REQUIRE(fix.pollUntil([&] { return !fix.engine().recoveryInFlight(); }, 5000));
    INFO(fix.debugMessagesDump());
    CHECK(fix.engine().audioSource() == ClockworkEngine::AudioSource::None);

    // Back, and the OS says so: reopened, and answering again.
    speakers->hidden = false;
    REQUIRE(sys->reportListChanged());
    const bool reopened = fix.pollUntil([&] { return fix.engine().audioSource() == ClockworkEngine::AudioSource::RealCallback; }, 5000);
    INFO(fix.debugMessagesDump());
    REQUIRE(reopened);
    OscReply reply;
    const bool answered = fix.pollUntil([&] {
        fix.send(osc_test::message("/dummy/ping"));
        return fix.waitForReply("/dummy/pong", reply, 200);
    }, 5000);
    INFO(fix.debugMessagesDump());
    REQUIRE(answered);
    CHECK(fix.engine().currentDevice().name == "Fake Speakers");
}

TEST_CASE("DeviceEvents: a recovery that waits for a device change leaves the "
          "device phase idle once both are done", "[DeviceEvents]") {
    // A recovery must mark the phase Recovering only once it holds the swap
    // gate. One that marks it first, queued behind a switch, saves the
    // switch's Swapping as the phase to put back and restores it after the
    // switch has finished. With the phase stuck at Swapping the watchdog,
    // which opens no device while a change is in flight, never opens one
    // again and the engine stops answering.
    auto cfg = fakeEngineConfig(makeSimpleSystem(), "Fake Speakers");
    cfg.watchdogRecoveryCooldownMs = 0;
    EngineFixture fix(cfg);
    subscribe(fix);

    const auto waits = [&] {
        int n = 0;
        for (const auto& line : fix.debugMessages())
            if (line.find("[recover] a device change is in flight") != std::string::npos) ++n;
        return n;
    };

    // A recovery waits a second for the gate, then gives up and says it did
    // not run, so a test thread held up longer than that between seeing it
    // wait and letting the gate go sees one that never ran. Ask again until
    // one waits and runs. Its log lines arrive here after it has finished;
    // its reopen.done is what says whether it ran.
    bool ranAfterWaiting = false;
    for (int attempt = 0; attempt < 5 && !ranAfterWaiting; ++attempt) {
        const int waitedBefore = waits();
        auto hold = fix.engine().testHoldSwapGate();   // a switch, mid-change
        std::string reason;
        REQUIRE(fix.engine().requestAudioRecovery(reason));
        REQUIRE(fix.pollUntil([&] { return waits() > waitedBefore; }, 10000));
        hold.unlock();                                 // the switch finishes
        OscReply done;                                 // and so does the recovery
        REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/reopen.done"), done, 10000));
        deviceLaneDone(fix.engine());
        CHECK(fix.engine().devicePhase() == ClockworkEngine::DevicePhase::Idle);
        ranAfterWaiting = done.parsed().argInt(0) == 1;
    }
    INFO(fix.debugMessagesDump());
    REQUIRE(ranAfterWaiting);
}

// ── The device played on, changing where it stands ──────────────────────────
// It can die while still listed, or its driver can change its channels (an
// interface's control panel turning outputs on or off). The OS says so on a
// thread of its own; the engine looks at the device as its driver reports
// it now and acts where that differs from what it is playing. The device
// layer does not restart the device by itself.

TEST_CASE("DeviceEvents: the device played on dying where it stands is left "
          "for one that plays", "[DeviceEvents]") {
    // On a driver with a default, and on one without (ASIO): the device the
    // engine moves to is the default where there is one.
    const Driver driver = GENERATE(from_range(fake_audio::onePerWay(
        [](const FakeSystem::TypeSpec& t) { return t.openDeviceChanges != Says::nothing; },
        [](const FakeSystem::TypeSpec& t) { return t.namesDefault; })));
    CAPTURE(fake_audio::driverName(driver));
    auto sys = systemOn(driver);               // the default is Fake Speakers
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));

    sys->at("Fake Interface").dead = true;  // still listed, gone all the same
    REQUIRE(sys->reportOpenDeviceChanged());

    checkPlayingOn(fix, "Fake Speakers");
}

TEST_CASE("DeviceEvents: the device played on losing channels is played on "
          "with the channels it has", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    auto interface = sys->device("Fake Interface");
    interface->maxOutputChannels = 4;
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.numOutputChannels = 4;
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().activeOutputChannels == 4);

    {   // its control panel turns two of its outputs off
        auto hold = fix.engine().testHoldSwapGate();
        interface->maxOutputChannels = 2;
    }
    REQUIRE(sys->reportOpenDeviceChanged());

    const bool narrowed = fix.pollUntil(
        [&] { return fix.engine().currentDevice().activeOutputChannels == 2; }, 10000);
    INFO(fix.debugMessagesDump());
    CHECK(narrowed);
    CHECK(fix.engine().currentDevice().name == "Fake Interface");
    checkCoherent(fix);
}

TEST_CASE("DeviceEvents: a device that cannot be opened again leaves the engine "
          "on the default at the default's rate", "[DeviceEvents]") {
    // Headphones at 44.1 kHz report no channels, so the engine opens them
    // again; that fails, putting them back fails too, and it falls back to
    // the default, a device running at 48 kHz. The DSP must be rebuilt at
    // 48 kHz, not left at 44.1 kHz.
    auto sys = makeSimpleSystem();                // the default is Fake Speakers
    sys->at("Fake Speakers").sampleRates = { 48000.0 };
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.sampleRate = 44100;
    EngineFixture fix(cfg);
    REQUIRE(fix.engine().currentDevice().activeSampleRate == 44100.0);
    REQUIRE(clockwork_sample_rate() == 44100.0);

    {   // going: no channels, and it will not open
        auto hold = fix.engine().testHoldSwapGate();
        auto& phones = sys->at("Fake Interface");
        phones.maxOutputChannels = 0;
        phones.maxInputChannels  = 0;
        phones.failOpen          = true;
    }
    REQUIRE(sys->reportOpenDeviceChanged());

    checkPlayingOn(fix, "Fake Speakers");
    CHECK(clockwork_sample_rate() == 48000.0);
}

TEST_CASE("DeviceEvents: a switch at the same rate that fails while the device "
          "that was playing has gone leaves the engine on the default at the "
          "default's rate", "[DeviceEvents]") {
    // The same way back, from a switch that kept the DSP as it was: a device
    // picked at the rate already playing, which will not open, while the
    // device that was playing has gone too.
    auto sys = makeSimpleSystem();                // the default is Fake Speakers
    sys->at("Fake Speakers").sampleRates = { 48000.0 };
    sys->types[0].devices.push_back(device("Fake Dock", 2, 0));
    sys->at("Fake Dock").failOpen = true;
    auto cfg = fakeEngineConfig(sys, "Fake Interface");
    cfg.sampleRate = 44100;
    EngineFixture fix(cfg);
    subscribe(fix);
    REQUIRE(clockwork_sample_rate() == 44100.0);

    {   // the device playing goes as the switch begins
        auto hold = fix.engine().testHoldSwapGate();
        sys->at("Fake Interface").failOpen = true;
    }
    const auto done = pick(fix, "Fake Dock");
    INFO("the switch said: " << done.argString(5));

    checkPlayingOn(fix, "Fake Speakers");
    CHECK(clockwork_sample_rate() == 48000.0);
}

TEST_CASE("DeviceEvents: the engine's own switch, reported back by the device, "
          "rebuilds nothing more", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));
    subscribe(fix);
    REQUIRE(switchWhenFree(fix, "Fake Interface", 44100.0).success);
    OscReply ownRebuild;                         // the switch's own
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("setup"), ownRebuild, 10000));
    deviceLaneDone(fix.engine());
    fix.clearReplies();

    // What CoreAudio says after a switch: the rate, the buffer, the format.
    for (int i = 0; i < 3; ++i) REQUIRE(sys->reportOpenDeviceChanged());
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    INFO(fix.debugMessagesDump());
    int rebuilds = 0;
    for (auto& r : fix.allReplies())
        if (r.address == CLOCKWORK_SYS("setup")) ++rebuilds;
    CHECK(rebuilds == 0);
    checkCoherent(fix);
}

TEST_CASE("DeviceEvents: the device played on is played on at the rate the "
          "system's sound settings change it to", "[DeviceEvents]") {
    // A shared-mode device runs at the rate the system sets. When the user
    // changes it, the stream the engine opened cannot carry on — the driver
    // says so — and the engine rebuilds at the new rate rather than playing
    // at the wrong pitch or not at all.
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));    // at 48k
    subscribe(fix);

    sys->at("Fake Interface").systemRate = 44100.0;
    REQUIRE(sys->reportOpenDeviceChanged());

    OscReply setup;
    const bool rebuilt = fix.waitForReply(CLOCKWORK_SYS("setup"), setup, 10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(rebuilt);
    CHECK(setup.parsed().argInt(0) == 44100);
    CHECK(fix.engine().currentDevice().name == "Fake Interface");
    checkCoherent(fix);
}

TEST_CASE("DeviceEvents: a device plugged back in is reported as it is now, "
          "not as it was", "[DeviceEvents]") {
    auto sys = makeSimpleSystem();
    EngineFixture fix(fakeEngineConfig(sys, "Fake Interface"));   // at 48k
    subscribe(fix);
    auto speakers = sys->device("Fake Speakers");

    speakers->hidden = true;                     // unplugged...
    REQUIRE(sys->reportListChanged());
    deviceLaneDone(fix.engine());
    {   // ...and back, as something that plays only at 96k
        auto hold = fix.engine().testHoldSwapGate();
        speakers->sampleRates = { 96000.0 };
        speakers->hidden = false;
    }
    fix.clearReplies();
    REQUIRE(sys->reportListChanged());

    const bool reported = fix.pollUntil([&] { return playsAtTheRate(fix, "Fake Speakers") >= 0; },
                                        10000);
    INFO(fix.debugMessagesDump());
    REQUIRE(reported);
    CHECK(playsAtTheRate(fix, "Fake Speakers") == 0);
}


TEST_CASE("DeviceEvents: the user's microphone coming back joins the default output, "
          "and the user's output coming back as the default goes is played on with "
          "that microphone", "[DeviceEvents]") {
    auto sys = std::make_shared<fake_audio::FakeSystem>();
    auto speakers   = device("Fake Speakers", 2, 0);
    auto headphones = device("Fake Headphones", 2, 0);
    auto mic        = device("Fake Microphone", 0, 2);
    sys->types.push_back({ "FakeDriver", { speakers, headphones, mic }, 0 });
    EngineFixture fix(fakeEngineConfig(sys, "Fake Speakers"));
    subscribe(fix);

    // The user's choice: headphones, and a microphone.
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("devices/switch"))
        << "Fake Headphones" << 0.0f << static_cast<osc::int32>(0) << "Fake Microphone";
    fix.send(b.end());
    OscReply done;
    REQUIRE(fix.waitForReply(CLOCKWORK_SYS("devices/switch.done"), done, 10000));
    REQUIRE(done.parsed().argInt(0) == 1);

    {   // both unplugged at once
        auto hold = fix.engine().testHoldSwapGate();
        headphones->hidden = true;
        mic->hidden = true;
    }
    REQUIRE(sys->reportListChanged());
    REQUIRE(fix.pollUntil([&] {
        const auto cur = fix.engine().currentDevice();
        return cur.name == "Fake Speakers" && cur.activeInputChannels == 0;
    }, 10000));

    // The microphone comes back first: it joins what is playing.
    fix.clearReplies();
    mic->hidden = false;
    REQUIRE(sys->reportListChanged());
    const bool joined = recordingFrom(fix, "Fake Speakers", "Fake Microphone");
    INFO(fix.debugMessagesDump());
    CHECK(joined);
    CHECK(fix.pollUntil([&] { return reportedInput(fix, "Fake Microphone"); }, 10000));

    {   // the headphones come back as the speakers go
        auto hold = fix.engine().testHoldSwapGate();
        speakers->hidden = true;
        headphones->hidden = false;
    }
    REQUIRE(sys->reportListChanged());
    CHECK(recordingFrom(fix, "Fake Headphones", "Fake Microphone"));
    CHECK(fix.pollUntil([&] { return reported(fix, "Fake Headphones"); }, 10000));
    CHECK(fix.engine().preferredOutputDevice() == "Fake Headphones");
    checkCoherent(fix);
}
