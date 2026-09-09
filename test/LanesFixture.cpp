// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * LanesFixture.cpp — see LanesFixture.h.
 */
#include "LanesFixture.h"

#include "lanes/lanes.h"
#include "dsp_api.h"      // DspFpEnv
#include "OscTestUtils.h"

#include <cstring>

namespace lanes_test {
namespace {

uint32_t   gBlockSize  = 0;
uint64_t   gSamplePos  = 0;
bool       gBooted     = false;

void drainInto(std::vector<EgressFrame>& out,
               uint32_t (*drain)(ClockworkEgressFn, void*, uint32_t)) {
    drain([](void* ctx, uint32_t sourceId, uint32_t route,
             const uint8_t* osc, uint32_t len, uint32_t seq) {
        auto* v = static_cast<std::vector<EgressFrame>*>(ctx);
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
// the process's: clockwork holds the pointer across every rebuild, and a
// region that outlived neither would be a use-after-free the tests would find
// in the most confusing possible way.
alignas(16) static uint8_t gGuestMemory[kGuestMemoryBytes];
alignas(16) static uint8_t gInbox[kInboxBytes];
alignas(16) static uint8_t gOutbox[kOutboxBytes];

const uint8_t* guestMemory() { return gGuestMemory; }
uint8_t*       inbox()       { return gInbox; }
const uint8_t* outbox()      { return gOutbox; }

uint32_t boot() {
    if (gBooted) return gBlockSize;
    // Before clockwork_init: the reservation is read at every init_memory, and
    // widens the channel counts the DSP is allocated for.
    if (kReservedLanes > 0) clockwork_reserve_lanes(kReservedLanes);
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
             gInbox,  kInboxBytes,
             gOutbox, kOutboxBytes);
    gBlockSize = clockwork_block_size();
    gBooted    = true;
    return gBlockSize;
}

void ticked(uint64_t frames) { gSamplePos += frames; }

uint64_t tick() {
    boot();
    const uint64_t at  = gSamplePos;
    const double   ntp = ntpAtFrame(gSamplePos);
    clockwork_tick(ntp, kOutChannels, kInChannels);
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
