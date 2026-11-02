// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
//  The attach endpoint: how a process other than the engine gets the
//  engine's anonymous shared-memory segments (docs/SEGMENTS.md).
//
//  A segment has no name (shm_segment.hpp), so a client cannot open it; it
//  has to be GIVEN it. That needs a channel that can carry an OS object, and
//  only a local one can: a Unix-domain socket passes a file descriptor as
//  ancillary data (SCM_RIGHTS); a Windows named pipe tells the server the
//  client's pid, and the server duplicates its handle into that process. TCP
//  and UDP cannot do either, which is why this is its own small endpoint
//  rather than a message on the command transport — a host that speaks TCP to
//  its engine still needs its GUI to reach the published segment.
//
//  PROTOCOL. Connect; the client sends the secret's auth frame (a
//  4-byte big-endian length, then /clockwork/auth with the 32 bytes as a
//  blob, the same first frame the stream transports ask for), then one
//  4-byte big-endian ROLE word: 1 a reader, 2 a peer. The server answers
//  with one `hello` (magic, version, the segment's size, on Windows the
//  duplicated handle value, the role, and a status):
//
//    a wrong or missing auth frame, or no role within AUTH_DEADLINE: closed
//      without a hello and counted (refused()) — a probe learns nothing;
//    a reader: the PUBLISHED segment's read-only descriptor rides alongside
//      the hello, then the connection closes;
//    a peer, where the server has no command segment: status 1, no
//      descriptor; held by another peer: status 2; an unknown role: 3 — each
//      counted (declined()) and closed;
//    a peer, otherwise: the COMMAND segment's read-write descriptor, and the
//      connection STAYS OPEN: it is the lease. Closing it, on purpose or by
//      dying, releases the segment for the next peer.
//
//  TRUST. Attaching is a command path (a peer gains control of the engine)
//  and asks for the per-boot secret (clockwork_comms_secret.h). The endpoint
//  also lives in a per-user directory (XDG_RUNTIME_DIR or TMPDIR, both 0700)
//  with a 0600 socket, and BOTH ends check the peer's uid: the server hands
//  memory only to its own user, and a client refuses a server run by anyone
//  else, so a planted path yields nothing. On Windows the pipe carries an
//  owner-only DACL and rejects remote clients.
//
//  The default endpoint is derived from the engine's port so that a launcher
//  and a reader that only share the port still meet; --shm-endpoint names any
//  other path or pipe.

#pragma once

#include "shm_segment.hpp"
#include "clockwork_comms_secret.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <array>
#include <optional>

#include "process_env.h"
#include "workers/thread_name.h"
#include <thread>
#include <stop_token>

#ifdef _WIN32
#  include <sddl.h>
#  ifdef _MSC_VER
#    pragma comment(lib, "advapi32.lib")
#  endif
#else
#  include <poll.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <sys/types.h>
#endif

