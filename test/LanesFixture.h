// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * LanesFixture.h — boot clockwork once and drive it a block at a time.
 *
 * The lanes ABI (src/lanes/lanes.h) is the whole of clockwork reduced to five
 * calls: clockwork_init once, then per block clockwork_ingress_write -> clockwork_tick -> read
 * clockwork_audio_out -> clockwork_egress_rt_drain. It needs no audio device, no JUCE
 * device manager and no threads, which makes it the honest surface for
 * testing what clockwork does with bytes and samples.
 *
 * Clockwork is a process singleton — one arena, one clock, one ingress —
 * so booting is done ONCE for the whole test binary and every test shares it.
 * That is not a shortcut: it is what a host does, and a test that re-booted
 * per case would be testing a lifecycle no host performs.
 *
 * tick() returns the ABSOLUTE frame index of the block it just rendered, which
 * is what lets a test assert exact sample values against a formula rather than
 * against whatever the previous test happened to leave behind.
 */
#pragma once

#include <cstdint>
#include <vector>

namespace lanes_test {

// A modest desktop-sized session. Small enough to be quick, large enough that
// wraparound and multi-channel arithmetic are exercised for real.
inline constexpr double   kSampleRate  = 48000.0;
inline constexpr uint32_t kOutChannels = 2;
// Two in as well as two out. Clockwork owns the input staging buffer now
// (it used to hand the DSP an address computed inside the DSP's own bus pool),
// so a session with no input channels would leave that half of dsp_process
// untested — see the echo case in test_dsp_boundary.cpp.
inline constexpr uint32_t kInChannels  = 2;
inline constexpr uint32_t kBufLength   = 64;

// Reserved lanes, above the device's channels (clockwork_reserve_lanes). Zero
// in the main test binary: reserving widens the engine's channel counts, and
// every case in that binary is written against kInChannels/kOutChannels.
//
// clockwork_lanes_reserved_tests compiles this file with a non-zero value, so
// the lane path — a source port attached above the device's width, pulled into
// the DSP's inputs — is exercised for real in a binary of its own rather than
// perturbing the shared fixture. Clockwork is a process singleton, so a second
// geometry needs a second process.
#ifndef LANES_FIXTURE_RESERVED_LANES
#define LANES_FIXTURE_RESERVED_LANES 0
#endif
inline constexpr uint32_t kReservedLanes = LANES_FIXTURE_RESERVED_LANES;

// ── The two regions the fixture supplies as a host ──────────────────────────
//
// A host that hands the guest neither a config block nor a memory region is a
// host that leaves half of DspConfig untested, so this one supplies both. The
// values are sentinels chosen to be recognisable in a hex dump and impossible
// to produce by accident: a test asserting on them is asserting that
// clockwork carried these exact bytes, not that it produced a plausible number.
inline constexpr uint32_t kGuestConfigSlots[4] = {
    0xC0FFEE01u, 0xC0FFEE02u, 0xC0FFEE03u, 0xC0FFEE04u
};

// Multi-megabyte, on purpose. This is the bulk channel — the region a client
// writes a sample into and a guest writes a rendered blob into — so a fixture
// that could only hold a few kilobytes would test the plumbing at a size where
// the plumbing is not the point. 4 MB is enough to be honestly "bulk" and
// small enough that dsp_new can still scan the whole thing to prove it arrived
// zeroed.
//
// It is also comfortably larger than the egress ring (OUT_BUFFER_SIZE, 128 KB),
// which lets a test ask for a payload the ring cannot carry and watch it be
// refused there while the region carries it without trouble.
inline constexpr uint32_t kGuestMemoryBytes = 4 * 1024 * 1024;   // the arena
// Bulk staging, one writer each: the client writes the inbox, the guest the
// outbox. Separate regions so neither test can mask a two-writer bug.
inline constexpr uint32_t kInboxBytes  = 4 * 1024 * 1024;
inline constexpr uint32_t kOutboxBytes = 4 * 1024 * 1024;

// The region the fixture reserves for the guest, so a test can check
// clockwork did not write into it.
const uint8_t* guestMemory();   // the arena
uint8_t*       inbox();         // client writes, guest reads
const uint8_t* outbox();        // guest writes, client reads

// The NTP instant the fixture calls frame 0. There is no real-time clock in
// play: the fixture self-clocks from kBaseNtp + frames/rate exactly as a
// self-driven host does, so a test that wants to schedule something can name
// the exact NTP time a given absolute frame will be rendered at.
inline constexpr double   kBaseNtp     = 2208988800.0;

// The NTP time at which absolute frame `frame` is rendered.
inline double ntpAtFrame(uint64_t frame) {
    return kBaseNtp + static_cast<double>(frame) / kSampleRate;
}

// Boot clockwork if it is not already up; returns the block size it settled
// on. Safe to call from every test case — only the first call does anything.
uint32_t boot();

// Render one block. Returns the absolute frame index of its FIRST sample, so
// out[ch][i] corresponds to stream position (returned + i).
uint64_t tick();
// A test that ticked the engine by another door (clockwork_embed_render)
// says so, so this fixture's frame count stays the engine's.
void ticked(uint64_t frames);

// One frame off an egress ring.
struct EgressFrame {
    uint32_t             sourceId = 0;
    uint32_t             route    = 0;
    uint32_t             seq      = 0;
    std::vector<uint8_t> data;
};

// Drain everything currently on the RT egress ring.
std::vector<EgressFrame> drainRt();
// Drain everything currently on the NRT egress ring.
std::vector<EgressFrame> drainNrt();

// Write one OSC packet into the IN ring. Returns false when the ring is full.
bool ingress(const uint8_t* osc, uint32_t len, uint32_t sourceId = 0);

// Tick until a frame with OSC address `address` has been drained, or until
// `maxBlocks` blocks have gone by. Returns EVERYTHING drained across those
// blocks, so a test can assert on what else came out too. Pass nullptr for
// `address` to simply tick `maxBlocks` and collect.
std::vector<EgressFrame> tickUntil(int maxBlocks, const char* address);

} // namespace lanes_test
