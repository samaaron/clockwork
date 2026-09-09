// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_dsp_boundary.cpp — the message half of src/dsp_api.h, driven end to end.
 *
 * test_audio_path.cpp pins the sample half: bytes written by dsp_process
 * arriving at the hardware join. This file pins the other direction and the
 * declaration that governs it:
 *
 *   - a message crossing the boundary and a reply coming back. "/dummy/ping" goes
 *     in the IN ring, out through clockwork's classifier and default route
 *     to dsp_osc(), and the DSP answers through DspHost::emit_osc with the
 *     origin it was handed — which must arrive on the RT egress ring, routed
 *     to that same origin. Nothing in that chain is a clockwork type: the boundary
 *     carries a uint32 token in and a uint32 token out, where it used to carry
 *     the previous engine's reply-address struct holding a function pointer.
 *
 *   - dsp_describe(), and the one branch it governs. holds_schedule is read
 *     once at boot and decides where a timed message waits; the placeholder
 *     declares zero, so clockwork holds it, which is what the scheduled case
 *     below observes.
 *
 * The reply is asserted for its ROUTE and ORIGIN as well as its address,
 * because an answer that reaches the wrong client is a bug a
 * "did /dummy/pong come back?" assertion cannot see: with a single test client
 * and a broadcast route it would pass just as happily.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "lanes/lanes.h"
#include "shared_memory.h"   // EgressRoute
#include "dsp_api.h"
#include "clock/clock_math.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <catch2/catch_approx.hpp>
#include <algorithm>

using Catch::Approx;
#include <vector>

namespace {

// A token that is not 0, because 0 means "broadcast" and would make the
// routing assertion below vacuous.
constexpr uint32_t kClient = 0x5eed;

const lanes_test::EgressFrame* findAddress(
    const std::vector<lanes_test::EgressFrame>& frames, const char* address) {
    for (const auto& f : frames) {
        if (osc_test::parseAddress(f.data.data(),
                                   static_cast<uint32_t>(f.data.size())) == address)
            return &f;
    }
    return nullptr;
}

} // namespace

