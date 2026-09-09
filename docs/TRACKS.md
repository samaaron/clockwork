<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2025-2026 Sam Aaron -->

# Tracks

A track is a name, an ordered chain of hosted plugins, and a pair of stereo
lanes the DSP graph can address. It is how a plugin gets a place to live.
`src/plugin_track.{h,cpp}` is the module; `src/plugin_bridge_verbs.cpp` is
its OSC surface; [TRACKS.md](TRACKS.md) is the layer under
it. Both run in the **plugin bridge**, a separate process
(`src/native/PluginBridgeMain.cpp`, contract in `src/plugin_bridge.h`);
`src/native/TrackControl.cpp` is the engine's side — it owns the shared
memory, spawns the bridge, and relays the verbs. The engine itself never
loads a plugin.

## What a track is for

A track is **studio state, not code**. It is created, named, and filled with
plugins from a control surface — a GUI, a rig file — and *referenced* from
code by name, the way a hardware channel is patched once and then played.
The language sees nothing of plugins. It sees a named thing it can:

- **send audio to and hear back** — an effect chain, as a send/return;
- **play** — notes, controllers, pitch bend, to the instruments in the chain;
- **set parameters on**, by the parameter's own name.

Two tracks with one name would leave code addressing whichever came first, so
names are unique and a create with a taken name is refused. Ids and node
handles are never reused: a stale one names *nothing*, not the track that
took its slot.

## Where a track sits

Every track owns a fixed SEND lane and RETURN lane, both stereo, laid out
above the device's channels:

```
send   = output channels  base + 2*slot, +1     the graph writes here
return = input  channels  base + 2*slot, +1     the graph reads here
```

`TrackControl::reserveLanes()` reserves `CLOCKWORK_TRACK_MAX * 2` lanes (32) with
`clockwork_reserve_lanes` *before the engine boots*; `init_memory` widens the DSP's
input and output staging to `base + lanes` and publishes `base` through
`clockwork_lane_base()`. `base` is 32 for any device up to 32 channels wide, and
the next multiple of 8 above a wider one. A device so wide that the lanes
would not fit under `kMaxChannels` (128) gets no lanes, and `base` stays 0.

The lanes are a pair of PORTS ([PORTS.md](PORTS.md)): a sink
`tracks/send` bound at `base` and a source `tracks/return` bound at the same
channels, both over rings in the shared segment. From the engine's block
loop a track is the same thing as Link audio or a file being written — a
port with a binding — and the block loop knows nothing about plugins.

The device never sees the lanes: it copies only its own channel count. From
inside the graph they are ordinary channels — `Out.ar(base + 2*slot, sig)`
sends, `SoundIn.ar(base + 2*slot)` returns. No UGen, no ring, no port.

## The block

Each block the engine's loop pulls the return port into the input staging
(`pull_port_sources`, before `dsp_process`) and pushes the send lanes out to
the send port after it (`push_port_sinks`). The send port's transfer signal
(`TrackControl::onSendTransfer`) stamps the block's time and the session
clock into the segment and posts a semaphore — the doorbell. The bridge's
render thread wakes on it, takes one block from the send, walks every slot
(`clockwork_track_process`), and puts one block on the return. For each slot the
walk:

1. copies the send lane into a work buffer (or silence, if nothing lives in
   the slot — an empty slot's return lane must read as silence, not as what
   the last track to hold it left behind);
2. runs the chain first to last — an INSTRUMENT generates and is mixed in, an
   EFFECT transforms in place, a BYPASSED node is skipped;
3. ramps the track's gain (mute is a gain of zero, same ramp) across the
   block;
4. writes the result to the return lane, which `dsp_process` reads this
   block.

The return is primed with `slack_blocks` of silence when the ports open, so
what goes out at block *k* comes back at block *k + slack*: **two blocks by
default**, ~5 ms at 128 frames and 48 kHz. One block is what a plugin would
cost in process; the other is a whole callback interval of grace for the
bridge to render in, without which any render that overran the gap between
two callbacks would be a dropout. `CLOCKWORK_PLUGIN_BRIDGE_SLACK=1` trades that
grace away. A bridge that fell behind and caught up would leave the return
deeper than it found it, so the engine trims it to `slack − 1` blocks each
time it posts. A note played *on* a track is late by the same round trip,
because it is rendered in the bridge too — events cross with the block they
belong to and land on their frame within it.

