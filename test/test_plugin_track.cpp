// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_plugin_track.cpp — tracks: names, lanes, chains, and what a block of
 * audio comes back as.
 *
 * The plugin under test is plugins/vst3/ClockworkTestGain.cpp: out = in * gain, one
 * parameter (id 0, plain 0..2, default 0.5). Two of them in series multiply,
 * which is what makes chain ORDER observable in the samples rather than only
 * in a list.
 *
 * The block walk is exercised through clockwork_track_process with the lane base
 * given explicitly, on staging this file lays out itself — the same walk the
 * installed hook runs on the core's staging, minus the core. The core's own
 * lane layout is test_lanes' business.
 */
#include "DsoTestUtils.h"
#include "plugin_host.h"
#include "plugin_track.h"
#include "clock/clock_math.h"        // ntpToOscTimetag: a block time the timeline case can place
#include "clock/timeline_mirror.h"   // the engine's mirror of the follower timelines
#include "lanes/lanes.h"
#include "shared_memory.h"           // ClockworkClockState: the session clock the tracks are handed

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

const char* kBundle = CLOCKWORK_VST3_TEST_PLUGIN_BUNDLE;

constexpr double   kSampleRate = 48000.0;
constexpr uint32_t kFrames     = 64;
constexpr uint32_t kBase       = 4;                       // a stereo device rounded up
constexpr uint32_t kWidth      = kBase + CLOCKWORK_TRACK_MAX * 2;

// Empty the studio between cases. The tracks are process-global — it is one
// studio — so a case that leaves tracks behind fails the next one.
struct CleanTracks {
    CleanTracks()  { clockwork_track_clear(); }
    ~CleanTracks() { clockwork_track_clear(); }
};

ClockworkTrackId makeTrack(const char* name) {
    const char* err = nullptr;
    const ClockworkTrackId t = clockwork_track_create(name, &err);
    INFO(std::string(err ? err : ""));
    REQUIRE(t != CLOCKWORK_TRACK_NONE);
    return t;
}

ClockworkTrackHandle addGain(ClockworkTrackId t, uint32_t at = 999) {
    const char* err = nullptr;
    const ClockworkTrackHandle h = clockwork_track_add_plugin(t, kBundle, 0, at, kSampleRate, kFrames, &err);
    INFO(std::string(err ? err : ""));
    REQUIRE(h != CLOCKWORK_TRACK_NO_HANDLE);
    return h;
}

uint32_t slotOf(ClockworkTrackId t) {
    ClockworkTrackInfo i {};
    REQUIRE(clockwork_track_info(t, &i));
    return i.slot;
}

// Channel-major staging the shape of the core's: `out` is what the graph
// wrote last block (the sends live there), `in` is what it will read this
// block (the returns land there).
struct Staging {
    std::vector<std::vector<float>> in, out;
    std::vector<float*> inp, outp;

    Staging() : in(kWidth, std::vector<float>(kFrames, 0.0f)),
                out(kWidth, std::vector<float>(kFrames, 0.0f)) {
        for (uint32_t c = 0; c < kWidth; ++c) { inp.push_back(in[c].data()); outp.push_back(out[c].data()); }
    }
    void send(uint32_t slot, float v) {
        std::fill(out[kBase + 2 * slot].begin(),     out[kBase + 2 * slot].end(),     v);
        std::fill(out[kBase + 2 * slot + 1].begin(), out[kBase + 2 * slot + 1].end(), v);
    }
    void poison(uint32_t slot, float v) {
        std::fill(in[kBase + 2 * slot].begin(),     in[kBase + 2 * slot].end(),     v);
        std::fill(in[kBase + 2 * slot + 1].begin(), in[kBase + 2 * slot + 1].end(), v);
    }
    void run(int64_t when = 0) {
        clockwork_track_process(inp.data(), kWidth, outp.data(), kWidth, kFrames, when, kBase);
    }
    const float* ret(uint32_t slot, uint32_t ch = 0) const { return in[kBase + 2 * slot + ch].data(); }
    void requireReturn(uint32_t slot, float v, double margin = 1e-5) const {
        for (uint32_t ch = 0; ch < 2; ++ch)
            for (uint32_t i = 0; i < kFrames; ++i)
                REQUIRE(ret(slot, ch)[i] == Catch::Approx(v).margin(margin));
    }
};

}  // namespace

// ── Names and ids ────────────────────────────────────────────────────────────

TEST_CASE("an empty studio has no tracks", "[plugin][track]") {
    CleanTracks clean;
    REQUIRE(clockwork_track_count() == 0);
    REQUIRE(clockwork_track_list(nullptr, 0) == 0);
    REQUIRE(clockwork_track_find("anything") == CLOCKWORK_TRACK_NONE);
    REQUIRE(clockwork_track_resolve("anything") == CLOCKWORK_TRACK_NONE);
}

