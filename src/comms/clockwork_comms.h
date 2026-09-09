// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_comms.h — the command transports, as a client library.
 *
 * An engine has no sockets. It has one door for replies, IOscTransport, and
 * whoever embeds it decides what stands behind that door: nothing (an
 * in-process host answers its own calls through CallbackTransport), or one of
 * the transports here. Each of these listens on the wire, writes what it
 * receives through the client boundary with an origin token of its own
 * minting, and resolves that token back to a connection when the engine
 * replies. The origin registry and every notify audience live here, on the
 * client's side, because "who is on the other end" was never the engine's
 * question.
 *
 * This is the native counterpart of what the JavaScript client does for the
 * worklet: the wire is the client's, on every platform. A host links
 * clockwork_comms and constructs what it needs; SuperSonic's own main
 * (clockwork-supersonic/host/) is one such host.
 *
 *   UdpOscTransport        the scsynth-compatible UDP command port
 *   StreamOscTransport     TCP, Unix-socket stream, or Windows named pipe
 *   UdsDgramOscTransport   Unix datagram socket, owner-only
 *   ShmTransport           the engine's side of the shared-memory command plane
 *   OriginTable            (ip, port) ↔ stable origin token
 *
 * Serving the shared-memory segment to another process (shm_attach.hpp) is a
 * host decision of the same kind, and belongs beside these.
 */
#pragma once

#include "OriginTable.h"
#include "UdpOscTransport.h"
#include "StreamOscTransport.h"
#include "UdsDgramOscTransport.h"
#include "ShmTransport.h"
