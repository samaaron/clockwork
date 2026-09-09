// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * ClientVerbs.h — what a build with no client verbs says when sent one.
 *
 * CLOCKWORK_CLIENT_VERBS=OFF compiles the client half of the verb surface
 * out (docs/SURFACE.md; the list is clockwork_sys_is_client_verb in
 * clockwork_sys.h). The boundaries that answered those verbs still receive
 * them, and refuse each BY NAME to the caller that sent it — with the reason,
 * so a client learns that this build dropped the verb rather than that the
 * verb was never real — and say so in the log, rate-limited, since a client
 * that sends one will usually send thousands.
 */
#pragma once

#include "OscEgress.h"
#include "clockwork_config.h"   // clockwork_log
#include "clockwork_sys.h"      // clockwork_sys_refuse, CLOCKWORK_CLIENT_VERBS_ABSENT

#include <atomic>
#include <cstdint>

#if !CLOCKWORK_CLIENT_VERBS
inline void refuseClientVerb(OscEgress* egress, uint32_t token,
                             const uint8_t* data, uint32_t size) {
    static std::atomic<uint32_t> refusals{0};
    const uint32_t n = refusals.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || n % 100 == 0)
        clockwork_log("ERROR: %s refused — %s [%u so far]",
                      reinterpret_cast<const char*>(data), CLOCKWORK_CLIENT_VERBS_ABSENT, n + 1);
    if (!egress) return;
    clockwork_sys_refuse(data, size, CLOCKWORK_CLIENT_VERBS_ABSENT,
                         [egress, token](const uint8_t* d, uint32_t len) {
                             egress->reply(token, d, len);
                         });
}
#endif
