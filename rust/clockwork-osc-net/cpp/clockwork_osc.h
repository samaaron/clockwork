// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_osc.h — C ABI for the clockwork OSC networking subsystem (Rust / std::net).
 *
 * The C++ engine boundary (src/native/OscControl) creates one instance with an emit
 * callback, then drives it:
 *   - clockwork_osc_configure(): (re)bind the external cue server.
 *   - clockwork_osc_send():      send a now-due scheduled OSC packet to a host:port.
 * Inbound external OSC is re-framed to /external-osc-cue <ip> <port> <address>
 * <args...> and handed back via the emit callback (which may fire on the cue
 * server's recv thread, so the host impl must be thread-safe). The subsystem
 * owns its sockets + recv threads and never touches the audio thread.
 *
 * The standalone server's command transports also live here: the raw UDP
 * ingress (UdpOscTransport), the UDS datagram server (UdsDgramOscTransport),
 * and the framed stream servers — TCP, UDS stream, Windows named pipe — behind
 * one ClockworkOscStream handle (StreamOscTransport).
 *
 * Must match rust/clockwork-osc-net/src/{ffi,uds,stream,pipe}.rs.
 */
#ifndef CLOCKWORK_SS_OSC_H
#define CLOCKWORK_SS_OSC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct ClockworkOsc ClockworkOsc;

/* `kind` codes for clockwork_osc_emit_fn (shares clockwork_osc::ffi values). */
#define CLOCKWORK_OSC_EMIT_BROADCAST 0 /* fan out to the /clockwork/osc/notify audience */
#define CLOCKWORK_OSC_EMIT_REPLY 1     /* reply to the current caller (unused by the cue server) */

/* Emit an OSC packet to the engine — an /external-osc-cue push for inbound
 * external OSC. `osc`/`len` are only valid for the duration of the call. */
typedef void (*clockwork_osc_emit_fn)(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);

/* Create the subsystem. `ctx` and `emit` must outlive it. Null on failure. */
ClockworkOsc* clockwork_osc_create(void* ctx, clockwork_osc_emit_fn emit);

/* Stop the cue recv threads, close the sockets, free the instance. */
void clockwork_osc_destroy(ClockworkOsc* handle);

/* (Re)configure the cue server: bind `port` (0 = unbind); `loopback` != 0 binds
 * 127.0.0.1, else all interfaces; `cues_on` != 0 forwards inbound OSC as cues.
 * Off the audio thread. */
void clockwork_osc_configure(ClockworkOsc* handle, int32_t port, int32_t loopback, int32_t cues_on);

/* Send one OSC packet `data`/`len` to `host`:`port`. `host` is a byte string
 * (ptr + len, not NUL-terminated). Off the audio thread. */
void clockwork_osc_send(ClockworkOsc* handle, const uint8_t* host, uint32_t host_len,
                 int32_t port, const uint8_t* data, uint32_t len);

/* Raw OSC ingress (separate from the cue server): receive datagrams on `port`
 * and hand the raw OSC bytes to `emit` (kind = broadcast) without re-framing.
 * `loopback` != 0 binds 127.0.0.1 + ::1, else all interfaces. The standalone
 * host uses this for its control port. Null on bind failure. */
typedef struct ClockworkOscIngress ClockworkOscIngress;
ClockworkOscIngress* clockwork_osc_ingress_start(void* ctx, clockwork_osc_emit_fn emit,
                                   int32_t port, int32_t loopback);
void clockwork_osc_ingress_stop(ClockworkOscIngress* handle);

/* Source-bearing OSC ingress: like clockwork_osc_ingress_start, but delivers each
 * datagram verbatim TOGETHER WITH the sender's (ip, port), so an engine transport
 * can intern an origin token and address a reply back. `bind_addr` is a byte
 * string (ptr + len, not NUL-terminated): empty = all interfaces, else an
 * IPv4/IPv6 literal or hostname (e.g. "127.0.0.1" for loopback). `ip` in the
 * callback is valid only for the call. Null on bind failure; free with
 * clockwork_osc_ingress_stop. */
typedef void (*clockwork_osc_emit_src_fn)(void* ctx, const uint8_t* ip, uint32_t ip_len,
                                   int32_t port, const uint8_t* osc, uint32_t len);
ClockworkOscIngress* clockwork_osc_ingress_start_with_src(void* ctx, clockwork_osc_emit_src_fn emit,
                                            int32_t port,
                                            const uint8_t* bind_addr, uint32_t bind_addr_len);

/* ── UDS datagram ingress (unix only) ─────────────────────────────────────────
 * The kernel-ACL'd sibling of the UDP control port: binds a socket file at
 * `path` (created 0600, replacing a stale file; put it in a 0700 directory to
 * close the bind→chmod window) and delivers each datagram verbatim together
 * with the sender's socket path, so a transport can intern an origin token and
 * reply. A sender that did not bind its own path arrives with an empty peer
 * path and is unaddressable (macOS has no autobind). All byte strings are
 * ptr + len, not NUL-terminated. Start returns null on failure, and always
 * null on Windows (see the named-pipe API below). */
/* The command transports — UDS datagram, the TCP/UDS stream servers and the
 * Windows named pipe — are a CLIENT library's ABI now, in
 * src/comms/clockwork_comms_abi.h (crate clockwork-comms). */

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_SS_OSC_H */
