// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_host.h — what clockwork needs from a plugin, whatever format it is.
 *
 * One abstraction, one thin adapter per format: CLAP (`plugin_clap.cpp`) and
 * VST3 (`plugin_vst3.cpp`), with AU and LV2 later on the same boundary. Both
 * formats are MIT — CLAP always was, and Steinberg relicensed VST3 at 3.8.1 —
 * so licensing chooses nothing here and this is purely an engineering shape.
 *
 * MODELLED ON CLAP, NOT ON THE INTERSECTION. CLAP is the more expressive and
 * more explicit of the two, so the VST3 adapter emulates what it can and
 * refuses the rest honestly. Taking the common denominator instead would lose
 * per-voice modulation for everyone — including the format that has it, and
 * it is exactly what a per-voice process model would most want.
 *
 * EDITORS. Both formats make one optional — CLAP's is an extension, VST3's
 * `createView` may return null — so plugin_editor_has() answering false is an
 * answer rather than a failure. VST3 editors are implemented; CLAP's are not.
 *
 * EDITORS ARE POSSIBLE HERE because the engine links AppKit for its CoreAudio
 * backend and its main thread already pumps a Cocoa run loop for AUHAL and
 * GameController. See plugin_editor_window.h.
 *
 * PLACEMENT. v1 hosts inline: clockwork runs the DSP, then the plugin, then
 * the device, all in one callback, so nothing is delayed. The send/return
 * placement — a plugin running beside the DSP, its output returning next block
 * — needs the port substrate in docs/PORTS.md and waits for it. Inline needs
 * no rings at all, which is why this can exist before that does.
 *
 * NOT REALTIME-SAFE, AND CANNOT BE. Plugins allocate, take locks and
 * occasionally do file IO on the audio thread, and clockwork asserts the
 * opposite with rt_alloc::Guard. Hosting one means accepting that a third
 * party can violate that guarantee inside plugin_process — the guard is
 * therefore suspended across that call, deliberately and visibly, rather than
 * being quietly weakened everywhere. A plugin that crashes takes the audio
 * thread with it; out-of-process hosting is the only real answer to that and
 * is not attempted here.
 */
#ifndef CLOCKWORK_PLUGIN_HOST_H
#define CLOCKWORK_PLUGIN_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An opened plugin instance. Opaque; the format is the adapter's business. */
struct HostedPlugin;
struct ClockworkClockState;

typedef enum PluginFormat {
    kPluginFormatUnknown = 0,
    kPluginFormatClap    = 1,
    kPluginFormatVst3    = 2,
} PluginFormat;

/* What a scan found. Strings are owned by the adapter and stay valid until
   the next scan of the same path. */
typedef struct PluginDesc {
    PluginFormat format;
    const char*  id;       /* the format's own stable identifier */
    const char*  name;
    const char*  vendor;
    uint32_t     index;    /* which plugin inside the file — bundles hold many */

    /* Non-zero if this plugin GENERATES rather than transforms.
       Both formats state it — VST3 in the class's subCategories, CLAP in its
       feature list — so asking the user "instrument or effect?" is asking them
       to retype something the plugin already said. A host that does not read
       this ends up with a role dropdown beside every Add button. */
    int32_t      is_instrument;
} PluginDesc;

typedef struct PluginParam {
    uint32_t    id;        /* the format's own parameter id, not an index */
    const char* name;
    double      min;
    double      max;
    double      value;     /* current */

    /* Which group this parameter belongs to, as the plugin declares it.
       Parameters carrying the same unit are meant to be shown together — it is
       how a plugin says "these four are the filter". A host that ignores it
       renders a flat wall of controls in declaration order, which is what a
       plugin's own editor never does. 0 is the root unit: ungrouped. */
    int32_t     group;

    /* The group's NAME, as the plugin calls it — "Filter", "Oscillator 1".
       Empty string when the plugin gives none, never NULL.

       `group` alone cannot be shown to anyone. VST3 unit ids are hashes, not
       small integers, so a host printing the number renders "GROUP 1065737767"
       — which is not a caption, it is a leak of an internal identifier. The id
       is still what you compare on; this is what you draw. */
    const char* group_name;

    /* Non-zero if the host may automate it. Plugins expose plenty that no user
       wants on a panel — hidden internals, read-outs, program slots — and a
       host that shows everything shows mostly noise. */
    int32_t     automatable;
} PluginParam;

/* ── Discovery ──────────────────────────────────────────────────────────── */

/*
 * List the plugins in a file. Writes at most `cap` entries and returns how
 * many exist, which may exceed `cap` — a bundle can hold many. Returns 0 if
 * the file is not a plugin this build can load, which is a refusal rather
 * than an error: scanning a directory means opening a great deal that is not
 * a plugin.
 */
uint32_t plugin_scan(const char* path, PluginDesc* out, uint32_t cap);

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/*
 * Open and activate one plugin from a file. `max_block` is the largest
 * `frames` that plugin_process will ever be given; both formats want to know
 * that up front so they can allocate before the audio thread exists.
 *
 * NULL with `err` set on failure. `err` points at storage the adapter owns.
 */
