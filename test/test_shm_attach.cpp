// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_shm_attach.cpp — the attach endpoint (shm_attach.hpp): the only way a
 * process other than the engine reaches the anonymous segment.
 *
 * The engine here is in-process, but the hand-off is the real one: a socket
 * (or pipe) round trip, a duplicated handle, a fresh mapping. The
 * cross-process case rides the same code in test_shm_peer_crash.cpp.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "TestSecret.h"
#include "shm_attach.hpp"
#include "clockwork_client.h"

#include <chrono>
#include <string>
#include <vector>
#ifdef _WIN32
#include <process.h>   // _getpid
#else
#include <unistd.h>    // getpid
#endif

#if !defined(_WIN32)
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

using detail_shm_segment::shm_handle_valid;
using detail_shm_segment::shm_segment_client;

namespace {

ClockworkEngine::Config attachConfig(unsigned port) {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.shmSegment = true;   // non-zero is what creates the segment
    return cfg;
}

// An endpoint no other test, and no other process running these tests, is
// using: the derived name carries the case's port (unique to the case) and
// this process's id (unique to the run, so two runs at once do not share it).
std::string endpointFor(unsigned port) {
#ifdef _WIN32
    const auto pid = static_cast<unsigned long>(_getpid());
#else
    const auto pid = static_cast<unsigned long>(getpid());
#endif
    return shm_attach::default_endpoint(port) + "." + std::to_string(pid);
}

} // namespace

TEST_CASE("attach: a reader receives the engine's segment over the endpoint",
          "[shm][attach]") {
    constexpr unsigned kPort = 57330;
    EngineFixture fx(attachConfig(kPort));
    const std::string ep = endpointFor(kPort);

    shm_attach::server srv;
    std::string err;
    REQUIRE(srv.start(ep, fx.engine().shmNativeHandle(),
                      fx.engine().shmSegmentSize(), testSecret(), &err));
    INFO(err);

    // The hand-off: a handle of our own, and the size the engine declared.
    uint64_t size = 0;
    const auto h = shm_attach::receive(ep, testSecretBytes().data(), &err, &size);
    INFO(err);
    REQUIRE(shm_handle_valid(h));
    CHECK(size == fx.engine().shmSegmentSize());

    // Mapped from it, the segment is the engine's: published, layout-checked,
    // and live — a metric the engine writes is visible through it.
    shm_segment_client client(h);
    REQUIRE(client.get_metrics() != nullptr);
    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fx.waitForReply("/dummy/pong", r));
    CHECK(client.get_metrics()->messages_processed.load() > 0);

    // The server keeps serving: a second reader gets its own handle.
    const auto h2 = shm_attach::receive(ep, testSecretBytes().data(), &err);
    INFO(err);   // this receive's reason, not the first's: INFO keeps the string as it was when it ran
    REQUIRE(shm_handle_valid(h2));
    CHECK(h2 != h);
    shm_segment_client second(h2);
    CHECK(second.get_base() != client.get_base());   // two mappings, same pages
    client.get_base()[0] = 0x77;
    CHECK(second.get_base()[0] == 0x77);

    // Stopped, the endpoint is gone: nothing to connect to, and on POSIX no
    // socket file left behind.
    srv.stop();
    CHECK_FALSE(srv.running());
    CHECK_FALSE(shm_handle_valid(shm_attach::receive(ep, testSecretBytes().data(), &err)));
#if !defined(_WIN32)
    struct stat st {};
    CHECK(::stat(ep.c_str(), &st) != 0);
#endif
}

