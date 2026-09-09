// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_port_bus.h — where a port meets the DSP.
 *
 * `clockwork_ports.h` says what a port is and refuses to say what it is for. This
 * says the one thing clockwork has to decide: WHICH DSP CHANNELS a given
 * port occupies. A source is read into input channels starting at
 * `first_channel` just before dsp_process; a sink is written from output
 * channels starting at `first_channel` just after it. Direction comes from the
 * port, so there is one call, not two.
 *
 * WHY THIS IS NOT IN clockwork_ports.h. A channel range is a routing decision and
 * the substrate has no business holding one — it would be the first thing in
 * that file that assumed the other end was a DSP. It is also not in the
 * endpoint crates, because an endpoint does not know what its port is wired
 * to; that is exactly the indirection ports buy.
 *
 * WHY AN EXPLICIT BINDING RATHER THAN "SOURCES IN SLOT ORDER". Because slot
 * order is stable and channel order derived from it is not: close the first of
 * three sources and the other two would each shift down a channel, handing the
 * DSP somebody else's audio on a channel it is already processing. That is the
 * precise fault dsp_api.h and clockwork_ports.h both forbid, arriving through the
 * one door neither of them watches. An attachment is a slot of its own — a
 * table entry that is never compacted — so detaching one leaves every other
 * binding on exactly the channels it had.
 *
 * WHAT THE DSP MAKES OF IT is not here either, but it is one read away: every
 * attach and detach updates the channel map (ClockworkChannelMapState,
 * shared_memory.h), which a guest reaches through DspConfig::channel_map and a
 * client through the segment header. That is what turns a channel range into
 * an identity — without it the DSP has channels 8 and 9 and no way to know
 * they are one stereo file.
 *
 * A source OVERWRITES its channel range; it does not mix into it. Two sources
 * on the same channel are a configuration error clockwork does not attempt
 * to resolve, and mixing would silently make one of them quieter instead.
 *
 * LINK. A Link peer is a source and Link's aux publish is a sink, and neither
 * needs anything here that disk did not: open a port, attach it to a channel
 * range, and have the bridge produce/consume on its own thread. Peer channels
 * need room above the device's input channels, so the ceiling that matters is
 * DspConfig::max_input_channels — what the DSP allocated for — not the live
 * per-block count the device happens to supply.
 */
#ifndef CLOCKWORK_PORT_BUS_H
#define CLOCKWORK_PORT_BUS_H

#include "clockwork_ports.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How many bindings there can be at once. Small on purpose: this is
   clockwork's routing table, not a mixer. */
#define CLOCKWORK_PORT_BUS_MAX 16

/*
 * Bind an open port to a DSP channel range beginning at `first_channel`.
 * Returns 1 on success, 0 if the port is not open, the table is full, or the
 * port is already attached. Channels the session does not have are simply not
 * transferred — the substrate's own mismatch rule, not a refusal.
 *
 * Control thread. Takes effect on the next block.
 */
int clockwork_port_bus_attach(ClockworkPort port, uint32_t first_channel);

/* Detach a port. The entry is cleared, and no other binding moves. Safe to
   call for a port that is not attached, and safe while audio is running. */
void clockwork_port_bus_detach(ClockworkPort port);

/* Detach everything. For shutdown and for tests. */
void clockwork_port_bus_detach_all(void);

/* How many bindings are live. Diagnostics. */
uint32_t clockwork_port_bus_attached(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_PORT_BUS_H */
