// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * params.h — the parameter table inside the engine (docs/PARAMS.md): the
 * region's reset, and the route that answers /clockwork/param/… on the
 * audio thread of every host. The public shape is clockwork_params.h.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include "clockwork_core.h"

namespace clockwork::params {

// The region at a cold boot: the header written, every slot free.
void reset(ClockworkCore* core);

// "/clockwork/param/define", "list", "set" and "text", registered under "param/"
// beside the other audio-thread verbs. routeCtx is the core; callCtx the
// DrainCallCtx. Allocates nothing; parses within the frame's bounds.
bool route(void* routeCtx, const void* callCtx, const uint8_t* data, std::size_t len);

} // namespace clockwork::params
