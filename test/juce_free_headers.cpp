// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * juce_free_headers.cpp — compiled with JUCE off the include path.
 *
 * JUCE stays inside Smoothie, behind its worklet (smoothie/README.md). These
 * are headers above that line, and this file is built by a target that
 * cannot see JUCE: a header that starts including it fails this build,
 * rather than quietly carrying JUCE up into the engine and its hosts.
 */
#include "smoothie_worklet.h"
#include "ClockworkProcessor.h"
