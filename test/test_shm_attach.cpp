// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_shm_attach.cpp — the attach endpoint (shm_attach.hpp): the only way a
 * process other than the engine reaches the anonymous segments, and the
 * protocol docs/SEGMENTS.md states: the secret first, then a role; a reader
 * is handed the published segment read-only, a peer the command segment
 * read-write and one at a time, its connection the lease.
 *
 * The engine here is in-process, but the hand-off is the real one: a socket
 * (or pipe) round trip, a duplicated handle, a fresh mapping. The
 * cross-process case rides the same code in test_shm_peer_crash.cpp.
 */
#include <catch2/catch_test_macros.hpp>

#include "EngineFixture.h"
#include "OscTestUtils.h"
#include "TestSecret.h"
#include "shm_attach.hpp"
#include "ShmTransport.h"
#include "clockwork_client.h"
#include "ThreadWakes.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <process.h>   // _getpid
#else
#include <unistd.h>    // getpid
#endif

#if !defined(_WIN32)
#  include <cerrno>
#  include <sys/mman.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

using detail_shm_segment::shm_handle_valid;
using detail_shm_segment::shm_segment_client;
using detail_shm_segment::shm_command_client;

namespace {

ClockworkEngine::Config attachConfig(unsigned port, bool commands = false) {
    ClockworkEngine::Config cfg = EngineFixture::defaultConfig();
    cfg.shmSegment  = true;      // non-zero is what creates the published segment
    cfg.shmCommands = commands;  // and this the command segment
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

// Serve an engine's two segments, as a host does: the reader handle of the
// published one and, when the engine has it, the command segment.
bool serve(shm_attach::server& srv, const std::string& ep, ClockworkEngine& e, std::string* err) {
    return srv.start(ep, e.shmReaderHandle(), e.shmSegmentSize(),
                     e.shmCommandHandle(), e.shmCommandSize(), e.shmCommandWakeEvent(), testSecret(), err);
}

#if !defined(_WIN32)
// Whether a writable mapping of `fd` is possible.
bool mapsWritable(int fd, size_t bytes) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return false;
    ::munmap(p, bytes);
    return true;
}

// A raw client: connect, send `first` (nothing when empty), read what comes
// back within 5 s. Returns the bytes read; 0 means closed without a hello.
std::vector<uint8_t> raw(const std::string& ep, const std::vector<uint8_t>& first) {
    sockaddr_un addr {};
    REQUIRE(shm_attach::fill_sockaddr(ep, &addr));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    if (!first.empty()) REQUIRE(::send(fd, first.data(), first.size(), 0) == static_cast<ssize_t>(first.size()));
    timeval tv { 5, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::vector<uint8_t> got(64);
    const ssize_t n = ::recv(fd, got.data(), got.size(), 0);
    ::close(fd);
    got.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return got;
}

std::vector<uint8_t> authThenRole(uint32_t role) {
    const auto frame = shm_attach::auth_frame(testSecretBytes().data());
    std::vector<uint8_t> bytes(frame.begin(), frame.end());
    bytes.push_back(static_cast<uint8_t>(role >> 24));
    bytes.push_back(static_cast<uint8_t>(role >> 16));
    bytes.push_back(static_cast<uint8_t>(role >> 8));
    bytes.push_back(static_cast<uint8_t>(role));
    return bytes;
}
#endif

// An engine with the shared-memory command plane served at an endpoint, its
// own control pass (or, with hostDrives, the test's), no audio thread and
// nothing pumped, and one peer attached through the C client API: what a peer
// gets while no block renders.
struct IdlePeerEngine {
    ShmTransport       transport;
    ClockworkEngine    engine;
    shm_attach::server srv;
    ClockworkClient*   peer = nullptr;

    explicit IdlePeerEngine(unsigned port, bool hostDrives = false) {
        engine.onDebug = [](const std::string&) {};
        engine.onReply = [](const uint8_t*, uint32_t) {};
        engine.setTransport(&transport);
        auto cfg = attachConfig(port, true);
        cfg.hostDrivesControl = hostDrives;
        engine.init(cfg);
        transport.bindPlaneSlot(engine.peerPlaneSlot());
        REQUIRE(transport.ready());
        std::string err;
        REQUIRE(serve(srv, endpointFor(port), engine, &err));
        ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
        peer = clockwork_client_open_shm_peer(endpointFor(port).c_str(), testSecretBytes().data(), &st);
        REQUIRE(peer != nullptr);
    }
    ~IdlePeerEngine() {
        if (peer) clockwork_client_close(peer);
        srv.stop();
        engine.shutdown();
    }
    static bool applies() {
        const auto cfg = EngineFixture::defaultConfig();
        return cfg.manualAudioPump && !cfg.hostDrivesControl;
    }
};

// Wall clock only: nothing here renders a block.
bool within(const std::function<bool()>& f, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return f();
}

// How many of `address` the peer has been sent since the last call.
int takeReplies(ClockworkClient* peer, const char* address) {
    ClockworkClientMessage msgs[64];
    int got = 0;
    for (uint32_t n; (n = clockwork_client_poll(peer, msgs, 64)) > 0;)
        for (uint32_t i = 0; i < n; ++i)
            if (std::strcmp(reinterpret_cast<const char*>(msgs[i].bytes), address) == 0) ++got;
    return got;
}

} // namespace

TEST_CASE("attach: a reader receives the published segment over the endpoint, read-only",
          "[shm][attach]") {
    constexpr unsigned kPort = 57330;
    EngineFixture fx(attachConfig(kPort));
    const std::string ep = endpointFor(kPort);

    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, ep, fx.engine(), &err));
    INFO(err);

    // The hand-off: a handle of our own, and the size the engine declared.
    uint64_t size = 0;
    const auto h = shm_attach::receive(ep, testSecretBytes().data(), &err, &size);
    INFO(err);
    REQUIRE(shm_handle_valid(h));
    CHECK(size == fx.engine().shmSegmentSize());
