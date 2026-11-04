// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// The globals guard's canary: one file static that scripts/globals.list
// does not name. scripts/check-globals.sh compiles it the way it compiles
// the libraries, and the check fails unless the scan reports it.
static int clockwork_globals_canary = 0;

int* clockwork_globals_canary_address() { return &clockwork_globals_canary; }
