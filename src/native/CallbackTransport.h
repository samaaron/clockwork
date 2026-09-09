// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * CallbackTransport.h — in-process IOscTransport for embedders and tests.
 *
 * Replies and broadcasts surface through the engine's onReply callback (debug
 * through onDebug); the single observer is the host that set them. This is the
 * engine's default transport, so a bare `ClockworkEngine` works standalone.
 *
 * The notify sets gate broadcasts (hasNotifySubscribers): a notify broadcast
 * reaches onReply once something has subscribed. The MIDI, gamepad and OSC sets
 * gate nothing — those broadcasts always reach the observer; they only tell a
 * first subscribe from a repeat. Only the NRT gateway calls these, so no
 * locking is needed.
 */
#pragma once

#include "IOscTransport.h"
#include "shared_memory.h"   // EgressRoute

#include <cstdint>
#include <functional>
#include <set>

class CallbackTransport : public IOscTransport {
public:
    // Bound to the engine's onReply member by pointer so a callback swapped at
    // runtime is always seen.
    explicit CallbackTransport(
        const std::function<void(const uint8_t*, uint32_t)>* onReply,
        const std::function<void(uint32_t, uint32_t, const uint8_t*, uint32_t)>* onRouted = nullptr)
        : mOnReply(onReply), mOnRouted(onRouted) {}

    bool send(uint32_t token, const uint8_t* data, uint32_t size, bool networkOnly) override {
        routed(token, EGRESS_REPLY, data, size);
        if (networkOnly) return false;            // no in-process observer for snapshots
        if (mOnReply && *mOnReply) { (*mOnReply)(data, size); return true; }
        return false;
    }

    void broadcastNotify(const uint8_t* data, uint32_t size) override {
        routed(0, EGRESS_BROADCAST_NOTIFY, data, size);
        if (mOnReply && *mOnReply) (*mOnReply)(data, size);
    }

    void broadcastLink(const uint8_t*, uint32_t) override {}  // no in-process Link audience

    bool hasNotifySubscribers() const override {
        return !mNotify.empty() || !mNotifyPorts.empty();
    }
    bool subscribeNotify(uint32_t token) override { return mNotify.insert(token).second; }
    void subscribeNotifyPort(int port) override { mNotifyPorts.insert(port); }
    void unsubscribeNotify(uint32_t token) override { mNotify.erase(token); }
    void clearNotify() override { mNotify.clear(); mNotifyPorts.clear(); }

    // An in-process caller has no port, so there is no Link-notify target.
    bool subscribeLink(uint32_t) override { return false; }
    void unsubscribeLink(uint32_t) override {}

    // MIDI notify: deliver to the single in-process observer so an
    // embedder/test can see /clockwork/midi/in/* events through onReply.
    void broadcastMidi(const uint8_t* data, uint32_t size) override {
        if (mOnReply && *mOnReply) (*mOnReply)(data, size);
    }
    bool subscribeMidi(uint32_t token) override { return mMidi.insert(token).second; }
    void unsubscribeMidi(uint32_t token) override { mMidi.erase(token); }

    // Gamepad notify: deliver to the single in-process observer so an
    // embedder/test can see /clockwork/gamepad/in/* events through onReply.
    void broadcastGamepad(const uint8_t* data, uint32_t size) override {
        if (mOnReply && *mOnReply) (*mOnReply)(data, size);
    }
    bool subscribeGamepad(uint32_t token) override { return mGamepad.insert(token).second; }
    void unsubscribeGamepad(uint32_t token) override { mGamepad.erase(token); }

    // OSC-cue notify: deliver to the single in-process observer so an
    // embedder/test can see /external-osc-cue events through onReply.
    void broadcastOsc(const uint8_t* data, uint32_t size) override {
        if (mOnReply && *mOnReply) (*mOnReply)(data, size);
    }
    bool subscribeOsc(uint32_t token) override { return mOsc.insert(token).second; }
    void unsubscribeOsc(uint32_t token) override { mOsc.erase(token); }

private:
    void routed(uint32_t origin, uint32_t route, const uint8_t* d, uint32_t n) const {
        if (mOnRouted && *mOnRouted) (*mOnRouted)(origin, route, d, n);
    }

    const std::function<void(const uint8_t*, uint32_t)>* mOnReply;
    const std::function<void(uint32_t, uint32_t, const uint8_t*, uint32_t)>* mOnRouted = nullptr;
    std::set<uint32_t> mNotify;
    std::set<int>      mNotifyPorts;
    std::set<uint32_t> mMidi;
    std::set<uint32_t> mGamepad;
    std::set<uint32_t> mOsc;
};
