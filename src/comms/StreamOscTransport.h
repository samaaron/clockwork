// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork
 * Copyright (c) 2026 Sam Aaron
 *
 *
 * StreamOscTransport.h — the connection-oriented OSC transport, one class for
 * TCP, UDS stream, and Windows named pipes (they share the ClockworkOscStream C ABI
 * and the 4-byte big-endian length-prefixed wire format).
 *
 * The client is a connection id: minted from 1 by the Rust leaf, never
 * reused, dead the moment the connection closes. The transport keeps the
 * id of its client and tells nobody else. The Rust leaf admits one client.
 *
 * The NRT gateway is the sole caller of the IOscTransport methods, but
 * on_closed fires on a reader thread: the client has gone, and the core is
 * told (clockwork_client_gone), so what it asked to be told ends with its
 * connection.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "IOscTransport.h"
#include "clockwork_comms_abi.h"

class StreamOscTransport : public IOscTransport {
public:
    StreamOscTransport() = default;
    // A transport owns a socket and a thread: not a thing to copy or move.
    StreamOscTransport(const StreamOscTransport&) = delete;
    StreamOscTransport& operator=(const StreamOscTransport&) = delete;
    // Called per inbound packet: (osc, len). Set before start().
    using IngestFn = std::function<void(const uint8_t*, uint32_t)>;

    ~StreamOscTransport() override;

    void setIngest(IngestFn fn) { mIngest = std::move(fn); }
    // The host's secret (clockwork_comms_abi.h), read by start(); the server
    // keeps its own copy, so `secret` need only live until then. Without one
    // start() fails.
    void setSecret(const ClockworkCommsSecret* secret) { mSecret = secret; }

    // Pick exactly one endpoint before start().
    void initialiseTcp(int port, const std::string& bindAddress);
    void initialiseUds(const std::string& path);   // unix only
    void initialisePipe(const std::string& name);  // Windows only

    // The engine's core, from bindCore: start() before it is told logs the
    // server's lines to the console. The engine takes it back (nullptr)
    // before it closes it; that waits for a reader thread that is telling
    // the core its client has gone, so the core is never closed under one.
    void bindCore(ClockworkCore* core) override {
        const std::lock_guard<std::mutex> held(mCoreLock);
        mCore = core;
    }

    bool start();   // listen + accept; false on failure / unsupported platform
    void stop();

    // The actual bound TCP port (differs from the requested one for port-0
    // starts); 0 for path/name-addressed servers.
    int boundPort() const;
    // Connections closed for not presenting the secret.
    uint64_t refusedConnections() const;

    // ── IOscTransport ──────────────────────────────────────────────────────────
    bool deliver(const uint8_t* data, uint32_t size) override;

private:
    enum class Kind { None, Tcp, Uds, Pipe };

    static void onPacket(void* ctx, uint32_t conn, const uint8_t* osc, uint32_t len);
    static void onClosed(void* ctx, uint32_t conn);


    Kind         mKind = Kind::None;
    ClockworkCore* mCore = nullptr;     // under mCoreLock once the server runs
    std::mutex   mCoreLock;             // a reader thread's use of the core, against the engine taking it back
    const ClockworkCommsSecret* mSecret = nullptr;
    int          mPort = 0;             // Tcp
    std::string  mBindAddress;          // Tcp
    std::string  mEndpoint;             // Uds path / pipe name
    // Published once it is listening: start() may come after the engine is
    // up, its gateway already broadcasting through this transport (a boot
    // that says setup), so the gateway reads it with one load. stop() frees
    // it, and so comes once the engine has stopped sending.
    std::atomic<ClockworkOscStream*> mServer{nullptr};
    IngestFn     mIngest;

    // The client's connection: the one whose packets are arriving, 0 with
    // none. A reader thread's to write, the gateway's to read.
    std::atomic<uint32_t> mClient{0};
};
