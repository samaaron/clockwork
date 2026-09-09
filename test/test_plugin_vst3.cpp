// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_plugin_vst3.cpp — the VST3 adapter, against a plugin whose behaviour
 * is known to the sample.
 *
 * The plugin under test is plugins/vst3/ClockworkTestGain.cpp: out = in * gain, one
 * parameter (id 0, plain 0..2, default 0.5), zero latency, state = the plain
 * gain as eight little-endian bytes. plugins/clap's test plugin is the same to
 * the letter, so a later differential test can put a block through both and
 * compare samples.
 *
 * The two tests that matter most, and why:
 *
 *   NORMALISATION. VST3 has no parameter ranges — everything is 0..1 on the
 *   wire and only the plugin's controller knows what that means. So gain 0.5
 *   is normalised 0.25 inside the plugin, and if the adapter ever leaked that
 *   clockwork would be setting a quarter of what it asked for. The tests
 *   assert plain values in and plain audio out.
 *
* Everything here goes through plugin_host.h's own C symbols — through
 * src/plugin_host.cpp, the dispatcher, which is what actually decides that a
 * .vst3 is VST3's to open. Calling clockwork_plugin_vst3::* directly would test the
 * adapter but not the boundary clockwork will use.
 *
 *   SAMPLE ACCURACY. A parameter change carries a frame offset, and "at that
 *   frame" has to mean exactly that frame — not the top of the block, not the
 *   next one. The test feeds a constant and asserts the step lands on the
 *   sample it was asked for and on no earlier one, which is the only way to
 *   tell a working offset from an ignored one.
 */
#include "DsoTestUtils.h"
#include "plugin_host.h"
#include "rt_alloc.h"
#include "clock/clock_math.h"   // ntpToOscTimetag: a block time the clock case can place
#include "shared_memory.h"      // ClockworkClockState: the clock the musical-time case hands in

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Two tests reach past the abstraction: one to establish that the plugin
// really has no editor before asserting the host copes with one that has
// not, one to read the ProcessContext's state word back by its flag names.
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

namespace {

const char* kBundle = CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE;
const char* kSo     = CLOCKWORK_VST3_TEST_PLUGIN_SO;

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kMaxBlock = 512;

// A scoped opened plugin, so a failed assertion cannot leak the module.
struct Opened {
    HostedPlugin* p = nullptr;
    const char* err = nullptr;
    explicit Opened(const char* path, uint32_t index = 0)
        : p(plugin_open(path, index, kSampleRate, kMaxBlock, &err)) {}
    ~Opened() { if (p) plugin_close(p); }
    Opened(const Opened&) = delete;
    Opened& operator=(const Opened&) = delete;
};

// Channel-major block, with the pointer array plugin_process wants.
struct Block {
    std::vector<std::vector<float>> ch;
    std::vector<float*> ptrs;
    explicit Block(uint32_t channels, uint32_t frames)
        : ch(channels, std::vector<float>(frames, 0.0f)) {
        for (auto& c : ch) ptrs.push_back(c.data());
    }
    float* const* p() { return ptrs.data(); }
    const float* const* cp() const { return const_cast<Block*>(this)->ptrs.data(); }
};

double decodeStateGain(const std::vector<uint8_t>& blob) {
    double v = 0.0;
    std::memcpy(&v, blob.data(), sizeof(double));
    return v;
}

std::vector<uint8_t> saveState(HostedPlugin* p) {
    const uint32_t n = plugin_state_save(p, nullptr, 0);
    std::vector<uint8_t> blob(n);
    if (n) REQUIRE(plugin_state_save(p, blob.data(), n) == n);
    return blob;
}

} // namespace

TEST_CASE("vst3: scanning a bundle finds the plugin", "[plugin][vst3]") {
    PluginDesc descs[4] {};
    const uint32_t n = plugin_scan(kBundle, descs, 4);

    // Two classes are advertised — the audio module and its controller — and
    // exactly one of them is a plugin. Reporting the controller as well is the
    // classic VST3 scanner bug.
    REQUIRE(n == 1);
    CHECK(descs[0].format == kPluginFormatVst3);
    CHECK(std::string(descs[0].name) == "ClockworkTestGain");
    CHECK(std::string(descs[0].vendor) == "Clockwork");
    CHECK(descs[0].index == 0);
    REQUIRE(descs[0].id != nullptr);
    CHECK(std::strlen(descs[0].id) == 32);   // the class TUID, as hex
}