TEST_CASE("a track is created by name and found by it", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("bass");
    REQUIRE(clockwork_track_count() == 1);
    REQUIRE(clockwork_track_find("bass") == t);
    // Names are the API: what code addresses, resolved on the audio thread.
    REQUIRE(clockwork_track_resolve("bass") == t);

    ClockworkTrackInfo i {};
    REQUIRE(clockwork_track_info(t, &i));
    REQUIRE(std::string(i.name) == "bass");
    REQUIRE(i.gain == 1.0f);
    REQUIRE(i.mute == 0);
    REQUIRE(i.node_count == 0);
}

TEST_CASE("names must be unique, present and short", "[plugin][track]") {
    CleanTracks clean;
    makeTrack("bass");
    const char* err = nullptr;
    REQUIRE(clockwork_track_create("bass", &err) == CLOCKWORK_TRACK_NONE);
    REQUIRE(err != nullptr);
    REQUIRE(clockwork_track_create("", &err) == CLOCKWORK_TRACK_NONE);
    REQUIRE(clockwork_track_create(nullptr, &err) == CLOCKWORK_TRACK_NONE);
    const std::string tooLong(CLOCKWORK_TRACK_NAME_MAX + 8, 'x');
    REQUIRE(clockwork_track_create(tooLong.c_str(), &err) == CLOCKWORK_TRACK_NONE);
    REQUIRE(clockwork_track_count() == 1);
}

TEST_CASE("renaming moves the name, not the id or the slot", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("bass");
    const uint32_t slot = slotOf(t);
    const char* err = nullptr;
    REQUIRE(clockwork_track_rename(t, "sub", &err));
    REQUIRE(clockwork_track_find("bass") == CLOCKWORK_TRACK_NONE);
    REQUIRE(clockwork_track_find("sub") == t);
    REQUIRE(clockwork_track_resolve("sub") == t);
    REQUIRE(slotOf(t) == slot);

    // Into another track's name: refused, and nothing changed.
    makeTrack("lead");
    REQUIRE(clockwork_track_rename(t, "lead", &err) == 0);
    REQUIRE(clockwork_track_find("sub") == t);
}

TEST_CASE("ids are never reused", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId first = makeTrack("a");
    REQUIRE(clockwork_track_remove(first));
    const ClockworkTrackId second = makeTrack("a");
    // The same name, a different track. A client holding the old id — or the
    // old name resolved earlier — must not find itself addressing this one.
    REQUIRE(second != first);
    REQUIRE(clockwork_track_info(first, nullptr) == 0);
    REQUIRE(clockwork_track_remove(first) == 0);
}

TEST_CASE("a removed track frees its slot for the next", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId a = makeTrack("a");
    const ClockworkTrackId b = makeTrack("b");
    const uint32_t sa = slotOf(a);
    REQUIRE(slotOf(b) != sa);
    clockwork_track_remove(a);
    // Lowest free slot, so the lanes stay packed near the base.
    const ClockworkTrackId c = makeTrack("c");
    REQUIRE(slotOf(c) == sa);
}

TEST_CASE("the slot mask says which lanes carry a track", "[plugin][track]") {
    CleanTracks clean;
    REQUIRE(clockwork_track_slot_mask() == 0);
    const ClockworkTrackId a = makeTrack("a");
    const ClockworkTrackId b = makeTrack("b");
    REQUIRE(clockwork_track_slot_mask() == ((1u << slotOf(a)) | (1u << slotOf(b))));
    clockwork_track_remove(a);
    REQUIRE(clockwork_track_slot_mask() == (1u << slotOf(b)));
    clockwork_track_clear();
    REQUIRE(clockwork_track_slot_mask() == 0);
}

TEST_CASE("the studio is bounded by its lanes", "[plugin][track]") {
    CleanTracks clean;
    for (int i = 0; i < CLOCKWORK_TRACK_MAX; ++i)
        makeTrack(("t" + std::to_string(i)).c_str());
    const char* err = nullptr;
    REQUIRE(clockwork_track_create("one-too-many", &err) == CLOCKWORK_TRACK_NONE);
    REQUIRE(err != nullptr);
    REQUIRE(clockwork_track_count() == CLOCKWORK_TRACK_MAX);
}

TEST_CASE("moving reorders the list and nothing else", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId a = makeTrack("a");
    const ClockworkTrackId b = makeTrack("b");
    const ClockworkTrackId c = makeTrack("c");
    const uint32_t sc = slotOf(c);

    REQUIRE(clockwork_track_move(c, 0));
    std::vector<ClockworkTrackInfo> l(3);
    REQUIRE(clockwork_track_list(l.data(), 3) == 3);
    REQUIRE(l[0].id == c);
    REQUIRE(l[1].id == a);
    REQUIRE(l[2].id == b);
    REQUIRE(slotOf(c) == sc);           // display order; the lanes are the slot's

    REQUIRE(clockwork_track_move(c, 999));
    clockwork_track_list(l.data(), 3);
    REQUIRE(l[2].id == c);

    // The two-call pattern: the total comes back even when the cap is short.
    ClockworkTrackInfo one {};
    REQUIRE(clockwork_track_list(&one, 1) == 3);
}

