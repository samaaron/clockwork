// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
//  The attach endpoint: how a reader in another process gets the engine's
//  anonymous shared-memory segment.
//
//  The segment has no name (shm_segment.hpp), so a reader cannot open it; it
//  has to be GIVEN it. That needs a channel that can carry an OS object, and
//  only a local one can: a Unix-domain socket passes a file descriptor as
//  ancillary data (SCM_RIGHTS); a Windows named pipe tells the server the
//  client's pid, and the server duplicates its handle into that process. TCP
//  and UDP cannot do either, which is why this is its own small endpoint
//  rather than a message on the command transport — a host that speaks TCP to
//  its engine (Sonic Pi's daemon does) still needs the GUI to reach the
//  segment.
//
//  PROTOCOL. Connect; the server sends one `hello` (magic, version, segment
//  size, and on Windows the duplicated handle value) with the descriptor
//  riding alongside on POSIX, then closes. No request, no state: attaching is
//  the connection.
//
//  TRUST. The endpoint lives in a per-user directory (XDG_RUNTIME_DIR or
//  TMPDIR, both 0700) with a 0600 socket, and BOTH ends check the peer's uid:
//  the server hands memory only to its own user, and a client refuses a
//  server run by anyone else, so a planted path yields nothing. On Windows
//  the pipe carries an owner-only DACL and rejects remote clients.
//
//  The default endpoint is derived from the engine's port so that a launcher
//  and a reader that only share the port still meet; --shm-endpoint names any
//  other path or pipe.

#pragma once

#include "shm_segment.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <sddl.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "advapi32.lib")
#  endif
#else
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  if defined(__linux__)
#    include <sys/types.h>
#  else
#    include <sys/types.h>
#  endif
#endif

namespace shm_attach {

using detail_shm_segment::shm_native_handle;
using detail_shm_segment::shm_invalid_handle;
using detail_shm_segment::shm_handle_valid;

// What the server sends. Fixed layout, native byte order: both ends are on
// one machine.
struct hello {
    uint32_t magic;
    uint32_t version;
    uint64_t size;      // the segment's byte length, for the reader's checks
    uint64_t handle;    // Windows: the HANDLE value in the receiver's table;
                        // POSIX: 0, the descriptor travels as ancillary data
};
static constexpr uint32_t MAGIC   = 0x43574154;  // "CWAT"
static constexpr uint32_t VERSION = 1;
static_assert(sizeof(hello) == 24, "hello is a wire format");

// The endpoint an engine on `port` uses unless told otherwise. A per-user
// location: on Linux the session runtime dir, on macOS the per-user TMPDIR,
// and a uid-tagged /tmp path only where neither is set.
inline std::string default_endpoint(unsigned port) {
#ifdef _WIN32
    return "\\\\.\\pipe\\clockwork-shm-" + std::to_string(port);
#else
    auto dir_from = [](const char* var) -> std::string {
        const char* v = std::getenv(var);
        if (!v || !*v) return {};
        std::string d(v);
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        return d;
    };
    std::string dir = dir_from("XDG_RUNTIME_DIR");
    if (dir.empty()) dir = dir_from("TMPDIR");
    if (dir.empty())
        return "/tmp/clockwork-shm-" + std::to_string(static_cast<unsigned>(::getuid()))
             + "-" + std::to_string(port) + ".sock";
    return dir + "/clockwork-shm-" + std::to_string(port) + ".sock";
#endif
}

#ifdef _WIN32

// A pipe name as the API wants it: "\\.\pipe\" plus whatever was given, unless
// it already is one.
inline std::wstring pipe_path(const std::string& endpoint) {
    std::string p = endpoint;
    if (p.rfind("\\\\.\\pipe\\", 0) != 0) p = "\\\\.\\pipe\\" + p;
    return clockwork_path::to_wide(p);
}

// Owner-only security descriptor (SYSTEM + the current user), from SDDL —
// the same descriptor the Rust pipe transport builds. Freed with LocalFree.
inline PSECURITY_DESCRIPTOR owner_only_sd() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return nullptr;
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::string buf(needed, '\0');
    const BOOL ok = GetTokenInformation(token, TokenUser, buf.data(), needed, &needed);
    CloseHandle(token);
    if (!ok) return nullptr;
    auto* user = reinterpret_cast<TOKEN_USER*>(buf.data());
    LPWSTR sidW = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sidW)) return nullptr;
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;";
    sddl += sidW;
    sddl += L")";
    LocalFree(sidW);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              &sd, nullptr))
        return nullptr;
    return sd;
}

