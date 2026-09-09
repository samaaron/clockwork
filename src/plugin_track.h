// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_track.h — named tracks of hosted plugins, on clockwork's own lanes.
 *
 * plugin_host.h can open a plugin and run a block. This is what gives one a
 * place to live: a TRACK, which is a name, an ordered chain of plugins, and a
 * pair of stereo lanes the DSP graph can address.
 *
 * WHAT A TRACK IS. Studio state, not code. A track is created, named and
 * filled in from a control surface (a GUI, a rig file) and REFERENCED from
 * code by name — the way a hardware channel is patched once and then played.
 * The language sees nothing of plugins; it sees a named thing it can send
 * audio to, receive audio from, and play notes on.
 *
 * WHERE A TRACK SITS. Each track owns a fixed SEND lane and RETURN lane, both
 * stereo, above the device's channels:
 *
 *   send   = output channels  base + 2*slot, +1   (the graph writes here)
 *   return = input channels   base + 2*slot, +1   (the graph reads here)
 *
 * `base` is where the engine's lanes start (clockwork_lane_base() in the engine,
 * handed to this model with clockwork_track_set_lane_base). The graph sends to a
 * track with an ordinary Out.ar on the send channels, and hears it back with
 * SoundIn.ar on the return channels — no new UGen, nothing the graph has to
 * learn.
 *
 * Every block the engine hands what the graph put on the send lanes to the
 * bridge and takes back what the bridge rendered from the block before:
 * the walk here reads each send, runs it through the track's chain, and
 * writes the result to the track's return. The round trip is the bridge's
 * slack — two blocks by default: one is what the plugin would cost in
 * process, the other is a whole callback interval of grace for the render
 * (src/plugin_bridge.h) — and a note is late by the same amount, because it
 * is rendered in the bridge too. The lanes never reach the device: the device copies only its
 * own channel count.
 *
 * A CHAIN, IN THE USER'S ORDER. Nodes run first to last. An INSTRUMENT
 * generates and is MIXED INTO the track; an EFFECT transforms the track in
 * place; a BYPASSED node is skipped. Which a node is comes from the plugin
 * itself (PluginDesc::is_instrument), never from the caller. An empty chain
 * is a wire: send goes straight to return.
 *
 * NAMES ARE THE API. Code addresses a track by name; the GUI addresses it by
 * id. An id is never reused, so a stale one names NOTHING rather than the
 * track that took its slot. Node handles likewise.
 *
 * THREADS. Everything that creates, removes, loads, saves or reorders is
 * control-thread only and takes a lock. Everything marked AUDIO THREAD reads
 * the published snapshot without locking and may be called from inside the
 * block — that is how a scheduled note lands on its frame.
 *
 * NOT REALTIME-SAFE beyond that, and inherits every warning in plugin_host.h:
 * a plugin allocates and takes locks on the audio thread. The tracks do not
 * pretend otherwise; they make the window visible (plugin_health).
 *
 * NOT THE ENGINE'S. This model runs in the plugin bridge process
 * (src/native/PluginBridgeMain.cpp), which meets the engine in shared memory
 * (src/plugin_bridge.h): the engine's lanes are a sink and a source port
 * over that memory, and the bridge walks them with clockwork_track_process on a
 * staging of its own. The engine links none of this; a plugin that crashes
 * takes the bridge down and the engine notices. It depends on nothing in
 * clockwork core — only the plugin adapters and a clock it is handed.
 *
 * WHICH CLOCK A TRACK FOLLOWS. A track's plugins are told the musical time
 * — tempo, beat, bar, meter — of the SESSION CLOCK, unless the track is bound
 * to another timeline (clockwork_track_set_timeline): a midi follower timeline the
 * engine hears on a port, so a tempo-synced delay on that track follows the
 * external clock rather than Link. The model never resolves a timeline
 * name — the registry is the engine's — it holds the id it was given and,
 * each block, reads that id's snapshot from the mirror the engine keeps in
 * the segment (clockwork_track_set_timelines). The name is kept for reporting.
 *
 * ONE NOTE, SEVERAL INSTRUMENTS. Every instrument on a track receives every
 * note, so two instruments on a track are a LAYER — the same note plays
 * both and they mix. To address one of them, give it a listen channel
 * (clockwork_track_node_set_channel): a node on channel N takes only events sent
 * on N; a node on 0, the default, takes all. That is the discriminator a
 * multitimbral plugin already understands, so the code that plays a track
 * needs no new word for it — `channel:` on the note is enough.
 */