TEST_CASE("vst3: a bare shared object scans the same as its bundle", "[plugin][vst3]") {
    // An installed plugin is a bundle directory; one just built in a tree is a
    // plain .so. Both have to work or the tests would be testing a layout
    // nobody ships.
    PluginDesc a[2] {}, b[2] {};
    REQUIRE(plugin_scan(kBundle, a, 2) == 1);
    const std::string bundleId = a[0].id;
    REQUIRE(plugin_scan(kSo, b, 2) == 1);
    CHECK(std::string(b[0].id) == bundleId);
}

TEST_CASE("vst3: scan reports the count even when it cannot fit it", "[plugin][vst3]") {
    CHECK(plugin_scan(kBundle, nullptr, 0) == 1);
}

TEST_CASE("vst3: scanning something that is not a plugin returns 0", "[plugin][vst3]") {
    const std::string path = std::string(std::tmpnam(nullptr)) + ".vst3";
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fputs("this is not a shared object\n", f);
        std::fclose(f);
    }
    // A refusal, not an error: scanning a directory means opening a great deal
    // that is not a plugin.
    CHECK(plugin_scan(path.c_str(), nullptr, 0) == 0);
    PluginDesc d[2] {};
    CHECK(plugin_scan(path.c_str(), d, 2) == 0);
    std::remove(path.c_str());
}

TEST_CASE("vst3: opening a path that is not there fails with a message", "[plugin][vst3]") {
    const char* err = nullptr;
    HostedPlugin* p = plugin_open("/nonexistent/definitely/not/here.vst3", 0,
                                  kSampleRate, kMaxBlock, &err);
    CHECK(p == nullptr);
    REQUIRE(err != nullptr);
    CHECK(std::strlen(err) > 0);
}

TEST_CASE("vst3: asking for a plugin index the module has not got fails", "[plugin][vst3]") {
    const char* err = nullptr;
    HostedPlugin* p = plugin_open(kBundle, 7, kSampleRate, kMaxBlock, &err);
    CHECK(p == nullptr);
    REQUIRE(err != nullptr);
    CHECK(std::string(err).find("index") != std::string::npos);
}

TEST_CASE("vst3: open, report the format, and close", "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);
    CHECK(o.err == nullptr);
    CHECK(plugin_format(o.p) == kPluginFormatVst3);
    CHECK(plugin_latency(o.p) == 0);
}

TEST_CASE("vst3: the parameter is reported in PLAIN units, not VST3's 0..1",
          "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);
    REQUIRE(plugin_param_count(o.p) == 1);

    PluginParam pp {};
    REQUIRE(plugin_param_info(o.p, 0, &pp) == 1);
    CHECK(pp.id == 0);
    CHECK(std::string(pp.name) == "gain");
    // The plugin's own normalised default is 0.25. If any of these three read
    // 0/1/0.25 the adapter has leaked VST3's normalisation into clockwork.
    CHECK(pp.min == 0.0);
    CHECK(pp.max == 2.0);
    CHECK(pp.value == 0.5);

    CHECK(plugin_param_info(o.p, 1, &pp) == 0);   // no such parameter
}

TEST_CASE("vst3: processing applies the gain exactly", "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 128;
    Block in(2, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        in.ch[0][i] = static_cast<float>(i);            // a ramp
        in.ch[1][i] = -static_cast<float>(i);
    }

    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);

    // Default gain 0.5, exactly — this is float multiply by 0.5, so equality
    // is the right assertion and an approximate one would hide a wrong value.
    for (uint32_t i = 0; i < kFrames; ++i) {
        CHECK(out.ch[0][i] == static_cast<float>(i) * 0.5f);
        CHECK(out.ch[1][i] == -static_cast<float>(i) * 0.5f);
    }
}

TEST_CASE("vst3: outputs clockwork has no input for come back silent",
          "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 64;
    Block in(1, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) in.ch[0][i] = 1.0f;
    for (uint32_t i = 0; i < kFrames; ++i) out.ch[1][i] = 99.0f;  // must be overwritten

    plugin_process(o.p, in.cp(), 1, out.p(), 2, kFrames, 0);

    for (uint32_t i = 0; i < kFrames; ++i) {
        CHECK(out.ch[0][i] == 0.5f);
        CHECK(out.ch[1][i] == 0.0f);
    }
}

