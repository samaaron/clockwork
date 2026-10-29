// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_transports.cpp — acceptance tests for the stream command transport
 * (StreamOscTransport over TCP and a UDS stream socket) against real client
 * sockets. No engine: the transports talk to a recorded
 * ingest callback, exactly as a host wires them to engine.ingest().
 *
 * Unix-only: the client side uses POSIX sockets directly. The shared
 * framing/registry logic is covered on every platform by the Rust suite
 * (rust/clockwork-osc-net).
 */
#ifndef _WIN32

#include <catch2/catch_test_macros.hpp>

#include "comms/StreamOscTransport.h"
#include "OscTestUtils.h"
#include "StreamTestClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using osc_test::message;
using osc_test::parseAddress;

// Record every ingested (packet, token) pair.
struct IngestLog {
    std::mutex mu;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> items;

    void record(const uint8_t* d, uint32_t n, uint32_t token) {
        std::lock_guard<std::mutex> lk(mu);
        items.emplace_back(token, std::vector<uint8_t>(d, d + n));
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return items.size();
    }
    std::pair<uint32_t, std::vector<uint8_t>> at(size_t i) {
        std::lock_guard<std::mutex> lk(mu);
        return items.at(i);
    }
};

bool waitUntil(const std::function<bool()>& f, int ms = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// A per-process short temp dir — sun_path is ~104 bytes on macOS, so keep
// socket paths compact.
std::string tmpSock(const char* name) {
    std::string dir = "/tmp/ss-tp-" + std::to_string(getpid());
    mkdir(dir.c_str(), 0700);
    std::string p = dir + "/" + name;
    unlink(p.c_str());
    return p;
}

// ── tiny blocking clients (and StreamTestClient.h) ──────────────────────────

} // namespace

TEST_CASE("StreamOscTransport/TCP: framed round trip, token routing, disconnect prune",
          "[transport][tcp]") {
    IngestLog log;
    StreamOscTransport t;
    t.setIngest([&](const uint8_t* d, uint32_t n, uint32_t tok) { log.record(d, n, tok); });
    t.initialiseTcp(0, "127.0.0.1");  // port 0 → ephemeral, read back below
    t.setSecret(testSecret());
    REQUIRE(t.start());
    REQUIRE(t.boundPort() > 0);

    int fd = connectTcp(t.boundPort());
    auto probe = message("/status");
    writeFramed(fd, probe.data);
    REQUIRE(waitUntil([&] { return log.count() == 1; }));

    auto [token, pkt] = log.at(0);
    CHECK(token >= 1);  // 0 is the in-process caller
    CHECK(parseAddress(pkt.data(), static_cast<uint32_t>(pkt.size())) == "/status");

    // Reply routing: send to the origin token reaches this client, framed.
    auto reply = message("/status.reply", 1);
    REQUIRE(t.send(token, reply.ptr(), reply.size(), false));
    auto body = readFramed(fd);
    REQUIRE(!body.empty());
    CHECK(parseAddress(body.data(), static_cast<uint32_t>(body.size())) == "/status.reply");

    // Subscription lifetime == connection lifetime.
    CHECK(t.subscribeNotify(token));
    CHECK(t.hasNotifySubscribers());
    close(fd);
    REQUIRE(waitUntil([&] { return !t.hasNotifySubscribers(); }));
    CHECK_FALSE(t.send(token, reply.ptr(), reply.size(), false));  // token is dead

    t.stop();
}

// The secret through the C++ class: a transport given none does not start,
// and a client whose first frame is not the secret gets nothing in, is closed
// and is counted.
TEST_CASE("StreamOscTransport/TCP: no secret, no port, and a client without it gets nothing",
          "[transport][tcp][secret]") {
    IngestLog log;
    StreamOscTransport bare;
    bare.initialiseTcp(0, "127.0.0.1");
    CHECK_FALSE(bare.start());

    StreamOscTransport t;
    t.setIngest([&](const uint8_t* d, uint32_t n, uint32_t tok) { log.record(d, n, tok); });
    t.initialiseTcp(0, "127.0.0.1");
    t.setSecret(testSecret());
    REQUIRE(t.start());

    const int fd = connectTcpWithoutSecret(t.boundPort());
    writeFramed(fd, message("/status").data);
    CHECK(readFramed(fd).empty());   // closed, not answered
    REQUIRE(waitUntil([&] { return t.refusedConnections() == 1; }));
    CHECK(log.count() == 0);
    close(fd);

    t.stop();
}

TEST_CASE("StreamOscTransport/UDS: round trip over an owner-only socket file",
          "[transport][uds]") {
    IngestLog log;
    StreamOscTransport t;
    t.setIngest([&](const uint8_t* d, uint32_t n, uint32_t tok) { log.record(d, n, tok); });
    std::string path = tmpSock("st.sock");
    t.initialiseUds(path);
    t.setSecret(testSecret());
    REQUIRE(t.start());

    struct stat st{};
    REQUIRE(stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    int fd = connectUnixStream(path);
    auto probe = message("/g_queryTree", 0, 0);
    writeFramed(fd, probe.data);
    REQUIRE(waitUntil([&] { return log.count() == 1; }));
    auto [token, pkt] = log.at(0);
    CHECK(parseAddress(pkt.data(), static_cast<uint32_t>(pkt.size())) == "/g_queryTree");

    auto reply = message("/g_queryTree.reply");
    REQUIRE(t.send(token, reply.ptr(), reply.size(), false));
    auto body = readFramed(fd);
    REQUIRE(!body.empty());
    CHECK(parseAddress(body.data(), static_cast<uint32_t>(body.size())) == "/g_queryTree.reply");

    close(fd);
    t.stop();
    CHECK(access(path.c_str(), F_OK) != 0);  // stop unlinks the socket path
}

#endif // !_WIN32