#if !defined(_WIN32)
    // Read-only, and the kernel is what says so.
    CHECK_FALSE(mapsWritable(h, size));
    CHECK((errno == EACCES || errno == EPERM));   // POSIX, and XNU's POSIX shm
#endif

    // Mapped from it, the segment is the engine's: published, layout-checked,
    // and live — a metric the engine writes is visible through it.
    shm_segment_client client(h);
    REQUIRE(client.get_metrics() != nullptr);
    fx.send(osc_test::message("/dummy/ping"));
    OscReply r;
    REQUIRE(fx.waitForReply("/dummy/pong", r));
    CHECK(client.get_metrics()->messages_processed.load() > 0);

    // The server keeps serving: a second reader gets its own handle onto the
    // same pages.
    const auto h2 = shm_attach::receive(ep, testSecretBytes().data(), &err);
    INFO(err);   // this receive's reason, not the first's: INFO keeps the string as it was when it ran
    REQUIRE(shm_handle_valid(h2));
    CHECK(h2 != h);
    shm_segment_client second(h2);
    CHECK(second.get_base() != client.get_base());   // two mappings, same pages
    CHECK(second.get_metrics()->messages_processed.load()
          == client.get_metrics()->messages_processed.load());

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

TEST_CASE("attach: the command segment is handed to one peer at a time, and only where it exists",
          "[shm][attach][peer]") {
    constexpr unsigned kPort = 57336;
    std::string err;

    SECTION("a server without the command plane declines a peer, with the reason") {
        EngineFixture fx(attachConfig(kPort, false));
        const std::string ep = endpointFor(kPort);
        shm_attach::server srv;
        REQUIRE(serve(srv, ep, fx.engine(), &err));
        CHECK_FALSE(shm_handle_valid(fx.engine().shmCommandHandle()));

        auto lease = shm_attach::receive_command_plane(ep, testSecretBytes().data(), &err);
        CHECK_FALSE(lease.valid());
        CHECK(err.find("no shared-memory command plane") != std::string::npos);
        CHECK(srv.declined() == 1);
        CHECK(srv.refused() == 0);   // the secret was right: this is not a refusal
        // And a reader is still served.
        CHECK(shm_handle_valid(shm_attach::receive(ep, testSecretBytes().data(), &err)));
    }

    SECTION("with the plane: one holder, the connection is the lease") {
        EngineFixture fx(attachConfig(kPort, true));
        const std::string ep = endpointFor(kPort);
        shm_attach::server srv;
        REQUIRE(serve(srv, ep, fx.engine(), &err));
        REQUIRE(shm_handle_valid(fx.engine().shmCommandHandle()));

        uint64_t size = 0;
        auto first = shm_attach::receive_command_plane(ep, testSecretBytes().data(), &err, &size);
        INFO(err);
        REQUIRE(first.valid());
        CHECK(size == fx.engine().shmCommandSize());
        CHECK(srv.holding());
#if !defined(_WIN32)
        CHECK(mapsWritable(first.handle, size));   // the peer writes it
#endif
        // The plane is the engine's: the geometry the creator wrote.
        {
            shm_command_client peer(detail_shm_segment::shm_dup_handle(first.handle));
            CHECK(peer.get_peer_plane()->cmd_ring_size == SHM_PEER_CMD_RING_SIZE);
        }

        // Held: a second peer is declined, and told why.
        auto second = shm_attach::receive_command_plane(ep, testSecretBytes().data(), &err);
        CHECK_FALSE(second.valid());
        CHECK(err.find("another peer holds") != std::string::npos);
        CHECK(srv.declined() == 1);
        // Readers are not affected by the hold.
        CHECK(shm_handle_valid(shm_attach::receive(ep, testSecretBytes().data(), &err)));

        // Released: closing the lease is the release, and the next peer is served.
        first.release();
        CHECK_FALSE(first.valid());
        REQUIRE(fx.pollUntil([&] { return !srv.holding(); }, 3000));
        auto third = shm_attach::receive_command_plane(ep, testSecretBytes().data(), &err);
        INFO(err);
        CHECK(third.valid());
        CHECK(srv.holding());
        // Going out of scope releases too.
    }
}