// ── Chains ───────────────────────────────────────────────────────────────────

TEST_CASE("adding gives a live handle and the plugin's own name", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const ClockworkTrackHandle h = addGain(t);
    REQUIRE(clockwork_track_node_track(h) == t);

    ClockworkTrackNode n {};
    REQUIRE(clockwork_track_node_info(h, &n));
    REQUIRE(std::string(n.name).find("ClockworkTestGain") != std::string::npos);
    REQUIRE(std::string(n.format) == "vst3");
    REQUIRE(n.is_instrument == 0);
    REQUIRE(n.bypass == 0);
    REQUIRE(std::strlen(n.id) > 0);
    REQUIRE(std::string(n.path) == kBundle);
}

TEST_CASE("a bad path fails without disturbing the chain", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const ClockworkTrackHandle good = addGain(t);
    const char* err = nullptr;
    REQUIRE(clockwork_track_add_plugin(t, "/not/a/plugin.vst3", 0, 999, kSampleRate, kFrames, &err)
            == CLOCKWORK_TRACK_NO_HANDLE);
    REQUIRE(err != nullptr);
    REQUIRE(clockwork_track_add_plugin(t, "", 0, 999, kSampleRate, kFrames, &err) == CLOCKWORK_TRACK_NO_HANDLE);
    REQUIRE(clockwork_track_add_plugin(CLOCKWORK_TRACK_NONE, kBundle, 0, 999, kSampleRate, kFrames, &err)
            == CLOCKWORK_TRACK_NO_HANDLE);
    ClockworkTrackInfo i {};
    clockwork_track_info(t, &i);
    REQUIRE(i.node_count == 1);
    REQUIRE(clockwork_track_node_track(good) == t);
}

TEST_CASE("handles are never reused and a stale one addresses nothing", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const ClockworkTrackHandle first = addGain(t);
    REQUIRE(clockwork_track_remove_plugin(first));
    REQUIRE(clockwork_track_remove_plugin(first) == 0);
    const ClockworkTrackHandle second = addGain(t);
    REQUIRE(second != first);
    REQUIRE(clockwork_track_node_track(first) == CLOCKWORK_TRACK_NONE);

    // Every verb, because "nothing happens" has to be true of all of them.
    clockwork_track_node_param(first, 0, 1.0, 0);
    clockwork_track_editor_show(first);
    clockwork_track_editor_hide(first);
    REQUIRE(clockwork_track_move_plugin(first, 0) == 0);
    REQUIRE(clockwork_track_node_bypass(first, 1) == 0);
    REQUIRE(clockwork_track_node_info(first, nullptr) == 0);
    REQUIRE(clockwork_track_node_param_count(first) == 0);
}

TEST_CASE("a chain lists in order and moves", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const ClockworkTrackHandle a = addGain(t);
    const ClockworkTrackHandle b = addGain(t);
    const ClockworkTrackHandle c = addGain(t);
    std::vector<ClockworkTrackNode> n(3);
    REQUIRE(clockwork_track_nodes(t, n.data(), 3) == 3);
    REQUIRE(n[0].handle == a);
    REQUIRE(n[1].handle == b);
    REQUIRE(n[2].handle == c);

    REQUIRE(clockwork_track_move_plugin(c, 0));
    clockwork_track_nodes(t, n.data(), 3);
    REQUIRE(n[0].handle == c);
    REQUIRE(n[1].handle == a);

    REQUIRE(clockwork_track_move_plugin(c, 999));
    clockwork_track_nodes(t, n.data(), 3);
    REQUIRE(n[2].handle == c);

    // Insert AT an index rather than append: a GUI drops a plugin between two.
    const ClockworkTrackHandle d = addGain(t, 1);
    std::vector<ClockworkTrackNode> m(4);
    REQUIRE(clockwork_track_nodes(t, m.data(), 4) == 4);
    REQUIRE(m[1].handle == d);
}

TEST_CASE("removing a track closes its chain", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const ClockworkTrackHandle h = addGain(t);
    REQUIRE(clockwork_track_remove(t));
    REQUIRE(clockwork_track_node_track(h) == CLOCKWORK_TRACK_NONE);
    REQUIRE(clockwork_track_nodes(t, nullptr, 0) == 0);
}

// ── Parameters ───────────────────────────────────────────────────────────────

TEST_CASE("a node's parameters are listed for a control surface", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const ClockworkTrackHandle h = addGain(t);
    REQUIRE(clockwork_track_node_param_count(h) >= 1);
    PluginParam p {};
    REQUIRE(clockwork_track_node_param_info(h, 0, &p));
    REQUIRE(p.name != nullptr);
    REQUIRE(std::strlen(p.name) > 0);
}

