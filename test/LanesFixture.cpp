// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * LanesFixture.cpp — see LanesFixture.h.
 */
#include "LanesFixture.h"
#include <catch2/catch_test_macros.hpp>
#include "clockwork_client.h"
#include "metrics_schema.h"
#include "clockwork_osc.h"

#include "lanes/lanes.h"
#include "audio_processor.h"   // destroy_dsp, rebuild_dsp
#include "guest_fence.h"       // armed here: one thread, so the fence can hold every region
#include "dsp_api.h"      // DspFpEnv
#include "OscTestUtils.h"

#include <algorithm>
#include <cstring>

namespace lanes_test {
namespace {

uint32_t   gBlockSize  = 0;
uint64_t   gSamplePos  = 0;
uint64_t   gDspOrigin  = 0;   // gSamplePos when freshDsp() last made an instance
bool       gBooted     = false;
double     gBaseNtp    = kEpochNtp;
bool       gResume     = false;   // another host rendered since this fixture last ticked (ticked())

void drainInto(std::vector<EgressFrame>& out,
               uint32_t (*drain)(ClockworkEgressFn, void*, uint32_t)) {
    drain([](void* ctx, uint32_t sourceId, uint32_t route,
             const uint8_t* osc, uint32_t len, uint32_t seq) {
        auto* v = static_cast<std::vector<EgressFrame>*>(ctx);
        // Every frame must decode with the parser the clients read with.
        INFO("an egress frame of " << len << " bytes the client parser (clockwork-osc) would not decode");
        CHECK(clockwork_osc_decodes(osc, len) == 1);
        EgressFrame f;
        f.sourceId = sourceId;
        f.route    = route;
        f.seq      = seq;
        f.data.assign(osc, osc + len);
        v->push_back(std::move(f));
    }, &out, /*max_frames=*/0);
}

} // namespace

// Reserved for the guest. Static rather than heap-allocated so its lifetime is
// the process's: clockwork holds the pointer across every rebuild, so a
// region with a shorter lifetime would be a use-after-free.
alignas(16) static uint8_t gGuestMemory[kGuestMemoryBytes];
alignas(16) static uint8_t gAssets[kAssetsBytes];
alignas(16) static uint8_t gReturns[kReturnsBytes];

const uint8_t* guestMemory() { return gGuestMemory; }
namespace {
// Open a client on the engine's arena (not the guest's reservation) to read from it.
ClockworkClient* openClient() {
    ClockworkStatus st = CLOCKWORK_OK;
    uint8_t* const arena = static_cast<uint8_t*>(get_shared_memory_base());
    REQUIRE(arena != nullptr);
    ClockworkClient* c = clockwork_client_open_memory(arena, reinterpret_cast<const ClockworkArenaHeader*>(arena)->arena_bytes, &st);
    REQUIRE(c != nullptr);
    return c;
}

// Another host (clockwork_embed_render) ticked the engine on the real wall
// clock, so the engine's clock is now ahead of this fixture's timeline. Rebase
// gBaseNtp so that ntpAtFrame(gSamplePos) equals the sample clock the engine
// last published, and tell the engine this host's audio is starting: the next
// tick re-anchors at exactly that time (the engine floors an anchor at the
// published sample clock), so ntpAtFrame stays exact.
// The sample clock the engine last published, through a client: its NTP
// time for the first frame of the last block rendered, 0 before the first.
double readPublishedNtp() {
    ClockworkClient* c = openClient();
    ClockworkClientSampleClock sc {};
    sc.struct_bytes = sizeof sc;
    const ClockworkStatus st = clockwork_client_sample_clock(c, &sc);
    clockwork_client_close(c);
    return st == CLOCKWORK_OK ? sc.dac_ntp : 0.0;
}

void resume() {
    gResume = false;
    const double published = readPublishedNtp();
    if (published > 0.0)
        gBaseNtp = std::max(gBaseNtp, published - static_cast<double>(gSamplePos) / kSampleRate);
    clockwork_audio_starts(ntpAtFrame(gSamplePos), /*monotonic_secs*/ 0.0);
}
} // namespace

double baseNtp() {
    boot();
    return gBaseNtp;
}

double publishedNtp() {
    boot();
    return readPublishedNtp();
}

uint32_t metricWord(uint32_t offset) {
    ClockworkClient* c = openClient();
    uint32_t words[clockwork::metrics_schema::kMetricWords] = {};
    const uint32_t n = clockwork_client_metrics(c, words, clockwork::metrics_schema::kMetricWords);
    clockwork_client_close(c);
    REQUIRE(offset < n);
    return words[offset];
}
uint8_t*       assets()       { return gAssets; }
const uint8_t* returns()      { return gReturns; }

uint32_t boot() {
    if (gBooted) {
        if (gResume) resume();
        return gBlockSize;
    }
    // Before clockwork_init: the reservation is read at every init_memory, and
    // widens the channel counts the DSP is allocated for.
    if (kReservedLanes > 0) clockwork_reserve_lanes(kReservedLanes);
    if (kLinkAudioLanes > 0) clockwork_reserve_link_audio_lanes(kLinkAudioLanes);
    // This thread is the only one that touches the arena, so the guest fence
    // can hold every region the guest was not handed (guest_fence.h); in a
    // build without AddressSanitizer this arms nothing.
    clockwork_guest_fence_arm(true);
    // This fixture arms nothing on the thread that ticks, so denormals are
    // honoured, and it says so — the guest reports what it was told
    // (test_dsp_regions.cpp), which is the declaration's whole job.
    clockwork_declare_fp_env(DSP_FP_ENV_DENORMALS_HONOURED);
    // A full host: clockwork's geometry as arguments, a config block for the
    // guest, an arena reserved for the guest's own memory, and the two
    // one-way bulk staging regions.
    clockwork_init(kSampleRate,
             kBufLength,
             kInChannels,
             kOutChannels,
             /*verbosity*/ 0,
             gGuestMemory,
             kGuestMemoryBytes,
             kGuestConfigSlots,
             static_cast<uint32_t>(sizeof kGuestConfigSlots),
             // No placement span: this fixture's allocator is the system one,
             // which is what every native host passes.
             /*arena_bytes*/ 0,
             gAssets,  kAssetsBytes,
             gReturns, kReturnsBytes);
    gBlockSize = clockwork_block_size();
    gBooted    = true;
    return gBlockSize;
}

void ticked(uint64_t frames) {
    gSamplePos += frames;
    gResume = true;
}

uint64_t position() { return gSamplePos; }

uint64_t freshDsp() {
    boot();
    destroy_dsp();
    rebuild_dsp(kSampleRate, kBufLength);
    gDspOrigin = gSamplePos;
    return gSamplePos;
}

void comeBack(double rate, uint32_t block, uint32_t inputs, uint32_t outputs, const uint32_t (&config)[4]) {
    boot();
    const uint32_t before = clockwork_arena_generation(clockwork_arena_header());
    clockwork_init(rate, block, inputs, outputs, /*verbosity*/ 0,
                   gGuestMemory, kGuestMemoryBytes,
                   config, static_cast<uint32_t>(sizeof config),
                   /*arena_bytes*/ 0,
                   gAssets,  kAssetsBytes,
                   gReturns, kReturnsBytes);
    gBlockSize = clockwork_block_size();
    // A new instance counts its frames from here, as freshDsp's does.
    if (clockwork_arena_generation(clockwork_arena_header()) != before) gDspOrigin = gSamplePos;
}

uint64_t dspFrame(uint64_t streamFrame) { return streamFrame - gDspOrigin; }

uint64_t tick() {
    boot();
    const uint64_t at  = gSamplePos;
    const double   ntp = ntpAtFrame(gSamplePos);
    ClockworkBlockTime time {};
    time.struct_bytes = sizeof time;
    time.audio_frames = gSamplePos;
    time.wall_ntp     = ntp;   // its own time for a wall, and no monotonic clock: a fixture sees no gap
    clockwork_tick(&time, kOutChannels, kInChannels);
    gSamplePos += gBlockSize;
    return at;
}

std::vector<EgressFrame> drainRt() {
    std::vector<EgressFrame> out;
    drainInto(out, &clockwork_egress_rt_drain);
    return out;
}

std::vector<EgressFrame> drainNrt() {
    std::vector<EgressFrame> out;
    drainInto(out, &clockwork_egress_nrt_drain);
    return out;
}

bool ingress(const uint8_t* osc, uint32_t len, uint32_t sourceId) {
    boot();
    return clockwork_ingress_write(osc, len, sourceId);
}

std::vector<EgressFrame> tickUntil(int maxBlocks, const char* address) {
    std::vector<EgressFrame> all;
    bool found = false;
    for (int i = 0; i < maxBlocks && !found; ++i) {
        tick();
        for (auto& f : drainRt()) {
            if (address != nullptr &&
                osc_test::parseAddress(f.data.data(),
                                       static_cast<uint32_t>(f.data.size())) == address)
                found = true;
            all.push_back(std::move(f));
        }
    }
    return all;   // everything drained; the caller looks for its address in it
}

} // namespace lanes_test
