// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * owner.h — gsl::owner<T*>: a raw pointer that owns what it points to.
 *
 * Clockwork's own objects live in smart pointers. Where a pointer has to be
 * raw — a handle handed across a C boundary, which the caller gives back
 * through the matching close function, or a snapshot handed to the audio
 * thread through an atomic — the pointer is typed owner<> instead, which is
 * the C++ Core Guidelines' spelling (I.11, R.3) and what clang-tidy's
 * cppcoreguidelines-owning-memory reads: a `new` into one and a `delete`
 * of one are the contract, not a leak or a double free waiting to happen.
 * A pointer that is not an owner<> must not be deleted.
 *
 * It is the type it wraps, nothing more: the whole of the Guidelines Support
 * Library's definition, which Clockwork does not otherwise use.
 */
#pragma once

namespace gsl {
template <class T>
using owner = T;
}