#if !defined(_WIN32)
TEST_CASE("attach: the hello names the role, the status and the layout it is",
          "[shm][attach]") {
    constexpr unsigned kPort = 57337;
    EngineFixture fx(attachConfig(kPort, false));
    const std::string ep = endpointFor(kPort);
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, ep, fx.engine(), &err));

    static_assert(sizeof(shm_attach::hello) == 40, "hello is a wire format");
    CHECK(shm_attach::VERSION == 4);

    const auto decode = [](const std::vector<uint8_t>& bytes) {
        shm_attach::hello h {};
        REQUIRE(bytes.size() == sizeof h);
        std::memcpy(&h, bytes.data(), sizeof h);
        return h;
    };

    // A reader: ok, the role echoed, the size the engine's.
    auto h = decode(raw(ep, authThenRole(shm_attach::ROLE_READER)));
    CHECK(h.magic == shm_attach::MAGIC);
    CHECK(h.version == 4);
    CHECK(h.role == shm_attach::ROLE_READER);
    CHECK(h.status == shm_attach::STATUS_OK);
    CHECK(h.size == fx.engine().shmSegmentSize());

    // A peer where there is no plane: a hello that says so, no size, no
    // descriptor.
    h = decode(raw(ep, authThenRole(shm_attach::ROLE_PEER)));
    CHECK(h.role == shm_attach::ROLE_PEER);
    CHECK(h.status == shm_attach::STATUS_NO_COMMAND_SEGMENT);
    CHECK(h.size == 0);

    // A role the server does not know.
    h = decode(raw(ep, authThenRole(77)));
    CHECK(h.status == shm_attach::STATUS_UNKNOWN_ROLE);
    CHECK(h.size == 0);
    CHECK(srv.declined() == 2);
    CHECK(srv.refused() == 0);
}

TEST_CASE("attach: the endpoint is a socket only its owner can open", "[shm][attach][security]") {
    constexpr unsigned kPort = 57331;
    EngineFixture fx(attachConfig(kPort));
    const std::string ep = endpointFor(kPort);
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, ep, fx.engine(), &err));

    struct stat st {};
    REQUIRE(::stat(ep.c_str(), &st) == 0);
    CHECK(S_ISSOCK(st.st_mode));
    CHECK((st.st_mode & 0777) == 0600);

    // A second server on a LIVE endpoint is refused: it must not unlink the
    // owner's socket and quietly take its readers (two engines on one port,
    // which a non-fatal UDP bind failure allows).
    shm_attach::server intruder;
    CHECK_FALSE(serve(intruder, ep, fx.engine(), &err));
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
    REQUIRE(serve(again, ep, fx.engine(), &err));
    CHECK(shm_handle_valid(shm_attach::receive(ep, testSecretBytes().data(), &err)));
}

