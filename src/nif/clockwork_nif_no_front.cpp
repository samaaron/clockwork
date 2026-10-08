// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// The NIF with no product front: every packet goes straight to the engine.
#include "clockwork_nif_front.h"

std::unique_ptr<OscFront> clockwork_nif_make_front(ClockworkEngine&, IOscTransport&) {
    return nullptr;
}