struct HostedPlugin* plugin_open(const char* path, uint32_t index,
                                 double sample_rate, uint32_t max_block,
                                 const char** err);

void plugin_close(struct HostedPlugin* p);

/* Which format actually loaded, for diagnostics and tests. */
PluginFormat plugin_format(const struct HostedPlugin* p);

/* ── Audio ──────────────────────────────────────────────────────────────── */

/*
 * Process one block. Same shape as dsp_process, and for the same reasons:
 * channel counts arrive per block because they change while running, and a
 * channel index is a stable slot.
 *
 * `block_time` is the OSC timetag of the first frame, so parameter changes
 * queued with a frame offset can be placed exactly.
 *
 * Called on the audio thread. See the header note: this is the one place
 * clockwork's no-allocation guarantee is knowingly suspended.
 */
void plugin_process(struct HostedPlugin* p,
                    const float* const* in, uint32_t n_in,
                    float* const* out, uint32_t n_out,
                    uint32_t frames, int64_t block_time);

/*
 * Frames of latency the plugin introduces, for delay compensation. Both
 * formats let this change while running (a plugin switching mode), so it is
 * asked per block rather than once — cheap, and the alternative is silent
 * drift against everything else.
 */
uint32_t plugin_latency(const struct HostedPlugin* p);

/* ── Parameters ─────────────────────────────────────────────────────────── */

uint32_t plugin_param_count(const struct HostedPlugin* p);

/* Fills `out` for parameter `index` (0..count-1). False if there is no such
   parameter. Note `PluginParam::id` is the format's id: pass THAT to
   plugin_param_set, never the index, because ids are stable across versions
   and indices are not. */
int plugin_param_info(const struct HostedPlugin* p, uint32_t index, PluginParam* out);

/*
 * Set a parameter, optionally at an exact frame within the next block.
 * `frame_offset` of 0 means the start of the block. Sample-accurate
 * automation is the whole reason the offset exists; both formats carry it
 * natively, which is why it belongs here rather than in a wrapper.
 */
void plugin_param_set(struct HostedPlugin* p, uint32_t id, double value,
                      uint32_t frame_offset);

/* ── Musical time ───────────────────────────────────────────────────────── */

/*
 * Give the plugin the session clock, so it can be told the tempo.
 *
 * Without this a plugin receives a sample position and a wall time and nothing
 * musical — which means a tempo-synced delay has nothing to sync TO, an LFO
 * set to 1/8 free-runs, and an arpeggiator cannot find the grid. They do not
 * fail loudly; they quietly behave as though the host had no tempo, which is
 * indistinguishable from the feature being broken.
 *
 * The pointer is clockwork's own ClockworkClockState — the same bytes the DSP
 * reads through DspConfig::clock, so there is one clock and no copy of it.
 * Read per block through readClockworkClock(), which is the only safe reader
 * (the coherence rules are a directional protocol, not a lock).
 *
 * NULL is a working state: the plugin is simply told nothing about tempo.
 */
void plugin_set_clock(struct HostedPlugin* p, const struct ClockworkClockState* clock);

/*
 * Render the coming blocks against THIS timeline rather than the session
 * clock: what a track bound to a midi follower timeline hands each of its
 * plugins before every block (plugin_track.cpp). The snapshot is copied,
 * so the caller's may go out of scope; NULL returns the plugin to the
 * clock. AUDIO THREAD — the same thread as plugin_process, and nothing
 * else: a plain copy, no atomics, no lock.
 */
struct ClockworkTimeline;
void plugin_set_timeline(struct HostedPlugin* p, const struct ClockworkTimeline* timeline);

/* ── Notes ──────────────────────────────────────────────────────────────── */

/*
 * Play a note. `frame_offset` places it exactly within the next block, the
 * same way parameter automation is placed, because a note landing on a block
 * boundary instead of its real moment is audible as swing on fast material.
 *
 * `velocity` is 0..1, not 0..127: both formats carry it as a float and
 * converting at the edges twice would only lose resolution. `channel` is
 * 0-based.
 *
 * These exist because an instrument is not addressable through parameters. An
 * effect needs none of this and a DSP that only enumerates parameters can host
 * effects and nothing else — which is what this header did before, and why a
 * loaded synth stayed silent.
 */
void plugin_note_on(struct HostedPlugin* p, int16_t channel, int16_t pitch,
                    float velocity, uint32_t frame_offset);
void plugin_note_off(struct HostedPlugin* p, int16_t channel, int16_t pitch,
                     float velocity, uint32_t frame_offset);

/*
 * Controllers, in the same sample-accurate shape as notes. `value` is 0..1
 * (the 7-bit number scaled), `bend` is -1..1 around centre. A VST3 plugin
 * receives them through its own MIDI-CC mapping (IMidiMapping) and a CLAP
 * plugin as raw MIDI on its note port; a plugin without a mapping for a
 * controller ignores it. all_notes_off silences every held voice — the
 * "panic" behind a chain change or a stopped run.
 */
