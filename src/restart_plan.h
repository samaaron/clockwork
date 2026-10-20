// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * restart_plan.h — the least a restart does to get the engine back where it
 * was (docs/RESTARTS.md).
 *
 * Audio stops and starts again: a device switched, a device lost and found,
 * a page whose worklet the browser took away. It may come back on the same
 * device or another, and either way it is the same event: the instance
 * carries on unless what it was built for has changed in a way it cannot
 * take. One rule, for every host — the native engine's device switch
 * (DevicePolicy's planSwap) and a host coming back over the memory it ran in
 * (clockwork_init) both ask it.
 *
 * Pure: no state, no device, no guest.
 */
#pragma once

#include "dsp_api.h"   // DSP_SURVIVES_*

#include <cstdint>

namespace clockwork {

// What an instance was built for, and what audio comes back with.
struct RestartFacts {
    double   rate    = 0;
    uint32_t block   = 0;   // clockwork's block, which the instance renders in
    uint32_t inputs  = 0;   // the device's channel ceilings
    uint32_t outputs = 0;
};

enum class RestartStep : uint8_t {
    Resume,        // nothing it was built for changed: it carries on
    Reconfigure,   // a change it declared it takes live: it is told (dsp_reconfigure)
    Rebuild,       // a change it cannot take: destroyed and built again
};

struct RestartPlan {
    RestartStep step = RestartStep::Resume;
    // Why a rebuild, for the log.
    bool rebuildForRate     = false;
    bool rebuildForChannels = false;
    bool rebuildForBlock    = false;
};

// `survives` is the instance's DspInfo::survives. No kind of change covers
// the block: an instance is laid out for the one it was built in.
inline RestartPlan planRestart(const RestartFacts& builtFor, const RestartFacts& now, uint32_t survives) {
    const bool rate     = now.rate != builtFor.rate;
    const bool channels = now.inputs != builtFor.inputs || now.outputs != builtFor.outputs;
    RestartPlan plan;
    plan.rebuildForRate     = rate && !(survives & DSP_SURVIVES_RATE_CHANGE);
    plan.rebuildForChannels = channels && !(survives & DSP_SURVIVES_CHANNEL_CHANGE);
    plan.rebuildForBlock    = now.block != builtFor.block;
    if (plan.rebuildForRate || plan.rebuildForChannels || plan.rebuildForBlock) plan.step = RestartStep::Rebuild;
    else if (rate || channels)                                                    plan.step = RestartStep::Reconfigure;
    return plan;
}

} // namespace clockwork
