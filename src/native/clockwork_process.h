// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_process.h — spawn a child, know whether it is alive, and end it.
 *
 * What the engine needs of the plugin bridge's process and nothing more:
 * start it with an argument list, poll it without blocking, kill it, and
 * reap it. posix_spawn on POSIX (no fork in a process with an audio thread,
 * a JUCE device and Cocoa on the main thread — fork copies none of those
 * safely); CreateProcessW on Windows. Header-only.
 */
#pragma once

#include "clockwork_path.h"

#include <cerrno>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <signal.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

class ClockworkProcess {
public:
    ClockworkProcess() = default;
    ~ClockworkProcess() { detach(); }
    ClockworkProcess(const ClockworkProcess&) = delete;
    ClockworkProcess& operator=(const ClockworkProcess&) = delete;

    // Start `exe` with `args` (argv[1..]). The child inherits the
    // environment and the standard streams, so its stderr lands in the
    // engine's log. False if it could not be started.
    bool spawn(const std::string& exe, const std::vector<std::string>& args) {
        detach();
#if defined(_WIN32)
        std::wstring cmd = quote(exe);
        for (const auto& a : args) { cmd += L' '; cmd += quote(a); }
        PROCESS_INFORMATION pi {};
        // The standard streams and ONLY those. bInheritHandles=TRUE on its
        // own hands the child every inheritable handle in the engine —
        // sockets, the audio device, the shared-memory mapping — and a
        // bridge holding the engine's command socket open after the engine
        // has gone is a port that cannot be rebound. The handle list
        // attribute limits inheritance to what is named in it.
        HANDLE std_handles[3] = { GetStdHandle(STD_INPUT_HANDLE),
                                  GetStdHandle(STD_OUTPUT_HANDLE),
                                  GetStdHandle(STD_ERROR_HANDLE) };
        DWORD n_std = 0;
        for (HANDLE h : std_handles) {
            if (h && h != INVALID_HANDLE_VALUE) {
                SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                std_handles[n_std++] = h;
            }
        }
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        std::vector<unsigned char> attr_buf(attr_size);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) return false;
        const bool listed = n_std > 0 &&
            UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      std_handles, n_std * sizeof(HANDLE), nullptr, nullptr);
        STARTUPINFOEXW si {};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.StartupInfo.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si.lpAttributeList = listed ? attrs : nullptr;
        // CREATE_NO_WINDOW: the bridge is a GUI-capable process (it opens
        // plugin editors) but has no console of its own to show.
        const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, listed ? TRUE : FALSE,
                                       CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                       nullptr, nullptr, &si.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs);
        if (!ok) return false;
        CloseHandle(pi.hThread);
        mHandle = pi.hProcess;
        mPid = static_cast<int>(pi.dwProcessId);
        return true;
#else
        std::vector<std::string> owned;
        owned.reserve(args.size() + 1);
        owned.push_back(exe);
        for (const auto& a : args) owned.push_back(a);
        std::vector<char*> argv;
        for (auto& s : owned) argv.push_back(s.data());
        argv.push_back(nullptr);
        pid_t pid = 0;
        if (::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0)
            return false;
        mPid = static_cast<int>(pid);
        return true;
#endif
    }

    int pid() const { return mPid; }
    bool running() const { return mPid > 0 && !mExited; }

    // Poll. True while the child is alive; once it has exited, reaps it and
    // remembers how (exit code, or the signal negated).
    bool alive() {
        if (mPid <= 0 || mExited) return false;
#if defined(_WIN32)
        if (WaitForSingleObject(mHandle, 0) != WAIT_OBJECT_0) return true;
        DWORD code = 0;
        GetExitCodeProcess(mHandle, &code);
        mExitCode = static_cast<int>(code);
        mExited = true;
        return false;
#else
        int status = 0;
        const pid_t r = ::waitpid(static_cast<pid_t>(mPid), &status, WNOHANG);
        if (r == 0) return true;
        if (r < 0) { mExited = true; mExitCode = -1; return false; }
        mExited = true;
        if (WIFEXITED(status))        mExitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) mExitCode = -WTERMSIG(status);
        else                          mExitCode = -1;
        return false;
#endif
    }

    // How it ended: the exit code, or minus the signal, or -1 for unknown.
    int exitCode() const { return mExitCode; }

    // End it now. Does not wait; call alive() (or wait()) afterwards.
    void kill() {
        if (mPid <= 0 || mExited) return;
#if defined(_WIN32)
        TerminateProcess(mHandle, 9);
#else
        ::kill(static_cast<pid_t>(mPid), SIGKILL);
#endif
    }

    // Block until it has ended, and reap it.
    void wait() {
        if (mPid <= 0 || mExited) return;
#if defined(_WIN32)
        WaitForSingleObject(mHandle, INFINITE);
        alive();
#else
        int status = 0;
        pid_t r;
        do { r = ::waitpid(static_cast<pid_t>(mPid), &status, 0); } while (r < 0 && errno == EINTR);
        mExited = true;
        if (r > 0 && WIFEXITED(status))        mExitCode = WEXITSTATUS(status);
        else if (r > 0 && WIFSIGNALED(status)) mExitCode = -WTERMSIG(status);
        else                                   mExitCode = -1;
#endif
    }

    // Forget the child without ending it (a child that is still running is
    // left to the OS to reap).
    void detach() {
#if defined(_WIN32)
        if (mHandle) CloseHandle(mHandle);
        mHandle = nullptr;
#endif
        mPid = 0;
        mExited = false;
        mExitCode = -1;
    }

private:
#if defined(_WIN32)
    // See clockwork_path::win_argv_quote for the rule; the argument goes over as
    // UTF-16 so a path with an accent reaches the child intact.
    static std::wstring quote(const std::string& s) {
        return clockwork_path::win_argv_quote(clockwork_path::to_wide(s));
    }
    HANDLE mHandle = nullptr;
#endif
    int  mPid = 0;
    bool mExited = false;
    int  mExitCode = -1;
};
