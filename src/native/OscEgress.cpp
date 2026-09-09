// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * OscEgress.cpp — deferred egress: frame on the producer side, dispatch to the
 * injected IOscTransport on the gateway side. No address/socket knowledge here.
 */
#include "clockwork_prefix.h"
#include "OscEgress.h"

#include "IOscTransport.h"
#include "comms/EgressRouter.h"
#include "osc_debug.h"
#include "lanes/lanes_internal.h"  // clockwork_egress_nrt_write (NRT egress producer)
#include "osc/OscOutboundPacketStream.h"
#include <cstring>

void OscEgress::init(IOscTransport*                                  transport,
                     const std::function<void(const std::string&)>*  onDebug) {
    mTransport = transport;
    mOnDebug   = onDebug;
}

// ── Producer side: frame `[route][osc]` into the NRT-out ring ─────────────────

void OscEgress::frame(Route route, uint32_t token, const uint8_t* osc, uint32_t size) {
    // The lanes NRT egress producer (src/lanes/lanes.cpp) owns the NRT-out
    // ring location and the producer lock.
    clockwork_egress_nrt_write(static_cast<uint32_t>(route), token, osc, size);
}

void OscEgress::reply(uint32_t token, const uint8_t* data, uint32_t size) {
    frame(REPLY, token, data, size);
}

void OscEgress::sendToCaller(uint32_t token, const uint8_t* data, uint32_t size) {
    frame(SEND_TO_CALLER, token, data, size);
}

void OscEgress::broadcastToTargets(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_NOTIFY, 0, data, size);
}

void OscEgress::broadcastLinkNotify(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_LINK, 0, data, size);
}

void OscEgress::broadcastMidiNotify(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_MIDI, 0, data, size);
}

void OscEgress::broadcastGamepadNotify(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_GAMEPAD, 0, data, size);
}

void OscEgress::broadcastOscNotify(const uint8_t* data, uint32_t size) {
    frame(BROADCAST_OSC, 0, data, size);
}

void OscEgress::debug(const char* text, uint32_t len) {
    char pkt[1024];
    uint32_t p = clockwork::buildDebugOsc(pkt, text, len);
    frame(BROADCAST_NOTIFY, 0, reinterpret_cast<const uint8_t*>(pkt), p);
}

// ── Gateway side: dispatch one framed message to the transport / onDebug ─────

void OscEgress::dispatchEgress(uint32_t originToken, uint32_t route,
                               const uint8_t* osc, uint32_t oscLen) {
    // Engine pre-dispatch hook. It may consume the message by answering
    // true; the engine's current hook inspects nothing and never does.
    if (mInterceptor && mInterceptor(osc, oscLen)) return;
    if (!mTransport) return;
    // The one routing table, shared with a host that drains egress itself.
    const std::function<void(const std::string&)> onDebug =
        [this](const std::string& s) { deliverDebug(s.data(), static_cast<uint32_t>(s.size())); };
    clockwork_route_egress(*mTransport, originToken, route, osc, oscLen, onDebug);
}


void OscEgress::deliverDebug(const char* text, uint32_t len) {
    if (mOnDebug && *mOnDebug) (*mOnDebug)(std::string(text, len));
}

// ── Subscriber registry — forwarded to the transport, keyed on the origin ────

bool OscEgress::subscribeCaller(uint32_t token) {
    return mTransport && mTransport->subscribeNotify(token);
}

void OscEgress::unsubscribeCaller(uint32_t token) {
    if (mTransport) mTransport->unsubscribeNotify(token);
}

void OscEgress::clearSubscribers() {
    if (mTransport) mTransport->clearNotify();
}

void OscEgress::subscribeNotifyPort(int port) {
    if (mTransport) mTransport->subscribeNotifyPort(port);
}

bool OscEgress::hasSubscribers() const {
    return mTransport && mTransport->hasNotifySubscribers();
}

bool OscEgress::subscribeCallerToLinkNotify(uint32_t token) {
    return mTransport && mTransport->subscribeLink(token);
}

void OscEgress::unsubscribeCallerFromLinkNotify(uint32_t token) {
    if (mTransport) mTransport->unsubscribeLink(token);
}

bool OscEgress::subscribeCallerToMidiNotify(uint32_t token) {
    return mTransport && mTransport->subscribeMidi(token);
}

void OscEgress::unsubscribeCallerFromMidiNotify(uint32_t token) {
    if (mTransport) mTransport->unsubscribeMidi(token);
}

bool OscEgress::subscribeCallerToGamepadNotify(uint32_t token) {
    return mTransport && mTransport->subscribeGamepad(token);
}

void OscEgress::unsubscribeCallerFromGamepadNotify(uint32_t token) {
    if (mTransport) mTransport->unsubscribeGamepad(token);
}

bool OscEgress::subscribeCallerToOscNotify(uint32_t token) {
    return mTransport && mTransport->subscribeOsc(token);
}

void OscEgress::unsubscribeCallerFromOscNotify(uint32_t token) {
    if (mTransport) mTransport->unsubscribeOsc(token);
}

// ── Small generic lifecycle broadcasts (gated by an audience) ────────────────

void OscEgress::sendStateChange(const char* state, const char* reason) {
    if (!hasSubscribers()) return;
    char buf[512];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(CLOCKWORK_SYS("statechange"))
      << state
      << reason
      << osc::EndMessage;
    broadcastToTargets(reinterpret_cast<const uint8_t*>(s.Data()),
                       static_cast<uint32_t>(s.Size()));
}

void OscEgress::sendSetup(int sampleRate, int bufferSize, uint32_t generation) {
    if (!hasSubscribers()) return;
    char buf[256];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(CLOCKWORK_SYS("setup"))
      << static_cast<osc::int32>(sampleRate)
      << static_cast<osc::int32>(bufferSize)
      << static_cast<osc::int32>(generation)
      << osc::EndMessage;
    broadcastToTargets(reinterpret_cast<const uint8_t*>(s.Data()),
                       static_cast<uint32_t>(s.Size()));
}

// Caller-targeted statechange. Connection-oriented clients (TCP/UDS/pipe)
// can only register AFTER engine init — the boot-time statechange broadcast
// fired into an audience they hadn't joined yet, so a new registrant is
// sent the current lifecycle state directly (see the /clockwork/notify
// handler). UDP clients registered pre-init via the boot queue and never
// needed this. Deliberately state-only: /clockwork/setup is a rebuild
// EVENT and must never be replayed to late joiners.
void OscEgress::sendStateChangeTo(uint32_t token, const char* state, const char* reason) {
    char buf[512];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(CLOCKWORK_SYS("statechange"))
      << state
      << reason
      << osc::EndMessage;
    // reply routing: this is part of the direct response to the caller's
    // /clockwork/notify, and must also reach in-process (embedder) callers.
    reply(token, reinterpret_cast<const uint8_t*>(s.Data()),
          static_cast<uint32_t>(s.Size()));
}