TEST_CASE("a parameter is set by name, on the node that has it", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const ClockworkTrackHandle h = addGain(t);
    PluginParam p {};
    REQUIRE(clockwork_track_node_param_info(h, 0, &p));
    const std::string name = p.name;

    // Exact, then case-insensitive: a name typed in code is looked up as a
    // human would, and one that matches nothing says so.
    REQUIRE(clockwork_track_param_by_name(t, CLOCKWORK_TRACK_NO_HANDLE, name.c_str(), 1.0, 0));
    std::string upper = name;
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    REQUIRE(clockwork_track_param_by_name(t, CLOCKWORK_TRACK_NO_HANDLE, upper.c_str(), 1.0, 0));
    REQUIRE(clockwork_track_param_by_name(t, CLOCKWORK_TRACK_NO_HANDLE, "no such parameter", 1.0, 0) == 0);
    REQUIRE(clockwork_track_param_by_name(t, h, name.c_str(), 1.0, 0));
    REQUIRE(clockwork_track_param_by_name(CLOCKWORK_TRACK_NONE, CLOCKWORK_TRACK_NO_HANDLE, name.c_str(), 1.0, 0) == 0);
}

TEST_CASE("a set by name is echoed to the edit listener as the host's", "[plugin][track]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const ClockworkTrackHandle h = addGain(t);
    PluginParam p {};
    REQUIRE(clockwork_track_node_param_info(h, 0, &p));

    struct Heard { ClockworkTrackHandle h = 0; uint32_t id = 0; double norm = -1; int own = -1; int n = 0; } heard;
    clockwork_track_set_param_edit_listener(
        [](void* ctx, ClockworkTrackHandle hh, uint32_t id, double norm, int own) {
            auto* hd = static_cast<Heard*>(ctx);
            hd->h = hh; hd->id = id; hd->norm = norm; hd->own = own; hd->n++;
        }, &heard);

    // Half-way up the plugin's own range arrives normalised, flagged as
    // the host's — a panel takes it as a value, not as a gesture.
    const double mid = p.min + 0.5 * (p.max - p.min);
    REQUIRE(clockwork_track_param_by_name(t, CLOCKWORK_TRACK_NO_HANDLE, p.name, mid, 0));
    REQUIRE(heard.n == 1);
    REQUIRE(heard.h == h);
    REQUIRE(heard.id == p.id);
    REQUIRE(heard.norm == Catch::Approx(0.5));
    REQUIRE(heard.own == 0);

    // Past the range it is clamped; by id it is not echoed at all.
    REQUIRE(clockwork_track_param_by_name(t, h, p.name, p.max + 100.0, 0));
    REQUIRE(heard.n == 2);
    REQUIRE(heard.norm == Catch::Approx(1.0));
    clockwork_track_node_param(h, p.id, mid, 0);
    REQUIRE(heard.n == 2);

    clockwork_track_set_param_edit_listener(nullptr, nullptr);
    REQUIRE(clockwork_track_param_by_name(t, h, p.name, mid, 0));
    REQUIRE(heard.n == 2);
}

// ── Audio ────────────────────────────────────────────────────────────────────

TEST_CASE("an empty slot returns silence, not what it held before", "[plugin][track][audio]") {
    CleanTracks clean;
    Staging s;
    for (uint32_t slot = 0; slot < CLOCKWORK_TRACK_MAX; ++slot) { s.poison(slot, 0.7f); s.send(slot, 0.3f); }
    s.run();
    for (uint32_t slot = 0; slot < CLOCKWORK_TRACK_MAX; ++slot) s.requireReturn(slot, 0.0f);
}

TEST_CASE("a lane base of zero is a base like any other", "[plugin][track][audio]") {
    // In the bridge the model runs over its own staging buffers, which begin
    // at zero; zero is not a sentinel.
    CleanTracks clean;
    makeTrack("t");
    Staging s;
    std::fill(s.in[0].begin(), s.in[0].end(), 0.7f);
    std::fill(s.out[0].begin(), s.out[0].end(), 0.3f);
    std::fill(s.out[1].begin(), s.out[1].end(), 0.3f);
    clockwork_track_process(s.inp.data(), kWidth, s.outp.data(), kWidth, kFrames, 0, 0);
    REQUIRE(s.in[0][0] == 0.3f);
    REQUIRE(s.in[1][0] == 0.3f);
}

TEST_CASE("an empty chain is a wire from send to return", "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("wire");
    const uint32_t slot = slotOf(t);
    Staging s;
    s.send(slot, 0.25f);
    s.run();
    s.requireReturn(slot, 0.25f);
    // Only ITS lanes: the neighbour's stayed silent.
    s.requireReturn((slot + 1) % CLOCKWORK_TRACK_MAX, 0.0f);
}

TEST_CASE("one effect is heard", "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const uint32_t slot = slotOf(t);
    const ClockworkTrackHandle h = addGain(t);

    // Plain 1.0 on a 0..2 range: unity, so the effect is provably in the path
    // without the value being the default.
    clockwork_track_node_param(h, 0, 1.0, 0);
    Staging s;
    s.send(slot, 0.5f);
    s.run();
    s.requireReturn(slot, 0.5f);

    clockwork_track_node_param(h, 0, 0.5, 0);
    s.send(slot, 0.5f);
    s.run();
    s.requireReturn(slot, 0.25f);
}

