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
 *     re-aggregate to pick up an input?
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
// JUCE's initialiseWithDefaultDevices + subsequent aggregate creation
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
// resolveAggregateRate
//
// After the engine TRIES to set both aggregate sub-devices to the desired
// (remembered) rate, this decides the rate to actually run at. The output
// is the clock master / audible path, so we run at the rate it settled on;
// forcing a rate the output doesn't share causes aggregate-level SRC
// distortion (and changes the system rate for nothing).
// =============================================================================

static double aggRate(double desired, double in, double out) {
    return clockwork::device::resolveAggregateRate(desired, in, out);
}

TEST_CASE("AggregateRate: output accepted the desired rate → use it",
          "[AggregateRate]") {
    // Remembered 44.1k, both sub-devices took it. Honour it.
    REQUIRE(aggRate(44100, 44100, 44100) == 44100);
}

TEST_CASE("AggregateRate: both refused but agree → adopt their shared rate",
          "[AggregateRate]") {
    // Built-in Speakers+Mic pinned at 48k, remembered pref 44.1k. Don't
    // force 44.1k over 48k (SRC); run 48k.
    REQUIRE(aggRate(44100, 48000, 48000) == 48000);
}

TEST_CASE("AggregateRate: sub-devices disagree → output (clock master) wins",
          "[AggregateRate]") {
    // Bluetooth HFP mic forced to 16k while the output is 48k. Run at the
    // output's 48k (no output-side SRC); the 16k input is resampled to match
    // — unavoidable for such a device, and only affects the input path.
    REQUIRE(aggRate(48000, 16000, 48000) == 48000);
    // Even if the input happened to take the desired rate, the output is
    // the master: a 44.1k input against a 48k output still runs at 48k.
    REQUIRE(aggRate(44100, 44100, 48000) == 48000);
}

// =============================================================================
// shouldFollowDefaultOutputChange
//
// The CoreAudio default-output listener fires whenever the system default
// changes — including changes our own aggregate create/destroy provokes.
// Following the wrong target cold-swaps + rebuilds the aggregate, which
// perturbs the device list and re-fires the listener: the storm/freeze.
// These cases lock in what we will and won't chase.
// =============================================================================

static bool follow(const std::string& nd, const std::string& cur, bool virt,
                   const std::string& selfPrefix = "MyProduct") {
    return clockwork::device::shouldFollowDefaultOutputChange(nd, cur, virt,
                                                            selfPrefix);
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
// usableAggregateRates
//
// The macOS rate dropdown collapses to the current rate when on an aggregate.
// This decides what it *should* offer: the rates both sub-devices support,
// so we never offer a rate that forces aggregate-internal SRC. These lock in
// the intersection + fallbacks.
// =============================================================================

static std::vector<int> aggRates(std::vector<int> out, std::vector<int> in) {
    return clockwork::device::usableAggregateRates(out, in);
}

TEST_CASE("UsableAggRates: both support the same set → that set",
          "[UsableAggRates]") {
    REQUIRE(aggRates({44100, 48000, 88200, 96000},
                     {44100, 48000, 88200, 96000})
            == std::vector<int>{44100, 48000, 88200, 96000});
}

TEST_CASE("UsableAggRates: intersection when input supports fewer",
          "[UsableAggRates]") {
    // Built-in speakers do 44.1–96k; a mic that only does 44.1/48 →
    // offer just the shared 44.1/48.
    REQUIRE(aggRates({44100, 48000, 88200, 96000}, {44100, 48000})
            == std::vector<int>{44100, 48000});
}

TEST_CASE("UsableAggRates: disjoint (BT 16k mic vs 48k out) → output rates",
          "[UsableAggRates]") {
    // The output is the clock master / audible path; offer its rates and let
    // the odd-rate input be resampled (unavoidable for a 16k HFP mic).
    REQUIRE(aggRates({44100, 48000}, {16000})
            == std::vector<int>{44100, 48000});
}

// ── Exclusive duplex pair resolution ─────────────────────────────────────────
// The PipeWire patchbay's input and output sides live on one filter node,
// so it cannot be half-paired with a stream device. A request that would
// produce a mixed pair must resolve to an explicit, truthful pair — never
// silently override the side the user just changed (seen in the field:
// switching output to System Default while the patchbay held
// the input hijacked the output back to the patchbay, then a reconcile
// pass amputated the input).

namespace {
clockwork::device::ExclusivePair xpair(const std::string& reqOut, const std::string& reqIn,
                                     const std::string& curOut, const std::string& curIn) {
    return clockwork::device::resolveExclusiveDuplexPair(
        reqOut, reqIn, curOut, curIn, "Patchbay (16 ch)", "System Default");
}
} // namespace

TEST_CASE("ExclusivePair: picking the exclusive device claims both sides",
          "[ExclusivePair]") {
    // Output dropdown pick...
    auto p = xpair("Patchbay (16 ch)", "", "System Default", "System Default");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "Patchbay (16 ch)");
    // ...and input dropdown pick (GUI re-sends the unchanged output).
    p = xpair("System Default", "Patchbay (16 ch)", "System Default", "System Default");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "Patchbay (16 ch)");
}