TEST_CASE("vst3: setting the parameter moves the output by the DENORMALISED amount",
          "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 64;
    Block in(2, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) { in.ch[0][i] = 1.0f; in.ch[1][i] = 1.0f; }

    // 1.5 in PLAIN units. VST3 will carry 0.75; if that leaked the output
    // would be 0.75, which is exactly the failure this asserts against.
    plugin_param_set(o.p, 0, 1.5, 0);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(out.ch[0][i] == 1.5f);

    PluginParam pp {};
    REQUIRE(plugin_param_info(o.p, 0, &pp) == 1);
    CHECK(pp.value == 1.5);

    // The extremes of the range, both of which are ordinary normalised values
    // in disguise (1.0 -> 0.5, 2.0 -> 1.0).
    plugin_param_set(o.p, 0, 1.0, 0);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(out.ch[0][i] == 1.0f);

    plugin_param_set(o.p, 0, 2.0, 0);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(out.ch[0][i] == 2.0f);
}

TEST_CASE("vst3: a parameter change lands on its exact frame and not before",
          "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 64;
    constexpr uint32_t kAt     = 32;
    Block in(2, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) { in.ch[0][i] = 1.0f; in.ch[1][i] = 1.0f; }

    plugin_param_set(o.p, 0, 2.0, kAt);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);

    for (uint32_t i = 0; i < kAt; ++i)      CHECK(out.ch[0][i] == 0.5f);  // old gain
    for (uint32_t i = kAt; i < kFrames; ++i) CHECK(out.ch[0][i] == 2.0f); // from kAt on

    // The change persists into the next block without being re-sent.
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(out.ch[0][i] == 2.0f);
}

TEST_CASE("vst3: two changes in one block each land on their own frame",
          "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 64;
    Block in(2, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) { in.ch[0][i] = 1.0f; in.ch[1][i] = 1.0f; }

    plugin_param_set(o.p, 0, 1.0, 16);
    plugin_param_set(o.p, 0, 2.0, 48);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);

    for (uint32_t i = 0;  i < 16; ++i) CHECK(out.ch[0][i] == 0.5f);
    for (uint32_t i = 16; i < 48; ++i) CHECK(out.ch[0][i] == 1.0f);
    for (uint32_t i = 48; i < kFrames; ++i) CHECK(out.ch[0][i] == 2.0f);
}

TEST_CASE("vst3: state saves, changes, and reloads", "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    const std::vector<uint8_t> atDefault = saveState(o.p);
    REQUIRE(atDefault.size() == 8);              // one little-endian double
    CHECK(decodeStateGain(atDefault) == 0.5);

    plugin_param_set(o.p, 0, 1.25, 0);
    constexpr uint32_t kFrames = 32;
    Block in(2, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) { in.ch[0][i] = 1.0f; in.ch[1][i] = 1.0f; }
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);   // the change reaches the processor
    CHECK(out.ch[0][0] == 1.25f);

    const std::vector<uint8_t> changed = saveState(o.p);
    REQUIRE(changed.size() == 8);
    CHECK(decodeStateGain(changed) == 1.25);

    REQUIRE(plugin_state_load(o.p, atDefault.data(),
                              static_cast<uint32_t>(atDefault.size())) == 1);
    PluginParam pp {};
    REQUIRE(plugin_param_info(o.p, 0, &pp) == 1);
    CHECK(pp.value == 0.5);                      // the controller was primed too
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    for (uint32_t i = 0; i < kFrames; ++i) CHECK(out.ch[0][i] == 0.5f);
}

TEST_CASE("vst3: a state blob the plugin does not recognise is rejected",
          "[plugin][vst3]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    const uint8_t junk[4] = {1, 2, 3, 4};
    CHECK(plugin_state_load(o.p, junk, 4) == 0);

    // Right length, impossible value: still refused, so a blob from another
    // plugin cannot be silently absorbed just because it is eight bytes.
    double absurd = 1e9;
    uint8_t wide[8];
    std::memcpy(wide, &absurd, 8);
    CHECK(plugin_state_load(o.p, wide, 8) == 0);

    CHECK(plugin_state_load(o.p, nullptr, 0) == 0);
}