TEST_CASE("effects compose in chain order", "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const uint32_t slot = slotOf(t);
    const ClockworkTrackHandle a = addGain(t);
    const ClockworkTrackHandle b = addGain(t);
    // 0.5 then 0.25 — the product is 0.125, which no single node could produce
    // and which is therefore proof that BOTH ran.
    clockwork_track_node_param(a, 0, 0.5, 0);
    clockwork_track_node_param(b, 0, 0.25, 0);
    Staging s;
    s.send(slot, 1.0f);
    s.run();
    s.requireReturn(slot, 0.125f);
}

TEST_CASE("a bypassed node is skipped and comes back", "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const uint32_t slot = slotOf(t);
    const ClockworkTrackHandle h = addGain(t);
    clockwork_track_node_param(h, 0, 0.5, 0);
    Staging s;

    REQUIRE(clockwork_track_node_bypass(h, 1));
    s.send(slot, 1.0f);
    s.run();
    s.requireReturn(slot, 1.0f);

    // The plugin was never unloaded: its parameter is where it was left.
    REQUIRE(clockwork_track_node_bypass(h, 0));
    s.send(slot, 1.0f);
    s.run();
    s.requireReturn(slot, 0.5f);
}

TEST_CASE("removing a node takes it out of the audio path", "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const uint32_t slot = slotOf(t);
    const ClockworkTrackHandle a = addGain(t);
    const ClockworkTrackHandle b = addGain(t);
    clockwork_track_node_param(a, 0, 0.5, 0);
    clockwork_track_node_param(b, 0, 0.5, 0);
    Staging s;
    s.send(slot, 1.0f);
    s.run();
    REQUIRE(s.ret(slot)[0] == Catch::Approx(0.25f).margin(1e-5));

    // The published chain must be rebuilt, not merely the list: if removal only
    // edited the control-side vector the audio thread would go on hearing it.
    clockwork_track_remove_plugin(b);
    s.send(slot, 1.0f);
    s.run();
    REQUIRE(s.ret(slot)[0] == Catch::Approx(0.5f).margin(1e-5));
}

TEST_CASE("gain and mute shape the return, smoothly", "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("g");
    const uint32_t slot = slotOf(t);
    Staging s;
    s.send(slot, 1.0f);
    s.run();
    s.requireReturn(slot, 1.0f);

    REQUIRE(clockwork_track_set_gain(t, 0.5f));
    s.send(slot, 1.0f);
    s.run();
    // A ramp across the block: the first frame has moved off 1.0, the last
    // has arrived. A step would put every frame at 0.5.
    REQUIRE(s.ret(slot)[0] < 1.0f);
    REQUIRE(s.ret(slot)[0] > 0.5f);
    REQUIRE(s.ret(slot)[kFrames - 1] == Catch::Approx(0.5f).margin(1e-5));
    s.send(slot, 1.0f);
    s.run();
    s.requireReturn(slot, 0.5f);

    REQUIRE(clockwork_track_set_mute(t, 1));
    s.send(slot, 1.0f);
    s.run();
    REQUIRE(s.ret(slot)[kFrames - 1] == Catch::Approx(0.0f).margin(1e-5));
    s.send(slot, 1.0f);
    s.run();
    s.requireReturn(slot, 0.0f);

    REQUIRE(clockwork_track_set_mute(t, 0));
    s.send(slot, 1.0f);
    s.run();
    s.send(slot, 1.0f);
    s.run();
    s.requireReturn(slot, 0.5f);

    REQUIRE(clockwork_track_set_gain(CLOCKWORK_TRACK_NONE, 1.0f) == 0);
}

TEST_CASE("two tracks are two lanes, not one", "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId a = makeTrack("a");
    const ClockworkTrackId b = makeTrack("b");
    const ClockworkTrackHandle h = addGain(b);
    clockwork_track_node_param(h, 0, 0.5, 0);
    Staging s;
    s.send(slotOf(a), 1.0f);
    s.send(slotOf(b), 1.0f);
    s.run();
    s.requireReturn(slotOf(a), 1.0f);
    s.requireReturn(slotOf(b), 0.5f);
}

TEST_CASE("notes and controllers on a track without instruments are harmless",
          "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    addGain(t);
    // An effect is not sent notes. None of these may crash, on a real track
    // or on none.
    clockwork_track_note(t, 1, 0, 60, 0.8f, 0);
    clockwork_track_note(t, 0, 0, 60, 0.0f, 10);
    clockwork_track_cc(t, 0, 1, 0.5f, 0);
    clockwork_track_pitch_bend(t, 0, 0.25f, 0);
    clockwork_track_all_notes_off(t, 0);
    clockwork_track_note(CLOCKWORK_TRACK_NONE, 1, 0, 60, 0.8f, 0);
    clockwork_track_all_notes_off(CLOCKWORK_TRACK_NONE, 0);
    Staging s;
    s.send(slotOf(t), 1.0f);
    s.run();
    s.requireReturn(slotOf(t), 0.5f);
}

