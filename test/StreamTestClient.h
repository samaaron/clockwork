// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * StreamTestClient.h — a blocking client for clockwork's stream transports
 * (TCP and UDS stream), for tests that talk to one through a real socket:
 * connect, write a length-prefixed OSC frame, read one back.
 *
 * Unix-only: POSIX sockets, used directly.
 */
#pragma once
#ifndef _WIN32

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

inline void writeFramed(int fd, const std::vector<uint8_t>& pkt) {
    uint32_t be = htonl(static_cast<uint32_t>(pkt.size()));
    REQUIRE(write(fd, &be, 4) == 4);
    REQUIRE(write(fd, pkt.data(), pkt.size()) == static_cast<ssize_t>(pkt.size()));
}

// Read one length-prefixed frame; empty on EOF/timeout.
inline std::vector<uint8_t> readFramed(int fd) {
    auto readAll = [&](uint8_t* dst, size_t n) {
        size_t got = 0;
        while (got < n) {
            ssize_t r = read(fd, dst + got, n - got);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    };
    uint8_t hdr[4];
    if (!readAll(hdr, 4)) return {};
    uint32_t len = (uint32_t(hdr[0]) << 24) | (uint32_t(hdr[1]) << 16)
                 | (uint32_t(hdr[2]) << 8) | uint32_t(hdr[3]);
    std::vector<uint8_t> body(len);
    if (!readAll(body.data(), len)) return {};
    return body;
}

inline void setRecvTimeout(int fd) {
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

inline int connectTcp(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
    setRecvTimeout(fd);
    return fd;
}

inline int connectUnixStream(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
    setRecvTimeout(fd);
    return fd;
}

#endif // _WIN32
