// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * IOscTransport.h — the engine→outside egress boundary.
 *
 * The engine deals only in opaque origin tokens and OSC bytes. A transport owns
 * everything platform-specific behind this interface: how a token resolves to a
 * destination, what the notify/link subscriber audiences actually are, and how a
 * packet physically leaves. This is the *only* place address/socket/pid
 * knowledge lives. Implementations: StreamOscTransport (TCP / UDS stream /
 * named pipe), ShmTransport (the segment's peer plane), NifTransport (BEAM — fans out to
 * the registered Erlang pids via enif_send), and CallbackTransport
 * (in-process — embedders and tests, surfacing through the engine's onReply
 * callback).
 *
 * The NRT gateway is the sole caller of these methods (one thread), so
 * implementations need no internal send serialisation beyond their own resource
 * locks (e.g. a socket write lock shared with a recv thread).
 */
#pragma once

#include <cstdint>

class IOscTransport {
public:
    virtual ~IOscTransport() = default;

    // WHAT DID NOT ARRIVE IS SAID. A send returns false, and a broadcast the
    // number of its audience it did not reach, when the frame was meant for a
    // destination and could not be handed to it: a token no peer answers to
    // any more, a connection that refused the write, a datagram the socket
    // refused, a full reply ring. Whoever routes egress counts every one —
    // the engine's gateway, or a host draining egress itself
    // (PerformanceMetrics::egress_frames_undelivered, EgressRouter.h). A
    // broadcast to an empty audience missed no one.

    // Reply to the peer an origin token resolves to. `networkOnly` means "do not
    // deliver to an in-process observer" (Link snapshots): a real transport
    // (a socket, the NIF) sends regardless; a callback sink has no one it is meant for,
    // and drops it having missed no one.
    virtual bool send(uint32_t token, const uint8_t* data, uint32_t size,
                      bool networkOnly) = 0;

    // Fan out to the device-notify / Link-notify subscriber audiences.
    virtual uint32_t broadcastNotify(const uint8_t* data, uint32_t size) = 0;
    virtual uint32_t broadcastLink(const uint8_t* data, uint32_t size) = 0;

    // Notify-audience registry. subscribeNotify is caller-relative (resolve the
    // token); it and the others return true if newly added.
    // hasNotifySubscribers gates whether the engine bothers to emit.
    virtual bool hasNotifySubscribers() const = 0;
    virtual bool subscribeNotify(uint32_t token) = 0;
    virtual void unsubscribeNotify(uint32_t token) = 0;
    virtual void clearNotify() = 0;

    // Link-notify audience (separate from device-notify). Caller-relative; an
    // unaddressable caller (e.g. in-process, no port) is rejected → false.
    virtual bool subscribeLink(uint32_t token) = 0;
    virtual void unsubscribeLink(uint32_t token) = 0;

    // MIDI-notify audience: receives /clockwork/midi/in/* events and
    // /clockwork/midi/ports change pushes. Caller-relative, like the Link audience.
    virtual uint32_t broadcastMidi(const uint8_t* data, uint32_t size) = 0;
    virtual bool subscribeMidi(uint32_t token) = 0;
    virtual void unsubscribeMidi(uint32_t token) = 0;

    // Gamepad-notify audience: receives /clockwork/gamepad/in/* events and
    // /clockwork/gamepad/devices change pushes. Caller-relative, like the MIDI audience.
    virtual uint32_t broadcastGamepad(const uint8_t* data, uint32_t size) = 0;
    virtual bool subscribeGamepad(uint32_t token) = 0;
    virtual void unsubscribeGamepad(uint32_t token) = 0;

    // OSC-cue audience: receives /external-osc-cue pushes (external OSC arriving
    // on the cue server). Caller-relative, like the MIDI audience. Default
    // no-ops; every transport here overrides them except the NIF bridge.
    virtual uint32_t broadcastOsc(const uint8_t*, uint32_t) { return 0; }
    virtual bool subscribeOsc(uint32_t) { return false; }
    virtual void unsubscribeOsc(uint32_t) {}
};