TEST_CASE("dsp boundary: a message crosses the boundary and its reply comes back",
          "[boundary][lanes]") {
    lanes_test::boot();
    lanes_test::drainRt();   // start from a clean egress ring

    const auto ping = osc_test::message("/dummy/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));

    // Written before the tick, so this tick drains it, dispatches it and
    // writes the reply — one block, no polling.
    const auto frames = lanes_test::tickUntil(4, "/dummy/pong");
    const auto* pong  = findAddress(frames, "/dummy/pong");

    REQUIRE(pong != nullptr);
    // Routed back to the sender, not broadcast: DspHost::emit_osc was called
    // with the origin dsp_osc() was given, and clockwork turned that into a
    // reply frame addressed to it.
    CHECK(pong->sourceId == kClient);
    CHECK(pong->route    == EGRESS_REPLY);
}

TEST_CASE("dsp boundary: the clock snapshot is answered on the audio thread",
          "[boundary][lanes][clock]") {
    // A caller that wants the clock the CURRENT BLOCK is rendered against
    // cannot wait for a control-thread hop, so this verb is answered inline on
    // the audio thread from the published mirror.
    //
    // One reply, not six: bpm and beat origin only mean anything together, and
    // a client assembling them from separate answers can catch a tempo change
    // between two of them.
    lanes_test::boot();
    lanes_test::drainRt();

    const auto ask = osc_test::message("/clockwork/clock/state/get");
    REQUIRE(lanes_test::ingress(ask.ptr(), ask.size(), kClient));

    const auto frames = lanes_test::tickUntil(4, "/clockwork/clock/state.reply");
    const auto* reply = findAddress(frames, "/clockwork/clock/state.reply");
    REQUIRE(reply != nullptr);
    CHECK(reply->sourceId == kClient);   // to the asker, not the notify audience

    const auto r = osc_test::parseReply(reply->data.data(),
                                        static_cast<uint32_t>(reply->data.size()));
    // A running clock: a tempo that is a tempo, and a meter that is a meter.
    CHECK(r.argDouble(0) > 0.0);         // bpm
    CHECK(r.argInt(5) > 0);              // meter_num
    CHECK(r.argInt(6) > 0);              // meter_den

    // The DSP never saw it — the prefix decided that before the guest was
    // consulted, which is what makes this clockwork's answer and not scsynth's.
    for (const auto& f : frames)
        CHECK(osc_test::parseAddress(f.data.data(),
                                     static_cast<uint32_t>(f.data.size())) != "/dummy/pong");
}

TEST_CASE("dsp boundary: an address clockwork claims never reaches the DSP",
          "[boundary][lanes]") {
    // The other half of the namespace rule (docs/BOUNDARY.md §3). "/clockwork/" is
    // answered by clockwork on the audio thread; the DSP is not consulted
    // and cannot answer, so a "/clockwork/" verb that looked like a DSP verb
    // must still come back from clockwork.
    lanes_test::boot();
    lanes_test::drainRt();

    const auto ping = osc_test::message("/clockwork/ping");
    REQUIRE(lanes_test::ingress(ping.ptr(), ping.size(), kClient));

    const auto frames = lanes_test::tickUntil(4, "/clockwork/pong");
    const auto* pong  = findAddress(frames, "/clockwork/pong");
    REQUIRE(pong != nullptr);
    CHECK(pong->sourceId == kClient);

    // And nothing under that prefix was answered by the DSP as well — one
    // claim, not two.
    CHECK(findAddress(frames, "/dummy/pong") == nullptr);
}

TEST_CASE("dsp boundary: dsp_describe answers before any instance exists",
          "[boundary]") {
    // Static by contract: a host can report what it is linked against without
    // booting audio. Clockwork reads holds_schedule from here exactly once,
    // and the whole schedule branch turns on it.
    const DspInfo* info = dsp_describe();
    REQUIRE(info != nullptr);
    REQUIRE(info->name != nullptr);
    CHECK(std::string(info->name) == "dummy");
    CHECK(info->version != nullptr);
    // The placeholder keeps no schedule, so clockwork holds timed messages
    // for it — which is what the scheduled case below relies on.
    CHECK(info->holds_schedule == 0);
    // The memory wants, the one thing the declaration has ever grown by. They
    // are static for the same reason holds_schedule is: a host checks them
    // against its memory profile before booting audio, and refuses rather
    // than spills (test_dsp_regions.cpp). The placeholder's are modest so
    // every host in this tree meets them.
    CHECK(info->arena_bytes_wanted == 512u * 1024u);
    CHECK(info->arena_bulk_bytes_wanted == 128u * 1024u);
    // Five fields, and that is the whole declaration. Before the wants it had
    // only ever shrunk: verbs naming a guest's definitions went when
    // DspConfig::persistent took over surviving a rebuild
    // (test_dsp_regions.cpp); stream_verb went when the channel map took over
    // saying which channel is which, which a guest reads rather than is told
    // (test_channel_map.cpp).
}

// Only in a build that HAS a store to wait in. The other configuration —
// CLOCKWORK_SCHEDULER=OFF, where this same DSP asks clockwork to hold and
// it cannot — is pinned by test_scheduler_absent.cpp, which asserts the
// message arrives at once with its timetag intact instead.
#if CLOCKWORK_SCHEDULER
TEST_CASE("dsp boundary: a timed message waits in clockwork and arrives on time",
          "[boundary][lanes][timing]") {
    // holds_schedule is 0, so clockwork parks a timestamped bundle and
    // releases it in the block it comes due. Observed through the pulse grid,
    // which is the one thing about the placeholder that is checkable to the
    // frame: the bundle carries a reconfigure, so "did it fire yet" is
    // answerable from the samples rather than from a log line.
    const uint32_t bl = lanes_test::boot();

    // Due a good many blocks out — far enough that a harness which fired it
    // immediately, or forwarded it straight to the DSP, is unmistakable.
    constexpr int kBlocksAhead = 40;
    const uint64_t now = lanes_test::tick();
    const uint64_t dueFrame = now + static_cast<uint64_t>(kBlocksAhead) * bl;

    const auto inner = osc_test::message("/dummy/pulse", 5, 100);
    const auto pkt   = osc_test::bundle(
        static_cast<uint64_t>(clockwork::ntpToOscTimetag(
            lanes_test::ntpAtFrame(dueFrame))),
        { inner });
    REQUIRE(lanes_test::ingress(pkt.ptr(), pkt.size(), kClient));

    // The next block drains the bundle. It must NOT have taken effect: the
    // left channel's pulse grid is still the 24000-frame default, so a sample
    // that would be silent on the new 4800-frame grid but sounding on the old
    // one (or the other way round) still follows the old one. Rather than
    // hunting for such a frame, assert the whole block against the old grid.
    constexpr uint64_t kOldPeriod = 24000, kOldWidth = 480;   // 500 ms / 10 ms @ 48k
    constexpr uint64_t kNewPeriod = 4800,  kNewWidth = 240;   // 100 ms /  5 ms @ 48k
    const auto onOld = [](uint64_t n) { return (n % kOldPeriod) < kOldWidth; };
    const auto onNew = [](uint64_t n) { return (n % kNewPeriod) < kNewWidth; };
    // Left channel only: it is the positive one, and its grid is the whole
    // question here. (test_audio_path.cpp pins the channel relationship.)

    {
        const uint64_t at  = lanes_test::tick();
        const float*   out = clockwork_audio_out();
        for (uint32_t i = 0; i < bl; ++i)
            REQUIRE((out[i] != 0.0f) == onOld(at + i));
    }

    // Tick well past the due time, then check the grid moved. Deliberately not
    // pinned to one exact block: the fire window is [block start, block start +
    // one block) in OSC time, and which side of a boundary a given NTP double
    // rounds to is not the contract. That the message waited, and then arrived,
    // is.
    for (int i = 0; i < kBlocksAhead + 4; ++i) lanes_test::tick();

    {
        const uint64_t at  = lanes_test::tick();
        const float*   out = clockwork_audio_out();
        for (uint32_t i = 0; i < bl; ++i)
            REQUIRE((out[i] != 0.0f) == onNew(at + i));
    }

    // Restore the default for whatever runs next — the fixture is a process
    // singleton and this case has just moved a global.
    const auto restore = osc_test::message("/dummy/pulse", 10, 500);
    REQUIRE(lanes_test::ingress(restore.ptr(), restore.size(), 0));
    lanes_test::tick();
    lanes_test::drainRt();
    {
        const uint64_t at  = lanes_test::tick();
        const float*   out = clockwork_audio_out();
        for (uint32_t i = 0; i < bl; ++i)
            REQUIRE((out[i] != 0.0f) == onOld(at + i));
    }
}
#endif // CLOCKWORK_SCHEDULER

TEST_CASE("dsp boundary: input reaches the DSP on the channel the host filled",
          "[boundary][lanes][audio]") {
    // The input half of dsp_process, which nothing else here touches.
    //
    // Clockwork owns this buffer now. get_audio_input_bus() used to return
    // an address computed INSIDE the DSP's own signal storage — base +
    // outputs*block, the previous engine's internal channel layout, arithmetic
    // clockwork had no business doing on memory it did not own. It is now
    // clockwork's own channel-major staging, and dsp_process is handed
    // channel pointers into it. If that wiring were wrong the DSP would read silence, or the wrong
    // channel, or memory belonging to something else — and nothing but this
    // would notice.
    //
    // The placeholder's echo mode makes it observable: output channel c is
    // input channel c, verbatim, so a distinguishable value per channel says
    // exactly which input landed where.
    const uint32_t bl = lanes_test::boot();
    REQUIRE(lanes_test::kInChannels >= 2);

    const auto echo = osc_test::message("/dummy/echo");
    REQUIRE(lanes_test::ingress(echo.ptr(), echo.size(), 0));
    lanes_test::tick();          // the block that applies it
    lanes_test::drainRt();

    // A different constant per channel, and a ramp within each, so a
    // half-block copy or an off-by-one offset is visible too.
    float* in = clockwork_audio_in();
    REQUIRE(in != nullptr);
    for (uint32_t ch = 0; ch < lanes_test::kInChannels; ++ch)
        for (uint32_t i = 0; i < bl; ++i)
            in[ch * bl + i] = static_cast<float>(ch + 1) + static_cast<float>(i) / 1000.0f;

    lanes_test::tick();
    const float* out = clockwork_audio_out();
    REQUIRE(out != nullptr);
    for (uint32_t ch = 0; ch < lanes_test::kInChannels; ++ch)
        for (uint32_t i = 0; i < bl; ++i)
            REQUIRE(out[ch * bl + i] ==
                    static_cast<float>(ch + 1) + static_cast<float>(i) / 1000.0f);

    // Put the placeholder back for whatever runs next.
    const auto restore = osc_test::message("/dummy/pulse", 10, 500);
    REQUIRE(lanes_test::ingress(restore.ptr(), restore.size(), 0));
    lanes_test::tick();
    lanes_test::drainRt();
}

// ── Channel counts move while running ────────────────────────────────────────
//
// Ableton Link peers join and leave, aux sinks come and go, a device switch
// brings a different interface. So the counts arrive per block rather than in
// the config, and a channel index is a STABLE SLOT: a stream ending must not
// renumber the streams after it, because renumbering silently hands the DSP
// somebody else's audio on a channel it is already processing.
//
// Driven straight against the DSP rather than through clockwork: the
// contract under test is dsp_api.h's, and going direct means the assertions
// are about the boundary and not about a device backend.

namespace {

struct DirectDsp {
    DspConfig cfg{};
    struct Dsp* dsp = nullptr;

    DirectDsp(uint32_t max_in, uint32_t max_out) {
        cfg.sample_rate = 48000.0;
        cfg.block_size = 64;
        cfg.max_input_channels = max_in;
        cfg.max_output_channels = max_out;
        const char* err = nullptr;
        dsp = dsp_new(&cfg, nullptr, &err);
        REQUIRE(dsp != nullptr);
    }
    ~DirectDsp() { dsp_free(dsp); }
};

} // namespace

TEST_CASE("dsp boundary: channel counts are per block, and slots are stable",
          "[dsp][channels]") {
    constexpr uint32_t kFrames = 64;
    constexpr uint32_t kMax = 4;
    DirectDsp d(kMax, kMax);

    // Distinguishable input per slot: slot c carries the constant c + 1.
    std::vector<std::vector<float>> in_store(kMax, std::vector<float>(kFrames, 0.0f));
    std::vector<std::vector<float>> out_store(kMax, std::vector<float>(kFrames, -99.0f));
    const float* in_ptrs[kMax];
    float* out_ptrs[kMax];
    for (uint32_t c = 0; c < kMax; ++c) {
        for (uint32_t i = 0; i < kFrames; ++i) in_store[c][i] = static_cast<float>(c + 1);
        in_ptrs[c] = in_store[c].data();
        out_ptrs[c] = out_store[c].data();
    }

    // Echo mode, so what comes out names the input slot it came from.
    const auto echo = osc_test::message("/dummy/echo");
    // when=1 (immediately), origin=0, block_time=0 — the same block time the
    // dsp_process calls below render at.
    dsp_osc(d.dsp, echo.ptr(), echo.size(), 1, 0, 0);

    SECTION("a slot keeps its identity when the count shrinks") {
        dsp_process(d.dsp, in_ptrs, 4, out_ptrs, 4, kFrames, 0);
        for (uint32_t c = 0; c < 4; ++c) {
            REQUIRE(out_store[c][0] == Approx(static_cast<float>(c + 1)));
        }

        // Two streams end. The survivors must NOT be renumbered: slot 0 is
        // still input 0, not whatever slid down into it.
        for (auto& o : out_store) std::fill(o.begin(), o.end(), -99.0f);
        dsp_process(d.dsp, in_ptrs, 2, out_ptrs, 2, kFrames, 0);
        REQUIRE(out_store[0][0] == Approx(1.0f));
        REQUIRE(out_store[1][0] == Approx(2.0f));

        // And nothing was written above the live count.
        REQUIRE(out_store[2][0] == Approx(-99.0f));
        REQUIRE(out_store[3][0] == Approx(-99.0f));
    }

    SECTION("growing back is not a rebuild") {
        dsp_process(d.dsp, in_ptrs, 1, out_ptrs, 1, kFrames, 0);
        REQUIRE(out_store[0][0] == Approx(1.0f));

        // A peer arrives mid-session. No dsp_new, no dropout: the next block
        // simply carries more channels.
        dsp_process(d.dsp, in_ptrs, 4, out_ptrs, 4, kFrames, 0);
        for (uint32_t c = 0; c < 4; ++c) {
            REQUIRE(out_store[c][0] == Approx(static_cast<float>(c + 1)));
        }
    }

    SECTION("an output with no matching input is silent, not stale") {
        // Three outputs, one input: the two unmatched outputs must be written
        // silent rather than left holding whatever was in the buffer.
        dsp_process(d.dsp, in_ptrs, 1, out_ptrs, 3, kFrames, 0);
        REQUIRE(out_store[0][0] == Approx(1.0f));
        REQUIRE(out_store[1][0] == Approx(0.0f));
        REQUIRE(out_store[2][0] == Approx(0.0f));
    }

    SECTION("no input at all is silence, not a crash") {
        dsp_process(d.dsp, nullptr, 0, out_ptrs, 2, kFrames, 0);
        REQUIRE(out_store[0][0] == Approx(0.0f));
        REQUIRE(out_store[1][0] == Approx(0.0f));
    }
}

// ── DspHost::log — the third door out ───────────────────────────────────────

TEST_CASE("dsp boundary: what the DSP logs leaves as an addressed OSC message",
          "[boundary][lanes]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // The three ways out of a DSP are not interchangeable and this is the one
    // with no addressee at all. emit_osc answers an origin; send_sink names an
    // endpoint; log has neither, so clockwork has to give it one — it frames
    // the line as /clockwork/debug and puts it on the same egress ring as
    // everything else, which is what lets a host dispatch it to a console
    // without a second transport existing for diagnostics.
    const auto say = osc_test::message("/dummy/log", "boundary log probe");
    REQUIRE(lanes_test::ingress(say.ptr(), say.size(), kClient));

    const auto frames = lanes_test::tickUntil(4, "/clockwork/debug");
    const auto* line  = findAddress(frames, "/clockwork/debug");
    REQUIRE(line != nullptr);

    // The text arrived intact, as a string argument. Asserting the CONTENT and
    // not merely that something showed up: the ring carries clockwork's own
    // boot chatter too, and "a /clockwork/debug frame exists" would pass against
    // a log callback that dropped every line a DSP gave it.
    const auto r = osc_test::parseReply(line->data.data(),
                                        static_cast<uint32_t>(line->data.size()));
    REQUIRE(r.argString(0) == "boundary log probe");
}
