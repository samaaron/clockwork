// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * qt_keyword_headers.cpp — the headers a host includes, compiled as a Qt
 * program compiles them.
 *
 * Qt defines four ordinary words as macros in every file that includes it:
 * `slots` and `emit` as nothing, `signals` as `public`, and `foreach` as a
 * loop. A header that uses one as a name then reads `uint32_t ;` to a Qt
 * host, which gcc refuses. The words are defined here as Qt defines them, so
 * a header that takes one fails this build. Its target turns the compilers
 * that only warn of an empty declaration into ones that refuse it.
 */
#define slots
#define emit
#define signals public
#define foreach(variable, container) for (variable : container)
#include "host_headers.h"