// The endpoint asks for the secret before anything else: a client that sends
// a command instead, another secret, or nothing within the deadline is closed
// without a hello and counted as refused — it learns nothing, not even that
// there are roles — and the reader with the secret is still served.
TEST_CASE("attach: a client without the secret gets nothing, not even a hello", "[shm][attach][security]") {
    constexpr unsigned kPort = 57334;
    EngineFixture fx(attachConfig(kPort, true));
    const std::string ep = endpointFor(kPort);
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, ep, fx.engine(), &err));

    auto other = testSecretBytes();
    other[5] ^= 1;
    CHECK_FALSE(shm_handle_valid(shm_attach::receive(ep, other.data(), &err)));
    CHECK(err.find("refused") != std::string::npos);
    CHECK_FALSE(shm_attach::receive_command_plane(ep, other.data(), &err).valid());
    CHECK(err.find("refused") != std::string::npos);

    std::vector<uint8_t> command = { 0, 0, 0, 12, '/', 's', 't', 'a', 't', 'u', 's', 0, ',', 0, 0, 0 };
    CHECK(raw(ep, command).empty());
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(raw(ep, {}).empty());
    CHECK(std::chrono::steady_clock::now() - t0 >= shm_attach::AUTH_DEADLINE);
    // The secret right but no role within the deadline: nothing either.
    const auto frame = shm_attach::auth_frame(testSecretBytes().data());
    CHECK(raw(ep, std::vector<uint8_t>(frame.begin(), frame.end())).empty());
    CHECK(srv.refused() == 5);
    CHECK(srv.declined() == 0);

    CHECK(shm_handle_valid(shm_attach::receive(ep, testSecretBytes().data(), &err)));
    CHECK(srv.refused() == 5);
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
    CHECK_FALSE(shm_attach::receive_command_plane(nowhere, testSecretBytes().data(), &err).valid());
    CHECK_FALSE(err.empty());

    ClockworkStatus st = CLOCKWORK_OK;
    CHECK(clockwork_client_open_shm(nowhere.c_str(), testSecretBytes().data(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_NOT_FOUND);
    CHECK(clockwork_client_open_shm(nullptr, testSecretBytes().data(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_NOT_FOUND);
    CHECK(clockwork_client_open_shm(nowhere.c_str(), nullptr, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_ARG);
    CHECK(clockwork_client_open_shm_peer(nowhere.c_str(), testSecretBytes().data(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_NOT_FOUND);
}

TEST_CASE("attach: the C client API opens a reader by endpoint, and a reader cannot send or take",
          "[shm][attach]") {
    constexpr unsigned kPort = 57332;
    EngineFixture fx(attachConfig(kPort, true));

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
    REQUIRE(serve(srv, buf, fx.engine(), &err));

    ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
    ClockworkClient* c = clockwork_client_open_shm(buf, testSecretBytes().data(), &st);
    REQUIRE(c != nullptr);
    CHECK(st == CLOCKWORK_OK);

    // A reader reads: the metrics, the clock.
    uint32_t words[8];
    CHECK(clockwork_client_metrics(c, words, 8) > 0);
    ClockworkClientClock clock {};
    clock.struct_bytes = sizeof clock;
    CHECK(clockwork_client_clock(c, &clock) == CLOCKWORK_OK);

    // And cannot send, take replies, or reach the lanes or the in ring: its
    // segment has none of them, and saying so is the honest answer.
    const auto ping = osc_test::message("/dummy/ping");
    CHECK(clockwork_client_send(c, ping.ptr(), ping.size(), 0xC11E) == CLOCKWORK_E_PERM);
    CHECK(clockwork_client_send_begin(c, 64, &st) == nullptr);
    CHECK(st == CLOCKWORK_E_PERM);
    ClockworkClientMessage msgs[4];
    CHECK(clockwork_client_poll(c, msgs, 4) == 0);
    ClockworkRegion region {};
    CHECK(clockwork_client_region(c, CLOCKWORK_REGION_ASSETS, &region) == CLOCKWORK_E_PERM);
    CHECK(clockwork_client_region(c, CLOCKWORK_REGION_RETURNS, &region) == CLOCKWORK_E_PERM);
    CHECK(clockwork_client_region(c, CLOCKWORK_REGION_INGRESS, &region) == CLOCKWORK_E_PERM);
    CHECK(clockwork_client_region(c, CLOCKWORK_REGION_METRICS, &region) == CLOCKWORK_OK);
    CHECK(region.writable == 0);
    clockwork_client_close(c);

    // Another secret is refused, and says so.
    auto other = testSecretBytes();
    other[31] ^= 1;
    CHECK(clockwork_client_open_shm(buf, other.data(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_PERM);
}

TEST_CASE("attach: the C client API opens a peer by endpoint, which sends and takes through the plane",
          "[shm][attach][peer]") {
    constexpr unsigned kPort = 57338;
    // Wired the way a host wires --shm-commands: the engine's replies to the
    // peer leave through a ShmTransport bound to its plane slot.
    ShmTransport    transport;
    ClockworkEngine engine;
    engine_test::HostControlThread hostControl;   // the host's pass, when the config says the host runs it
    engine.onDebug = [](const std::string&) {};
    engine.onReply = [](const uint8_t*, uint32_t) {};
    engine.setTransport(&transport);
    const auto cfg = attachConfig(kPort, true);
    if (cfg.hostDrivesControl) hostControl.start(engine);
    engine.init(cfg);
    transport.bindPlaneSlot(engine.peerPlaneSlot());
    REQUIRE(transport.ready());
    const std::string ep = endpointFor(kPort);
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, ep, engine, &err));
    // Pumped: the engine's pass drains the plane as the audio thread ticks.
    const auto until = [&](const std::function<bool()>& f, int ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            engine.pumpAudioBlock();
            if (f()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };

    ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
    ClockworkClient* peer = clockwork_client_open_shm_peer(ep.c_str(), testSecretBytes().data(), &st);
    REQUIRE(peer != nullptr);
    CHECK(st == CLOCKWORK_OK);
    CHECK(srv.holding());

    // A reader's view too: the peer attached to both segments.
    uint32_t words[8];
    CHECK(clockwork_client_metrics(peer, words, 8) > 0);
    ClockworkRegion assets {};
    REQUIRE(clockwork_client_region(peer, CLOCKWORK_REGION_ASSETS, &assets) == CLOCKWORK_OK);
    CHECK(assets.writable == 1);
    CHECK(assets.bytes == engine.guestAssetsBytes());
    CHECK(clockwork_client_region(peer, CLOCKWORK_REGION_INGRESS, &assets) == CLOCKWORK_E_PERM);

    // A command through the plane, and the reply back through it.
    const auto get = osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{4343});
    REQUIRE(clockwork_client_send(peer, get.ptr(), get.size(), 0) == CLOCKWORK_OK);
    bool heard = false;
    ClockworkClientMessage msgs[8];
    REQUIRE(until([&] {
        const uint32_t n = clockwork_client_poll(peer, msgs, 8);
        for (uint32_t i = 0; i < n; ++i) {
            if (std::strcmp(reinterpret_cast<const char*>(msgs[i].bytes), CLOCKWORK_SYS("clock/tempo.reply")) == 0) {
                heard = true;
                CHECK(msgs[i].origin == SHM_PEER_ORIGIN_TOKEN);
            }
        }
        return heard;
    }, 3000));

    // A second peer is declined while this one holds the plane.
    CHECK(clockwork_client_open_shm_peer(ep.c_str(), testSecretBytes().data(), &st) == nullptr);
    CHECK(st == CLOCKWORK_E_BUSY);
    clockwork_client_close(peer);
    REQUIRE(until([&] { return !srv.holding(); }, 3000));
    srv.stop();
    hostControl.stop();   // a host stops its thread first
    engine.shutdown();
}

// A peer wakes the engine's control pass through the word in the command
// segment, so its command is answered with no audio block rendered, and an
// engine nobody writes to runs no pass at all.
TEST_CASE("attach: a peer's command wakes the engine's control pass, with no audio block and no timer",
          "[shm][attach][peer][wake]") {
    if (!IdlePeerEngine::applies()) SKIP("needs the engine's own control pass and no audio thread");
    IdlePeerEngine e(57339);

    const auto get = osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{4344});
    REQUIRE(clockwork_client_send(e.peer, get.ptr(), get.size(), 0) == CLOCKWORK_OK);
    CHECK(within([&] { return takeReplies(e.peer, CLOCKWORK_SYS("clock/tempo.reply")) == 1; }, 3000));

    // Quiet: no write, no block, no pass. A 1 ms timer would run a hundred.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t settled = e.engine.controlPasses();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(e.engine.controlPasses() == settled);
}

// The pass holds a peer's commands back while the reply ring lacks room for
// their answers. A peer that sends more than the reply ring holds answers for,
// then only reads, is answered in full: reading makes the room, and the engine
// answers the rest with no further write and no audio block.
TEST_CASE("attach: a peer that sends more than its reply ring holds, then only reads, gets every answer with no audio block",
          "[shm][attach][peer][wake]") {
    if (!IdlePeerEngine::applies()) SKIP("needs the engine's own control pass and no audio thread");
    IdlePeerEngine e(57340);

    const auto get = osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1});
    int sent = 0;
    ClockworkStatus st = CLOCKWORK_OK;
    while (sent < 100000 && (st = clockwork_client_send(e.peer, get.ptr(), get.size(), 0)) == CLOCKWORK_OK) ++sent;
    // Full: the command ring holds what the reply ring had no room to answer.
    REQUIRE(st == CLOCKWORK_E_FULL);

    int got = 0;
    within([&] { got += takeReplies(e.peer, CLOCKWORK_SYS("clock/tempo.reply")); return got >= sent; }, 5000);
    CHECK(e.engine.peerPlaneSlot()->load()->rep_dropped.load() == 0);
    CHECK(got == sent);
}

// A peer's answers reach its reply ring through the NRT egress ring, so the
// room a command is held back for counts the answers already queued there:
// after a short read, a pass takes no more commands than the reply ring has
// room to answer, and no answer is dropped. The test runs the passes, so
// each one ends where the test says.
TEST_CASE("attach: a peer that reads a few answers at a time loses none to a full reply ring",
          "[shm][attach][peer]") {
    if (!EngineFixture::defaultConfig().manualAudioPump) SKIP("needs no audio thread");
    IdlePeerEngine e(57346, true);
    ShmPeerPlaneHeader* plane = e.engine.peerPlaneSlot()->load();
    REQUIRE(plane != nullptr);

    // Passes until two in a row move nothing: no command taken, no answer
    // written to the reply ring or dropped. An answer framed in one pass
    // reaches the reply ring in the next, so one quiet pass may have framed
    // answers; two in a row cannot have.
    const auto passUntilStill = [&] {
        for (int passes = 0, quiet = 0; passes < 10000; ++passes) {
            const auto cmd = plane->cmd_tail.load(), rep = plane->rep_head.load();
            const auto dropped = plane->rep_dropped.load();
            REQUIRE(e.engine.controlPass());
            const bool moved = plane->cmd_tail.load() != cmd || plane->rep_head.load() != rep
                            || plane->rep_dropped.load() != dropped;
            quiet = moved ? 0 : quiet + 1;
            if (quiet == 2) return;
        }
        FAIL("the passes never settled");
    };
    std::vector<ClockworkClientMessage> msgs(16384);
    const auto read = [&](uint32_t max) {
        const uint32_t n = clockwork_client_poll(e.peer, msgs.data(), max);
        int answers = 0;
        for (uint32_t i = 0; i < n; ++i)
            if (std::strcmp(reinterpret_cast<const char*>(msgs[i].bytes), CLOCKWORK_SYS("clock/tempo.reply")) == 0)
                ++answers;
        return answers;
    };

    const auto get = osc_test::message(CLOCKWORK_SYS("clock/tempo/get"), int32_t{1});
    int sent = 0;
    while (sent < 100000 && clockwork_client_send(e.peer, get.ptr(), get.size(), 0) == CLOCKWORK_OK) ++sent;
    passUntilStill();
    // Held back for room: the reply ring is nearly full.
    REQUIRE(plane->rep_room_wanted.load() == 1);

    int got = read(100);
    REQUIRE(got == 100);
    passUntilStill();
    for (int answers; (answers = read(static_cast<uint32_t>(msgs.size()))) > 0;) {
        got += answers;
        passUntilStill();
    }
    CHECK(plane->rep_dropped.load() == 0);
    CHECK(got == sent);
}

TEST_CASE("attach: the server refuses to serve without a published segment or a secret", "[shm][attach]") {
    shm_attach::server srv;
    std::string err;
    CHECK_FALSE(srv.start(endpointFor(57333), detail_shm_segment::shm_invalid_handle, 0,
                          detail_shm_segment::shm_invalid_handle, 0, nullptr, testSecret(), &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(srv.running());

    constexpr unsigned kPort = 57335;
    EngineFixture fx(attachConfig(kPort));
    err.clear();
    CHECK_FALSE(srv.start(endpointFor(kPort), fx.engine().shmReaderHandle(), fx.engine().shmSegmentSize(),
                          detail_shm_segment::shm_invalid_handle, 0, nullptr, nullptr, &err));
    CHECK(err.find("secret") != std::string::npos);
    CHECK_FALSE(srv.running());
}

// ── The server's waits ───────────────────────────────────────────────────────
//
// The server's thread blocks only on a client, a stop or the lease, and on a
// client's request until its deadline. It waits with a pipe and poll on POSIX
// and with overlapped I/O and events on Windows; these cases reach the same
// paths on both.

namespace {

// A whole request: the secret's frame, then a role.
std::vector<uint8_t> requestFor(uint32_t role) {
    const auto frame = shm_attach::auth_frame(testSecretBytes().data());
    std::vector<uint8_t> bytes(frame.begin(), frame.end());
    for (int shift : { 24, 16, 8, 0 }) bytes.push_back(static_cast<uint8_t>(role >> shift));
    return bytes;
}

// A hand-made client of the endpoint: it connects, sends what a case gives
// it in the pieces the case chooses, and reads what comes back.
struct RawClient {
#if defined(_WIN32)
    HANDLE pipe = INVALID_HANDLE_VALUE;
    explicit RawClient(const std::string& ep) {
        const std::wstring name = shm_attach::pipe_path(ep);
        for (int attempt = 0; attempt < 3 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
            pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 5000);
        }
        REQUIRE(pipe != INVALID_HANDLE_VALUE);
    }
    ~RawClient() { if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe); }
    void send(const uint8_t* bytes, size_t n) {
        DWORD wrote = 0;
        REQUIRE(WriteFile(pipe, bytes, static_cast<DWORD>(n), &wrote, nullptr));
        REQUIRE(wrote == n);
    }
    // Returns what the server sent before it closed; empty if it sent nothing.
    std::vector<uint8_t> receive() {
        std::vector<uint8_t> got(64);
        DWORD n = 0;
        if (!ReadFile(pipe, got.data(), static_cast<DWORD>(got.size()), &n, nullptr)) n = 0;
        got.resize(n);
        return got;
    }
#else
    int fd = -1;
    explicit RawClient(const std::string& ep) {
        sockaddr_un addr {};
        REQUIRE(shm_attach::fill_sockaddr(ep, &addr));
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    }
    ~RawClient() { if (fd >= 0) ::close(fd); }
    void send(const uint8_t* bytes, size_t n) {
        REQUIRE(::send(fd, bytes, n, 0) == static_cast<ssize_t>(n));
    }
    std::vector<uint8_t> receive() {
        timeval tv { 30, 0 };   // only a server that never answers or closes reaches it
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        std::vector<uint8_t> got(64);
        const ssize_t n = ::recv(fd, got.data(), got.size(), 0);
        got.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return got;
    }
#endif
};

shm_attach::hello helloOf(const std::vector<uint8_t>& bytes) {
    shm_attach::hello h {};
    REQUIRE(bytes.size() == sizeof h);
    std::memcpy(&h, bytes.data(), sizeof h);
    return h;
}

} // namespace

TEST_CASE("attach: a request that arrives a few bytes at a time is answered", "[shm][attach]") {
    constexpr unsigned kPort = 57341;
    EngineFixture fx(attachConfig(kPort));
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, endpointFor(kPort), fx.engine(), &err));

    RawClient c(endpointFor(kPort));
    const auto request = requestFor(shm_attach::ROLE_READER);
    for (size_t at = 0; at < request.size(); at += 7) {
        c.send(request.data() + at, std::min<size_t>(7, request.size() - at));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));   // each piece a read of its own
    }
    const auto h = helloOf(c.receive());
    CHECK(h.magic == shm_attach::MAGIC);
    CHECK(h.status == shm_attach::STATUS_OK);
    CHECK(h.role == shm_attach::ROLE_READER);
    CHECK(h.size == fx.engine().shmSegmentSize());
    CHECK(srv.refused() == 0);
}

