// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_device_policy.cpp — Pure-function tests for device-management policies
 *
 * The big switchDevice / changeListenerCallback functions interleave
 * CoreAudio / JUCE / engine concerns with several narrow policy
 * decisions. Those decisions are extracted as pure static functions so
 * they can be tested directly without a real audio device:
 *
 *   - resolveWirelessExitRate: should we restore the pre-wireless rate
 *     when leaving AirPlay / Bluetooth?
 *   - decideHotplugAction: given visible devices, should we reopen
 *     because the output has gone, re-attach to the preferred output, or
 *     pair again to pick up an input?
 *
 * Each scenario here corresponds to a bug we've fixed or a behaviour
 * we want to lock in going forward.
 */
#include <catch2/catch_test_macros.hpp>
#include "DevicePolicy.h"
#include <string>
#include <vector>

// =============================================================================
// resolveWirelessExitRate
// =============================================================================

static double resolveRate(double requested, int preWireless, double current,
                          bool curIsWireless, bool targetIsWireless) {
    return clockwork::device::resolveWirelessExitRate(
        requested, preWireless, current, curIsWireless, targetIsWireless);
}

TEST_CASE("WirelessExit: caller-supplied rate is never overridden",
          "[WirelessExit]") {
    // Even if EVERY other condition says we should restore, a caller-
    // specified rate wins. No silent rewrites.
    REQUIRE(resolveRate(96000, 48000, 44100, true, false) == 96000);
    REQUIRE(resolveRate(44100, 48000, 44100, true, false) == 44100);
}

TEST_CASE("WirelessExit: target IS wireless = no restoration",
          "[WirelessExit]") {
    // Wireless → wireless (e.g. AirPlay → Bluetooth) — stay at
    // negotiated rate; don't restore the pre-detour rate mid-wireless.
    REQUIRE(resolveRate(0, 48000, 44100, true, true) == 0);
}

TEST_CASE("WirelessExit: AirPlay 44.1 → MBP Speakers restores 48k",
          "[WirelessExit]") {
    // The canonical scenario: MBP Speakers at 48k, user detoured
    // through AirPlay (forced 44.1), now switching back. 48000 wins.
    REQUIRE(resolveRate(0, 48000, 44100, true, false) == 48000);
}

// =============================================================================
// decideHotplugAction
// =============================================================================

using HD = clockwork::device::HotplugDecision;

static HD decide(const std::string& prefOut,
                 const std::string& prefIn,
                 const std::string& currentOut,
                 int inChan,
                 std::vector<std::string> visible) {
    return clockwork::device::decideHotplugAction(
        prefOut, prefIn, currentOut, inChan, visible);
}

TEST_CASE("Hotplug: the output being played on is listed under its "
          "disambiguated name = still here", "[Hotplug]") {
    auto d = decide("", "", "MOTU UltraLite", 0,
                    {"MacBook Pro Speakers", "MOTU UltraLite (2)"});
    REQUIRE_FALSE(d.reopen);
}

// =============================================================================
// selectBootOutputDevice — wireless-default fallback
//
// At boot, if macOS' default output is wireless (AirPlay/Bluetooth),
// JUCE's initialiseWithDefaultDevices + pairing a microphone with it
// triggers a ~15 s IOProc halt that times out a client's boot
// handshake. Pick a non-wireless device up front instead.
// =============================================================================

static std::string selectBoot(const std::string& defName, bool defWireless,
                              std::vector<std::string> visible,
                              std::vector<bool> wireless) {
    return clockwork::device::selectBootOutputDevice(defName, defWireless,
                                                    visible, wireless);
}

TEST_CASE("BootFallback: wireless default + non-wireless visible = pick it",
          "[BootFallback]") {
    // AirPlay is default but MBP Speakers is available. Pick MBP.
    auto picked = selectBoot(
        "Living Room Speakers", true,
        {"Living Room Speakers", "MacBook Pro Speakers"},
        {true, false});
    REQUIRE(picked == "MacBook Pro Speakers");
}

TEST_CASE("BootFallback: only wireless visible = empty (accept silence)",
          "[BootFallback]") {
    // If every visible output is wireless, the fallback is impossible.
    // Return empty and let the default path open the wireless device —
    // the alternative is silent boot.
    REQUIRE(selectBoot("AirPlay A", true,
                       {"AirPlay A", "AirPlay B"},
                       {true, true})
            .empty());
}