#else

// The uid on the other end of a connected Unix socket.
inline bool peer_uid(int fd, uid_t* out) {
#  if defined(__linux__)
    struct ucred cr {};
    socklen_t len = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) return false;
    *out = cr.uid;
    return true;
#  else
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(fd, &uid, &gid) != 0) return false;
    *out = uid;
    return true;
#  endif
}

inline bool fill_sockaddr(const std::string& path, sockaddr_un* addr) {
    std::memset(addr, 0, sizeof *addr);
    addr->sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr->sun_path)) return false;
    std::memcpy(addr->sun_path, path.c_str(), path.size() + 1);
    return true;
}

#endif

// ── Server: the engine's side ─────────────────────────────────────────────

class server {
public:
    server() = default;
    ~server() { stop(); }
    server(const server&) = delete;
    server& operator=(const server&) = delete;

    // Bind the endpoint and serve `segment` (not owned; must outlive the
    // server) of `size` bytes to every same-user connection until stop().
    // False, with `err` filled, when the endpoint cannot be bound.
    bool start(const std::string& endpoint, shm_native_handle segment, uint64_t size,
               std::string* err) {
        stop();
        if (!shm_handle_valid(segment)) { if (err) *err = "no segment to serve"; return false; }
        mEndpoint = endpoint;
        mSegment  = segment;
        mSize     = size;
        mStop.store(false);
#ifdef _WIN32
        mPipeName = pipe_path(endpoint);
        mSd = owner_only_sd();
        if (!mSd) { if (err) *err = "could not build the pipe's security descriptor"; return false; }
        // Claim the name now, so a taken name fails here and not on the thread.
        HANDLE first = make_instance(true);
        if (first == INVALID_HANDLE_VALUE) {
            if (err) *err = "CreateNamedPipe " + endpoint + ": " +
                            clockwork_path::last_error_text(GetLastError());
            LocalFree(mSd); mSd = nullptr;
            return false;
        }
        mThread = std::thread([this, first] { serve_loop(first); });
#else
        sockaddr_un addr {};
        if (!fill_sockaddr(endpoint, &addr)) {
            if (err) *err = "endpoint path too long for a Unix socket: " + endpoint;
            return false;
        }
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { if (err) *err = "socket: " + std::string(std::strerror(errno)); return false; }
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        // A socket already at the path is either a leftover from a crashed
        // engine or a LIVE engine's — and only the first may be replaced. Two
        // engines given one port is not hypothetical: a UDP bind failure is
        // non-fatal (scsynth compatibility), so the second engine keeps
        // running, and unlinking the first's endpoint would silently send its
        // new readers to the wrong segment. A connect tells them apart:
        // answered means owned, refused means dead. Anything that is not a
        // socket is somebody else's and bind fails on it.
        struct stat st {};
        if (::lstat(endpoint.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
            const int probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
            const bool live = probe >= 0
                && ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
            if (probe >= 0) ::close(probe);
            if (live) {
                if (err) *err = "another engine is already serving " + endpoint;
                ::close(fd);
                return false;
            }
            ::unlink(endpoint.c_str());
        }
        const mode_t old = ::umask(0077);
        const int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
        ::umask(old);
        if (bound != 0) {
            if (err) *err = "bind " + endpoint + ": " + std::strerror(errno);
            ::close(fd);
            return false;
        }
        ::chmod(endpoint.c_str(), 0600);
        if (::listen(fd, 8) != 0) {
            if (err) *err = "listen " + endpoint + ": " + std::strerror(errno);
            ::close(fd);
            ::unlink(endpoint.c_str());
            return false;
        }
        mListen = fd;
        mThread = std::thread([this] { serve_loop(); });
#endif
        return true;
    }

    void stop() {
        if (!mThread.joinable()) return;
        mStop.store(true);
#ifdef _WIN32
        // Wake a blocked ConnectNamedPipe by connecting to it ourselves; the
        // loop sees the flag and leaves. A failed connect is fine: the
        // thread was between instances and polls the flag anyway.
        HANDLE h = CreateFileW(mPipeName.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        mThread.join();
        if (mSd) { LocalFree(mSd); mSd = nullptr; }
#else
        mThread.join();
        ::close(mListen);
        mListen = -1;
        ::unlink(mEndpoint.c_str());
#endif
    }

    const std::string& endpoint() const { return mEndpoint; }
    bool running() const { return mThread.joinable(); }

private:
#ifdef _WIN32
    HANDLE make_instance(bool first) {
        SECURITY_ATTRIBUTES sa { sizeof(SECURITY_ATTRIBUTES), mSd, FALSE };
        return CreateNamedPipeW(
            mPipeName.c_str(),
            PIPE_ACCESS_OUTBOUND | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, sizeof(hello), sizeof(hello), 0, &sa);
    }

    void serve_loop(HANDLE instance) {
        while (true) {
            if (instance == INVALID_HANDLE_VALUE) {
                instance = make_instance(false);
                if (instance == INVALID_HANDLE_VALUE) { Sleep(50); if (mStop.load()) break; continue; }
            }
            const BOOL connected = ConnectNamedPipe(instance, nullptr)
                                   || GetLastError() == ERROR_PIPE_CONNECTED;
            if (mStop.load()) { CloseHandle(instance); break; }
            if (connected) serve_one(instance);
            DisconnectNamedPipe(instance);
            CloseHandle(instance);
            instance = INVALID_HANDLE_VALUE;
        }
    }

    void serve_one(HANDLE instance) {
        hello h { MAGIC, VERSION, mSize, 0 };
        ULONG pid = 0;
        if (GetNamedPipeClientProcessId(instance, &pid)) {
            HANDLE proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
            if (proc) {
                HANDLE dup = nullptr;
                if (DuplicateHandle(GetCurrentProcess(), mSegment, proc, &dup, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS))
                    h.handle = reinterpret_cast<uint64_t>(dup);
                CloseHandle(proc);
            }
        }
        DWORD written = 0;
        WriteFile(instance, &h, sizeof h, &written, nullptr);
        FlushFileBuffers(instance);
    }

    std::wstring         mPipeName;
    PSECURITY_DESCRIPTOR mSd = nullptr;
#else
    void serve_loop() {
        while (!mStop.load()) {
            pollfd p { mListen, POLLIN, 0 };
            const int n = ::poll(&p, 1, 200);
            if (n <= 0) continue;
            const int conn = ::accept(mListen, nullptr, nullptr);
            if (conn < 0) continue;
            ::fcntl(conn, F_SETFD, FD_CLOEXEC);
            // A client that connects and leaves before reading (a probe, a
            // crash) must not raise SIGPIPE in the ENGINE: the write below
            // reports EPIPE instead and the loop moves on.
#if defined(SO_NOSIGPIPE)
            const int one = 1;
            ::setsockopt(conn, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            serve_one(conn);
            ::close(conn);
        }
    }

    void serve_one(int conn) {
        uid_t uid = static_cast<uid_t>(-1);
        if (!peer_uid(conn, &uid) || uid != ::geteuid()) return;   // not ours: nothing

        hello h { MAGIC, VERSION, mSize, 0 };
        iovec iov { &h, sizeof h };
        alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int))];
        std::memset(ctl, 0, sizeof ctl);
        msghdr msg {};
        msg.msg_iov        = &iov;
        msg.msg_iovlen     = 1;
        msg.msg_control    = ctl;
        msg.msg_controllen = sizeof ctl;
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type  = SCM_RIGHTS;
        c->cmsg_len   = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &mSegment, sizeof(int));
#if defined(MSG_NOSIGNAL)
        (void)::sendmsg(conn, &msg, MSG_NOSIGNAL);
#else
        (void)::sendmsg(conn, &msg, 0);
#endif
    }

    int mListen = -1;