#ifndef CLOCKWORK_PLUGIN_TRACK_H
#define CLOCKWORK_PLUGIN_TRACK_H

#include <stdint.h>

struct ClockworkClockState;
struct ClockworkTimelineMirrorSlot;

#ifdef __cplusplus
extern "C" {
#endif

/* How many tracks can exist at once. Each costs two stereo lanes, reserved at
   install for the process's whole life, and the audio thread walks every slot
   every block. */
#define CLOCKWORK_TRACK_MAX        16
/* Nodes per chain. A bound on the published array, not a musical limit. */
#define CLOCKWORK_TRACK_MAX_NODES  16
/* A name, including its terminator. */
#define CLOCKWORK_TRACK_NAME_MAX   64

/* A track's identity for as long as it exists. Never reused. */
typedef uint32_t ClockworkTrackId;
#define CLOCKWORK_TRACK_NONE ((ClockworkTrackId)0)

/* A node's identity for as long as it is loaded. Never reused. */
typedef uint32_t ClockworkTrackHandle;
#define CLOCKWORK_TRACK_NO_HANDLE ((ClockworkTrackHandle)0)

/* ── Tracks ─────────────────────────────────────────────────────────────── */

/*
 * Create a track. `name` must be non-empty, under CLOCKWORK_TRACK_NAME_MAX, and not
 * already in use — a name is how code reaches a track, so two tracks with one
 * name would leave code addressing whichever came first. Returns the new id,
 * or CLOCKWORK_TRACK_NONE with `err` pointing at a static string.
 */
ClockworkTrackId clockwork_track_create(const char* name, const char** err);

/* Remove a track, closing every plugin in it. Non-zero if it existed. */
int clockwork_track_remove(ClockworkTrackId t);

/* Remove every track. */
void clockwork_track_clear(void);

/* Rename. Same rules as create. Non-zero on success. */
int clockwork_track_rename(ClockworkTrackId t, const char* name, const char** err);

/* Reorder the track list, 0 first; past the end means last. Display order
   only — a track's lanes are its slot's, and moving it changes nothing the
   graph can hear. Non-zero if the track exists. */
int clockwork_track_move(ClockworkTrackId t, uint32_t to_index);

/* The track with this name, or CLOCKWORK_TRACK_NONE. Control thread. */
ClockworkTrackId clockwork_track_find(const char* name);

/* The same, AUDIO THREAD SAFE: read from the published snapshot, so a
   scheduled event addressed by name resolves inside the block. */
ClockworkTrackId clockwork_track_resolve(const char* name);

uint32_t clockwork_track_count(void);

typedef struct ClockworkTrackInfo {
    ClockworkTrackId id;
    uint32_t   slot;             /* which lane pair, 0..CLOCKWORK_TRACK_MAX-1 */
    char       name[CLOCKWORK_TRACK_NAME_MAX];
    uint32_t   send_channel;     /* first output channel of the send lane */
    uint32_t   return_channel;   /* first input channel of the return lane */
    float      gain;             /* linear, applied after the chain */
    int32_t    mute;
    uint32_t   node_count;
    int32_t    timeline_id;      /* 0: the session clock; 1..: a midi follower slot */
    char       timeline[CLOCKWORK_TRACK_NAME_MAX];   /* its name: "link", "midi:<port>" */
} ClockworkTrackInfo;

/* Every track in display order. Returns how many exist, which may exceed
   `cap`; writes at most `cap`. NULL/0 asks the count. */
uint32_t clockwork_track_list(ClockworkTrackInfo* out, uint32_t cap);

/* One track. Non-zero if it exists. */
int clockwork_track_info(ClockworkTrackId t, ClockworkTrackInfo* out);

/* Post-chain gain (linear; 1 is unity) and mute. Control thread; the change
   is smoothed across the next block rather than stepped. */
int clockwork_track_set_gain(ClockworkTrackId t, float gain);
int clockwork_track_set_mute(ClockworkTrackId t, int mute);

/* Bind the track's plugins to a timeline: 0 with name "link" is the session
   clock (the default); 1.. is a midi follower slot, with the name the engine
   resolved it from, kept for reporting. Control thread; takes effect on the
   next block. 0 for an unknown track, a negative id, or a name that is empty
   or too long. */
int clockwork_track_set_timeline(ClockworkTrackId t, int32_t timeline_id, const char* name);

/* Where the engine's lanes start: the number a track's channels are counted
   from in ClockworkTrackInfo. Set by whoever owns the staging (the bridge, told by
   the engine), so the channels the model reports are the engine's own even
   though the model never sees the engine's memory. 0 until set. */
uint32_t clockwork_track_lane_base(void);
void     clockwork_track_set_lane_base(uint32_t base);

/* ── Chains ─────────────────────────────────────────────────────────────── */

/*
 * Open a plugin and insert it into a track's chain at `at_index` (past the
 * end means last). Control thread only — this loads a shared library.
 *
 * `index` selects within a bundle that holds several (see plugin_scan).
 * `max_block` of 0 means this module's own maximum.
 *
 * Returns the node's handle, or CLOCKWORK_TRACK_NO_HANDLE on failure with `err`
 * pointing at storage the adapter owns.
 */
ClockworkTrackHandle clockwork_track_add_plugin(ClockworkTrackId t, const char* path, uint32_t index,
                                    uint32_t at_index, double sample_rate,
                                    uint32_t max_block, const char** err);

/* Remove a node and close its plugin. Non-zero if it existed. Safe on a
   handle that is already gone. */
int clockwork_track_remove_plugin(ClockworkTrackHandle h);

/* Move a node within its chain, 0 first; past the end means last. */
int clockwork_track_move_plugin(ClockworkTrackHandle h, uint32_t to_index);

/* Skip a node without unloading it. Its state, editor and voices survive;
   only its contribution to the block stops. */
int clockwork_track_node_bypass(ClockworkTrackHandle h, int bypass);

/* The MIDI channel an instrument node listens on: 1..16, or 0 for every
   channel (the default). An effect node has no use for it but may hold it.
   Held notes are released on the change. 0 for a bad channel or handle. */
int clockwork_track_node_set_channel(ClockworkTrackHandle h, int16_t channel);

typedef struct ClockworkTrackNode {
    ClockworkTrackHandle handle;
    ClockworkTrackId     track;
    int32_t        is_instrument;
    int32_t        bypass;
    int32_t        channel;      /* listen channel, 0 = all */
    uint32_t       latency;      /* the plugin's reported latency, frames */
    char           name[128];    /* the plugin's own, not the filename */
    char           vendor[128];
    char           id[128];      /* the format's stable identifier */
    char           format[8];    /* "vst3" or "clap" */
    char           path[1024];
    uint32_t       index;        /* within the bundle at `path` */
} ClockworkTrackNode;

/* A track's chain in order. Same count/cap contract as clockwork_track_list. */
uint32_t clockwork_track_nodes(ClockworkTrackId t, ClockworkTrackNode* out, uint32_t cap);

/* One node. Non-zero if it exists. */
int clockwork_track_node_info(ClockworkTrackHandle h, ClockworkTrackNode* out);

/* The track a node is in, or CLOCKWORK_TRACK_NONE. */
ClockworkTrackId clockwork_track_node_track(ClockworkTrackHandle h);

/* Which slots hold a track, bit k for slot k, read from the published
   snapshot: audio-thread safe. The bridge mirrors it into the segment each
   block so the engine, which never sees the model, knows which return lanes
   carry a track and can tap them (a scope per track). */
uint32_t clockwork_track_slot_mask(void);

/* ── Playing a track — AUDIO THREAD SAFE ────────────────────────────────── */

/*
 * These read the published snapshot without locking, so a scheduled event
 * can be delivered from inside the block with `frame_offset` placing it on
 * its sample. They are also fine from the control thread, where an offset of
 * 0 means "next block".
 *
 * A note goes to EVERY instrument in the chain: a track is played as one
 * thing, and a user who layers two synths on a track means both to sound.
 * Effects are not sent notes. Controllers and bend go to every instrument
 * likewise; a plugin without a mapping for a controller ignores it.
 */
void clockwork_track_note(ClockworkTrackId t, int on, int16_t channel, int16_t pitch,
                    float velocity, uint32_t frame_offset);
void clockwork_track_cc(ClockworkTrackId t, int16_t channel, uint8_t number, float value,
                  uint32_t frame_offset);
void clockwork_track_pitch_bend(ClockworkTrackId t, int16_t channel, float bend,
                          uint32_t frame_offset);
/* CLOCKWORK_TRACK_NONE means every track — a stop, after the schedule is flushed. */
void clockwork_track_all_notes_off(ClockworkTrackId t, uint32_t frame_offset);

/* Set one node's parameter by the plugin's own id (not an index). */
void clockwork_track_node_param(ClockworkTrackHandle h, uint32_t id, double value,
                          uint32_t frame_offset);

/*
 * Set a parameter BY NAME, on the first node in the chain that has one so
 * called (or on the one node named by `h`, if non-zero). Names are matched
 * exactly, then case-insensitively. This is what code uses — `Cutoff` reads;
 * a parameter id is a number only the GUI has seen. Non-zero if a parameter
 * was found. Names are looked up in a table built at load, so this is as
 * cheap as a note.
 */
int clockwork_track_param_by_name(ClockworkTrackId t, ClockworkTrackHandle h, const char* name,
                            double value, uint32_t frame_offset);

/* ── Parameters, for a control surface ──────────────────────────────────── */

/*
 * A node's parameters, by index (see plugin_param_info). Control thread: the
 * strings are borrowed from the plugin and valid until the next call.
 */
uint32_t clockwork_track_node_param_count(ClockworkTrackHandle h);
struct PluginParam;
int clockwork_track_node_param_info(ClockworkTrackHandle h, uint32_t index, struct PluginParam* out);

/*
 * Hear about a parameter moving, with the node it belongs to. `own` is
 * non-zero for the plugin's OWN edit — someone turning a knob in its editor
 * window — and zero for a set BY NAME (clockwork_track_param_by_name: what code
 * says), echoed so that a panel drawing the parameter learns of it either
 * way. Without this the relay is one-way: a panel can drive a plugin and
 * never learn that the plugin moved, or that the code did. A set by id is
 * not echoed: that is the panel's own doing, and an echo of it would only
 * fight the knob it is dragging.
 *
 * Called on whatever thread the plugin edits on, or on the audio thread for
 * a set by name; keep it short and allocation-free. NULL detaches.
 */
typedef void (*ClockworkTrackParamEditFn)(void* ctx, ClockworkTrackHandle h, uint32_t id,
                                    double normalized, int own);
void clockwork_track_set_param_edit_listener(ClockworkTrackParamEditFn fn, void* ctx);

/*
 * Told before a plugin is opened (with its path) and after (with NULL),
 * whether the open came from clockwork_track_add_plugin or from a rig load. What
 * the bridge stamps into shared memory, so that if the open takes the
 * process down the engine can name the plugin that did it.
 */
typedef void (*ClockworkTrackLoadFn)(void* ctx, const char* path);
void clockwork_track_set_load_listener(ClockworkTrackLoadFn fn, void* ctx);

/* ── Editors ────────────────────────────────────────────────────────────── */

/*
 * Show or hide a node's editor in a window this process owns. Safe from the
 * control thread: the hop to the main thread happens inside. Returns at once;
 * the window appears when the main run loop next turns. Show returns 0 for
 * a handle that names nothing or a plugin with no editor to show.
 */
int  clockwork_track_editor_show(ClockworkTrackHandle h);
void clockwork_track_editor_hide(ClockworkTrackHandle h);

/* ── State ──────────────────────────────────────────────────────────────── */

/* One node's opaque state; same contract as plugin_state_save/load. */
uint32_t clockwork_track_node_state_save(ClockworkTrackHandle h, uint8_t* out, uint32_t cap);
int      clockwork_track_node_state_load(ClockworkTrackHandle h, const uint8_t* bytes, uint32_t len);

/*
 * A RIG is every track, in order, with every chain and every plugin's state,
 * plus the user's extra plugin folders — the whole studio, as a JSON file
 * that survives a restart and travels between machines.
 *
 * Plugins are recorded by format and stable id AND by path: on load the path
 * is tried first, and if it is gone (a different machine, a moved folder)
 * the id is looked up across every search folder. A plugin that cannot be
 * found is reported through `missing` and the rest of the rig loads around
 * it, in a chain with the same order; a rig is not refused because one
 * plugin is.
 *
 * Load REPLACES the current tracks. Control thread only, and slow: it opens
 * every plugin. `sample_rate` and `max_block` are handed to each.
 *
 * Both return non-zero on success. `missing`, if given, receives a count of
 * plugins that could not be opened on load.
 */
int clockwork_track_rig_save(const char* path, const char** err);
int clockwork_track_rig_load(const char* path, double sample_rate, uint32_t max_block,
                       uint32_t* missing, const char** err);

/* The same, as text in memory — what a client that keeps its own files, or a
   test, wants. Save returns bytes needed (NUL not counted); writes at most
   `cap`, always terminated when cap > 0. */
uint32_t clockwork_track_rig_to_json(char* out, uint32_t cap);
int      clockwork_track_rig_from_json(const char* json, double sample_rate,
                                 uint32_t max_block, uint32_t* missing,
                                 const char** err);

/* ── Wiring ─────────────────────────────────────────────────────────────── */

/*
 * Hand the tracks the session clock, so hosted plugins are told the tempo.
 * Plugins loaded afterwards read it too; NULL means no clock, and a plugin
 * then sees the defaults. In the bridge this is the engine's clock mirrored
 * into the shared segment.
 */
void clockwork_track_set_clock(const struct ClockworkClockState* clock);

/*
 * Hand the tracks the follower timelines, as the engine mirrors them each
 * block: `count` slots (clock/timeline_mirror.h), slot k carrying timeline
 * id k + 1. A track bound to an id reads its slot every block, on the audio
 * thread, without a lock; one bound to an id past `count`, or to a slot
 * nothing holds, renders against the 60 BPM placeholder every unheld
 * timeline answers with. NULL: no mirror, and every bound track gets the
 * placeholder. In the bridge this is the segment's own array.
 */
void clockwork_track_set_timelines(const struct ClockworkTimelineMirrorSlot* slots, uint32_t count);

/*
 * The block walk. `in` and `out` are staging arrays, channel-major, of
 * `n_in` and `n_out` channel pointers; track slot k sends from
 * out[base + 2k, +1] and returns into in[base + 2k, +1]. What is in `out`
 * is what the graph sent LAST block; what the walk leaves in `in` is what
 * the graph reads THIS block. A slot with no track returns silence. The
 * caller lays the staging out and says where the lanes start in it —
 * nothing here assumes the engine's layout.
 */
void clockwork_track_process(float* const* in, uint32_t n_in,
                       float* const* out, uint32_t n_out,
                       uint32_t frames, int64_t block_time, uint32_t lane_base);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_PLUGIN_TRACK_H */