TEST_CASE("vst3: a plugin with no editor loads and runs", "[plugin][vst3]") {
    // First establish the premise rather than assuming it: reach into the
    // module directly and confirm createView really does return null. A test
    // that only opened the plugin would pass just as well against a plugin
    // that HAS an editor, and would prove nothing.
    void* h = dlopen(kSo, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(h != nullptr);
    auto getFactory = reinterpret_cast<Steinberg::IPluginFactory* (PLUGIN_API*)()>(
        dlsym(h, "GetPluginFactory"));
    REQUIRE(getFactory != nullptr);
    Steinberg::IPluginFactory* factory = getFactory();
    REQUIRE(factory != nullptr);

    Steinberg::Vst::IEditController* ctrl = nullptr;
    for (Steinberg::int32 i = 0; i < factory->countClasses(); ++i) {
        Steinberg::PClassInfo ci {};
        if (factory->getClassInfo(i, &ci) != Steinberg::kResultOk) continue;
        if (std::strcmp(ci.category, kVstComponentControllerClass) != 0) continue;
        factory->createInstance(
            reinterpret_cast<Steinberg::FIDString>(ci.cid),
            reinterpret_cast<Steinberg::FIDString>(Steinberg::Vst::IEditController_iid),
            reinterpret_cast<void**>(&ctrl));
        break;
    }
    REQUIRE(ctrl != nullptr);
    CHECK(ctrl->createView(Steinberg::Vst::ViewType::kEditor) == nullptr);
    ctrl->release();
    factory->release();
    dlclose(h);

    // And then the thing that matters: the host never asks for a view, so a
    // plugin without one is completely ordinary.
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);
    CHECK(plugin_param_count(o.p) == 1);

    constexpr uint32_t kFrames = 16;
    Block in(2, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) { in.ch[0][i] = 1.0f; in.ch[1][i] = 1.0f; }
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    CHECK(out.ch[0][kFrames - 1] == 0.5f);
}

