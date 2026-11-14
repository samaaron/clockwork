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
 * engine reads no files, the front decodes into the assets area lane and commits,
 * and the sender hears scsynth's /done. That is a client's job, and this is
 * the seam a client is given to do it — the engine is not told.
 *
 * A front sees every packet in and every frame out, on the threads that
 * carry them (a transport's receive thread; the NRT gateway), and says for
 * each whether it took it. It may call engine.ingest and the transport's
 * send from threads of its own. What it must not do is touch the audio
 * thread: it is a client.
 *
 * A host that has a front installs it itself: FrontedTransport wraps the
 * transport the engine's replies leave by, and the host's ingest lambda asks
 * the front first. SuperSonic's main does both; a host with no front does
 * neither. The NIF, a host clockwork builds, does both for the front a product
 * gives it through clockwork_nif_front.h.
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

    // A frame from the engine for the client, before the transport delivers
    // it. True: taken (the front may have sent something else in its place).
    virtual bool egress(const uint8_t* data, uint32_t size) = 0;

    // One line for the boot log.
    virtual const char* describe() const = 0;
};

// The transport with a front in front of it: every frame is offered to the
// front first.
class FrontedTransport final : public IOscTransport {
public:
    void attach(IOscTransport* inner, OscFront* front) { mInner = inner; mFront = front; }
    void detachFront() { mFront = nullptr; }

    bool deliver(const uint8_t* data, uint32_t size) override {
        if (mFront && mFront->egress(data, size)) return true;
        return mInner && mInner->deliver(data, size);
    }

private:
    IOscTransport* mInner = nullptr;
    OscFront*      mFront = nullptr;
};
