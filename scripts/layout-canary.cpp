// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// The canary scripts/check-layout.sh must catch: engine code reaching a
// region through the target's default layout. Never compiled.
#include "shared_memory.h"
uint8_t* canary_in_ring(uint8_t* base) { return base + clockwork::kDefaultLayout.in_ring_start; }
