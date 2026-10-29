/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2025-2026 Sam Aaron
 *
 * clockwork_comms_abi.h — the C ABI of the command transports (crate
 * clockwork-comms): the per-boot secret, the owner-only Unix datagram
 * socket, the TCP/UDS stream servers and the Windows named pipe. The C++
 * transports in this directory are built on it. An ENGINE never includes
 * this: sockets are a client's.
 */
#ifndef CLOCKWORK_COMMS_ABI_H
#define CLOCKWORK_COMMS_ABI_H

#include <stdint.h>
#include "clockwork_osc.h"   /* the emit callback types the transports share with the cue server */

#ifdef __cplusplus
extern "C" {
#endif

/* ── The secret ───────────────────────────────────────────────────────────────
 * Every stream server starts with the host's secret and refuses to start
 * without one. Whoever launches the host makes 32 random bytes and gets them
 * to the host by a path other processes cannot read: never the command line
 * or the environment. Where the host reads them is the host's decision. A
 * client's first frame on a connection must be the secret's auth packet
 * (/clockwork/auth, the 32 bytes as a blob); nothing is delivered before it,
 * and a connection whose first frame is anything else, or that sends none
 * within 2 s, is closed and counted. */
typedef struct ClockworkCommsSecret ClockworkCommsSecret;
/* The 32 bytes the launcher made. Null for all zeros. */
ClockworkCommsSecret* clockwork_comms_secret_from_bytes(const uint8_t* bytes);
/* A new secret from the OS random source, for a process that launches its
 * own clients. Null if the source fails. */
ClockworkCommsSecret* clockwork_comms_secret_generate(void);
/* The packet a client sends first (unframed OSC), written to `out`. Returns
 * its length, or 0 for a null secret or an `out` too short. */
uint32_t clockwork_comms_secret_auth_packet(const ClockworkCommsSecret* secret,
                                            uint8_t* out, uint32_t cap);
void clockwork_comms_secret_free(ClockworkCommsSecret* secret);

/* ── Stream servers: TCP, UDS stream, Windows named pipes ─────────────────────
 * Connection-oriented OSC over one shared wire format: each packet is preceded
 * by a 4-byte big-endian length (the OSC-over-stream convention; max payload
 * 256 KiB, violations close the connection). Each server keeps its own copy
 * of `secret` and returns null without one. One client: a server has at most
 * one connection that presented the secret and one still presenting it. A
 * connect while a client is in, or while one is still presenting, is closed
 * at once (TCP/UDS) or fails busy (pipes); one that presents the secret once a
 * client is in is closed then. Nothing displaces a connection, so a silent
 * one holds the door for 2 s at most and never the client's place. What
 * stands behind the one client is the host's. Connection ids are
 * minted from 1 and never reused — a transport uses them directly as origin
 * tokens (0 = the in-process caller). `on_closed` fires once when a
 * connection that presented the secret ends (EOF, error, protocol violation)
 * but not during server stop, so a transport can drop that client's
 * subscriptions: subscription
 * lifetime == connection lifetime. Callbacks may fire on any reader
 * thread. */
typedef struct ClockworkOscStream ClockworkOscStream;
typedef void (*clockwork_osc_stream_packet_fn)(void* ctx, uint32_t conn_id,
                                        const uint8_t* osc, uint32_t len);
typedef void (*clockwork_osc_stream_closed_fn)(void* ctx, uint32_t conn_id);

/* TCP on `port` bound to `bind_addr` (empty = loopback; "0.0.0.0" for every
 * IPv4 interface, "::" for IPv6 — the secret crosses the wire in the clear,
 * so the log notes an open bind). Port 0 binds ephemerally — read it back
 * with clockwork_osc_stream_port. Null on bind failure. */
ClockworkOscStream* clockwork_osc_tcp_start(void* ctx, clockwork_osc_stream_packet_fn on_packet,
                              clockwork_osc_stream_closed_fn on_closed,
                              const ClockworkCommsSecret* secret, int32_t port,
                              const uint8_t* bind_addr, uint32_t bind_addr_len);
/* UDS stream at `path` (socket file created 0600, stale file replaced). Null
 * on failure, and always null on Windows. */
ClockworkOscStream* clockwork_osc_uds_stream_start(void* ctx, clockwork_osc_stream_packet_fn on_packet,
                                     clockwork_osc_stream_closed_fn on_closed,
                                     const ClockworkCommsSecret* secret,
                                     const uint8_t* path, uint32_t path_len);
/* Windows named pipe: `name` is a bare name (prefixed with \\.\pipe\) or a
 * full pipe path. Instances carry an owner-only DACL (SYSTEM + current user)
 * and PIPE_REJECT_REMOTE_CLIENTS; the first instance is squat-checked
 * (FILE_FLAG_FIRST_PIPE_INSTANCE). Always null on non-Windows platforms. */
ClockworkOscStream* clockwork_osc_pipe_start(void* ctx, clockwork_osc_stream_packet_fn on_packet,
                               clockwork_osc_stream_closed_fn on_closed,
                               const ClockworkCommsSecret* secret,
                               const uint8_t* name, uint32_t name_len);
/* The actual bound TCP port (for port-0 starts); 0 for path/name servers. */
int32_t clockwork_osc_stream_port(ClockworkOscStream* handle);
/* Connections closed for not presenting the secret. */
uint64_t clockwork_osc_stream_refused(ClockworkOscStream* handle);
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