#endif
    std::string        mEndpoint;
    shm_native_handle  mSegment = shm_invalid_handle;
    uint64_t           mSize = 0;
    std::atomic<bool>  mStop { false };
    std::thread        mThread;
};

// ── Client: the reader's side ─────────────────────────────────────────────

// Connect to `endpoint` and receive the segment. The returned handle is owned
// by the caller (hand it to shm_segment_client); invalid, with `err` filled,
// when there is no engine there, it is not ours, or the hand-off was
// malformed. `size_out`, if given, receives the length the server declared.
inline shm_native_handle receive(const std::string& endpoint, std::string* err,
                                 uint64_t* size_out = nullptr) {
#ifdef _WIN32
    const std::wstring name = pipe_path(endpoint);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(name.c_str(), 1000);   // every instance is mid-hand-off; wait for one
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        if (err) *err = "open " + endpoint + ": " + clockwork_path::last_error_text(GetLastError());
        return shm_invalid_handle;
    }
    hello h {};
    DWORD got = 0;
    const BOOL ok = ReadFile(pipe, &h, sizeof h, &got, nullptr);
    CloseHandle(pipe);
    if (!ok || got != sizeof h || h.magic != MAGIC || h.version != VERSION) {
        if (err) *err = "malformed hand-off from " + endpoint;
        return shm_invalid_handle;
    }
    if (h.handle == 0) {
        if (err) *err = "the engine could not duplicate its segment into this process";
        return shm_invalid_handle;
    }
    if (size_out) *size_out = h.size;
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h.handle));
#else
    sockaddr_un addr {};
    if (!fill_sockaddr(endpoint, &addr)) {
        if (err) *err = "endpoint path too long for a Unix socket: " + endpoint;
        return shm_invalid_handle;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { if (err) *err = "socket: " + std::string(std::strerror(errno)); return -1; }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err) *err = "connect " + endpoint + ": " + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    uid_t uid = static_cast<uid_t>(-1);
    if (!peer_uid(fd, &uid) || uid != ::geteuid()) {
        if (err) *err = "the engine at " + endpoint + " is not running as this user";
        ::close(fd);
        return -1;
    }
    // Bound the wait: a server that accepted but never speaks must not hang a
    // GUI's attach loop.
    timeval tv { 2, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    hello h {};
    iovec iov { &h, sizeof h };
    alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int))];
    std::memset(ctl, 0, sizeof ctl);
    msghdr msg {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctl;
    msg.msg_controllen = sizeof ctl;
    const ssize_t got = ::recvmsg(fd, &msg, 0);
    ::close(fd);

    int received = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS
            && c->cmsg_len >= CMSG_LEN(sizeof(int)))
            std::memcpy(&received, CMSG_DATA(c), sizeof(int));
    }
    if (got != static_cast<ssize_t>(sizeof h) || (msg.msg_flags & MSG_CTRUNC)
        || h.magic != MAGIC || h.version != VERSION || received < 0) {
        if (received >= 0) ::close(received);
        if (err) *err = "malformed hand-off from " + endpoint;
        return -1;
    }
    ::fcntl(received, F_SETFD, FD_CLOEXEC);
    if (size_out) *size_out = h.size;
    return received;
#endif
}

} // namespace shm_attach
