// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// A UDP socket for tests that speak to the engine over the wire: bound to a
// free loopback port, sends to an IPv4 address, receives with a timeout.
// Plain BSD sockets (Winsock on Windows), so a test never reaches into
// Smoothie for a socket: the client side of Clockwork has no JUCE.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace test_udp {

#ifdef _WIN32
using Handle = SOCKET;
constexpr Handle kNone = INVALID_SOCKET;
inline void closeHandle(Handle h) { closesocket(h); }
inline void startup() {
    static const int once = [] { WSADATA wsa; return WSAStartup(MAKEWORD(2, 2), &wsa); }();
    (void)once;
}
#else
using Handle = int;
constexpr Handle kNone = -1;
inline void closeHandle(Handle h) { ::close(h); }
inline void startup() {}
#endif

class Socket {
public:
    // Binds 127.0.0.1 on a free port; bound() says whether that worked.
    Socket() {
        startup();
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ == kNone) return;
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port        = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { closeHandle(fd_); fd_ = kNone; return; }
        socklen_t len = sizeof a;
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len) == 0) port_ = ntohs(a.sin_port);
    }
    ~Socket() { if (fd_ != kNone) closeHandle(fd_); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool bound() const { return fd_ != kNone && port_ > 0; }
    int port() const { return port_; }

    // One datagram to an IPv4 literal.
    bool send(const char* ip, int port, const void* data, size_t len) const {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port   = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, ip, &to.sin_addr) != 1) return false;
        const auto n = ::sendto(fd_, static_cast<const char*>(data), static_cast<int>(len), 0,
                                reinterpret_cast<const sockaddr*>(&to), sizeof to);
        return n == static_cast<decltype(n)>(len);
    }

    // The next datagram within `timeoutMs`: its length, or -1 when none came.
    // The sender's address and port are reported when asked for.
    int recv(void* buf, size_t cap, int timeoutMs, std::string* fromIp = nullptr, int* fromPort = nullptr) const {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd_, &set);
        timeval tv{};
        tv.tv_sec  = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (::select(static_cast<int>(fd_) + 1, &set, nullptr, nullptr, &tv) <= 0) return -1;
        sockaddr_in from{};
        socklen_t fromLen = sizeof from;
        const auto n = ::recvfrom(fd_, static_cast<char*>(buf), static_cast<int>(cap), 0,
                                  reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) return -1;
        if (fromIp) {
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, text, sizeof text);
            *fromIp = text;
        }
        if (fromPort) *fromPort = ntohs(from.sin_port);
        return static_cast<int>(n);
    }

private:
    Handle fd_   = kNone;
    int    port_ = 0;
};

// A loopback port nothing holds right now, for a server the test tells to bind.
inline int freeLoopbackPort() {
    Socket probe;
    return probe.port();
}

} // namespace test_udp
