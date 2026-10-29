// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * StreamOscTransport.cpp — see StreamOscTransport.h. Listening, framing, and
 * the connection registry live in the Rust leaf (clockwork_osc stream API); this
 * side is the audience bookkeeping and the IOscTransport boundary. No JUCE.
 */
#include "StreamOscTransport.h"

#include <algorithm>
#include <cstdio>

StreamOscTransport::~StreamOscTransport() {
    stop();
}

void StreamOscTransport::initialiseTcp(int port, const std::string& bindAddress) {
    mKind = Kind::Tcp;
    mPort = port;
    mBindAddress = bindAddress;
}

void StreamOscTransport::initialiseUds(const std::string& path) {
    mKind = Kind::Uds;
    mEndpoint = path;
}

void StreamOscTransport::initialisePipe(const std::string& name) {
    mKind = Kind::Pipe;
    mEndpoint = name;
}

bool StreamOscTransport::start() {
    if (mServer.load(std::memory_order_acquire)) return true;
    const auto* ep = reinterpret_cast<const uint8_t*>(mEndpoint.data());
    const auto epLen = static_cast<uint32_t>(mEndpoint.size());
    ClockworkOscStream* server = nullptr;
    switch (mKind) {
    case Kind::Tcp:
        server = clockwork_osc_tcp_start(this, &onPacket, &onClosed, mSecret, mPort,
                                         reinterpret_cast<const uint8_t*>(mBindAddress.data()),
                                         static_cast<uint32_t>(mBindAddress.size()));
        break;
    case Kind::Uds:
        server = clockwork_osc_uds_stream_start(this, &onPacket, &onClosed, mSecret, ep, epLen);
        break;
    case Kind::Pipe:
        server = clockwork_osc_pipe_start(this, &onPacket, &onClosed, mSecret, ep, epLen);
        break;
    case Kind::None:
        break;
    }
    mServer.store(server, std::memory_order_release);
    return server != nullptr;
}

void StreamOscTransport::stop() {
    if (auto* server = mServer.exchange(nullptr, std::memory_order_acq_rel))
        clockwork_osc_stream_stop(server);
}

int StreamOscTransport::boundPort() const {
    auto* server = mServer.load(std::memory_order_acquire);
    return server ? clockwork_osc_stream_port(server) : 0;
}

uint64_t StreamOscTransport::refusedConnections() const {
    auto* server = mServer.load(std::memory_order_acquire);
    return server ? clockwork_osc_stream_refused(server) : 0;
}

// Rust reader thread → ingest carrying the connection id as the origin token.
void StreamOscTransport::onPacket(void* ctx, uint32_t conn, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<StreamOscTransport*>(ctx);
    if (self->mIngest) self->mIngest(osc, len, conn);
}

// Rust reader thread, once per ended connection: subscription lifetime ==
// connection lifetime, so the client leaves every audience.
void StreamOscTransport::onClosed(void* ctx, uint32_t conn) {
    auto* self = static_cast<StreamOscTransport*>(ctx);
    std::lock_guard<std::mutex> lk(self->mMutex);
    self->removeConn(self->mNotifyTargets, conn);
    self->removeConn(self->mLinkNotifyTargets, conn);
    self->removeConn(self->mMidiNotifyTargets, conn);
    self->removeConn(self->mGamepadNotifyTargets, conn);
    self->removeConn(self->mOscNotifyTargets, conn);
}

// ── IOscTransport: a token is a conn id — hand it straight to the leaf ───────

bool StreamOscTransport::send(uint32_t token, const uint8_t* data, uint32_t size,
                              bool /*networkOnly*/) {
    // networkOnly is moot — there is no in-process observer to skip.
    auto* server = mServer.load(std::memory_order_acquire);
    if (!server || token == 0) return false;
    return clockwork_osc_stream_send(server, token, data, size) != 0;
}

uint32_t StreamOscTransport::broadcast(const std::vector<uint32_t>& list,
                                       const uint8_t* data, uint32_t size) {
    // Copy the audience out so a send never runs under the audience lock (a
    // reader thread pruning via onClosed must not wait on a slow write).
    std::vector<uint32_t> targets;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        targets = list;
    }
    auto* server = mServer.load(std::memory_order_acquire);
    if (!server) return static_cast<uint32_t>(targets.size());
    uint32_t missed = 0;
    for (uint32_t conn : targets)
        if (clockwork_osc_stream_send(server, conn, data, size) == 0) ++missed;
    return missed;
}

uint32_t StreamOscTransport::broadcastNotify(const uint8_t* data, uint32_t size) {
    return broadcast(mNotifyTargets, data, size);
}
uint32_t StreamOscTransport::broadcastLink(const uint8_t* data, uint32_t size) {
    return broadcast(mLinkNotifyTargets, data, size);
}

bool StreamOscTransport::hasNotifySubscribers() const {
    std::lock_guard<std::mutex> lk(mMutex);
    return !mNotifyTargets.empty();
}

bool StreamOscTransport::subscribeNotify(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mNotifyTargets, token);
}
// Registering an explicit localhost reply *port* is a UDP concept (the GUI's
// /clockwork/devices/report path); a stream peer subscribes caller-relative.
void StreamOscTransport::unsubscribeNotify(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mNotifyTargets, token);
}
void StreamOscTransport::clearNotify() {
    std::lock_guard<std::mutex> lk(mMutex);
    mNotifyTargets.clear();
}

bool StreamOscTransport::subscribeLink(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mLinkNotifyTargets, token);
}
void StreamOscTransport::unsubscribeLink(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mLinkNotifyTargets, token);
}
uint32_t StreamOscTransport::broadcastMidi(const uint8_t* data, uint32_t size) {
    return broadcast(mMidiNotifyTargets, data, size);
}
bool StreamOscTransport::subscribeMidi(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mMidiNotifyTargets, token);
}
void StreamOscTransport::unsubscribeMidi(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mMidiNotifyTargets, token);
}
uint32_t StreamOscTransport::broadcastGamepad(const uint8_t* data, uint32_t size) {
    return broadcast(mGamepadNotifyTargets, data, size);
}
bool StreamOscTransport::subscribeGamepad(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mGamepadNotifyTargets, token);
}
void StreamOscTransport::unsubscribeGamepad(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mGamepadNotifyTargets, token);
}
uint32_t StreamOscTransport::broadcastOsc(const uint8_t* data, uint32_t size) {
    return broadcast(mOscNotifyTargets, data, size);
}
bool StreamOscTransport::subscribeOsc(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    return addConn(mOscNotifyTargets, token);
}
void StreamOscTransport::unsubscribeOsc(uint32_t token) {
    std::lock_guard<std::mutex> lk(mMutex);
    removeConn(mOscNotifyTargets, token);
}

// Audience size is naturally bounded: ids are live connections (≤ max_conns)
// and onClosed prunes the dead — no LRU eviction needed. Callers hold mMutex.
bool StreamOscTransport::addConn(std::vector<uint32_t>& list, uint32_t token) {
    if (token == 0) return false;  // the in-process caller has no connection
    if (std::find(list.begin(), list.end(), token) != list.end()) return false;
    list.push_back(token);
    return true;
}

void StreamOscTransport::removeConn(std::vector<uint32_t>& list, uint32_t token) {
    list.erase(std::remove(list.begin(), list.end(), token), list.end());
}