TEST_CASE("attach: a client that says nothing is refused at the deadline, and the server serves on",
          "[shm][attach]") {
    constexpr unsigned kPort = 57342;
    EngineFixture fx(attachConfig(kPort));
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, endpointFor(kPort), fx.engine(), &err));

    {
        // Timed from before the connect: the server's deadline starts after
        // it, so the close can come no sooner than AUTH_DEADLINE after this.
        const auto t0 = std::chrono::steady_clock::now();
        RawClient silent(endpointFor(kPort));
        CHECK(silent.receive().empty());   // closed with no hello
        CHECK(std::chrono::steady_clock::now() - t0 >= shm_attach::AUTH_DEADLINE);
    }
    CHECK(within([&] { return srv.refused() == 1; }, 5000));
    CHECK(shm_handle_valid(shm_attach::receive(endpointFor(kPort), testSecretBytes().data(), &err)));
    INFO(err);
}

TEST_CASE("attach: a stop while a client says nothing ends at once, not at the auth deadline",
          "[shm][attach]") {
    constexpr unsigned kPort = 57343;
    EngineFixture fx(attachConfig(kPort));
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, endpointFor(kPort), fx.engine(), &err));

    RawClient silent(endpointFor(kPort));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // the server is waiting on its request
    const auto t0 = std::chrono::steady_clock::now();
    srv.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < shm_attach::AUTH_DEADLINE / 2);
    CHECK_FALSE(srv.running());
}

