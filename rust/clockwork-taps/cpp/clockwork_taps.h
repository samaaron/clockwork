/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2026 Sam Aaron
 *
 * clockwork_taps.h — the tap substrate (rust/clockwork-taps): a core's tap
 * slots, each claimed in the core's arena by one owner and given back by
 * that owner alone. The engine claims them for the guest (DspHost::tap_open)
 * and for the tracks' returns (TrackControl); both go through these two
 * calls, declared here once.
 *
 * A claim is a DspTapHandle (dsp_api.h): `slot` is the slot's address in the
 * arena, `channels` how many it carries; the other two fields are unused.
 *
 * Pure C.
 */
#pragma once

#include "clockwork_core.h"   /* ClockworkTaps */
#include "dsp_api.h"          /* DspTapHandle */

#ifdef __cplusplus
extern "C" {
#endif

/* Claim slot `index` of `taps` — a core's (clockwork_core_taps), laid out in
 * `arena` (clockwork_lanes_base) — for `channels` channels, writing the claim
 * into `hnd`. 1 when claimed; 0 when the slot is not there, is another's, or
 * `taps`/`arena` are null. `max_frames` is unused: a stream's capacity is the
 * build's. The core's audio thread, or its control thread while no block
 * runs. */
int clockwork_tap_get(const ClockworkTaps* taps, void* arena, int index, int channels, int max_frames,
                      DspTapHandle* hnd);

/* Give the slot `hnd` claims back, in the core it was claimed in. Nothing
 * for a handle that holds no claim. Same threads as clockwork_tap_get. */
void clockwork_tap_release(const ClockworkTaps* taps, void* arena, DspTapHandle* hnd);

#ifdef __cplusplus
}
#endif
