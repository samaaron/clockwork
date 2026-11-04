// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
#pragma once

// Which sanitizer this test binary was built with (CLOCKWORK_SANITIZE), for
// a case whose measure the sanitizer itself distorts: resident memory under
// a shadow, a wait against a slowed engine, a fence ASan's own poisoning
// would trip. GCC says so with a macro, Clang with a feature query.
#if defined(__SANITIZE_THREAD__)
#  define CLOCKWORK_UNDER_TSAN 1
#elif defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define CLOCKWORK_UNDER_TSAN 1
#  endif
#endif
#ifndef CLOCKWORK_UNDER_TSAN
#  define CLOCKWORK_UNDER_TSAN 0
#endif

#if defined(__SANITIZE_ADDRESS__)
#  define CLOCKWORK_UNDER_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define CLOCKWORK_UNDER_ASAN 1
#  endif
#endif
#ifndef CLOCKWORK_UNDER_ASAN
#  define CLOCKWORK_UNDER_ASAN 0
#endif
