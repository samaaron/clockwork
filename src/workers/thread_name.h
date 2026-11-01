// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * thread_name.h — a name for the calling thread, as debuggers, profilers,
 * crash reports and the system's own thread lists show it.
 *
 * Every long-lived thread clockwork starts names itself first thing. Names are
 * "cw-" and what the thread does, at most 15 characters: Linux keeps no more.
 */
#pragma once

namespace clockwork {

// Name the calling thread. Longer names are cut to 15 characters; a platform
// with no thread names ignores it.
void name_this_thread(const char* name) noexcept;

}  // namespace clockwork
