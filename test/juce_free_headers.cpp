// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * juce_free_headers.cpp — the headers a host includes, compiled with JUCE off
 * the include path.
 *
 * Clockwork uses JUCE inside, for its devices; a host never sees it. These
 * are the headers a host includes — SuperSonic's list — and this file is built
 * by a target that cannot see JUCE: a header that starts including it fails
 * this build, rather than quietly handing JUCE to every host.
 */
#include "smoothie_worklet.h"
#include "ClockworkEngine.h"
#include "ClockworkProcessor.h"
#include "TrackControl.h"
#include "GuestConfigText.h"
#include "IOscTransport.h"
#include "OscBuilder.h"
#include "UdpOscTransport.h"
#include "UdsDgramOscTransport.h"
#include "clockwork_audio_file.h"
#include "clockwork_client.h"
#include "clockwork_product.h"
#include "clock/clock_math.h"
#include "dsp_api.h"
#include "memory_profile.h"
#include "rt_alloc.h"
#include "shared_memory.h"
#include "shm_audio_buffer.hpp"
#include "shm_segment.hpp"
