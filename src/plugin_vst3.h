// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_vst3.h — the VST3 half of plugin_host.h.
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
 */
#ifndef CLOCKWORK_PLUGIN_VST3_H
#define CLOCKWORK_PLUGIN_VST3_H

#include "plugin_host.h"

struct ClockworkClockState;
struct ClockworkTimeline;

namespace clockwork_plugin_vst3 {

/* One opened VST3 instance: module handle, component, processor, controller,
   the scratch buffers and the pending-parameter queue. Opaque here. */
struct Instance;

uint32_t  scan(const char* path, PluginDesc* out, uint32_t cap);

Instance* open(const char* path, uint32_t index, double sample_rate,
               uint32_t max_block, const char** err);
void      close(Instance* p);

void      process(Instance* p, const float* const* in, uint32_t n_in,
                  float* const* out, uint32_t n_out,
                  uint32_t frames, int64_t block_time);

uint32_t  latency(const Instance* p);

uint32_t  param_count(const Instance* p);
int       param_info(const Instance* p, uint32_t index, PluginParam* out);
void      param_set(Instance* p, uint32_t id, double value, uint32_t frame_offset);
// Queue a note for the next block. `on` selects note-on or note-off.
// The session clock, read per block to fill ProcessContext's musical fields.
void      set_clock(Instance* p, const struct ClockworkClockState* clock);
// A timeline to render the coming blocks against instead of the clock; NULL
// returns to the clock. Audio thread (plugin_host.h plugin_set_timeline).
void      set_timeline(Instance* p, const struct ClockworkTimeline* timeline);

void      note(Instance* p, bool on, int16_t channel, int16_t pitch,
               float velocity, uint32_t frame_offset);
// A MIDI controller and pitch bend, delivered as the parameter the plugin's
// IMidiMapping assigns them — VST3 has no CC event. Silent where it maps none.
void      cc(Instance* p, int16_t channel, uint8_t number, float value, uint32_t frame_offset);
void      pitch_bend(Instance* p, int16_t channel, float bend, uint32_t frame_offset);
void      all_notes_off(Instance* p, uint32_t frame_offset);

// Called when the PLUGIN reports one of its own parameter edits — a knob
// turned in its editor. Invoked on the plugin's thread, which for a GUI is the
// main thread.
void      set_param_listener(Instance* p, void (*fn)(void*, uint32_t, double), void* ctx);

// Editor. Main thread only — VST3 views are UI objects.
bool      editor_has(Instance* p);
bool      editor_open(Instance* p, void* parent_native_view);
void      editor_close(Instance* p);
bool      editor_size(Instance* p, uint32_t* w, uint32_t* h);
bool      editor_set_size(Instance* p, uint32_t w, uint32_t h);

uint32_t  state_save(Instance* p, uint8_t* out, uint32_t cap);
int       state_load(Instance* p, const uint8_t* bytes, uint32_t len);

}  // namespace clockwork_plugin_vst3

#endif /* CLOCKWORK_PLUGIN_VST3_H */
