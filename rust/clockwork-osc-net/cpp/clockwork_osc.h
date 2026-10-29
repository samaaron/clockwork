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
 * Must match rust/clockwork-osc-net/src/ffi.rs. The command transports are a
 * client library's ABI: src/comms/clockwork_comms_abi.h (crate
 * clockwork-comms).
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

/* `n` more cues dropped by the cue port's rate limit. On a cue recv thread. */
typedef void (*clockwork_osc_dropped_fn)(void* ctx, uint32_t n);

/* Create the subsystem. `ctx`, `emit` and `dropped` must outlive it. Null on
 * failure. The cue port starts with the default rate limit: 2000 cues/s for
 * the port and 1000/s for each sender. */
ClockworkOsc* clockwork_osc_create(void* ctx, clockwork_osc_emit_fn emit, clockwork_osc_dropped_fn dropped);

/* Stop the cue recv threads, close the sockets, free the instance. */
void clockwork_osc_destroy(ClockworkOsc* handle);

/* (Re)configure the cue server: bind `port` (0 = unbind); `loopback` != 0 binds
 * 127.0.0.1, else all interfaces; `cues_on` != 0 forwards inbound OSC as cues.
 * Off the audio thread. */
void clockwork_osc_configure(ClockworkOsc* handle, int32_t port, int32_t loopback, int32_t cues_on);

/* The cue port's rate limit: `per_second` cues for the port, `per_sender` for
 * each address, 0 or less for no limit on that side. A cue over it is dropped
 * and counted through the dropped callback. Off the audio thread. */
void clockwork_osc_set_limit(ClockworkOsc* handle, int32_t per_second, int32_t per_sender);

/* Send one OSC packet `data`/`len` to `host`:`port`. `host` is a byte string
 * (ptr + len, not NUL-terminated). Off the audio thread. Non-zero when the
 * socket took the datagram; 0 when it could not be sent (no such host, a
 * socket that refused it, a bad argument). */
int32_t clockwork_osc_send(ClockworkOsc* handle, const uint8_t* host, uint32_t host_len,
                           int32_t port, const uint8_t* data, uint32_t len);

/* The command transports — UDS datagram, the TCP/UDS stream servers and the
 * Windows named pipe — are a CLIENT library's ABI now, in
 * src/comms/clockwork_comms_abi.h (crate clockwork-comms). */

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_SS_OSC_H */
