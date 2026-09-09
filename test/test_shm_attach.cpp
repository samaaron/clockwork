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
#include "shm_attach.hpp"
#include "clockwork_client.h"

#include <string>

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
    cfg.udpPort = static_cast<int>(port);   // non-zero is what creates the segment
    return cfg;
}

// An endpoint no other test (and no other run of this one) is using: the
// derived name carries the port, and the port is unique to the case.
std::string endpointFor(unsigned port) { return shm_attach::default_endpoint(port); }

} // namespace

TEST_CASE("attach: a reader receives the engine's segment over the endpoint",
          "[shm][attach]") {
    constexpr unsigned kPort = 57330;
    EngineFixture fx(attachConfig(kPort));
    const std::string ep = endpointFor(kPort);

    shm_attach::server srv;
    std::string err;
    REQUIRE(srv.start(ep, fx.engine().shmNativeHandle(),
                      fx.engine().shmSegmentSize(), &err));
    INFO(err);

    // The hand-off: a handle of our own, and the size the engine declared.
    uint64_t size = 0;
    const auto h = shm_attach::receive(ep, &err, &size);
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
    const auto h2 = shm_attach::receive(ep, &err);
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
    CHECK_FALSE(shm_handle_valid(shm_attach::receive(ep, &err)));
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
                      fx.engine().shmSegmentSize(), &err));

    struct stat st {};
    REQUIRE(::stat(ep.c_str(), &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    CHECK((st.st_mode & 0777) == 0600);

    // A second server on a LIVE endpoint is refused: it must not unlink the
    // owner's socket and quietly take its readers (two engines on one port,
    // which a non-fatal UDP bind failure allows).
    shm_attach::server intruder;
    CHECK_FALSE(intruder.start(ep, fx.engine().shmNativeHandle(),
                               fx.engine().shmSegmentSize(), &err));
    CHECK(err.find("already serving") != std::string::npos);
    CHECK(shm_handle_valid(shm_attach::receive(ep, &err)));   // the owner still answers

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
                        fx.engine().shmSegmentSize(), &err));
    CHECK(shm_handle_valid(shm_attach::receive(ep, &err)));
}
#endif

TEST_CASE("attach: nothing at the endpoint is a clean failure, not a hang",
          "[shm][attach]") {
    std::string err;
#if defined(_WIN32)
    const std::string nowhere = "clockwork-shm-test-nowhere";
#else
    const std::string nowhere = "/nonexistent-dir/clockwork-shm-nowhere.sock";
#endif
    CHECK_FALSE(shm_handle_valid(shm_attach::receive(nowhere, &err)));
    CHECK_FALSE(err.empty());

    ClockworkStatus st = CLOCKWORK_OK;
    CHECK(clockwork_client_open_shm(nowhere.c_str(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_NOT_FOUND);
    CHECK(clockwork_client_open_shm(nullptr, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_NOT_FOUND);
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
                      fx.engine().shmSegmentSize(), &err));

    ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
    ClockworkClient* c = clockwork_client_open_shm(buf, &st);
    REQUIRE(c != nullptr);
    CHECK(st == CLOCKWORK_OK);
    clockwork_client_close(c);
}

TEST_CASE("attach: the server refuses to serve without a segment", "[shm][attach]") {
    shm_attach::server srv;
    std::string err;
    CHECK_FALSE(srv.start(endpointFor(57333), detail_shm_segment::shm_invalid_handle, 0, &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(srv.running());
}