TEST_CASE("a chain survives being rebuilt under a running block", "[plugin][track][audio]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("fx");
    const uint32_t slot = slotOf(t);
    Staging s;

    // Edits interleaved with blocks, which is the pattern the publication
    // scheme exists for. Nothing here can prove the absence of a race, but a
    // use-after-free in retire() shows up as a crash or a sanitiser report,
    // and this is the shape that would trigger one.
    std::vector<ClockworkTrackHandle> live;
    for (int round = 0; round < 8; ++round) {
        const ClockworkTrackHandle h = addGain(t);
        clockwork_track_node_param(h, 0, 1.0, 0);
        live.push_back(h);
        s.send(slot, 1.0f);
        s.run();
        REQUIRE(s.ret(slot)[0] == Catch::Approx(1.0f).margin(1e-4));

        if (round % 3 == 2 && !live.empty()) {
            clockwork_track_remove_plugin(live.front());
            live.erase(live.begin());
            s.send(slot, 1.0f);
            s.run();
        }
    }
    REQUIRE(clockwork_track_nodes(t, nullptr, 0) == live.size());
}

// ── Timelines ────────────────────────────────────────────────────────────────

TEST_CASE("a track follows the session clock until bound to a timeline", "[plugin][track][clock]") {
    CleanTracks clean;
    const ClockworkTrackId t = makeTrack("bound");
    ClockworkTrackInfo i {};
    REQUIRE(clockwork_track_info(t, &i));
    CHECK(i.timeline_id == 0);
    CHECK(std::string(i.timeline) == "link");

    // The model holds the id it was given and the name for reporting; the
    // name is the engine's to resolve, so any name goes with any id here.
    REQUIRE(clockwork_track_set_timeline(t, 2, "midi:iac-1"));
    REQUIRE(clockwork_track_info(t, &i));
    CHECK(i.timeline_id == 2);
    CHECK(std::string(i.timeline) == "midi:iac-1");
    std::vector<ClockworkTrackInfo> l(1);
    REQUIRE(clockwork_track_list(l.data(), 1) == 1);
    CHECK(l[0].timeline_id == 2);
    CHECK(std::string(l[0].timeline) == "midi:iac-1");

    REQUIRE(clockwork_track_set_timeline(t, 0, "link"));
    REQUIRE(clockwork_track_info(t, &i));
    CHECK(i.timeline_id == 0);
    CHECK(std::string(i.timeline) == "link");

    // Refused, and the binding stands: no such track, a negative id, a name
    // that is empty or too long.
    REQUIRE(clockwork_track_set_timeline(t, 1, "midi:x"));
    CHECK(clockwork_track_set_timeline(CLOCKWORK_TRACK_NONE, 1, "midi:x") == 0);
    CHECK(clockwork_track_set_timeline(t, -1, "midi:x") == 0);
    CHECK(clockwork_track_set_timeline(t, 1, "") == 0);
    CHECK(clockwork_track_set_timeline(t, 1, nullptr) == 0);
    const std::string tooLong(CLOCKWORK_TRACK_NAME_MAX + 8, 'x');
    CHECK(clockwork_track_set_timeline(t, 1, tooLong.c_str()) == 0);
    REQUIRE(clockwork_track_info(t, &i));
    CHECK(i.timeline_id == 1);
    CHECK(std::string(i.timeline) == "midi:x");
}

namespace {

// The clock and the mirror the tracks are handed live on the test's stack:
// take them back before they go, or the next case's plugins read freed memory.
struct HandedClocks {
    ~HandedClocks() { clockwork_track_set_clock(nullptr); clockwork_track_set_timelines(nullptr, 0); }
};

// A follower timeline as the engine would mirror it: beat 0 at `anchorNtp`.
ClockworkTimeline follower(int id, double bpm, double anchorNtp, int num, int den) {
    ClockworkTimeline t = clockwork_timeline_placeholder(id);
    t.bpm = bpm;
    t.anchor_beat = 0.0;
    t.anchor_ntp = anchorNtp;
    t.playing = 1;
    t.anchored = 1;
    t.meter_num = num;
    t.meter_den = den;
    return t;
}

}  // namespace