TEST_CASE("ExclusivePair: changing output away drops the carried input, "
          "never the output choice", "[ExclusivePair]") {
    // The #3553 follow-up scenario: on patchbay both sides, user picks
    // System Default output; input request is empty (= keep current).
    auto p = xpair("System Default", "", "Patchbay (16 ch)", "Patchbay (16 ch)");
    REQUIRE(p.output == "System Default");
    REQUIRE(p.input == "__none__");
}

TEST_CASE("ExclusivePair: changing input away keeps the patchbay output",
          "[ExclusivePair]") {
    // On patchbay both sides, user picks a stream input; the GUI re-sends
    // the (unchanged) patchbay output alongside it. The output the user
    // chose survives — the filter node keeps its output ports while the
    // input ports draw from the other device, so this pair opens as asked.
    // Yielding the output here cost a 16-channel device and forced a
    // channel-count cold swap.
    auto p = xpair("Patchbay (16 ch)", "Built-in Audio Analog Stereo",
                   "Patchbay (16 ch)", "Patchbay (16 ch)");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "Built-in Audio Analog Stereo");
}

TEST_CASE("ExclusivePair: an input-only ask never moves the patchbay output",
          "[ExclusivePair]") {
    // The boot-restore shape: output left empty (= keep current) while an
    // input is named. Resolving the empty output to the fallback moved the
    // user off the patchbay on every launch.
    auto p = xpair("", "System Default", "Patchbay (16 ch)", "");
    REQUIRE(p.output.empty());
    REQUIRE(p.input == "System Default");
}

TEST_CASE("ExclusivePair: exclusive output with inputs disabled is legal, "
          "not a conflict", "[ExclusivePair]") {
    // Re-picking the patchbay output while inputs are off must not force
    // the output anywhere; inputs stay as requested.
    auto p = xpair("Patchbay (16 ch)", "", "Patchbay (16 ch)", "");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "");
    p = xpair("Patchbay (16 ch)", "__none__", "System Default", "System Default");
    REQUIRE(p.output == "Patchbay (16 ch)");
    REQUIRE(p.input == "__none__");
}

// =============================================================================
// chooseBootInputDevice
// =============================================================================
// The embedder hands the user's saved input over (SuperSonic: scsynth's -H);
// boot's aggregate promotion pairs the opened output with this choice.
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

// Suitability mask (parallel to visibleInputs, selectBootOutputDevice-style):
// switchDevice never aggregates a wireless input (HFP 16 kHz mono; CoreAudio
// IOProc freeze), so boot pairing must apply the same vetting — a saved
// Bluetooth input pref falls back to the system default instead of
// deterministically rebuilding the bad aggregate on every boot.

static std::string chooseInput(const std::string& requested,
                               const std::string& fallback,
                               const std::vector<std::string>& visible,
                               const std::vector<bool>& suitable) {
    return clockwork::device::chooseBootInputDevice(requested, fallback,
                                                  visible, suitable);
}

// The system default is an input like any other and gets the same vetting.
// Issue #3555: with no saved input pref the default fell through unvetted,
// so a Bluetooth HFP mic (16 kHz mono) became an aggregate sub-device —
// the engine came up at 16 kHz and the rate/buffer churn corrupted the
// heap (ASan: temp-buffer overflow). An empty return means "pair nothing":
// boot output-only rather than poison the aggregate.

// =============================================================================
// DeviceInfo aggregate suitability
//
// One predicate answers "can this device be half of an aggregate":
// wireless (Bluetooth/AirPlay) is out — HAL can't open it and HFP mode
// wrecks rates. Virtual (Loopback/BlackHole/NDI) is deliberately IN:
// aggregates work when the hardware sub-device is the clock master
// (AggregateDeviceHelper's master selection; the virtual-output + mic
// recipe is field-verified). A stale comment in switchDevice used to
// claim the opposite — these pin the truth.
// =============================================================================

#include "DeviceInfo.h"

static DeviceInfo withTransport(uint32_t fourCC) {
    DeviceInfo d;
    d.name = "X";
    d.transportType = fourCC;
    return d;
}

TEST_CASE("Aggregate suitability: wireless transports are unsuitable",
          "[DeviceInfo]") {
    REQUIRE_FALSE(withTransport(CoreAudioTransport::kBluetooth)
                      .isSuitableForAggregate());
    REQUIRE_FALSE(withTransport(CoreAudioTransport::kAirPlay)
                      .isSuitableForAggregate());
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
               int outs, int ins, uint32_t transport = 0) {
    DeviceInfo d;
    d.name = name;
    d.typeName = driver;
    d.maxOutputChannels = outs;
    d.maxInputChannels = ins;
    d.transportType = transport;
    return d;
}
} // namespace

