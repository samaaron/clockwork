// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * juce_free_headers.cpp — the headers a host includes, compiled with JUCE off
 * the include path.
 *
 * Clockwork uses JUCE inside, for its devices; a host never sees it. These
 * are the headers a host includes (host_headers.h), and this file is built
 * by a target that cannot see JUCE: a header that starts including it fails
 * this build, rather than quietly handing JUCE to every host.
 */
#include "host_headers.h"