Audio for a track the bridge is not rendering (it crashed; it is
restarting) is silence: the return reads zeros where a block should be, and
the engine's own graph never waits.

The chain is published as an immutable snapshot behind an atomic pointer.
The audio thread reads it without locking; every control-side edit builds a
new snapshot, swaps it in, and waits two block-epochs before freeing the old
one (250 ms cap, immediate if no block has ever run). Plugins are closed only
after the snapshot that referenced them has been retired, and `Track`/`Node`
are heap objects with stable addresses, so a plugin's edit-listener context
outlives every republish.

## Playing a track

`clockwork_track_note`, `clockwork_track_cc`, `clockwork_track_pitch_bend`,
`clockwork_track_all_notes_off`, `clockwork_track_node_param` and
`clockwork_track_param_by_name` are **audio-thread safe**: they resolve through the
snapshot and hand the event to the plugin with a frame offset. That is what
lets a scheduled event land on its sample rather than on the next block
boundary. `clockwork_track_resolve(name)` is the audio-safe name lookup for the
same reason.

A note goes to *every* instrument in the chain, the way Live layers the
instruments in a rack: a track is played as one thing, and a user who puts
two synths on it means both to sound. Effects are not sent notes. To
address ONE of several, give it a **listen channel** (`track/plugin/channel`,
1–16; 0 is every channel, the default) and play it with the matching
`channel:` — the MIDI convention, and the one every plugin already
understands.

A parameter set by name goes to the first node in the chain that has one so
called, or to the one node named by a handle. Exact match first, then
case-insensitive; the table is built at load, so this costs what a note does.

## Rigs

A rig is every track, in order, with every chain and every plugin's state,
plus the user's extra plugin folders — the whole studio as a JSON file:

```json
{ "clockwork_rig": 1,
  "plugin_folders": ["/Users/me/Plugins"],
  "tracks": [
    { "name": "bass", "gain": 1.0, "mute": 0,
      "chain": [
        { "format": "vst3", "id": "…", "name": "Diva", "vendor": "u-he",
          "path": "/Library/Audio/Plug-Ins/VST3/Diva.vst3", "index": 0,
          "bypass": 0, "state": "<base64>" } ] } ] }
```

On load the path is tried first; if the file is gone (another machine, a
moved folder) the id is looked up across every search folder — the one time
the engine scans on its own, because the user asked for those plugins. A
plugin that still cannot be found is counted in `missing` and the rest of the
rig loads around it, in the same order; a rig is not refused because one
plugin is. Load replaces the current tracks. Ids on load are fresh.

## The bridge

Third-party code that allocates and locks on the audio thread, opens
windows, and crashes has no place in the process whose one job is to make
the next block on time. So plugins live in `clockwork-plugin-bridge` (the name is
`CLOCKWORK_PLUGIN_BRIDGE_NAME`; a product gives it its own), a binary built beside
the host's and spawned by `TrackControl::init` with the engine's pid as its
one argument. On macOS it is an app bundle, `<name>.app`, with the bundle
identifier `CLOCKWORK_PLUGIN_BRIDGE_BUNDLE_ID`: that is what OBS's application
capture and a ScreenCaptureKit filter select a process by, and a bare
executable has none, so its editor windows could be seen but not chosen. The two share one memory segment (`src/plugin_bridge.h`):

```
Header      geometry, liveness, the block time, the mirrored session clock
send ring   the engine's sink   → the bridge's source     (clockwork_port_open_shared)
return ring the bridge's sink   → the engine's source
ctl ring    control verbs, engine → bridge, with the caller's token
rt ring     audio-thread verbs, engine → bridge, [frame offset][osc]
out ring    replies and broadcasts, bridge → engine, [egress route][osc]
mirror      the rig as JSON, rewritten by the bridge after every change
```

The engine's ingress routes every `/clockwork/track/*` verb to
`TrackControl`, which writes it to a ring and returns; the bridge decodes
it (`TrackVerbs`), acts, and writes its reply — or a broadcast, or a debug
line — to the out ring, which the engine's NRT gateway drains once per block
and hands to `OscEgress` with the route and token the frame carries. A
reply reaches the caller that asked; a broadcast reaches every subscriber;
neither side knows the other's sockets.