#if !defined(_WIN32)
TEST_CASE("attach: the endpoint is a socket only its owner can open", "[shm][attach][security]") {
    constexpr unsigned kPort = 57331;
    EngineFixture fx(attachConfig(kPort));
    const std::string ep = endpointFor(kPort);
    shm_attach::server srv;
    std::string err;
    REQUIRE(srv.start(ep, fx.engine().shmNativeHandle(),
                      fx.engine().shmSegmentSize(), testSecret(), &err));

    struct stat st {};
    REQUIRE(::stat(ep.c_str(), &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    CHECK((st.st_mode & 0777) == 0600);

    // A second server on a LIVE endpoint is refused: it must not unlink the
    // owner's socket and quietly take its readers (two engines on one port,
    // which a non-fatal UDP bind failure allows).
    shm_attach::server intruder;
    CHECK_FALSE(intruder.start(ep, fx.engine().shmNativeHandle(),
                               fx.engine().shmSegmentSize(), testSecret(), &err));
    CHECK(err.find("already serving") != std::string::npos);
    CHECK(shm_handle_valid(shm_attach::receive(ep, testSecretBytes().data(), &err)));   // the owner still answers

    // A leftover socket file from a crashed engine on the same endpoint is
    // replaced, not a reason to fail: a bound-then-abandoned socket leaves
    // the file behind with nobody answering, which is exactly what a crash
    // leaves.
    srv.stop();
    {
        sockaddr_un addr {};
        REQUIRE(shm_attach::fill_sockaddr(ep, &addr));
        const int dead = ::socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(dead >= 0);
        REQUIRE(::bind(dead, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        ::close(dead);   // the file stays; connect() to it is refused
        REQUIRE(::stat(ep.c_str(), &st) == 0);
    }
    shm_attach::server again;
    REQUIRE(again.start(ep, fx.engine().shmNativeHandle(),
                        fx.engine().shmSegmentSize(), testSecret(), &err));
    CHECK(shm_handle_valid(shm_attach::receive(ep, testSecretBytes().data(), &err)));
}

// The segment holds the command ring, so the endpoint asks for the secret: a
// reader that sends a command instead, another secret, or nothing within the
// deadline is closed without a hello and counted, and the reader with the
// secret is still served.
TEST_CASE("attach: a reader without the secret gets no segment", "[shm][attach][security]") {
    constexpr unsigned kPort = 57334;
    EngineFixture fx(attachConfig(kPort));
    const std::string ep = endpointFor(kPort);
    shm_attach::server srv;
    std::string err;
    REQUIRE(srv.start(ep, fx.engine().shmNativeHandle(),
                      fx.engine().shmSegmentSize(), testSecret(), &err));

    auto other = testSecretBytes();
    other[5] ^= 1;
    CHECK_FALSE(shm_handle_valid(shm_attach::receive(ep, other.data(), &err)));
    CHECK(err.find("refused") != std::string::npos);

    // A raw client: a command as the first frame, then one that says nothing.
    auto raw = [&](const std::vector<uint8_t>& first) {
        sockaddr_un addr {};
        REQUIRE(shm_attach::fill_sockaddr(ep, &addr));
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        if (!first.empty()) REQUIRE(::send(fd, first.data(), first.size(), 0) == static_cast<ssize_t>(first.size()));
        timeval tv { 5, 0 };
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char byte = 0;
        const ssize_t n = ::recv(fd, &byte, 1, 0);
        ::close(fd);
        return n;   // 0: closed without a hello
    };
    std::vector<uint8_t> command = { 0, 0, 0, 12, '/', 's', 't', 'a', 't', 'u', 's', 0, ',', 0, 0, 0 };
    CHECK(raw(command) == 0);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(raw({}) == 0);
    CHECK(std::chrono::steady_clock::now() - t0 >= shm_attach::AUTH_DEADLINE);
    CHECK(srv.refused() == 3);

    CHECK(shm_handle_valid(shm_attach::receive(ep, testSecretBytes().data(), &err)));
    CHECK(srv.refused() == 3);
}
#endif

// The frame receive() builds is the one the library would: the same bytes
// clockwork_comms_secret_auth_packet encodes, after their length.
TEST_CASE("attach: the auth frame is the library's auth packet, framed", "[shm][attach]") {
    const auto frame = shm_attach::auth_frame(testSecretBytes().data());
    std::vector<uint8_t> packet(256);
    packet.resize(clockwork_comms_secret_auth_packet(testSecret(), packet.data(),
                                                     static_cast<uint32_t>(packet.size())));
    REQUIRE(packet.size() == shm_attach::AUTH_PACKET_LEN);
    CHECK(std::vector<uint8_t>(frame.begin() + 4, frame.end()) == packet);
    CHECK(frame[3] == shm_attach::AUTH_PACKET_LEN);
    CHECK(frame[0] == 0); CHECK(frame[1] == 0); CHECK(frame[2] == 0);
}

TEST_CASE("attach: nothing at the endpoint is a clean failure, not a hang",
          "[shm][attach]") {
    std::string err;
#if defined(_WIN32)
    const std::string nowhere = "clockwork-shm-test-nowhere";
#else
    const std::string nowhere = "/nonexistent-dir/clockwork-shm-nowhere.sock";
#endif
    CHECK_FALSE(shm_handle_valid(shm_attach::receive(nowhere, testSecretBytes().data(), &err)));
    CHECK_FALSE(err.empty());

    ClockworkStatus st = CLOCKWORK_OK;
    CHECK(clockwork_client_open_shm(nowhere.c_str(), testSecretBytes().data(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_NOT_FOUND);
    CHECK(clockwork_client_open_shm(nullptr, testSecretBytes().data(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_NOT_FOUND);
    CHECK(clockwork_client_open_shm(nowhere.c_str(), nullptr, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_ARG);
}

TEST_CASE("attach: the C client API opens by endpoint", "[shm][attach]") {
    constexpr unsigned kPort = 57332;
    EngineFixture fx(attachConfig(kPort));

    // The derived endpoint, as a C caller would obtain it.
    char buf[512];
    const uint32_t need = clockwork_client_default_endpoint(kPort, buf, sizeof buf);
    REQUIRE(need > 0);
    REQUIRE(need < sizeof buf);
    CHECK(std::string(buf) == shm_attach::default_endpoint(kPort));
    CHECK(std::string(buf).find(std::to_string(kPort)) != std::string::npos);
    // Too small a buffer reports the need and writes nothing usable.
    char tiny[4];
    CHECK(clockwork_client_default_endpoint(kPort, tiny, sizeof tiny) == need);
    CHECK(tiny[0] == '\0');

    shm_attach::server srv;
    std::string err;
    REQUIRE(srv.start(buf, fx.engine().shmNativeHandle(),
                      fx.engine().shmSegmentSize(), testSecret(), &err));

    ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
    ClockworkClient* c = clockwork_client_open_shm(buf, testSecretBytes().data(), &st);
    REQUIRE(c != nullptr);
    CHECK(st == CLOCKWORK_OK);
    clockwork_client_close(c);

    // Another secret is refused, and says so.
    auto other = testSecretBytes();
    other[31] ^= 1;
    CHECK(clockwork_client_open_shm(buf, other.data(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_PERM);
}

TEST_CASE("attach: the server refuses to serve without a segment or a secret", "[shm][attach]") {
    shm_attach::server srv;
    std::string err;
    CHECK_FALSE(srv.start(endpointFor(57333), detail_shm_segment::shm_invalid_handle, 0, testSecret(), &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(srv.running());

    constexpr unsigned kPort = 57335;
    EngineFixture fx(attachConfig(kPort));
    err.clear();
    CHECK_FALSE(srv.start(endpointFor(kPort), fx.engine().shmNativeHandle(),
                          fx.engine().shmSegmentSize(), nullptr, &err));
    CHECK(err.find("secret") != std::string::npos);
    CHECK_FALSE(srv.running());
}
