// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * EngineControl.h — the clock/ and engine control-endpoint implementations,
 * owned by the engine.
 *
 * The handlers run on the control pass's thread. They reach the engine for device /
 * driver / record / link state, and the egress hub (`mEgress`) for replies and
 * subscriber pushes — never the transport directly. The engine registers
 * EngineControl's sinks on its NRT control route table (mControlRoutes).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class OscEgress;
class ClockworkEngine;
class ClockworkClock;
class LinkAudioHost;
struct DrainCallCtx;
struct SwapResult;

class EngineControl {
public:
    void init(ClockworkEngine* engine, OscEgress* egress, ClockworkClock* clock,
              LinkAudioHost* linkAudio) {
        mEngine    = engine;
        mEgress    = egress;
        mClockworkClock  = clock;
        mLinkAudio = linkAudio;
    }

    // The NRT reader threads the origin token as call metadata, then calls
    // these through that table (see ClockworkEngine's NRT gateway control drain).
    bool handleLinkCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);
    bool handleEngineCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);

private:
    // Refuse one claimed-but-unknown address under the reserved prefix.
    void refuseUnknown(uint32_t token, const uint8_t* data, uint32_t size);

    // The end of a /clockwork/devices/switch that did not go through the
    // debounced switchDevice (which reports its own): name the device the
    // engine is on now, broadcast switch.done, and re-report the devices on
    // success. On the device lane.
    void finishSwitch(SwapResult result, const std::string& requestedOutput,
                      const std::string& requestedInput);

    ClockworkEngine* mEngine     = nullptr;
    OscEgress*        mEgress     = nullptr;
    ClockworkClock*       mClockworkClock = nullptr;
    LinkAudioHost*  mLinkAudio = nullptr;
};