namespace shm_attach {

using detail_shm_segment::shm_native_handle;
using detail_shm_segment::shm_invalid_handle;
using detail_shm_segment::shm_handle_valid;

// The roles a client asks for, and the statuses a hello carries.
enum : uint32_t { ROLE_READER = 1, ROLE_PEER = 2 };
enum : uint32_t {
    STATUS_OK                 = 0,
    STATUS_NO_COMMAND_SEGMENT = 1,   // the host did not enable the command plane
    STATUS_HELD               = 2,   // another peer holds it
    STATUS_UNKNOWN_ROLE       = 3,
};

// What the server sends. Fixed layout, native byte order: both ends are on
// one machine.
struct hello {
    uint32_t magic;
    uint32_t version;
    uint64_t size;      // the segment's byte length; 0 when nothing is handed over
    uint64_t handle;    // Windows: the HANDLE value in the receiver's table;
                        // POSIX: 0, the descriptor travels as ancillary data
    uint32_t role;      // the role granted, as asked
    uint32_t status;    // STATUS_*
    uint64_t wake_event;  // a peer on Windows: the event that wakes the engine's
                          // control pass, duplicated into the receiver (it may
                          // only signal it); 0 elsewhere, where the wake word in
                          // the command segment is woken directly
};
static constexpr uint32_t MAGIC   = 0x43574154;  // "CWAT"
static constexpr uint32_t VERSION = 4;           // the hello's layout; a client built for another refuses it
static_assert(sizeof(hello) == 40, "hello is a wire format");

// How long the server waits for a client's auth frame and role.
static constexpr std::chrono::seconds AUTH_DEADLINE { 2 };

// What the client sends first: the secret's auth packet, framed. The packet
// is "/clockwork/auth" then ",b" then the blob, each OSC-padded to four
// bytes: 16 + 4 + 4 + 32 = 56 bytes, after a 4-byte big-endian length.
// clockwork_comms_secret_auth_packet encodes the same bytes; a test holds
// the two together. Then the role, a 4-byte big-endian word.
static constexpr uint32_t AUTH_PACKET_LEN = 56;
static constexpr uint32_t AUTH_FRAME_LEN  = 4 + AUTH_PACKET_LEN;
static constexpr uint32_t ROLE_LEN        = 4;
static constexpr uint32_t REQUEST_LEN     = AUTH_FRAME_LEN + ROLE_LEN;
inline std::array<uint8_t, AUTH_FRAME_LEN> auth_frame(const uint8_t* secret32) {
    std::array<uint8_t, AUTH_FRAME_LEN> f {};
    f[3] = AUTH_PACKET_LEN;
    static constexpr char kAddr[16] = "/clockwork/auth";   // 15 chars + NUL
    std::memcpy(f.data() + 4, kAddr, 16);
    std::memcpy(f.data() + 20, ",b\0\0", 4);
    f[27] = 32;                                            // blob length, big-endian
    std::memcpy(f.data() + 28, secret32, 32);
    return f;
}
inline std::array<uint8_t, REQUEST_LEN> request(const uint8_t* secret32, uint32_t role) {
    std::array<uint8_t, REQUEST_LEN> r {};
    const auto f = auth_frame(secret32);
    std::memcpy(r.data(), f.data(), f.size());
    r[AUTH_FRAME_LEN + 0] = static_cast<uint8_t>(role >> 24);
    r[AUTH_FRAME_LEN + 1] = static_cast<uint8_t>(role >> 16);
    r[AUTH_FRAME_LEN + 2] = static_cast<uint8_t>(role >> 8);
    r[AUTH_FRAME_LEN + 3] = static_cast<uint8_t>(role);
    return r;
}

// Reads a request, which any process of the user's may have sent. Returns
// the role it asks for if its frame has an auth packet's length and carries
// `secret`, or nothing. Fuzzed by test/fuzz/fuzz_attach.cpp.
inline std::optional<uint32_t> admitted_role(const std::array<uint8_t, REQUEST_LEN>& req,
                                             const ClockworkCommsSecret* secret) {
    const uint32_t len = (uint32_t(req[0]) << 24) | (uint32_t(req[1]) << 16)
                       | (uint32_t(req[2]) << 8) | uint32_t(req[3]);
    if (len != AUTH_PACKET_LEN || clockwork_comms_secret_admits(secret, req.data() + 4, AUTH_PACKET_LEN) != 1)
        return std::nullopt;
    return (uint32_t(req[AUTH_FRAME_LEN]) << 24) | (uint32_t(req[AUTH_FRAME_LEN + 1]) << 16)
         | (uint32_t(req[AUTH_FRAME_LEN + 2]) << 8) | uint32_t(req[AUTH_FRAME_LEN + 3]);
}

// The client's check of a hello: true if it is a hand-off this build
// understands, for the role it asked. Fuzzed with admitted_role.
inline bool hello_matches(const hello& h, uint32_t role) {
    return h.magic == MAGIC && h.version == VERSION && h.role == role;
}

// errno's text, from the standard library's thread-safe table: strerror's is not.
inline std::string errnoText() { return std::system_category().message(errno); }

// The endpoint an engine on `port` uses unless told otherwise. A per-user
// location: on Linux the session runtime dir, on macOS the per-user TMPDIR,
// and a uid-tagged /tmp path only where neither is set.
inline std::string default_endpoint(unsigned port) {
#ifdef _WIN32
    return "\\\\.\\pipe\\clockwork-shm-" + std::to_string(port);
#else
    auto dir_from = [](const char* var) -> std::string {
        const char* v = clockwork::processEnv(var);
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

// The reason a status is, for a client's error text.
inline std::string status_text(uint32_t status, const std::string& endpoint) {
    switch (status) {
        case STATUS_NO_COMMAND_SEGMENT:
            return "no shared-memory command plane at " + endpoint + ": the host did not enable one";
        case STATUS_HELD:
            return "another peer holds the command plane at " + endpoint;
        case STATUS_UNKNOWN_ROLE:
            return "the engine at " + endpoint + " does not know the role asked for";
        default:
            return "the engine at " + endpoint + " answered with status " + std::to_string(status);
    }
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

    // Bind the endpoint and serve, to every same-user connection that
    // presents `secret` (copied; the caller's may go), the published segment
    // `reader` (its READ-ONLY handle, `reader_size` bytes) to readers and,
    // when `command` is valid, the command segment (`command_size` bytes) to
    // one peer at a time, with `command_wake_event` (Windows: the event that
    // wakes the engine's control pass, duplicated into the peer; null
    // elsewhere). No handle is owned; all must outlive the server. False,
    // with `err` filled, when there is no published segment, no secret, or
    // the endpoint cannot be bound.
    bool start(const std::string& endpoint,
               shm_native_handle reader, uint64_t reader_size,
               shm_native_handle command, uint64_t command_size,
               clockwork::WaitEvent command_wake_event,
               const ClockworkCommsSecret* secret, std::string* err) {
        stop();
        if (!shm_handle_valid(reader)) { if (err) *err = "no published segment to serve"; return false; }
        if (!secret) { if (err) *err = "no secret: the attach endpoint asks every client for it"; return false; }
        mSecret      = clockwork_comms_secret_clone(secret);
        mEndpoint    = endpoint;
        mReader      = reader;
        mReaderSize  = reader_size;
        mCommand     = command;
        mCommandSize = shm_handle_valid(command) ? command_size : 0;
        mCommandWakeEvent = command_wake_event;
        mHolding.store(false, std::memory_order_relaxed);
#ifdef _WIN32
        mPipeName = pipe_path(endpoint);
        mSd = owner_only_sd();
        if (!mSd) { if (err) *err = "could not build the pipe's security descriptor"; return false; }
        mStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        mIo   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // Claim the name now, so a taken name fails here and not on the thread.
        HANDLE first = mStop && mIo ? make_instance(true) : INVALID_HANDLE_VALUE;
        if (first == INVALID_HANDLE_VALUE) {
            if (err) *err = "CreateNamedPipe " + endpoint + ": " +
                            clockwork_path::last_error_text(GetLastError());
            LocalFree(mSd); mSd = nullptr;
            close_events();
            return false;
        }
        mThread = std::jthread([this, first](const std::stop_token& stop) { serve_loop(first, stop); });
#else
        sockaddr_un addr {};
        if (!fill_sockaddr(endpoint, &addr)) {
            if (err) *err = "endpoint path too long for a Unix socket: " + endpoint;
            return false;
        }
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { if (err) *err = "socket: " + errnoText(); return false; }
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
            if (err) *err = "bind " + endpoint + ": " + errnoText();
            ::close(fd);
            return false;
        }
        ::chmod(endpoint.c_str(), 0600);
        if (::listen(fd, 8) != 0) {
            if (err) *err = "listen " + endpoint + ": " + errnoText();
            ::close(fd);
            ::unlink(endpoint.c_str());
            return false;
        }
        if (::pipe(mWake) != 0) {
            if (err) *err = "pipe: " + errnoText();
            ::close(fd);
            ::unlink(endpoint.c_str());
            return false;
        }
        for (const int w : mWake) ::fcntl(w, F_SETFD, FD_CLOEXEC);
        ::fcntl(mWake[1], F_SETFL, O_NONBLOCK);   // a full pipe is already a wake
        mListen = fd;
        mThread = std::jthread([this](const std::stop_token& stop) { serve_loop(stop); });
#endif
        return true;
    }

    void stop() {
        if (!mThread.joinable()) return;
        // request_stop() wakes the loop through its own stop_callback (on
        // Windows the stop event; on POSIX the wake pipe), so the
        // destructor's join needs nothing more than this.
        mThread.request_stop();
#ifdef _WIN32
        mThread.join();
        release_lease();
        if (mSd) { LocalFree(mSd); mSd = nullptr; }
        close_events();
#else
        mThread.join();
        release_lease();
        ::close(mListen);
        mListen = -1;
        for (int& w : mWake) { ::close(w); w = -1; }
        ::unlink(mEndpoint.c_str());
#endif
        clockwork_comms_secret_free(mSecret);
        mSecret = nullptr;
    }

    const std::string& endpoint() const { return mEndpoint; }
    bool running() const { return mThread.joinable(); }
    // Connections closed for not presenting the secret (and a role) in time.
    uint64_t refused() const { return mRefused.load(std::memory_order_relaxed); }
    // Connections with the secret that asked for what they could not have:
    // the command plane where there is none or while held, an unknown role.
    uint64_t declined() const { return mDeclined.load(std::memory_order_relaxed); }
    // Whether a peer holds the command segment right now.
    bool holding() const { return mHolding.load(std::memory_order_acquire); }

private:
    // What serve_one decided about the connection after the hello.
    enum class after { close, keep_as_lease };

    // The hello for a request: which segment (if any) goes with it, and
    // whether the connection is kept as the lease. Counts what it declines.
    hello answer(uint32_t role, shm_native_handle* segment, after* then) {
        hello h { MAGIC, VERSION, 0, 0, role, STATUS_OK, 0 };
        *segment = shm_invalid_handle;
        *then    = after::close;
        switch (role) {
            case ROLE_READER:
                h.size   = mReaderSize;
                *segment = mReader;
                return h;
            case ROLE_PEER:
                if (!shm_handle_valid(mCommand)) {
                    h.status = STATUS_NO_COMMAND_SEGMENT;
                } else if (mHolding.load(std::memory_order_acquire)) {
                    h.status = STATUS_HELD;
                } else {
                    h.size   = mCommandSize;
                    *segment = mCommand;
                    *then    = after::keep_as_lease;
                    return h;
                }
                break;
            default:
                h.status = STATUS_UNKNOWN_ROLE;
                break;
        }
        mDeclined.fetch_add(1, std::memory_order_relaxed);
        return h;
    }

#ifdef _WIN32
    // Instances are overlapped, so every wait on one (for a client, its
    // request, the hello going out) also waits on the stop's event, with a
    // deadline wherever the client could stall it.
    HANDLE make_instance(bool first) {
        SECURITY_ATTRIBUTES sa { sizeof(SECURITY_ATTRIBUTES), mSd, FALSE };
        return CreateNamedPipeW(
            mPipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, sizeof(hello), REQUEST_LEN, 0, &sa);
    }

    // Finishes an overlapped operation just started on `pipe` (`started` is
    // what the call returned): waits for it to complete, the stop, or
    // `deadline` (none: no deadline). Returns true, with `*n` set, if it
    // completed well. Otherwise cancels it and waits for the cancel, so `ov`
    // is free again.
    //
    // A timeout is believed only once steady_clock agrees. Windows' timed
    // waits run on the scheduler's tick and can come back a little before
    // the deadline they were given, which closed a silent client a fraction
    // of a millisecond inside AUTH_DEADLINE; the POSIX server re-reads the
    // clock the same way.
    bool finish_io(HANDLE pipe, OVERLAPPED& ov, BOOL started,
                   std::optional<std::chrono::steady_clock::time_point> deadline, DWORD* n) {
        if (!started && GetLastError() != ERROR_IO_PENDING) return false;
        HANDLE waits[2] = { ov.hEvent, mStop };
        for (;;) {
            const DWORD r = WaitForMultipleObjects(2, waits, FALSE, deadline ? ms_until(*deadline) : INFINITE);
            if (r == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &ov, n, FALSE) != 0;
            if (r == WAIT_TIMEOUT && deadline && std::chrono::steady_clock::now() < *deadline) continue;
            break;
        }
        CancelIoEx(pipe, &ov);
        GetOverlappedResult(pipe, &ov, n, TRUE);
        return false;
    }

    // Waits until a client connects to `instance`, or the stop. Returns true
    // if one connected.
    bool await_client(HANDLE instance) {
        OVERLAPPED ov {};
        ov.hEvent = mIo;
        const BOOL started = ConnectNamedPipe(instance, &ov);
        if (!started && GetLastError() == ERROR_PIPE_CONNECTED) return true;   // came before the call
        DWORD n = 0;
        return finish_io(instance, ov, started, std::nullopt, &n);
    }

    static DWORD ms_until(std::chrono::steady_clock::time_point deadline) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        return left.count() > 0 ? static_cast<DWORD>(left.count()) : 0;
    }

    void serve_loop(HANDLE instance, const std::stop_token& stop) {
        clockwork::name_this_thread("cw-attach");
        const std::stop_callback wake(stop, [this] { SetEvent(mStop); });
        while (!stop.stop_requested()) {
            if (instance == INVALID_HANDLE_VALUE) {
                instance = make_instance(false);
                // Out of instances or memory: try again shortly, unless stopped.
                if (instance == INVALID_HANDLE_VALUE) { WaitForSingleObject(mStop, 50); continue; }
            }
            if (!await_client(instance)) {   // stopped, or a client gone before it was served
                DisconnectNamedPipe(instance);
                CloseHandle(instance);
                instance = INVALID_HANDLE_VALUE;
                continue;
            }
            // The next instance BEFORE this one is served, so the name always
            // has an instance behind it. A second reader connecting meanwhile
            // finds the next instance, or for the moment before this one is
            // closed a busy one, which it waits on; with no instance it would
            // be told the pipe does not exist, which a client takes as final.
            HANDLE next = make_instance(false);
            if (serve_one(instance) == after::keep_as_lease) {
                // The peer's connection is the lease: watched until it goes.
                hold_lease(instance);
            } else {
                DisconnectNamedPipe(instance);
                CloseHandle(instance);
            }
            instance = next;
        }
        if (instance != INVALID_HANDLE_VALUE) CloseHandle(instance);
    }

    // Reads the client's request, however many reads it arrives in. Returns
    // false if it is not all there within AUTH_DEADLINE, or on the stop.
    bool read_request(HANDLE instance, std::array<uint8_t, REQUEST_LEN>* req) {
        const auto deadline = std::chrono::steady_clock::now() + AUTH_DEADLINE;
        DWORD got = 0;
        while (got < REQUEST_LEN) {
            OVERLAPPED ov {};
            ov.hEvent = mIo;
            DWORD n = 0;
            const BOOL started = ReadFile(instance, req->data() + got, REQUEST_LEN - got, nullptr, &ov);
            if (!finish_io(instance, ov, started, deadline, &n) || n == 0) return false;
            got += n;
        }
        return true;
    }

    after serve_one(HANDLE instance) {
        std::array<uint8_t, REQUEST_LEN> req {};
        std::optional<uint32_t> role;
        if (!read_request(instance, &req) || !(role = admitted_role(req, mSecret))) {
            mRefused.fetch_add(1, std::memory_order_relaxed);
            return after::close;
        }
        shm_native_handle segment = shm_invalid_handle;
        after then = after::close;
        hello h = answer(*role, &segment, &then);
        // Claimed before the hello goes, as on POSIX; given back below if
        // nothing is handed over.
        if (then == after::keep_as_lease) mHolding.store(true, std::memory_order_release);
        if (shm_handle_valid(segment)) {
            ULONG pid = 0;
            if (GetNamedPipeClientProcessId(instance, &pid)) {
                HANDLE proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
                if (proc) {
                    HANDLE dup = nullptr;
                    // The reader handle carries FILE_MAP_READ and nothing
                    // more, so its duplicate is read-only too.
                    if (DuplicateHandle(GetCurrentProcess(), segment, proc, &dup, 0, FALSE,
                                        DUPLICATE_SAME_ACCESS))
                        h.handle = reinterpret_cast<uint64_t>(dup);
                    // The peer wakes the control pass through this; it may
                    // signal it and nothing more.
                    HANDLE wake = nullptr;
                    if (then == after::keep_as_lease && mCommandWakeEvent
                        && DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(mCommandWakeEvent),
                                           proc, &wake, EVENT_MODIFY_STATE, FALSE, 0))
                        h.wake_event = reinterpret_cast<uint64_t>(wake);
                    CloseHandle(proc);
                }
            }
            if (h.handle == 0 && then == after::keep_as_lease) {   // nothing handed over: no lease
                then = after::close;
                mHolding.store(false, std::memory_order_release);
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + AUTH_DEADLINE;
        OVERLAPPED ov {};
        ov.hEvent = mIo;
        DWORD written = 0;
        const BOOL started = WriteFile(instance, &h, sizeof h, nullptr, &ov);
        const bool wrote = finish_io(instance, ov, started, deadline, &written) && written == sizeof h;
        if (!wrote && then == after::keep_as_lease) {   // the peer went: no lease
            mHolding.store(false, std::memory_order_release);
            return after::close;
        }
        if (wrote && then == after::close) await_close(instance, deadline);
        return then;
    }

    // DisconnectNamedPipe discards data the client has not read. So after
    // the hello on an instance that is not the lease, wait until the client
    // closes its end (it does once it has read the hello), the deadline
    // passes, or the server stops.
    void await_close(HANDLE instance, std::chrono::steady_clock::time_point deadline) {
        uint8_t byte = 0;
        for (;;) {
            OVERLAPPED ov {};
            ov.hEvent = mIo;
            DWORD n = 0;
            // The client's close ends the read with an error; a byte it sends
            // is ignored.
            const BOOL started = ReadFile(instance, &byte, 1, nullptr, &ov);
            if (!finish_io(instance, ov, started, deadline, &n) || n == 0) return;
        }
    }

    // Keep the peer's pipe instance as the lease, and watch it from a thread
    // of its own: its read ends when the peer closes or dies, or when
    // release_lease disconnects it. The hold itself was claimed before the
    // hello went.
    void hold_lease(HANDLE instance) {
        if (mLease != INVALID_HANDLE_VALUE) {
            if (mLeaseWatch.joinable()) mLeaseWatch.join();
            CloseHandle(mLease);
        }
        mLease = instance;
        mLeaseWatch = std::jthread([this, instance] {
            OVERLAPPED ov {};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // without one, the instance is waited on
            uint8_t byte = 0;
            DWORD got = 0;
            while ((ReadFile(instance, &byte, 1, nullptr, &ov) || GetLastError() == ERROR_IO_PENDING)
                   && GetOverlappedResult(instance, &ov, &got, TRUE) && got > 0) {}   // the peer sends nothing; anything is ignored
            if (ov.hEvent) CloseHandle(ov.hEvent);
            mHolding.store(false, std::memory_order_release);
        });
    }
    void release_lease() {
        if (mLease != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(mLease);   // unblocks the watcher's ReadFile
            if (mLeaseWatch.joinable()) mLeaseWatch.join();
            CloseHandle(mLease);
            mLease = INVALID_HANDLE_VALUE;
        }
        mHolding.store(false, std::memory_order_release);
    }

    void close_events() {
        for (HANDLE* e : { &mStop, &mIo }) {
            if (*e) CloseHandle(*e);
            *e = nullptr;
        }
    }

    std::wstring         mPipeName;
    PSECURITY_DESCRIPTOR mSd = nullptr;
    HANDLE               mLease = INVALID_HANDLE_VALUE;
    std::jthread         mLeaseWatch;
    HANDLE               mStop = nullptr;   // manual reset: set by the stop
    HANDLE               mIo = nullptr;     // the serve thread's overlapped operations
#else
    void serve_loop(const std::stop_token& stop) {
        clockwork::name_this_thread("cw-attach");
        // The stop writes a byte to the wake pipe, which the poll watches
        // beside the listening socket and the lease.
        const std::stop_callback wake(stop, [this] {
            const uint8_t byte = 1;
            while (::write(mWake[1], &byte, 1) < 0 && errno == EINTR) {}
        });
        while (!stop.stop_requested()) {
            pollfd fds[3] = { { mListen, POLLIN, 0 }, { mWake[0], POLLIN, 0 }, { mLease, POLLIN, 0 } };
            const nfds_t n = mLease >= 0 ? 3 : 2;
            if (::poll(fds, n, -1) <= 0) continue;
            if (n == 3 && fds[2].revents != 0) {
                // The lease: the peer sends nothing, so anything readable is
                // either its end (the release) or bytes to ignore.
                uint8_t byte = 0;
                const ssize_t got = ::recv(mLease, &byte, 1, MSG_DONTWAIT);
                if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    || (fds[2].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
                    release_lease();
            }
            if ((fds[0].revents & POLLIN) == 0) continue;
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
            if (serve_one(conn) == after::keep_as_lease) {
                mLease = conn;   // the hold itself was claimed before the hello went
            } else {
                ::close(conn);
            }
        }
    }

    // Reads the client's request, however many reads it arrives in. Returns
    // false if it is not all there within AUTH_DEADLINE, or on the stop.
    bool read_request(int conn, std::array<uint8_t, REQUEST_LEN>* req) {
        const auto deadline = std::chrono::steady_clock::now() + AUTH_DEADLINE;
        size_t got = 0;
        while (got < req->size()) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) return false;
            pollfd fds[2] = { { conn, POLLIN, 0 }, { mWake[0], POLLIN, 0 } };
            if (::poll(fds, 2, static_cast<int>(left.count())) < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (fds[1].revents != 0) return false;   // stopping
            if (fds[0].revents == 0) continue;
            const ssize_t n = ::recv(conn, req->data() + got, req->size() - got, MSG_DONTWAIT);
            if (n == 0) return false;   // closed
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                return false;
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }

    after serve_one(int conn) {
        uid_t uid = static_cast<uid_t>(-1);
        if (!peer_uid(conn, &uid) || uid != ::geteuid()) return after::close;   // not ours: nothing
        std::array<uint8_t, REQUEST_LEN> req {};
        std::optional<uint32_t> role;
        if (!read_request(conn, &req) || !(role = admitted_role(req, mSecret))) {
            mRefused.fetch_add(1, std::memory_order_relaxed);
            return after::close;
        }
        shm_native_handle segment = shm_invalid_handle;
        after then = after::close;
        const hello h = answer(*role, &segment, &then);
        // The hold is claimed BEFORE the hello goes: a peer that has its
        // segment must already be the holder, or a second peer could be let
        // in between the hand-off and the claim.
        if (then == after::keep_as_lease) mHolding.store(true, std::memory_order_release);

        iovec iov { const_cast<hello*>(&h), sizeof h };
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctl{};
        msghdr msg {};
        msg.msg_iov    = &iov;
        msg.msg_iovlen = 1;
        if (shm_handle_valid(segment)) {
            msg.msg_control    = ctl.data();
            msg.msg_controllen = ctl.size();
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type  = SCM_RIGHTS;
            c->cmsg_len   = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(c), &segment, sizeof(int));
        }
#if defined(MSG_NOSIGNAL)
        const ssize_t sent = ::sendmsg(conn, &msg, MSG_NOSIGNAL);
#else
        const ssize_t sent = ::sendmsg(conn, &msg, 0);
#endif
        if (sent != static_cast<ssize_t>(sizeof h)) {   // the peer went: no lease
            if (then == after::keep_as_lease) mHolding.store(false, std::memory_order_release);
            return after::close;
        }
        return then;
    }

    void release_lease() {
        if (mLease >= 0) ::close(mLease);
        mLease = -1;
        mHolding.store(false, std::memory_order_release);
    }

    int mListen = -1;
    int mLease  = -1;
    int mWake[2] = { -1, -1 };   // the stop's wake: a byte in [1] wakes the poll on [0]
#endif
    std::string            mEndpoint;
    shm_native_handle      mReader  = shm_invalid_handle;
    uint64_t               mReaderSize = 0;
    shm_native_handle      mCommand = shm_invalid_handle;
    uint64_t               mCommandSize = 0;
    clockwork::WaitEvent   mCommandWakeEvent = nullptr;
    ClockworkCommsSecret*  mSecret = nullptr;
    std::atomic<uint64_t>  mRefused { 0 };
    std::atomic<uint64_t>  mDeclined { 0 };
    std::atomic<bool>      mHolding { false };
    std::jthread           mThread;   // joins in its destructor; stop() is request_stop + join
};

// ── Client: the reader's and the peer's side ──────────────────────────────

// A peer's hold on the command segment: the segment's handle (the caller's
// to own — hand it to shm_command_client) and the attach connection, kept
// open for as long as the hold lasts. Destroying or releasing it is the
// release, as is the process's death.
struct command_lease {
    shm_native_handle handle = shm_invalid_handle;
    // Windows: the event that wakes the engine's control pass (the caller's to
    // own — hand it to shm_command_client with the handle). Null elsewhere.
    clockwork::WaitEvent wake_event = nullptr;
#ifdef _WIN32
    HANDLE connection = INVALID_HANDLE_VALUE;
#else
    int connection = -1;
#endif
    command_lease() = default;
    command_lease(const command_lease&) = delete;
    command_lease& operator=(const command_lease&) = delete;
    command_lease(command_lease&& o) noexcept { *this = std::move(o); }
    command_lease& operator=(command_lease&& o) noexcept {
        if (this != &o) {
            release();
            handle = o.handle;
            wake_event = o.wake_event;
            connection = o.connection;
            o.handle = shm_invalid_handle;
            o.wake_event = nullptr;
#ifdef _WIN32
            o.connection = INVALID_HANDLE_VALUE;
#else
            o.connection = -1;
#endif
        }
        return *this;
    }
    ~command_lease() { release(); }

    // Whether this holds the command segment. The handle itself may already
    // have been handed to a shm_command_client; the hold is the connection.
    bool valid() const {
#ifdef _WIN32
        return connection != INVALID_HANDLE_VALUE;
#else
        return connection >= 0;
#endif
    }
    // Give the command segment up. The handle, if still here, is closed too.
    void release() {
#ifdef _WIN32
        if (connection != INVALID_HANDLE_VALUE) CloseHandle(connection);
        connection = INVALID_HANDLE_VALUE;
#else
        if (connection >= 0) ::close(connection);
        connection = -1;
#endif
        detail_shm_segment::shm_close_native(handle);
        handle = shm_invalid_handle;
#ifdef _WIN32
        if (wake_event) CloseHandle(static_cast<HANDLE>(wake_event));
#endif
        wake_event = nullptr;
    }
};

namespace detail {

// Connect to `endpoint`, present the secret and the role, and take the
// hello. The handle returned is the caller's; `connection` is the attach
// connection, left open and returned only for a peer (the lease), closed
// otherwise. Invalid, with `err` filled, when there is no engine there, it
// is not ours, the secret was refused, the role was declined, or the
// hand-off was malformed.
#ifdef _WIN32
inline shm_native_handle attach(const std::string& endpoint, const uint8_t* secret32, uint32_t role,
                                std::string* err, uint64_t* size_out, HANDLE* connection,
                                clockwork::WaitEvent* wake_event) {
    *connection = INVALID_HANDLE_VALUE;
    *wake_event = nullptr;
    if (!secret32) { if (err) *err = "no secret to present"; return shm_invalid_handle; }
    const auto req = request(secret32, role);
    const std::wstring name = pipe_path(endpoint);
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(name.c_str(), 1000);   // every instance is mid-hand-off; wait for one
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        if (err) *err = "open " + endpoint + ": " + clockwork_path::last_error_text(GetLastError());
        return shm_invalid_handle;
    }
    DWORD written = 0;
    if (!WriteFile(pipe, req.data(), REQUEST_LEN, &written, nullptr) || written != REQUEST_LEN) {
        if (err) *err = "could not present the secret to " + endpoint;
        CloseHandle(pipe);
        return shm_invalid_handle;
    }
    hello h {};
    DWORD got = 0;
    const BOOL ok = ReadFile(pipe, &h, sizeof h, &got, nullptr);
    if (!ok || got == 0) {
        if (err) *err = "refused by " + endpoint + ": the secret was not accepted";
        CloseHandle(pipe);
        return shm_invalid_handle;
    }
    if (got != sizeof h || !hello_matches(h, role)) {
        if (err) *err = "malformed hand-off from " + endpoint;
        CloseHandle(pipe);
        return shm_invalid_handle;
    }
    if (h.status != STATUS_OK) {
        if (err) *err = status_text(h.status, endpoint);
        CloseHandle(pipe);
        return shm_invalid_handle;
    }
    if (h.handle == 0) {
        if (err) *err = "the engine could not duplicate its segment into this process";
        CloseHandle(pipe);
        return shm_invalid_handle;
    }
    if (size_out) *size_out = h.size;
    if (role == ROLE_PEER) {
        *connection = pipe;   // the lease
        *wake_event = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h.wake_event));
    } else {
        CloseHandle(pipe);
    }
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(h.handle));
}
#else
inline shm_native_handle attach(const std::string& endpoint, const uint8_t* secret32, uint32_t role,
                                std::string* err, uint64_t* size_out, int* connection,
                                clockwork::WaitEvent* wake_event) {
    *connection = -1;
    *wake_event = nullptr;   // the wake word in the command segment is woken directly
    if (!secret32) { if (err) *err = "no secret to present"; return shm_invalid_handle; }
    const auto req = request(secret32, role);
    sockaddr_un addr {};
    if (!fill_sockaddr(endpoint, &addr)) {
        if (err) *err = "endpoint path too long for a Unix socket: " + endpoint;
        return shm_invalid_handle;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { if (err) *err = "socket: " + errnoText(); return -1; }
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err) *err = "connect " + endpoint + ": " + errnoText();
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

    // The secret and the role first; a server that refuses the secret closes
    // without a hello.
    size_t sent = 0;
    while (sent < req.size()) {
#if defined(MSG_NOSIGNAL)
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(fd, req.data() + sent, req.size() - sent, 0);
#endif
        if (n <= 0) {
            if (err) *err = "could not present the secret to " + endpoint + ": " + errnoText();
            ::close(fd);
            return -1;
        }
        sent += static_cast<size_t>(n);
    }

    hello h {};
    iovec iov { &h, sizeof h };
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> ctl{};
    msghdr msg {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctl.data();
    msg.msg_controllen = ctl.size();
    const ssize_t got = ::recvmsg(fd, &msg, 0);

    int received = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS
            && c->cmsg_len >= CMSG_LEN(sizeof(int)))
            std::memcpy(&received, CMSG_DATA(c), sizeof(int));
    }
    if (got == 0) {
        if (err) *err = "refused by " + endpoint + ": the secret was not accepted";
        ::close(fd);
        return -1;
    }
    if (got != static_cast<ssize_t>(sizeof h) || (static_cast<unsigned>(msg.msg_flags) & static_cast<unsigned>(MSG_CTRUNC)) != 0u
        || !hello_matches(h, role)) {
        if (received >= 0) ::close(received);
        if (err) *err = "malformed hand-off from " + endpoint;
        ::close(fd);
        return -1;
    }
    if (h.status != STATUS_OK) {
        if (received >= 0) ::close(received);
        if (err) *err = status_text(h.status, endpoint);
        ::close(fd);
        return -1;
    }
    if (received < 0) {
        if (err) *err = "malformed hand-off from " + endpoint + ": no descriptor";
        ::close(fd);
        return -1;
    }
    ::fcntl(received, F_SETFD, FD_CLOEXEC);
    if (size_out) *size_out = h.size;
    if (role == ROLE_PEER) *connection = fd;   // the lease
    else ::close(fd);
    return received;
}
#endif

} // namespace detail

// A reader: the published segment's READ-ONLY descriptor, the caller's to
// own (hand it to shm_segment_client). Invalid, with `err` filled, when
// there is no engine there, it is not ours, the secret was refused, or the
// hand-off was malformed. `size_out`, if given, receives the length the
// server declared.
inline shm_native_handle receive(const std::string& endpoint, const uint8_t* secret32,
                                 std::string* err, uint64_t* size_out = nullptr) {
#ifdef _WIN32
    HANDLE conn = INVALID_HANDLE_VALUE;
#else
    int conn = -1;
#endif
    clockwork::WaitEvent none = nullptr;
    return detail::attach(endpoint, secret32, ROLE_READER, err, size_out, &conn, &none);
}

// A peer: the command segment's read-write descriptor and the lease that
// holds it. Not valid, with `err` filled, for every reason receive() has
// and two more: the host did not enable the command plane, or another peer
// holds it.
inline command_lease receive_command_plane(const std::string& endpoint, const uint8_t* secret32,
                                           std::string* err, uint64_t* size_out = nullptr) {
    command_lease lease;
    lease.handle = detail::attach(endpoint, secret32, ROLE_PEER, err, size_out, &lease.connection, &lease.wake_event);
    return lease;
}

} // namespace shm_attach
