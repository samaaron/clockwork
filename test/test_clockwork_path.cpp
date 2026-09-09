// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_clockwork_path.cpp — pins the platform-independent half of src/clockwork_path.h.
 *
 * The UTF-8/UTF-16 conversions are thin wrappers over Win32 and only exist
 * there; what CAN be checked on every host is the Windows argv quoting rule,
 * which is pure string work and is the part that goes wrong in practice. The
 * expectations below are what Microsoft's C runtime parses back to the given
 * argument — the rule is documented in "Parsing C Command-Line Arguments".
 */
#include <catch2/catch_test_macros.hpp>

#include "clockwork_path.h"

using clockwork_path::win_argv_quote;

TEST_CASE("win_argv_quote leaves a plain argument alone", "[clockwork_path][windows]") {
    CHECK(win_argv_quote(L"--pid") == L"--pid");
    CHECK(win_argv_quote(L"4877") == L"4877");
    CHECK(win_argv_quote(L"C:\\Plugins\\Surge.vst3") == L"C:\\Plugins\\Surge.vst3");
}

TEST_CASE("win_argv_quote wraps whitespace", "[clockwork_path][windows]") {
    CHECK(win_argv_quote(L"C:\\Program Files\\Common Files\\VST3") ==
          L"\"C:\\Program Files\\Common Files\\VST3\"");
    CHECK(win_argv_quote(L"a\tb") == L"\"a\tb\"");
    CHECK(win_argv_quote(L"") == L"\"\"");
}

TEST_CASE("win_argv_quote escapes an embedded quote", "[clockwork_path][windows]") {
    CHECK(win_argv_quote(L"say \"hi\"") == L"\"say \\\"hi\\\"\"");
}

TEST_CASE("win_argv_quote doubles backslashes only before a quote", "[clockwork_path][windows]") {
    // A path ending in a backslash, once wrapped: the closing quote follows
    // the backslash, so that backslash doubles — and only that one.
    CHECK(win_argv_quote(L"C:\\Program Files\\") == L"\"C:\\Program Files\\\\\"");
    // Backslashes before an inner quote double, plus the escaping one.
    CHECK(win_argv_quote(L"x\\\\\" y") == L"\"x\\\\\\\\\\\" y\"");
    // Backslashes not before a quote are left alone even inside quotes.
    CHECK(win_argv_quote(L"C:\\a b\\c") == L"\"C:\\a b\\c\"");
}