TEST_CASE("vst3: an edit made in the plugin's own editor reaches the processor",
          "[plugin][vst3]") {
    // VST3 splits a plugin in two, and an editor talks only to the controller
    // half. When a knob is turned there the plugin calls the host's
    // IComponentHandler::performEdit — and it is the HOST that must carry the
    // new value to the processor in the next block's IParameterChanges. A
    // host that only listens sees every knob move and hears nothing change.
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    struct Seen { uint32_t id = 0; double norm = -1.0; int calls = 0; } seen;
    plugin_set_param_listener(o.p, [](void* ctx, uint32_t id, double norm) {
        auto* s = static_cast<Seen*>(ctx);
        s->id = id; s->norm = norm; ++s->calls;
    }, &seen);

    // The plugin has no window, so turn its knob through the boundary it offers.
    void* h = dlopen(kSo, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(h != nullptr);
    auto turn = reinterpret_cast<bool (*)(double)>(dlsym(h, "ClockworkTestGainTurn"));
    REQUIRE(turn != nullptr);
    REQUIRE(turn(2.0));

    // The host heard about it, as a normalised value...
    CHECK(seen.calls == 1);
    CHECK(seen.id == 0);
    CHECK(seen.norm == 1.0);

    // ...and, the part that has never worked in any host that stops there,
    // the processor applies it on the next block.
    constexpr uint32_t kFrames = 32;
    Block in(2, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) { in.ch[0][i] = 1.0f; in.ch[1][i] = 1.0f; }
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    CHECK(out.ch[0][0] == 2.0f);
    CHECK(out.ch[0][kFrames - 1] == 2.0f);
    CHECK(out.ch[1][kFrames - 1] == 2.0f);

    // And the host's own view of the parameter agrees with both.
    PluginParam pp {};
    REQUIRE(plugin_param_info(o.p, 0, &pp) == 1);
    CHECK(pp.value == 2.0);

    dlclose(h);
}

TEST_CASE("vst3: the session clock's tempo, meter and bar reach the process context",
          "[plugin][vst3][clock]") {
    using Steinberg::Vst::ProcessContext;
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    // The plugin keeps the last ProcessContext it was handed: the only way to
    // see what the host SAID, since a gain shows none of it in its samples.
    void* h = dlopen(kSo, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(h != nullptr);
    auto last = reinterpret_cast<bool (*)(double*, double*, double*, int*, int*, unsigned*)>(
        dlsym(h, "ClockworkTestGainLastContext"));
    REQUIRE(last != nullptr);

    constexpr uint32_t kFrames = 16;
    Block in(2, kFrames), out(2, kFrames);
    double tempo = 0, beat = 0, barStart = 0;
    int num = 0, den = 0;
    unsigned state = 0;

    // No clock: no musical time, and a transport that is said to roll, as it
    // always was for a plugin with no host tempo.
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK((state & ProcessContext::kTempoValid) == 0);
    CHECK((state & ProcessContext::kTimeSigValid) == 0);
    CHECK((state & ProcessContext::kPlaying) != 0);

    // 120 BPM from beat 0 at NTP 1000, in 7/8, playing. The block at 1010 is
    // beat 20: bar 5 of 3.5 quarter notes, which began at beat 17.5.
    ClockworkClockState clock;
    ClockworkClockState::initDefaults(clock);
    clock.setTempo(120.0, 1000.0);
    clock.setMeter(7, 8);
    clock.setTransport(true, 1000.0);
    plugin_set_clock(o.p, &clock);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, clockwork::ntpToOscTimetag(1010.0));
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK((state & ProcessContext::kTempoValid) != 0);
    CHECK((state & ProcessContext::kProjectTimeMusicValid) != 0);
    CHECK((state & ProcessContext::kTimeSigValid) != 0);
    CHECK((state & ProcessContext::kBarPositionValid) != 0);
    CHECK((state & ProcessContext::kPlaying) != 0);
    CHECK(tempo == 120.0);
    CHECK(beat == Catch::Approx(20.0));
    CHECK(num == 7);
    CHECK(den == 8);
    CHECK(barStart == Catch::Approx(17.5));

    // The transport stops; the grid does not. Beat 22 is bar 6, from 21.
    clock.setTransport(false, 1011.0);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, clockwork::ntpToOscTimetag(1011.0));
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK((state & ProcessContext::kPlaying) == 0);
    CHECK(beat == Catch::Approx(22.0));
    CHECK(barStart == Catch::Approx(21.0));

    // The meter can change under a running plugin: same beat, new bars.
    clock.setMeter(3, 4);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, clockwork::ntpToOscTimetag(1011.0));
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK(num == 3);
    CHECK(den == 4);
    CHECK(beat == Catch::Approx(22.0));
    CHECK(barStart == Catch::Approx(21.0));   // bars of 3: 21 is a bar line here too

    // A block with no time is placed at the anchor, beat 0.
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK(beat == 0.0);
    CHECK(barStart == 0.0);

    // Taking the clock away takes the musical time with it.
    plugin_set_clock(o.p, nullptr);
    plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK((state & ProcessContext::kTempoValid) == 0);
    dlclose(h);
}

TEST_CASE("vst3: the RT guard is suspended for the plugin and restored after",
          "[plugin][vst3][rt_alloc]") {
    Opened o(kBundle);
    REQUIRE(o.p != nullptr);

    constexpr uint32_t kFrames = 64;
    Block in(2, kFrames), out(2, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) { in.ch[0][i] = 1.0f; in.ch[1][i] = 1.0f; }

    rt_alloc::Guard guard;              // as the block driver sets it
    rt_alloc::reset();
    for (int b = 0; b < 8; ++b)
        plugin_process(o.p, in.cp(), 2, out.p(), 2, kFrames, 0);
    plugin_latency(o.p);

    // The window is exactly the call into plugin code; it must close again, or
    // every later allocation on this thread would go uncounted.
    CHECK(rt_alloc::g_in_rt == true);

#if !defined(RT_ALLOC_HOOKS_UNAVAILABLE)
    // The adapter's own per-block work allocates nothing: every buffer and
    // every parameter queue is sized at open. (What the PLUGIN does inside the
    // suspended window is deliberately not counted — see plugin_host.h.)
    CHECK(rt_alloc::g_allocs.load() == 0);
#endif
}
