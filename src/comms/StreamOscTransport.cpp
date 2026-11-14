// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * StreamOscTransport.cpp — see StreamOscTransport.h. Listening, framing, and
 * the connection registry live in the Rust leaf (clockwork_osc stream API); this
 * side is which connection the client is and the IOscTransport boundary. No JUCE.
 */
#include "StreamOscTransport.h"

#include "lanes/lanes.h"   // clockwork_client_gone
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
        server = clockwork_osc_tcp_start(mCore, this, &onPacket, &onClosed, mSecret, mPort,
                                         reinterpret_cast<const uint8_t*>(mBindAddress.data()),
                                         static_cast<uint32_t>(mBindAddress.size()));
        break;
    case Kind::Uds:
        server = clockwork_osc_uds_stream_start(mCore, this, &onPacket, &onClosed, mSecret, ep, epLen);
        break;
    case Kind::Pipe:
        server = clockwork_osc_pipe_start(mCore, this, &onPacket, &onClosed, mSecret, ep, epLen);
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
    self->mClient.store(conn, std::memory_order_release);
    if (self->mIngest) self->mIngest(osc, len, conn);
}

// Rust reader thread, once per ended connection: the client has gone, and
// what it asked to be told goes with it (lanes.h).
void StreamOscTransport::onClosed(void* ctx, uint32_t conn) {
    auto* self = static_cast<StreamOscTransport*>(ctx);
    uint32_t was = conn;
    if (!self->mClient.compare_exchange_strong(was, 0u, std::memory_order_acq_rel)) return;
    const std::lock_guard<std::mutex> held(self->mCoreLock);
    if (self->mCore) clockwork_client_gone(self->mCore);
}

// To the client's connection; with none, it reached no one.
bool StreamOscTransport::deliver(const uint8_t* data, uint32_t size) {
    auto* server = mServer.load(std::memory_order_acquire);
    const uint32_t client = mClient.load(std::memory_order_acquire);
    if (!server || client == 0) return false;
    return clockwork_osc_stream_send(server, client, data, size) != 0;
}
