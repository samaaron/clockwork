// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * guest_fence.h — the guest keeps to the memory it was handed.
 *
 * A guest is native code called on the engine thread with pointers into the
 * arena (dsp_api.h), and nothing in a plain build stops it reading or writing
 * the rest of the process. Under AddressSanitizer this fence turns the
 * contract into a check: while a guest entry point is open, every arena
 * region the guest was not handed — not its owner, not in its audience — is
 * poisoned, so a stray access is a sanitizer report with the address and the
 * stack, the way any other finding is. When the entry point closes the poison
 * is lifted. A host callback (DspHost) runs inside the guest's call but is the
 * engine's own code, so its body lifts the poison for its own duration and
 * puts it back.
 *
 * Poison is process-wide, not per thread. In a running engine, clients and
 * transports read and write the arena from their own threads while the audio
 * thread is inside the guest, and every one of those would be reported. So
 * the fence is ARMED BY A HARNESS, never by the engine: a test fixture whose
 * one thread is the only thing touching the arena arms it, and the engine
 * under that fixture runs with it. Natively, a fence that is not armed costs
 * nothing; in a build without AddressSanitizer every function here is empty.
 */
#pragma once
#include <cstdint>

// Arm or disarm the fence. Armed, every guest entry point from then on is
// fenced. Disarming lifts any poison that is up.
void clockwork_guest_fence_arm(bool armed);

// The arena the fence guards: the engine says so when it takes an arena and
// again, with nullptr, when it lets it go.
void clockwork_guest_fence_arena(const uint8_t* base);

// A guest entry point opened or closed (clockwork_guest_call_begin/end).
void clockwork_guest_fence_close();
void clockwork_guest_fence_open();

// The body of a host callback: the engine's own work inside the guest's call.
struct HostCallScope {
    HostCallScope();
    ~HostCallScope();
    HostCallScope(const HostCallScope&) = delete;
    HostCallScope& operator=(const HostCallScope&) = delete;
private:
    bool mLifted = false;
};
