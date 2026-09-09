// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork_gamepad.h — C ABI for the clockwork gamepad subsystem (Rust: gilrs,
 * or Apple's GameController framework on macOS).
 *
 * The C++ engine boundary (src/native/GamepadControl) creates one instance,
 * supplying one thread-safe host callback, and forwards decoded "/clockwork/gamepad/"
 * OSC into it via clockwork_gamepad_handle_osc(). The device IO lives on a
 * process-global poll thread (created on first use, kept for the life of the
 * process — gilrs's hotplug machinery has no shutdown path) and never touches
 * the audio thread; an instance is a host *registration* against it. Results
 * return through the callback, which may fire on the poll thread. Replies
 * ("/clockwork/gamepad/devices.reply") are emitted synchronously on the caller's thread.
 *
 * Must match rust/clockwork-gamepad/src/ffi.rs.
 */
#ifndef CLOCKWORK_SS_GAMEPAD_H
#define CLOCKWORK_SS_GAMEPAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. */
typedef struct ClockworkGamepad ClockworkGamepad;

/* `kind` codes for clockwork_gamepad_emit_fn. Must match clockwork_osc::ffi's
 * EMIT_* values (shared with clockwork_midi.h). */
#define CLOCKWORK_GAMEPAD_EMIT_BROADCAST 0 /* fan out to the /clockwork/gamepad/notify audience */
#define CLOCKWORK_GAMEPAD_EMIT_REPLY 1     /* reply to the current caller */

/* Emit an OSC packet to the engine: BROADCAST for "/clockwork/gamepad/in/" events + the
 * "/clockwork/gamepad/devices" push, REPLY for "/clockwork/gamepad/devices.reply". `osc`/`len` are
 * only valid for the duration of the call. May fire on the poll thread. */
typedef void (*clockwork_gamepad_emit_fn)(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);

/* Register the host callback (starting the process-global device IO on first
 * use). `ctx` and the callback must outlive the instance. NULL on failure. */
ClockworkGamepad* clockwork_gamepad_create(void* ctx, clockwork_gamepad_emit_fn emit);

/* Deregister the host and stop all rumble; no callback fires after this
 * returns. The poll thread parks until the next clockwork_gamepad_create. */
void clockwork_gamepad_destroy(ClockworkGamepad* handle);

/* Feed one decoded "/clockwork/gamepad/" OSC packet (off the audio thread). */
void clockwork_gamepad_handle_osc(ClockworkGamepad* handle, const uint8_t* data, uint32_t len);

/* Emit a /clockwork/gamepad/devices.reply snapshot to the caller (e.g. on new
 * subscription). */
void clockwork_gamepad_emit_devices(ClockworkGamepad* handle);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_SS_GAMEPAD_H */
