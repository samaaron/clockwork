// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_host_transport.h — a host's own transport, said to the timeline.
 *
 * The session's timeline (its tempo, its meter, whether it plays, where its
 * beat is) starts at its default and is set by its sources: its clients
 * (/clockwork/clock/tempo/set and the rest) and its Link peers. A host with
 * a transport of its own (a DAW around a plugin) is one more source
 * (clockwork_host_tempo and the rest, lanes.h; clockwork_embed_tempo and
 * the rest, clockwork_embed.h). It says its transport in the clock's own
 * four operations, each for a frame of its audio, from the thread that
 * ticks:
 *
 *   - tempo: the timeline's from that frame, the beat playing then held;
 *   - meter: the timeline's time signature, and so the length of its bar;
 *   - playing or stopped;
 *   - beat, where it is at that frame, in quarter notes, in one of two ways.
 *     By request (CLOCKWORK_BEAT_REQUEST) the timeline's bars are put on its
 *     bars: the grid is moved by the shorter way round the bar, under a bar,
 *     so the timeline keeps a beat count of its own, and a host that jumps
 *     to another bar line moves nothing. By force (CLOCKWORK_BEAT_FORCE) the
 *     timeline's beat is its beat, number and all, so a host that loops or
 *     jumps takes the timeline with it.
 *
 * A host says them again with each render, and what is there already moves
 * nothing: a tempo that is the timeline's, a beat within half a frame of the
 * grid's. Between one saying and the next its beat is a line in its own
 * frames, running on at the tempo, and each block's grid is kept on that
 * line: the engine's time may be steered by a wall clock, and the timeline
 * stays on the host's audio. A beat said while the timeline is stopped moves
 * nothing: a stopped host's beat stands still, and the timeline runs on at
 * the tempo.
 *
 * No source has the timeline to itself, as none has on a Link session. What
 * a client or a peer sets is taken, and lasts until the host next says its
 * own, a render later; what the host never says (a time signature, from a
 * host that has only a tempo) is left to the others. The clients hear what
 * the host moved as they hear a Link peer's
 * (/clockwork/clock/notify/tempo and notify/transport). Once a host has said
 * anything, SC_FLAG_HOST_TIMELINE in the clock state says a host has a
 * transport (clockwork_clock_state.h), for a reader that would wait for it
 * or show whose tempo it is. A host that never says anything has none, and
 * the flag stays clear. Where the build has Link and it is enabled, what the
 * host moved is given to the Link session as well, so its peers follow the
 * host.
 *
 * Pure C.
 */
#ifndef CLOCKWORK_HOST_TRANSPORT_H
#define CLOCKWORK_HOST_TRANSPORT_H

/* How a host says its beat. */
#define CLOCKWORK_BEAT_REQUEST 0u   /* its place in the bar: the timeline's bars on its bars */
#define CLOCKWORK_BEAT_FORCE   1u   /* its beat number */

#endif /* CLOCKWORK_HOST_TRANSPORT_H */