TEST_CASE("BootFallback: skip the default itself when it's in the list",
          "[BootFallback]") {
    // Default name appears in visible list with its own wireless flag.
    // We should still pick a DIFFERENT non-wireless device, not the
    // wireless default itself.
    auto picked = selectBoot(
        "AirPlay", true,
        {"AirPlay", "MacBook Pro Speakers"},
        {true, false});
    REQUIRE(picked == "MacBook Pro Speakers");
}

// =============================================================================
// shouldFollowDefaultOutputChange
//
// The default-output listener fires whenever the system default changes.
// Following the wrong target cold-swaps (and on macOS rebuilds a pair's
// aggregate), which perturbs the device list and re-fires the listener: the
// storm/freeze. These cases lock in what we will and won't chase.
// =============================================================================

static bool follow(const std::string& nd, const std::string& cur, bool virt) {
    return clockwork::device::shouldFollowDefaultOutputChange(nd, cur, virt);
}

TEST_CASE("FollowDefault: virtual device (NDI/Loopback) → do NOT follow",
          "[FollowDefault]") {
    // NDI Audio (virtual) became the system default — must not be chased.
    REQUIRE_FALSE(follow("NDI Audio", "MacBook Pro Speakers", true));
    REQUIRE_FALSE(follow("Loopback Audio", "iRig USB", true));
}

// =============================================================================
// resolveInputWidth
//
// One answer to "how many input channels do we actually request". Both
// switchDevice's auto-enable block and enableInputChannels resolve the -1
// sentinel against the boot -i flag, and the result must be clamped to the
// device's probed capacity: WASAPI rejects setAudioDeviceSetup outright
// when asked for more inputs than exist (CoreAudio silently clamps, which
// is how the unclamped kRequestMaxChannels path shipped). Previously
// enableInputChannels resolved WITHOUT clamping — the Windows boot path
// (enableInputChannels(-1) from Main) sent 64 input bits into WASAPI.
// =============================================================================

static int width(int requested, int boot, int probed) {
    return clockwork::device::resolveInputWidth(requested, boot, probed);
}

TEST_CASE("InputWidth: auto sentinel + boot auto-max clamps to probed count",
          "[InputWidth]") {
    // THE Windows boot bug: -i -1 → kRequestMaxChannels must not survive
    // a successful probe.
    REQUIRE(width(-1, -1, 2) == 2);
    REQUIRE(width(-1, -1, 8) == 8);
}

TEST_CASE("InputWidth: auto sentinel + boot disabled inputs + probe failed → stereo",
          "[InputWidth]") {
    // Nothing known about the device and no boot count to lean on: WASAPI
    // rejects an over-request outright, so request the width every device has.
    REQUIRE(width(-1, 0, -1) == 2);
    REQUIRE(width(-1, 0, 0) == 2);
}

TEST_CASE("InputWidth: explicit request clamps to probed capacity",
          "[InputWidth]") {
    REQUIRE(width(4, -1, 2) == 2);
    REQUIRE(width(4, -1, 8) == 4);
    REQUIRE(width(4, -1, -1) == 4);
}

// =============================================================================
// chooseBootInputDevice
// =============================================================================
// The embedder hands the user's saved input over (SuperSonic: scsynth's -H);
// boot pairs the opened output with this choice.
// Before it existed, boot always paired the system default input and the
// user's saved input arrived one cold swap later (a whole second studio
// boot).

static std::string chooseInput(const std::string& requested,
                               const std::string& fallback,
                               const std::vector<std::string>& visible) {
    return clockwork::device::chooseBootInputDevice(requested, fallback, visible);
}

TEST_CASE("BootInput: requested input visible = requested wins", "[BootInput]") {
    REQUIRE(chooseInput("Loopback Audio", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "Loopback Audio" })
            == "Loopback Audio");
}

TEST_CASE("BootInput: requested input unplugged = system default", "[BootInput]") {
    // The stale pref is the GUI's to notice and clear; boot must still
    // come up with a working input rather than none.
    REQUIRE(chooseInput("MOTU M4", "MacBook Pro Microphone",
                        { "MacBook Pro Microphone", "Loopback Audio" })
            == "MacBook Pro Microphone");
}

// =============================================================================
// What a device is, as its driver says (DeviceInfo's traits)
// =============================================================================

