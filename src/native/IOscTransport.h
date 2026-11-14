// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * IOscTransport.h — the way out of the engine, to its client.
 *
 * An engine has one client at a time, and everything it sends out is for
 * that client: an answer, a notice, what the guest said. The engine deals
 * only in OSC bytes. A transport owns everything platform-specific behind
 * this interface: who the client is and how a packet physically reaches it.
 * This is the *only* place address/socket/pid knowledge lives.
 * Implementations: StreamOscTransport (TCP / UDS stream / named pipe),
 * ShmTransport (the segment's peer plane), NifTransport (BEAM — its
 * registered Erlang pids, via enif_send), and CallbackTransport (in process
 * — embedders and tests, through the engine's onReply callback).
 *
 * What the client asked to be told is the core's to keep (lanes.h,
 * clockwork_client_wants), not a transport's: a notice is written only
 * while the client has asked for its kind. A transport tells the core when
 * its client has gone (clockwork_client_gone), so the next client starts
 * with nothing asked for.
 *
 * The NRT gateway is the sole caller of deliver (one thread), so
 * implementations need no internal send serialisation beyond their own resource
 * locks (e.g. a socket write lock shared with a recv thread).
 */
#pragma once

#include <cstdint>

struct ClockworkCore;

class IOscTransport {
public:
    virtual ~IOscTransport() = default;

    // The engine this transport carries, told by the engine when its core
    // exists (init): what the transport's own log lines name. A transport
    // with nothing to say of its own ignores it.
    virtual void bindCore(ClockworkCore*) {}   // nullptr at shutdown: the core is about to close

    // To the client. False when the frame reached no one: no client there,
    // a connection that refused the write, a full reply ring. Whoever drains
    // egress counts every one — the engine's gateway, or a host draining
    // egress itself (PerformanceMetrics::egress_frames_undelivered,
    // EgressRouter.h) — so what left an egress ring and was heard by nobody
    // is said.
    virtual bool deliver(const uint8_t* data, uint32_t size) = 0;
};
