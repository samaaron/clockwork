// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * host_headers.h — the headers a host includes: SuperSonic's list, and the
 * embed's. The checks that hold them to something (juce_free_headers.cpp,
 * qt_keyword_headers.cpp) each compile this one list.
 */
#include "smoothie_worklet.h"
#include "smoothie_devices.h"
#include "ClockworkEngine.h"
#include "ClockworkProcessor.h"
#include "TrackControl.h"
#include "GuestConfigText.h"
#include "IOscTransport.h"
#include "OscBuilder.h"
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
#include "clockwork_embed.h"
