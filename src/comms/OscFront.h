// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * OscFront.h — a product's front at the client gateway.
 *
 * A host (SuperSonic's main) is the engine's client: sockets on one side,
 * engine.ingest and the transport's send on the other. A PRODUCT built on it
 * may want to stand between the two — to answer a verb its senders still use
 * that the engine no longer does, or to turn an engine reply into the words
 * a legacy client expects. SuperSonic answers /b_allocRead this way: the
 * engine reads no files, the front decodes into the inbox lane and commits,
 * and the sender hears scsynth's /done. That is a client's job, and this is
 * the seam a client is given to do it — the engine is not told.
 *
 * A front sees every packet in and every reply out, on the threads that
 * carry them (a transport's receive thread; the NRT gateway), and says for
 * each whether it took it. It may call engine.ingest and the transport's
 * send from threads of its own. What it must not do is touch the audio
 * thread: it is a client.
 *
 * A host that has a front installs it itself: FrontedTransport wraps the
 * transport the engine's replies leave by, and the host's ingest lambda asks
 * the front first. SuperSonic's main does both; a host with no front does
 * neither.
 */
#pragma once

#include "IOscTransport.h"

#include <cstdint>
#include <memory>

class ClockworkEngine;

class OscFront {
public:
    virtual ~OscFront() = default;

    // A packet from a client, before the engine sees it. True: taken.
    virtual bool ingress(const uint8_t* data, uint32_t size, uint32_t token) = 0;

    // A reply from the engine to `token`, before the transport sends it.
    // True: taken (the front may have sent something else in its place).
    virtual bool egress(uint32_t token, const uint8_t* data, uint32_t size) = 0;

    // One line for the boot log.
    virtual const char* describe() const = 0;
};

// The transport with a front in front of it: every send is offered to the
// front first; everything else goes straight through.
class FrontedTransport final : public IOscTransport {
public:
    void attach(IOscTransport* inner, OscFront* front) { mInner = inner; mFront = front; }
    void detachFront() { mFront = nullptr; }

    bool send(uint32_t token, const uint8_t* data, uint32_t size, bool networkOnly) override {
        if (mFront && mFront->egress(token, data, size)) return true;
        return mInner && mInner->send(token, data, size, networkOnly);
    }
    void broadcastNotify(const uint8_t* d, uint32_t n) override { if (mInner) mInner->broadcastNotify(d, n); }
    void broadcastLink(const uint8_t* d, uint32_t n) override   { if (mInner) mInner->broadcastLink(d, n); }
    bool hasNotifySubscribers() const override { return mInner && mInner->hasNotifySubscribers(); }
    bool subscribeNotify(uint32_t t) override   { return mInner && mInner->subscribeNotify(t); }
    void subscribeNotifyPort(int port) override { if (mInner) mInner->subscribeNotifyPort(port); }
    void unsubscribeNotify(uint32_t t) override { if (mInner) mInner->unsubscribeNotify(t); }
    void clearNotify() override                 { if (mInner) mInner->clearNotify(); }
    bool subscribeLink(uint32_t t) override     { return mInner && mInner->subscribeLink(t); }
    void unsubscribeLink(uint32_t t) override   { if (mInner) mInner->unsubscribeLink(t); }
    void broadcastMidi(const uint8_t* d, uint32_t n) override { if (mInner) mInner->broadcastMidi(d, n); }
    bool subscribeMidi(uint32_t t) override     { return mInner && mInner->subscribeMidi(t); }
    void unsubscribeMidi(uint32_t t) override   { if (mInner) mInner->unsubscribeMidi(t); }
    void broadcastGamepad(const uint8_t* d, uint32_t n) override { if (mInner) mInner->broadcastGamepad(d, n); }
    bool subscribeGamepad(uint32_t t) override  { return mInner && mInner->subscribeGamepad(t); }
    void unsubscribeGamepad(uint32_t t) override { if (mInner) mInner->unsubscribeGamepad(t); }
    void broadcastOsc(const uint8_t* d, uint32_t n) override { if (mInner) mInner->broadcastOsc(d, n); }
    bool subscribeOsc(uint32_t t) override      { return mInner && mInner->subscribeOsc(t); }
    void unsubscribeOsc(uint32_t t) override    { if (mInner) mInner->unsubscribeOsc(t); }

private:
    IOscTransport* mInner = nullptr;
    OscFront*      mFront = nullptr;
};
