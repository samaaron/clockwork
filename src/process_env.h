// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The process environment, read the one way Clockwork reads it. getenv is
// not thread-safe against a setenv elsewhere in the process; Clockwork never
// sets the environment, and reads it only while bringing itself up — before
// its threads start — so the one caveat does not apply, and every read goes
// through here to say so once.
#pragma once

#include <cstdlib>

namespace clockwork {

// The variable's value, or nullptr when it is not set. Startup only.
inline const char* processEnv(const char* name) {
    return std::getenv(name);   // NOLINT(concurrency-mt-unsafe): read at startup, never written by this process
}

}  // namespace clockwork
