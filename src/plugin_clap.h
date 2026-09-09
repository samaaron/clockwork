// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_clap.h — the CLAP half of plugin_host.h.
 *
 * NOT the public API. plugin_host.h declares ONE set of C symbols
 * (plugin_scan, plugin_open, …) and a build that hosts more than one format
 * cannot have two translation units defining them — plugin_clap.cpp and
 * plugin_vst3.cpp would collide at link. So each adapter exports its own
 * namespaced entry points through a header like this one, and plugin_host.cpp
 * — the dispatcher — is the single definition of the public symbols, trying
 * each compiled-in format in turn.
 *
 * The shapes below are plugin_host.h's, one for one, minus the name prefix.
 * plugin_format() has no counterpart: the format is the dispatcher's record of
 * which adapter claimed the file, not something an adapter can be asked.
 */
#ifndef CLOCKWORK_PLUGIN_CLAP_H
#define CLOCKWORK_PLUGIN_CLAP_H

#include "plugin_host.h"

struct ClockworkTimeline;

namespace clockwork_plugin_clap {

/* One opened CLAP instance: the DSO handle, the entry and factory it came
   from, the clap_plugin_t itself, its extensions, the staging buffers and the
   pending-parameter ring. Opaque here. */
struct Instance;

uint32_t  scan(const char* path, PluginDesc* out, uint32_t cap);

Instance* open(const char* path, uint32_t index, double sample_rate,
               uint32_t max_block, const char** err);
void      close(Instance* p);

void      process(Instance* p, const float* const* in, uint32_t n_in,
                  float* const* out, uint32_t n_out,
                  uint32_t frames, int64_t block_time);

uint32_t  latency(const Instance* p);

// The session clock, read per block to fill the transport event the plugin
// is handed; NULL (the default) runs the plugin free, with no transport.
void      set_clock(Instance* p, const struct ClockworkClockState* clock);
// A timeline to render the coming blocks against instead of the clock; NULL
// returns to the clock. Audio thread (plugin_host.h plugin_set_timeline).
void      set_timeline(Instance* p, const struct ClockworkTimeline* timeline);

uint32_t  param_count(const Instance* p);
int       param_info(const Instance* p, uint32_t index, PluginParam* out);
void      param_set(Instance* p, uint32_t id, double value, uint32_t frame_offset);

/* Notes on note port 0, in the CLAP dialect where the plugin speaks it and
   as MIDI bytes otherwise; controllers and bend as MIDI bytes only, since the
   CLAP dialect has no such events. A plugin with no note port takes none. */
void      note(Instance* p, bool on, int16_t channel, int16_t pitch,
               float velocity, uint32_t frame_offset);
void      cc(Instance* p, int16_t channel, uint8_t number, float value, uint32_t frame_offset);
void      pitch_bend(Instance* p, int16_t channel, float bend, uint32_t frame_offset);
void      all_notes_off(Instance* p, uint32_t frame_offset);

uint32_t  state_save(Instance* p, uint8_t* out, uint32_t cap);
int       state_load(Instance* p, const uint8_t* bytes, uint32_t len);

}  // namespace clockwork_plugin_clap

#endif /* CLOCKWORK_PLUGIN_CLAP_H */
