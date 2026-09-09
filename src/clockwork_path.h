// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_path.h — paths cross the wire as UTF-8; the OS may want otherwise.
 *
 * Every path clockwork handles arrives as an OSC string (a plugin folder,
 * a rig file, a recording), and OSC strings are UTF-8. On POSIX that is
 * also what the filesystem takes, and std::filesystem::path built from a
 * std::string is exactly right. On Windows it is not: a path built from a
 * narrow string is read in the ANSI code page, so a folder with an accent
 * in its name — or a user whose name has one, which is where the per-user
 * plugin folder lives — silently becomes a different folder. The Win32
 * calls clockwork makes directly (LoadLibrary, CreateFileMapping) take
 * UTF-16 and need the same conversion.
 *
 * So the conversion happens once, here, at the edge: from_utf8 for a path
 * about to meet the filesystem, to_utf8 for one about to go back on the
 * wire, and to_wide / from_wide for the Win32 calls. On POSIX the first two
 * are the identity, kept as calls so the boundary is visible in the code
 * that crosses it.
 */
#pragma once

#include <filesystem>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace clockwork_path {

#if defined(_WIN32)
inline std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string from_wide(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                          nullptr, nullptr);
    return s;
}

inline std::filesystem::path from_utf8(const std::string& s) { return std::filesystem::path(to_wide(s)); }
inline std::string to_utf8(const std::filesystem::path& p) { return from_wide(p.wstring()); }

// GetLastError() as text, for an error string a person will read. The
// trailing newline FormatMessage adds is removed; an unknown code becomes
// "error N" rather than nothing.
inline std::string last_error_text(unsigned long code) {
    wchar_t* buf = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string text;
    if (n && buf) {
        text = from_wide(std::wstring(buf, n));
        ::LocalFree(buf);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
            text.pop_back();
    }
    if (text.empty()) text = "error " + std::to_string(code);
    return text;
}
#else
inline std::filesystem::path from_utf8(const std::string& s) { return std::filesystem::path(s); }
inline std::string to_utf8(const std::filesystem::path& p) { return p.string(); }
#endif

// One argument for a Windows command line, spelled so that the C runtime's
// argv parser in the child reads back exactly `arg`. Windows has no argv:
// CreateProcess takes one string and every program re-splits it, by the
// rule that
//
//   - an argument holding a space, tab or quote is wrapped in quotes,
//   - a quote inside is escaped with a backslash, and
//   - a run of backslashes right before a quote — including the closing
//     quote — is doubled, since only then does the parser take them as
//     literal backslashes rather than as escaping the quote.
//
// The last rule is the one that gets missed, and the everyday case it exists
// for is a path that ends in a backslash. An empty argument is "" so that
// it survives as a (empty) argument at all. Pure string work, so it compiles
// and is tested on every platform; the Windows spawn feeds it UTF-16.
inline std::wstring win_argv_quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring q = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            q.append(backslashes * 2 + 1, L'\\');
            backslashes = 0;
            q += c;
            continue;
        }
        q.append(backslashes, L'\\');
        backslashes = 0;
        q += c;
    }
    q.append(backslashes * 2, L'\\');
    q += L'"';
    return q;
}

}  // namespace clockwork_path