void plugin_cc(struct HostedPlugin* p, int16_t channel, uint8_t number,
               float value, uint32_t frame_offset);
void plugin_pitch_bend(struct HostedPlugin* p, int16_t channel, float bend,
                       uint32_t frame_offset);
void plugin_all_notes_off(struct HostedPlugin* p, uint32_t frame_offset);

/*
 * Hear about the plugin's OWN parameter edits — someone turning a knob in its
 * editor. `value` is normalised 0..1, the format's native form for this.
 *
 * Without this an embedded editor is decorative: the plugin updates its own
 * controller and the host never learns, so nothing downstream of the host
 * changes. VST3 only.
 */
typedef void (*PluginParamEditFn)(void* ctx, uint32_t id, double normalized);
void plugin_set_param_listener(struct HostedPlugin* p, PluginParamEditFn fn, void* ctx);

/* ── Editor ─────────────────────────────────────────────────────────────── */

/*
 * The plugin's own GUI, embedded in a native view the CALLER owns.
 *
 * A genuinely headless host has no window and no event loop, and for one of
 * those the honest answer is still "no editor". But a host that has both
 * should be able to ask, so this is the narrowest thing that lets it:
 * create the view, attach it to a parent, report the size it wants. See the
 * note at the top of this header for why this engine has both.
 *
 * Clockwork does not create windows, run an event loop, or know what a
 * toolkit is. `parent_native_view` is an NSView* on macOS and an X11 window id
 * on Linux; producing one is the embedder's business.
 *
 * MAIN THREAD ONLY. Every call here touches UI objects, and no plugin expects
 * them from the audio thread.
 *
 * A plugin may legitimately have no editor (VST3's createView may return
 * null), so plugin_editor_has answering false is an answer, not a failure.
 */
int  plugin_editor_has(struct HostedPlugin* p);
int  plugin_editor_open(struct HostedPlugin* p, void* parent_native_view);
void plugin_editor_close(struct HostedPlugin* p);
/* The size the plugin wants, in pixels. False if it has no attached view. */
int  plugin_editor_size(struct HostedPlugin* p, uint32_t* w, uint32_t* h);
/* Tell an attached view the size it actually got. False if it refused. */
int  plugin_editor_set_size(struct HostedPlugin* p, uint32_t w, uint32_t h);

/* ── State ──────────────────────────────────────────────────────────────── */

/* Save into `out`, returning bytes written, or the size needed when `out` is
   NULL or `cap` is too small. Opaque: only the plugin knows what it means. */
uint32_t plugin_state_save(struct HostedPlugin* p, uint8_t* out, uint32_t cap);

/* Restore. False if the plugin rejected the blob — which it should, if the
   blob came from a different plugin. */
int plugin_state_load(struct HostedPlugin* p, const uint8_t* bytes, uint32_t len);

/* ── Health: which plugin is misbehaving ────────────────────────────────── */

/*
 * Every plugin is timed, always, in every build.
 *
 * A plugin runs third-party code on the audio thread and clockwork cannot
 * make it well behaved — it can only notice. So each plugin_process call is
 * bracketed by two clock reads and the result kept per instance.
 *
 * This costs NOTHING when no plugin is loaded, because the code only runs
 * inside plugin_process. There is no process-wide allocator hook and no global
 * operator new override, so a host that hosts no plugins pays nothing at all —
 * not "a little", nothing.
 *
 * Timing rather than allocation counting is deliberate. Counting allocations
 * precisely needs global `operator new` overrides, which would tax every
 * allocation in the whole host application, collide with any consumer that has
 * its own, and cannot coexist with ThreadSanitizer (see rt_alloc.h). It also
 * answers a narrower question than the one worth asking: allocation is only
 * one way to blow a block's budget, alongside locks, page faults, file IO, and
 * simply being slow. budget_overruns catches all of them.
 *
 * What timing cannot say is WHY. For that, run an instrumented build, where
 * allocations during the call are attributed to the plugin rather than hidden.
 */
typedef struct PluginHealth {
    const char* name;             /* the plugin's own, for a log a user submits */
    uint64_t    calls;
    uint64_t    max_ns;           /* worst single call */
    uint64_t    total_ns;
    uint64_t    budget_ns;        /* frames / sample_rate — what it must beat */
    uint64_t    budget_overruns;  /* calls that did not */
    uint64_t    allocations;      /* instrumented builds only; 0 otherwise */
} PluginHealth;

/* Read one plugin's health. False if there is no such plugin. */
int plugin_health(const struct HostedPlugin* p, PluginHealth* out);

/*
 * One line per plugin for the debug log. The point is that a user who submits
 * a log hands over the attribution without having to know what to look for, or
 * that there was anything to look for.
 *
 * Writes at most `cap` bytes including the terminator, returns the length
 * written, and never allocates.
 */
uint32_t plugin_health_line(const struct HostedPlugin* p, char* out, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_PLUGIN_HOST_H */