TEST_CASE("a bound track's plugins render against the mirrored timeline, the rest against the clock",
          "[plugin][track][clock][audio]") {
    CleanTracks clean;
    HandedClocks handed;
    // Two tracks in slot order, so the second is the last chain the block
    // walks — and the last ProcessContext the test plugin remembers is its.
    const ClockworkTrackId a = makeTrack("on-link");
    const ClockworkTrackId b = makeTrack("on-midi");
    addGain(a);
    addGain(b);
    void* h = dlopen(CLOCKWORK_VST3_TEST_PLUGIN_SO, RTLD_NOW | RTLD_LOCAL);
    REQUIRE(h != nullptr);
    auto last = reinterpret_cast<bool (*)(double*, double*, double*, int*, int*, unsigned*)>(
        dlsym(h, "ClockworkTestGainLastContext"));
    REQUIRE(last != nullptr);

    // The session clock: 120 BPM in 4/4 from NTP 1000. The mirror: slot 0
    // (timeline id 1) at 90 BPM in 3/4 from the same instant.
    ClockworkClockState clock;
    ClockworkClockState::initDefaults(clock);
    clock.setTempo(120.0, 1000.0);
    clock.setTransport(true, 1000.0);
    clockwork_track_set_clock(&clock);
    ClockworkTimelineMirrorSlot mirror[2];
    clockwork::timelineMirrorWrite(mirror[0], follower(1, 90.0, 1000.0, 3, 4));
    clockwork::timelineMirrorWrite(mirror[1], clockwork_timeline_placeholder(-1));   // nothing holds slot 2
    clockwork_track_set_timelines(mirror, 2);

    Staging s;
    const int64_t at = clockwork::ntpToOscTimetag(1010.0);   // 10 s in
    double tempo = 0, beat = 0, barStart = 0;
    int num = 0, den = 0;
    unsigned state = 0;

    // Unbound: the clock. Beat 20, bars of 4 from 0: the bar began at 20.
    s.run(at);
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK(tempo == 120.0);
    CHECK(beat == Catch::Approx(20.0));
    CHECK(num == 4);
    CHECK(barStart == Catch::Approx(20.0));

    // Bound to timeline 1: 90 BPM is beat 15 at 10 s, in bars of 3, from 15.
    REQUIRE(clockwork_track_set_timeline(b, 1, "midi:iac"));
    s.run(at);
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK(tempo == 90.0);
    CHECK(beat == Catch::Approx(15.0));
    CHECK(num == 3);
    CHECK(den == 4);
    CHECK(barStart == Catch::Approx(15.0));

    // The other track still follows the clock: bind it too and it is the
    // last one walked... so swap roles instead: unbind b, bind a, and the
    // last context (b's) is the clock's again while a's is the timeline's.
    REQUIRE(clockwork_track_set_timeline(b, 0, "link"));
    REQUIRE(clockwork_track_set_timeline(a, 1, "midi:iac"));
    s.run(at);
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK(tempo == 120.0);
    CHECK(num == 4);

    // Bound to a slot nothing holds, an id past the mirror, or with no mirror
    // at all: the 60 BPM placeholder every unheld timeline answers with,
    // not the clock — the binding is honoured.
    for (int id : {2, 7}) {
        REQUIRE(clockwork_track_set_timeline(b, id, "midi:ghost"));
        s.run(at);
        REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
        CHECK(tempo == 60.0);
        CHECK(num == 4);
        CHECK(beat == Catch::Approx(1010.0));   // from the epoch, one beat a second
    }
    REQUIRE(clockwork_track_set_timeline(b, 1, "midi:iac"));
    clockwork_track_set_timelines(nullptr, 0);
    s.run(at);
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK(tempo == 60.0);

    // The mirror moves under a running bridge: the next block sees it.
    clockwork_track_set_timelines(mirror, 2);
    clockwork::timelineMirrorWrite(mirror[0], follower(1, 100.0, 1000.0, 7, 8));
    s.run(at);
    REQUIRE(last(&tempo, &beat, &barStart, &num, &den, &state));
    CHECK(tempo == 100.0);
    CHECK(num == 7);
    CHECK(den == 8);
    dlclose(h);
}

// ── Rigs ─────────────────────────────────────────────────────────────────────