TEST_CASE("attach: the server's thread does not wake while nobody connects", "[shm][attach][wakes]") {
    if (!thread_wakes::countable()) SKIP("per-thread wake counts are read from Linux's /proc");
    constexpr unsigned kPort = 57344;
    EngineFixture fx(attachConfig(kPort));
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, endpointFor(kPort), fx.engine(), &err));
    // A loop that checked for its stop every 200 ms would wake five times here.
    const auto woke = thread_wakes::over("cw-attach", std::chrono::seconds(1));
    REQUIRE(woke);
    CHECK(*woke <= 1);
}

TEST_CASE("attach: a client that never reads its hello holds the server no longer than the deadline",
          "[shm][attach]") {
    constexpr unsigned kPort = 57345;
    EngineFixture fx(attachConfig(kPort));
    shm_attach::server srv;
    std::string err;
    REQUIRE(serve(srv, endpointFor(kPort), fx.engine(), &err));

    // On Windows a disconnect discards an unread hello, so the server waits
    // for the client to close, but only until the deadline or a stop.
    const auto request = requestFor(shm_attach::ROLE_READER);
    RawClient deaf(endpointFor(kPort));
    deaf.send(request.data(), request.size());
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(shm_handle_valid(shm_attach::receive(endpointFor(kPort), testSecretBytes().data(), &err)));
    INFO(err);
    CHECK(std::chrono::steady_clock::now() - t0 < 3 * shm_attach::AUTH_DEADLINE);

    RawClient deafToo(endpointFor(kPort));
    deafToo.send(request.data(), request.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // its hello written, unread
    const auto t1 = std::chrono::steady_clock::now();
    srv.stop();
    CHECK(std::chrono::steady_clock::now() - t1 < shm_attach::AUTH_DEADLINE / 2);
    CHECK_FALSE(srv.running());
}