TEST_CASE("ReportSelect: wireless devices are hidden from both lists",
          "[ReportSelect]") {
    auto sel = selectReportedDevices(
        { dev("Speakers", "CoreAudio", 2, 0),
          dev("AirPods", "CoreAudio", 2, 1, CoreAudioTransport::kBluetooth) },
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
        { dev("AirPods", "CoreAudio", 2, 0, CoreAudioTransport::kAirPlay),
          dev("Mic", "CoreAudio", 0, 1) },
        "AirPods", "", "CoreAudio", {});
    REQUIRE(sel.inputs.empty());
    // Virtual current output pairs fine — inputs stay.
    auto sel2 = selectReportedDevices(
        { dev("BlackHole", "CoreAudio", 2, 0, CoreAudioTransport::kVirtual),
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

// =============================================================================
// planBootInputPairing
// =============================================================================
// The whole boot input-pairing decision as one pure function (#3555). The
// engine's aggregate-promotion block previously made these choices inline
// against live CoreAudio state, and the paths that bypassed vetting are
// exactly the ones that shipped the RC7 Bluetooth boot crash: an unvetted
// system-default input (BT HFP mic) entered the aggregate, dragged the
// engine to 16 kHz and corrupted the heap via the rate/buffer churn.

using clockwork::device::planBootInputPairing;
using clockwork::device::BootInputPairing;

namespace {
DeviceInfo dev(const std::string& name, int outs, int ins,
               uint32_t transport = 0x626C746E /* 'bltn' */) {
    DeviceInfo d;
    d.name = name;
    d.maxOutputChannels = outs;
    d.maxInputChannels  = ins;
    d.transportType     = transport;
    return d;
}

const std::vector<DeviceInfo> kBtMachine = {
    dev("MacBook Pro Speakers",   2, 0),
    dev("MacBook Pro Microphone", 0, 1),
    dev("WH-1000XM5",             2, 1, CoreAudioTransport::kBluetooth),
    dev("Loopback Audio",         4, 4, CoreAudioTransport::kVirtual),
    dev("Multi-Output Device",    2, 0, CoreAudioTransport::kAggregate),
};
} // namespace

TEST_CASE("BootPairing: wireless system default input pairs nothing",
          "[BootPairing]") {
    auto p = planBootInputPairing("MacBook Pro Speakers", false,
                                  "", "WH-1000XM5", kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::None);
}

TEST_CASE("BootPairing: suitable system default input aggregates",
          "[BootPairing]") {
    auto p = planBootInputPairing("MacBook Pro Speakers", false,
                                  "", "MacBook Pro Microphone", kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::Aggregate);
    REQUIRE(p.inputName == "MacBook Pro Microphone");
}

TEST_CASE("BootPairing: wireless preferred input falls back to vetted default",
          "[BootPairing]") {
    auto p = planBootInputPairing("MacBook Pro Speakers", false,
                                  "WH-1000XM5", "MacBook Pro Microphone",
                                  kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::Aggregate);
    REQUIRE(p.inputName == "MacBook Pro Microphone");
}

TEST_CASE("BootPairing: aggregate-class output never aggregates",
          "[BootPairing]") {
    // Altan boot 1: -H "Multi-Output Device". Nesting an aggregate stalls
    // CoreAudio then bounces to defaults (#3554 follow-on).
    auto p = planBootInputPairing("Multi-Output Device", true,
                                  "", "MacBook Pro Microphone", kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::None);
}

TEST_CASE("BootPairing: unknown output cannot be judged — pairs nothing",
          "[BootPairing]") {
    // The opened output missing from the enumeration was a hole: the old
    // inline loop left outputSuitable=true when it found no entry.
    auto p = planBootInputPairing("Ghost Output", true,
                                  "", "MacBook Pro Microphone", kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::None);
}

TEST_CASE("BootPairing: virtual output aggregates with a hardware input",
          "[BootPairing]") {
    auto p = planBootInputPairing("Loopback Audio", false,
                                  "", "MacBook Pro Microphone", kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::Aggregate);
    REQUIRE(p.inputName == "MacBook Pro Microphone");
}

TEST_CASE("BootPairing: default IS the output — full duplex on default boots",
          "[BootPairing]") {
    auto p = planBootInputPairing("Loopback Audio", false,
                                  "", "Loopback Audio", kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::FullDuplexReopen);
    REQUIRE(p.inputName == "Loopback Audio");
}

TEST_CASE("BootPairing: default IS the output — -H boot leaves it alone",
          "[BootPairing]") {
    // The -H open already carried (or dropped) its input; reopening here
    // would discard the user's chosen output.
    auto p = planBootInputPairing("Loopback Audio", true,
                                  "", "Loopback Audio", kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::None);
}

TEST_CASE("BootPairing: wireless output pairs nothing", "[BootPairing]") {
    auto p = planBootInputPairing("WH-1000XM5", false,
                                  "", "MacBook Pro Microphone", kBtMachine);
    REQUIRE(p.action == BootInputPairing::Action::None);
}

// A pinned output (-H / GUI device choice, mPreferredOutputDevice) always
// wins over default-chasing (#3555 follow-on: a healthy -H 'Testy' boot was
// yanked onto the wireless system default by the follow handler, and the
// setDeviceMode("") routing erased the pin — the engine impersonated a user
// choice). The hot-plug reconciler owns returning to the pin; the follow
// handler must simply never fire while one is set.