TEST_CASE("a rig round-trips through JSON", "[plugin][track][rig]") {
    CleanTracks clean;
    const ClockworkTrackId a = makeTrack("bass");
    const ClockworkTrackId b = makeTrack("lead");
    const ClockworkTrackHandle h1 = addGain(a);
    const ClockworkTrackHandle h2 = addGain(a);
    clockwork_track_node_param(h1, 0, 0.25, 0);
    clockwork_track_node_param(h2, 0, 1.5, 0);
    REQUIRE(clockwork_track_node_bypass(h2, 1));
    REQUIRE(clockwork_track_set_gain(b, 0.75f));
    REQUIRE(clockwork_track_set_mute(b, 1));
    REQUIRE(clockwork_track_move(b, 0));

    // Parameter changes reach the plugin on its next block; state is read
    // from the plugin, so give it one.
    Staging s;
    s.run();

    std::string json(clockwork_track_rig_to_json(nullptr, 0) + 1, '\0');
    clockwork_track_rig_to_json(&json[0], static_cast<uint32_t>(json.size()));
    json.resize(std::strlen(json.c_str()));
    REQUIRE(json.find("\"clockwork_rig\"") != std::string::npos);
    REQUIRE(json.find("\"bass\"") != std::string::npos);

    clockwork_track_clear();
    REQUIRE(clockwork_track_count() == 0);

    uint32_t missing = 99;
    const char* err = nullptr;
    REQUIRE(clockwork_track_rig_from_json(json.c_str(), kSampleRate, kFrames, &missing, &err));
    REQUIRE(missing == 0);

    // Everything back: order, names, gain, mute, chains, bypass — and the
    // plugin's state, which is the part a user would notice missing.
    std::vector<ClockworkTrackInfo> l(2);
    REQUIRE(clockwork_track_list(l.data(), 2) == 2);
    REQUIRE(std::string(l[0].name) == "lead");
    REQUIRE(l[0].gain == Catch::Approx(0.75f));
    REQUIRE(l[0].mute == 1);
    REQUIRE(l[0].node_count == 0);
    REQUIRE(std::string(l[1].name) == "bass");
    REQUIRE(l[1].node_count == 2);

    std::vector<ClockworkTrackNode> n(2);
    REQUIRE(clockwork_track_nodes(l[1].id, n.data(), 2) == 2);
    REQUIRE(n[0].bypass == 0);
    REQUIRE(n[1].bypass == 1);

    // Loaded ids are fresh, never the saved ones.
    REQUIRE(l[0].id != a);
    REQUIRE(l[0].id != b);

    // The saved parameter came back with the state: 0.25 on the first node,
    // the second bypassed, so a 1.0 send returns 0.25.
    Staging s2;
    s2.send(l[1].slot, 1.0f);
    s2.run();
    s2.requireReturn(l[1].slot, 0.25f);
}

TEST_CASE("a rig with a plugin that cannot be found loads around it", "[plugin][track][rig]") {
    CleanTracks clean;
    const char* json =
        "{\"clockwork_rig\":1,\"plugin_folders\":[],\"tracks\":[{\"name\":\"x\",\"gain\":1,\"mute\":0,"
        "\"chain\":[{\"format\":\"vst3\",\"id\":\"no-such-id\",\"name\":\"Gone\",\"vendor\":\"\","
        "\"path\":\"/no/such/plugin.vst3\",\"index\":0,\"bypass\":0,\"state\":\"\"}]}]}";
    uint32_t missing = 0;
    const char* err = nullptr;
    REQUIRE(clockwork_track_rig_from_json(json, kSampleRate, kFrames, &missing, &err));
    REQUIRE(missing == 1);
    REQUIRE(clockwork_track_count() == 1);
    REQUIRE(clockwork_track_find("x") != CLOCKWORK_TRACK_NONE);
}

TEST_CASE("a rig that is not a rig is refused and changes nothing", "[plugin][track][rig]") {
    CleanTracks clean;
    makeTrack("keep");
    uint32_t missing = 0;
    const char* err = nullptr;
    REQUIRE(clockwork_track_rig_from_json("not json", kSampleRate, kFrames, &missing, &err) == 0);
    REQUIRE(err != nullptr);
    REQUIRE(clockwork_track_rig_from_json("{\"tracks\":[]}", kSampleRate, kFrames, &missing, &err) == 0);
    REQUIRE(clockwork_track_count() == 1);
    REQUIRE(clockwork_track_rig_from_json(nullptr, kSampleRate, kFrames, &missing, &err) == 0);
}

TEST_CASE("a rig file saves and loads", "[plugin][track][rig]") {
    CleanTracks clean;
    makeTrack("disk");
    const std::string path = (std::filesystem::temp_directory_path() / "clockwork_track_rig_test.json").string();
    const char* err = nullptr;
    REQUIRE(clockwork_track_rig_save(path.c_str(), &err));
    clockwork_track_clear();
    uint32_t missing = 0;
    REQUIRE(clockwork_track_rig_load(path.c_str(), kSampleRate, kFrames, &missing, &err));
    REQUIRE(clockwork_track_find("disk") != CLOCKWORK_TRACK_NONE);
    REQUIRE(clockwork_track_rig_load("/no/such/dir/rig.json", kSampleRate, kFrames, &missing, &err) == 0);
    REQUIRE(err != nullptr);
}

// ── Load listener ────────────────────────────────────────────────────────────

TEST_CASE("the load listener sees the path before an open and NULL after", "[plugin][track]") {
    CleanTracks clean;
    struct Seen { std::vector<std::string> paths; int nulls = 0; } seen;
    clockwork_track_set_load_listener([](void* ctx, const char* path) {
        auto* s = static_cast<Seen*>(ctx);
        if (path) s->paths.push_back(path); else ++s->nulls;
    }, &seen);
    const ClockworkTrackId t = makeTrack("l");
    const char* err = nullptr;
    clockwork_track_add_plugin(t, "/no/such/plugin.vst3", 0, 0, kSampleRate, kFrames, &err);
    clockwork_track_set_load_listener(nullptr, nullptr);
    REQUIRE(seen.paths == std::vector<std::string>{"/no/such/plugin.vst3"});
    REQUIRE(seen.nulls == 1);
}