**When it dies.** The engine checks the bridge's liveness every gateway
pass: a process that exited or was signalled, one that stopped rendering
(`bridge_heartbeat` still while `engine_blocks` moves — 400 blocks, ~1 s),
one that never joined (30 s). Each is a `track/error "bridge" <detail> 0`
broadcast and a respawn with `restore` set, and the new bridge rebuilds the
rig from the mirror — every track, chain and plugin state as of the last
change, minus whatever plugin it was loading when it died, which the
header names (`loading_plugin`) so the message can. Three deaths inside
30 s is a crash loop: the next bridge starts empty, and the message says
so. The audio rings are reset on a respawn; the control ring is not, so a
verb sent while the bridge was down is answered by the next one. Events
on the realtime ring from before the join are dropped: a note-on from
before a crash must not sound after it.

The bridge, for its part, leaves when `quit` is set, when the engine's pid
is no longer its parent, or when `generation` — bumped by the engine on
every spawn — is not the one it joined with. Its editor windows are its
own, on its own run loop; closing the bridge closes them.

## Discovery

`plugin_discovery` knows the platform folders (`~/Library/Audio/Plug-Ins`,
`/Library/Audio/Plug-Ins`, `~/.vst3`, `~/.clap`, `%COMMONPROGRAMFILES%`,
`%LOCALAPPDATA%`, …) and the user can add folders of their own with
`plugin_add_search_path`; extras are scanned first. Scanning is recursive but
does not descend into a bundle.

`track/scan` runs that scan in the bridge and answers `track/plugins`: one
entry per plugin, sorted by vendor then name, de-duplicated, in pages of
64 (`<total> <offset> <count>` first, like `plugin/params`) so a studio's
five hundred plugins never make one message a UDP relay cannot carry. A scan opens
every plugin it finds and runs its initialisation, which takes seconds and
is the likeliest place for a plugin to crash — the reason the verb exists
here and not in a GUI. The bridge is the one process built to be crashed:
a scan that dies takes it, the engine restarts it with its tracks, and
`track/error "bridge"` names the plugin that was loading (the scan stamps
`loading_plugin` exactly as an add does). Nothing else ever loads plugin
code: not the engine, not the GUI, so neither needs the entitlements that
loading foreign code demands. The scan runs on the bridge's main thread, so
control verbs and editor windows wait for it; the tracks keep playing.

## The OSC surface

Under `/clockwork/track/`. A reply goes to the sender; a broadcast goes to every
subscriber. `<track>` is an int id or a string name.

Audio-thread verbs — dispatched inside the block, so `/clockwork/schedule` can
place them on a frame:

```
track/note         <track> <on:i> <pitch:i> <velocity:f|i> [channel:i]
track/cc           <track> <number:i> <value:f|i> [channel:i]
track/bend         <track> <bend:f -1..1> [channel:i]
track/notes_off    [track]          no track, or "*": every track (a stop)
track/param        <track> <name:s> <value:f> [handle:i]
track/plugin/param <handle:i> <id:i> <value:f>
```

A velocity or controller value is a float 0..1 or an int 0..127.

Control verbs:

```
track/list                       → track/list.reply       <lane_base> <count> <track…>  (also broadcast)
track/create   <name>            → track/create.reply     <id> <ok> <name|error>
track/remove   <track>           → track/remove.reply     <id> <ok>
track/rename   <track> <name>    → track/rename.reply     <id> <ok> <name|error>
track/move     <track> <index>
track/clear
track/gain     <track> <gain:f>
track/mute     <track> <mute:i>
track/timeline <track> [name:s]  → track/timeline.reply <id> <ok> <timeline|error>
                                   which timeline the track's plugins are told the time of:
                                   "link" (the session clock, the default) or a midi follower
                                   timeline by its /clockwork/clock name ("midi:<port>", claimed
                                   if the port has not clocked yet); no name asks

track/plugin/add    <track> <path> [index] [at]
                                 → track/plugin/add.reply    <handle> <ok> <name|error> <path>
track/plugin/remove <handle>     → track/plugin/remove.reply <handle> <ok>
track/plugin/move   <handle> <index>
track/plugin/bypass <handle> <bypass:i>
track/plugin/channel <handle> <channel:i 0..16>   the listen channel; 0 = all
track/plugin/editor <handle> <show:i>
track/plugin/params <handle> [offset]  → broadcast, one page, see below

track/rig/save <path>            → track/rig/save.reply <ok> <path|error>
track/rig/load <path>            → track/rig/load.reply <ok> <missing> <path|error>

track/folders                    → track/folders.reply <n_extra> <dir…> <n_platform> <dir…>  (also broadcast)
track/folders/add    <dir>
track/folders/remove <dir>
track/scan                       → track/plugins.reply <total> <offset> <count> per plugin: <name> <vendor> <format> <path> <index> <is_instrument>
                                   (pages of 64, also broadcast as track/plugins; seconds of work, see Discovery)
```

