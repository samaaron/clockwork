// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
#pragma once
/*
 * GuestConfigBlock.h — the layout of the guest's config block, as the NATIVE
 * HOST writes it.
 *
 * This is a contract between this host and the guest it boots, and it is not
 * a clockwork concern. Clockwork core reserves a region for the block, copies
 * whatever bytes it is handed into it, and passes base and length to the guest
 * as DspConfig::guest_config. It does not read a single field, so nothing in
 * this file can break it, and a different guest is free to want a completely
 * different block.
 *
 * It lives in src/native/ for that reason: clockwork's own headers do not
 * describe it, and no code under src/ outside this directory includes it.
 *
 * The web host has the same arrangement in JavaScript — the product overrides
 * encodeGuestConfig() (js/clockwork.js) and the worklet copies the result in without
 * naming a slot.
 *
 * ── Why it is positional ────────────────────────────────────────────────────
 * Because the reader is on the other side of a C ABI in a separate repository.
 * A struct would have to be transcribed there anyway, and a transcription that
 * drifts is worse than an index that is spelled out. Both sides MUST agree on
 * every index: a native write at 17 read back at 18 once picked up the next
 * region's first word and made the guest create a stray shm segment on every
 * boot.
 *
 * The block is uint32_t throughout, and carries only the guest's own numbers.
 * Block size, the channel counts, verbosity and the web RT pool offset are
 * clockwork state and travel as arguments to clockwork_init() instead.
 *
 * SAMPLE RATE IS NOT HERE, and must not be added: a device switch changes the
 * rate under a running engine, so a copy in this block and DspConfig both
 * claiming to be it could not both be right. DspConfig::sample_rate is the
 * device's rate and is the only one.
 */

#include <cstdint>

namespace clockwork {
namespace guest_config {

/*
 * THE NUMBERS ARE THE GUEST'S, NOT THIS HOST'S.
 *
 * The block is opaque to clockwork and a host agrees its layout with its
 * guest — but this repository has ONE guest and TWO hosts writing to it, the
 * native one here and the web one in js/scsynth_options.js. So the reader
 * decides: dsp/scsynth/scsynth_dsp.cpp takes numControlBusChannels from slot
 * 7, realTimeMemorySize from 9, numRGens from 10 and loadGraphDefs from 13,
 * and both writers have to put them there.
 *
 * They did not. This host packed its nine fields into slots 0-8 while the
 * guest read the web layout, so the first five agreed by coincidence and the
 * rest did not: the guest took numRGens (64) as its control-bus count instead
 * of 16384, read realTimeMemorySize out of a slot nobody wrote and fell back
 * to a compile-time default, and never saw loadGraphDefs at all. Every native
 * test passed over it, because each wrong slot held either a plausible number
 * or a zero that looked like "not specified".
 *
 * The gaps are deliberate. Slots 5, 6, 8 and 14 are clockwork's own geometry
 * — channel counts, block size, sample rate — and they travel as clockwork_init
 * arguments because they describe what the device opened rather than what the
 * guest asked for. This host leaves them zero; the guest reads them from
 * DspConfig. test/world_options_contract.spec.mjs pins the whole agreement.
 */
enum : unsigned {
    kNumBuffers            =  0,
    kMaxNodes              =  1,
    kMaxGraphDefs          =  2,   /* definition table size */
    kMaxWireBufs           =  3,
    kNumAudioBusChannels   =  4,
    kNumControlBusChannels =  7,
    kRealTimeMemorySize    =  9,   /* KB */
    kNumRGens              = 10,
    kLoadGraphDefs         = 13,   /* load definitions from the filesystem */

    kSlotCount             = 18,   /* the span the guest reads */
};

} // namespace guest_config
} // namespace clockwork
