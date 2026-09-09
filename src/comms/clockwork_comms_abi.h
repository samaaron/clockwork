/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2025-2026 Sam Aaron
 *
 * clockwork_comms_abi.h — the C ABI of the command transports (crate
 * clockwork-comms): the owner-only Unix datagram socket, the TCP/UDS stream
 * servers and the Windows named pipe. The C++ transports in this directory
 * are built on it. An ENGINE never includes this: sockets are a client's.
 */
#ifndef CLOCKWORK_COMMS_ABI_H
#define CLOCKWORK_COMMS_ABI_H

#include <stdint.h>
#include "clockwork_osc.h"   /* the emit callback types the transports share with the cue server */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkOscUds ClockworkOscUds;
typedef void (*clockwork_osc_emit_path_fn)(void* ctx, const uint8_t* peer_path, uint32_t peer_len,
                                    const uint8_t* osc, uint32_t len);
ClockworkOscUds* clockwork_osc_uds_dgram_start(void* ctx, clockwork_osc_emit_path_fn emit,
                                 const uint8_t* path, uint32_t path_len);
/* Reply to the peer bound at `path` from the server's own socket. Returns 1 on
 * send, 0 on failure/empty path. Off the audio thread. */
int32_t clockwork_osc_uds_dgram_send(ClockworkOscUds* handle, const uint8_t* path, uint32_t path_len,
                              const uint8_t* data, uint32_t len);
/* Stop the recv thread, close the socket, unlink the path, free the server. */
void clockwork_osc_uds_stop(ClockworkOscUds* handle);

/* ── Stream servers: TCP, UDS stream, Windows named pipes ─────────────────────
 * Connection-oriented OSC over one shared wire format: each packet is preceded
 * by a 4-byte big-endian length (the OSC-over-stream convention; max payload
 * 64 KiB, violations close the connection). Admission control happens at
 * accept: at most `max_conns` concurrent connections, an over-cap connect is
 * immediately closed (TCP/UDS) or fails busy (pipes). Connection ids are
 * minted from 1 and never reused — a transport uses them directly as origin
 * tokens (0 = the in-process caller). `on_closed` fires once when a
 * connection ends (EOF, error, protocol violation) but not during server
 * stop, so a transport can drop that client's subscriptions: subscription
 * lifetime == connection lifetime. Callbacks may fire on any reader
 * thread. */
typedef struct ClockworkOscStream ClockworkOscStream;
typedef void (*clockwork_osc_stream_packet_fn)(void* ctx, uint32_t conn_id,
                                        const uint8_t* osc, uint32_t len);
typedef void (*clockwork_osc_stream_closed_fn)(void* ctx, uint32_t conn_id);

/* TCP on `port` bound to `bind_addr` (empty = all IPv4 interfaces; pass "::"
 * for IPv6). Port 0 binds ephemerally — read it back with clockwork_osc_stream_port.
 * Null on bind failure. */
ClockworkOscStream* clockwork_osc_tcp_start(void* ctx, clockwork_osc_stream_packet_fn on_packet,
                              clockwork_osc_stream_closed_fn on_closed, int32_t port,
                              const uint8_t* bind_addr, uint32_t bind_addr_len,
                              uint32_t max_conns);
/* UDS stream at `path` (socket file created 0600, stale file replaced). Null
 * on failure, and always null on Windows. */
ClockworkOscStream* clockwork_osc_uds_stream_start(void* ctx, clockwork_osc_stream_packet_fn on_packet,
                                     clockwork_osc_stream_closed_fn on_closed,
                                     const uint8_t* path, uint32_t path_len,
                                     uint32_t max_conns);
/* Windows named pipe: `name` is a bare name (prefixed with \\.\pipe\) or a
 * full pipe path. Instances carry an owner-only DACL (SYSTEM + current user)
 * and PIPE_REJECT_REMOTE_CLIENTS; the first instance is squat-checked
 * (FILE_FLAG_FIRST_PIPE_INSTANCE). Always null on non-Windows platforms. */
ClockworkOscStream* clockwork_osc_pipe_start(void* ctx, clockwork_osc_stream_packet_fn on_packet,
                               clockwork_osc_stream_closed_fn on_closed,
                               const uint8_t* name, uint32_t name_len,
                               uint32_t max_conns);
/* The actual bound TCP port (for port-0 starts); 0 for path/name servers. */
int32_t clockwork_osc_stream_port(ClockworkOscStream* handle);
/* Frame and send one packet to a live connection. 1 = sent, 0 = unknown/closed
 * connection or write failure. Off the audio thread. */
int32_t clockwork_osc_stream_send(ClockworkOscStream* handle, uint32_t conn_id,
                           const uint8_t* data, uint32_t len);
/* Stop accepting, close every connection, join the threads, free the server. */
void clockwork_osc_stream_stop(ClockworkOscStream* handle);

#ifdef __cplusplus
}
#endif
#endif /* CLOCKWORK_COMMS_ABI_H */