Every edit rebroadcasts:

```
track/list     <lane_base> <count>
                 per track: <id> <slot> <name> <send_ch> <return_ch> <gain> <mute> <node_count>
                 per node:  <handle> <is_instrument> <bypass> <channel> <name> <vendor> <format> <path> <index> <latency>
                 then, after the last track, per track again: <timeline>
                            (trailing so a reader that counts the nodes is not shifted by it — but
                            a reader that then requires "no more arguments" must read the trailing
                            strings too: Sonic Pi's did not, and rejected every list with a track in it)
track/state    <id> <gain> <mute> <timeline>        gain and mute, and the timeline appended
track/folders  as the reply
track/plugin/param/edit <handle> <id> <normalized>  a plugin's own editor moved a knob; the engine has
                                                    already applied it to the processor
track/plugin/param/value <handle> <id> <normalized> a parameter set BY NAME (track/param: what code says),
                                                    echoed so a panel drawing it can follow. A set by id
                                                    is not echoed — it was the panel's
track/error    <verb> <detail> <handle>             a create, rename, plugin/add, plugin/editor or rig verb failed;
                                                    also a rig that loaded with plugins missing. handle names
                                                    the plugin it concerns, 0 when none. verb "bridge": the
                                                    plugin process died or hung and is restarting (see above)
track/plugin/params <handle> <total> <offset> <count>
                 per param: <id> <name> <min> <max> <value> <group> <group_name> <automatable>
```

The last two are broadcast rather than replied because of how a GUI behind a
daemon is wired: its requests leave on a socket nothing reads, and what it does
hear is what the daemon forwards, over UDP. So a failure the user needs to
see goes out as `track/error`, and the parameter list goes out in pages —
a synth publishes thousands (Surge XT: 2855), more than fits a datagram or
the engine's egress ring. Each request answers ONE page of up to 48
parameters (fewer if their names are long) starting at `offset`, carrying
the total; the client asks for `offset + count` next and has the lot when
the offsets cover it. A client that asked for the whole list in one go got
the first twenty pages and silence: the ring is drained only between
requests.

Because a page is broadcast, EVERY subscriber hears every page, whoever
asked. A subscriber that continues a chain must continue only the page it
is waiting for — the one at its own `got` offset — and ask for the next
only when it took one. Two subscribers each continuing every page they
hear fork the chain at each step (1, 2, 4, 8… requests): Surge's sixty
pages became nineteen thousand in twelve seconds, and the engine did
nothing else.

A client keeps its picture of the studio from `track/list`: it carries the
lane base, so the client can compute a track's channels without asking
anything else.

## Limits, and where they come from

- `CLOCKWORK_TRACK_MAX` (16): each track costs two lanes for the process's life,
  and the audio thread walks every slot every block.
- `CLOCKWORK_TRACK_MAX_NODES` (16): a bound on the published array.
- `CLOCKWORK_TRACK_NAME_MAX` (64): including the terminator.
- Stereo only. A track is a stereo send/return; a plugin with a different
  layout is opened stereo.

## Tests

`test/test_plugin_track.cpp`, against `ClockworkTestGain` (out = in × gain): the
model and the block walk, in process, through `clockwork_track_process` on staging
the test lays out itself. `test/test_plugin_bridge.cpp` is the process: the
test makes the segment the engine would, spawns the real bridge binary,
sends verbs and blocks across, reads back what it rendered, kills it with
SIGKILL and checks the next one comes back with the tracks.
