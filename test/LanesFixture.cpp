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
#include "guest_fence.h"       // armed here: one thread per core, so the fence can hold every region
#include "dsp_api.h"      // DspFpEnv
#include "OscTestUtils.h"

#include <algorithm>
#include <cstring>

namespace lanes_test {

// Reserved for the guest, and the two bulk staging regions. A lane's for as
// long as it lives: clockwork holds the pointers across every rebuild, and
// the lane closes its core before it lets them go.
struct alignas(16) Lane::Buffers {
    uint8_t guest[kGuestMemoryBytes];
    uint8_t assets[kAssetsBytes];
    uint8_t returns[kReturnsBytes];
};

Lane::Lane(size_t guestBulkBytes, const ClockworkProfile* profile) : mBuffers(std::make_unique<Buffers>()) {
    ClockworkStatus made = CLOCKWORK_OK;
    mCore = clockwork_core_new(&made);
    if (profile) {
        const char* why = clockwork_set_profile(mCore, profile);
        INFO((why ? why : ""));
        REQUIRE(why == nullptr);
    }
    if (guestBulkBytes) clockwork_set_guest_bulk_bytes(mCore, guestBulkBytes);
    // Before clockwork_init: the reservation is read at every init_memory, and
    // widens the channel counts the DSP is allocated for.
    if (kReservedLanes > 0) clockwork_reserve_lanes(mCore, kReservedLanes);
    if (kLinkAudioLanes > 0) clockwork_reserve_link_audio_lanes(mCore, kLinkAudioLanes);
    // One thread at a time touches a lane's arena, so the guest fence can hold
    // every region the guest was not handed (guest_fence.h); in a build
    // without AddressSanitizer this arms nothing.
    clockwork_guest_fence_arm(true);
    // This fixture arms nothing on the thread that ticks, so denormals are
    // honoured, and it says so — the guest reports what it was told
    // (test_dsp_regions.cpp), which is the declaration's whole job.
    clockwork_declare_fp_env(mCore, DSP_FP_ENV_DENORMALS_HONOURED);
    // A full host: clockwork's geometry as arguments, a config block for the
    // guest, an arena reserved for the guest's own memory, and the two
    // one-way bulk staging regions.
    init(kSampleRate, kBufLength, kInChannels, kOutChannels, kGuestConfigSlots);
}

Lane::~Lane() { clockwork_core_close(mCore); }

void Lane::init(double rate, uint32_t block, uint32_t inputs, uint32_t outputs, const uint32_t (&config)[4]) {
    clockwork_init(mCore, rate, block, inputs, outputs, /*verbosity*/ 0,
                   mBuffers->guest, kGuestMemoryBytes,
                   config, static_cast<uint32_t>(sizeof config),
                   // No placement span: this fixture's allocator is the system
                   // one, which is what every native host passes.
                   /*arena_bytes*/ 0,
                   mBuffers->assets,  kAssetsBytes,
                   mBuffers->returns, kReturnsBytes);
    mBlockSize = clockwork_block_size(mCore);
}

const uint8_t* Lane::guestMemory() const { return mBuffers->guest; }
uint8_t*       Lane::assets()            { return mBuffers->assets; }
const uint8_t* Lane::returns() const     { return mBuffers->returns; }

ClockworkClient* Lane::openClient() const {
    ClockworkStatus st = CLOCKWORK_OK;
    uint8_t* const arena = static_cast<uint8_t*>(clockwork_lanes_base(mCore));
    if (!arena) return nullptr;
    return clockwork_client_open_memory(arena, reinterpret_cast<const ClockworkArenaHeader*>(arena)->arena_bytes, &st);
}

double Lane::publishedNtp() const {
    ClockworkClient* c = openClient();
    if (!c) return 0.0;
    ClockworkClientSampleClock sc {};
    sc.struct_bytes = sizeof sc;
    const ClockworkStatus st = clockwork_client_sample_clock(c, &sc);
    clockwork_client_close(c);
    return st == CLOCKWORK_OK ? sc.dac_ntp : 0.0;
}

bool Lane::metricWord(uint32_t offset, uint32_t& word) const {
    ClockworkClient* c = openClient();
    if (!c) return false;
    uint32_t words[clockwork::metrics_schema::kMetricWords] = {};
    const uint32_t n = clockwork_client_metrics(c, words, clockwork::metrics_schema::kMetricWords);
    clockwork_client_close(c);
    if (offset >= n) return false;
    word = words[offset];
    return true;
}

void Lane::ticked(uint64_t frames) {
    mSamplePos += frames;
    mResume = true;
}

// Another host (clockwork_embed_render) ticked the engine on the real wall
// clock, so the engine's clock is now ahead of this lane's timeline. Rebase
// so that ntpAt(position) equals the sample clock the engine last published,
// and tell the engine this host's audio is starting: the next tick
// re-anchors at exactly that time (the engine floors an anchor at the
// published sample clock), so frame times stay exact.
void Lane::resumeIfTicked() {
    if (!mResume) return;
    mResume = false;
    const double published = publishedNtp();
    if (published > 0.0)
        mBaseNtp = std::max(mBaseNtp, published - static_cast<double>(mSamplePos) / kSampleRate);
    clockwork_audio_starts(mCore, ntpAt(mSamplePos), /*monotonic_secs*/ 0.0);
}

uint64_t Lane::freshDsp() {
    destroy_dsp(mCore);
    rebuild_dsp(mCore, kSampleRate, kBufLength);
    mDspOrigin = mSamplePos;
    return mSamplePos;
}

void Lane::comeBack(double rate, uint32_t block, uint32_t inputs, uint32_t outputs, const uint32_t (&config)[4]) {
    const uint32_t before = clockwork_arena_generation(clockwork_arena_header(mCore));
    init(rate, block, inputs, outputs, config);
    // A new instance counts its frames from here, as freshDsp's does.
    if (clockwork_arena_generation(clockwork_arena_header(mCore)) != before) mDspOrigin = mSamplePos;
}

uint64_t Lane::tick() {
    const uint64_t at = mSamplePos;
    ClockworkBlockTime time {};
    time.struct_bytes = sizeof time;
    time.audio_frames = mSamplePos;
    time.wall_ntp     = ntpAt(mSamplePos);   // its own time for a wall, and no monotonic clock: a lane sees no gap
    clockwork_tick(mCore, &time, kOutChannels, kInChannels);
    mSamplePos += mBlockSize;
    return at;
}

std::vector<EgressFrame> Lane::drain(uint32_t (*from)(ClockworkCore*, ClockworkEgressFn, void*, uint32_t)) {
    std::vector<EgressFrame> out;
    from(mCore, [](void* ctx, uint32_t sourceId, uint32_t route,
                   const uint8_t* osc, uint32_t len, uint32_t seq) {
        auto* v = static_cast<std::vector<EgressFrame>*>(ctx);
        EgressFrame f;
        f.sourceId = sourceId;
        f.route    = route;
        f.seq      = seq;
        f.decodes  = clockwork_osc_decodes(osc, len) == 1;
        f.data.assign(osc, osc + len);
        v->push_back(std::move(f));
    }, &out, /*max_frames=*/0);
    return out;
}

std::vector<EgressFrame> Lane::drainRt()  { return drain(&clockwork_egress_rt_drain); }
std::vector<EgressFrame> Lane::drainNrt() { return drain(&clockwork_egress_nrt_drain); }

bool Lane::ingress(const uint8_t* osc, uint32_t len, uint32_t sourceId) {
    return clockwork_ingress_write(mCore, osc, len, sourceId);
}

// ── The default lane: the free functions' ─────────────────────────────────

Lane& defaultLane() {
    // The process's for the whole binary, as a host's engine is.
    static Lane* const lane = new Lane();
    return *lane;
}

namespace {
// Every frame must decode with the parser the clients read with.
std::vector<EgressFrame> checked(std::vector<EgressFrame> frames) {
    for (const auto& f : frames) {
        INFO("an egress frame of " << f.data.size() << " bytes the client parser (clockwork-osc) would not decode");
        CHECK(f.decodes);
    }
    return frames;
}
} // namespace

const uint8_t* guestMemory() { return defaultLane().guestMemory(); }
uint8_t*       assets()      { return defaultLane().assets(); }
const uint8_t* returns()     { return defaultLane().returns(); }

double baseNtp() {
    boot();
    return defaultLane().baseNtp();
}

double publishedNtp() {
    boot();
    return defaultLane().publishedNtp();
}

uint32_t metricWord(uint32_t offset) {
    uint32_t word = 0;
    REQUIRE(defaultLane().metricWord(offset, word));
    return word;
}

uint32_t boot() {
    Lane& lane = defaultLane();
    REQUIRE(lane.core() != nullptr);
    lane.resumeIfTicked();
    return lane.block();
}

void     ticked(uint64_t frames) { defaultLane().ticked(frames); }
uint64_t position()              { return defaultLane().position(); }

// The lane's core, booting it on the first ask. A core already there is
// handed back with no assertion, so a case's own threads may ask for it:
// Catch2's assertions are the test thread's alone.
ClockworkCore* core() {
    if (ClockworkCore* c = defaultLane().core()) return c;
    boot();
    return defaultLane().core();
}

uint64_t freshDsp() {
    boot();
    return defaultLane().freshDsp();
}

void comeBack(double rate, uint32_t block, uint32_t inputs, uint32_t outputs, const uint32_t (&config)[4]) {
    boot();
    defaultLane().comeBack(rate, block, inputs, outputs, config);
}

uint64_t dspFrame(uint64_t streamFrame) { return defaultLane().dspFrame(streamFrame); }

uint64_t tick() {
    boot();
    return defaultLane().tick();
}

std::vector<EgressFrame> drainRt()  { return checked(defaultLane().drainRt()); }
std::vector<EgressFrame> drainNrt() { return checked(defaultLane().drainNrt()); }

bool ingress(const uint8_t* osc, uint32_t len, uint32_t sourceId) {
    boot();
    return defaultLane().ingress(osc, len, sourceId);
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
