// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * IngressCallCtx.h — the immutable per-call metadata the dispatch path passes to
 * every backend via OscIngress::ingest(). One bundle (origin token + timetag)
 * travels with each message; each backend reads what it needs.
 */
#pragma once

#include <cstddef>
#include <cstdint>

struct DrainCallCtx {
    uint32_t      sourceId = 0;        // origin token — threaded to every backend so a
                                       // reply can be routed back to the sender. It is
                                       // also what dsp_osc() carries and DspHost::emit_osc
                                       // hands back, so no reply TYPE crosses the boundary.
    int64_t       when     = 0;        // OSC timetag of this message; 0/1 = immediate.
                                       // Passed to the DSP, which derives its own
                                       // sub-block placement from it; other handlers ignore.
    int64_t       blockTime = 0;       // this block's start in OSC time. Clockwork uses it
                                       // for lateness accounting; the DSP gets the same value
                                       // as dsp_process's block_time.
};

// The DSP's default ingress route: hands one OSC message or bundle straight to
// dsp_osc(). Registered as the OscIngress default, so an address clockwork
// did not claim by prefix reaches the DSP untouched. Defined in the
// audio_processor TU; only registered where a DSP is built in.
bool clockwork_dsp_default_route(void* routeCtx, const void* callCtx,
                          const uint8_t* data, std::size_t len);