#include "DeviceInfo.h"

// Traits as CoreAudio gives them: a wireless or aggregate-class device
// doesn't pair.
enum class Kind { wired, wireless, isVirtual, aggregate };
static DeviceInfo& setKind(DeviceInfo& d, Kind kind) {
    d.wireless       = kind == Kind::wireless;
    d.isVirtual      = kind == Kind::isVirtual;
    d.aggregateClass = kind == Kind::aggregate;
    d.pairs          = !(d.wireless || d.aggregateClass);
    return d;
}
// =============================================================================
// selectReportedDevices
//
// The list-shaping half of sendDeviceReport, extracted pure: which devices
// the GUI is offered. Filter order is contractual and pinned here:
// clutter/wireless split → known-bad-input removal → unpairable-current-
// output clears inputs → (grouped lists snapshot) → dedupe by name with
// active-driver preference → transient-enumeration suppression.
// =============================================================================

using clockwork::device::selectReportedDevices;

namespace {
DeviceInfo dev(const std::string& name, const std::string& driver,
               int outs, int ins, Kind kind = Kind::wired) {
    DeviceInfo d;
    d.name = name;
    d.typeName = driver;
    d.maxOutputChannels = outs;
    d.maxInputChannels = ins;
    return setKind(d, kind);
}
} // namespace

TEST_CASE("ReportSelect: wireless devices are hidden from both lists",
          "[ReportSelect]") {
    auto sel = selectReportedDevices(
        { dev("Speakers", "CoreAudio", 2, 0),
          dev("AirPods", "CoreAudio", 2, 1, Kind::wireless) },
        "Speakers", "", "CoreAudio", {});
    REQUIRE(sel.outputs.size() == 1);
    REQUIRE(sel.outputs[0].name == "Speakers");
    REQUIRE(sel.inputs.empty());
}

TEST_CASE("ReportSelect: unpairable current output clears the input list",
          "[ReportSelect]") {
    // Wireless current output: no separate mic can join it. The GUI's
    // "-- None --" row is client-side; an empty input list here is the
    // deliberate signal.
    auto sel = selectReportedDevices(
        { dev("AirPods", "CoreAudio", 2, 0, Kind::wireless),
          dev("Mic", "CoreAudio", 0, 1) },
        "AirPods", "", "CoreAudio", {});
    REQUIRE(sel.inputs.empty());
    // Virtual current output pairs fine — inputs stay.
    auto sel2 = selectReportedDevices(
        { dev("BlackHole", "CoreAudio", 2, 0, Kind::isVirtual),
          dev("Mic", "CoreAudio", 0, 1) },
        "BlackHole", "", "CoreAudio", {});
    REQUIRE(sel2.inputs.size() == 1);
}

TEST_CASE("ReportSelect: PipeWire detected via native driver or ALSA compat",
          "[ReportSelect]") {
    auto viaNative = selectReportedDevices(
        { dev("Card", "PipeWire", 2, 0) }, "", "", "PipeWire", {});
    REQUIRE(viaNative.pipewireActive);
    auto viaCompat = selectReportedDevices(
        { dev("PipeWire Sound Server", "ALSA", 2, 0) }, "", "", "ALSA", {});
    REQUIRE(viaCompat.pipewireActive);
    auto without = selectReportedDevices(
        { dev("hw:0", "ALSA", 2, 0) }, "", "", "ALSA", {});
    REQUIRE_FALSE(without.pipewireActive);
}

// =============================================================================
// planSwap
//
// The whole decision half of switchDevice, pure: scope resolution, ASIO
// mirroring, the rate precedence ladder (explicit > wireless-exit >
// probe-nearest > per-device memory), input auto-enable with WASAPI
// clamp, channel-count cold forcing, and the final hot/cold verdict.
// The executor (applySwapPlan side of switchDevice) mutates nothing the
// planner didn't decide.
// =============================================================================

using clockwork::device::planSwap;
using clockwork::device::SwapPlanRequest;
using clockwork::device::SwapSnapshot;

namespace {
SwapSnapshot snapTwoDevices() {
    SwapSnapshot s;
    s.hasDeviceManager = true;
    s.juceCurrentType = "CoreAudio";
    s.deviceTable = { {"CoreAudio", "Speakers"},
                      {"CoreAudio", "Interface"},
                      {"ASIO", "MOTU"} };
    s.currentOutputName = "Speakers";
    s.currentRate = 48000;
    s.currentOutputChannels = 2;
    s.bootInputChannels = -1;   // boot asked for auto-max
    return s;
}
} // namespace

TEST_CASE("PlanSwap: cross-driver pick forces cold and mirrors ASIO input",
          "[PlanSwap]") {
    SwapPlanRequest req;
    req.outputName = "MOTU";
    auto snap = snapTwoDevices();
    snap.intendedDriver = "ASIO";   // two-step driver→device flow pending
    auto plan = planSwap(req, snap);
    REQUIRE(plan.error.empty());
    REQUIRE(plan.scope.crossDriver);
    REQUIRE(plan.scope.targetDriver == "ASIO");
    REQUIRE(plan.isCold);
    REQUIRE(plan.inputName == "MOTU");   // full-duplex mirror
}

TEST_CASE("PlanSwap: wireless exit restores the pre-wireless rate",
          "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.currentOutputName = "AirPods";
    snap.wirelessDeviceNames = { "AirPods" };
    snap.currentRate = 44100;       // what AirPlay negotiated
    snap.preWirelessRate = 48000;
    SwapPlanRequest req;
    req.outputName = "Speakers";
    auto plan = planSwap(req, snap);
    REQUIRE(plan.sampleRate == 48000);
    REQUIRE(plan.restoredPreWirelessRate);
    REQUIRE(plan.isCold);
}

TEST_CASE("PlanSwap: naming an input with zero live inputs auto-enables, "
          "clamped to the probed width", "[PlanSwap]") {
    auto snap = snapTwoDevices();
    snap.currentInputChannels = 0;
    snap.probedInputChannels = 2;   // device has 2; boot asked auto-max (64)
    SwapPlanRequest req;
    req.outputName = "Interface";
    req.inputName  = "Interface";
    auto plan = planSwap(req, snap);
    REQUIRE(plan.enableInputWidth == 2);
    REQUIRE(plan.isCold);           // world must rebuild with input buses
}

// =============================================================================
// scopeInputsToDriver
// =============================================================================
// Boot pairing picks its input from a full enumeration, which spans every
// driver. Windows lists the same hardware under each driver with a different
// name, so without scoping a name saved during a session on another driver
// looks pairable -- and only fails at the open, after the output device is
// already up.

using clockwork::device::scopeInputsToDriver;

// One interface as Windows really enumerates it: an ASIO entry named for the
// box itself, and Windows Audio entries named for the endpoints.
static const std::vector<std::pair<std::string, std::string>> kInputTable = {
    { "Windows Audio", "In 1-2 (2- MOTU Pro Audio)" },
    { "Windows Audio", "Microphone (NVIDIA Broadcast)" },
    { "Windows Audio (Exclusive Mode)", "In 1-2 (2- MOTU Pro Audio)" },
    { "DirectSound",   "Primary Sound Capture Driver" },
    { "ASIO",          "MOTU Pro Audio" },
};

TEST_CASE("ScopeInputs: stale cross-driver pref stops pairing instead of "
          "failing the open", "[ScopeInputs][BootInput]") {
    // The shipped regression: prefs held driver "Windows Audio" with input
    // "MOTU Pro Audio" (ASIO's name for the same box, saved while on ASIO --
    // where the input selector mirrors the output). Unscoped, that name is
    // present in the enumeration, so pairing was attempted and JUCE answered
    // "No such device" with the output already open, dropping the engine to
    // no device at all.
    //
    // Scoped, the name is simply not a candidate, so the choice falls back to
    // the system default: pairing is skipped and boot keeps its output.
    const auto scoped = scopeInputsToDriver(kInputTable, "Windows Audio");
    REQUIRE(chooseInput("MOTU Pro Audio", "", scoped).empty());

    // Unscoped is what the bug did: the ASIO name resolves and would be
    // handed to the open path.
    const auto unscoped = scopeInputsToDriver(kInputTable, "");
    REQUIRE(chooseInput("MOTU Pro Audio", "", unscoped) == "MOTU Pro Audio");

    // The same box under ASIO still pairs normally -- scoping removes the
    // cross-driver case only.
    const auto asio = scopeInputsToDriver(kInputTable, "ASIO");
    REQUIRE(chooseInput("MOTU Pro Audio", "", asio) == "MOTU Pro Audio");
}
