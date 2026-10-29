/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2026 Sam Aaron
 *
 * clockwork_comms_secret.h — the per-boot secret every command port asks for
 * (crate clockwork-comms, src/secret.rs). Whoever launches the host makes 32
 * random bytes and gets them to the host by a path other processes cannot
 * read: never the command line or the environment. Where the host reads them
 * is the host's decision. A client proves it holds the secret with the first
 * frame on its connection: /clockwork/auth with the 32 bytes as a blob. The
 * server compares in time that does not depend on where the packet differs.
 *
 * Declared here, at the engine's include root, because the attach endpoint
 * (shm_attach.hpp) asks for the secret too; the command transports' ABI
 * (comms/clockwork_comms_abi.h) includes this.
 */
#ifndef CLOCKWORK_COMMS_SECRET_H
#define CLOCKWORK_COMMS_SECRET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkCommsSecret ClockworkCommsSecret;
/* The 32 bytes the launcher made. Null for all zeros. */
ClockworkCommsSecret* clockwork_comms_secret_from_bytes(const uint8_t* bytes);
/* A new secret from the OS random source, for a process that launches its
 * own clients. Null if the source fails. */
ClockworkCommsSecret* clockwork_comms_secret_generate(void);
/* A copy a server can own. */
ClockworkCommsSecret* clockwork_comms_secret_clone(const ClockworkCommsSecret* secret);
/* The packet a client sends first (unframed OSC), written to `out`. Returns
 * its length, or 0 for a null secret or an `out` too short. */
uint32_t clockwork_comms_secret_auth_packet(const ClockworkCommsSecret* secret,
                                            uint8_t* out, uint32_t cap);
/* 1 if `packet` is exactly this secret's auth packet, else 0. */
int32_t clockwork_comms_secret_admits(const ClockworkCommsSecret* secret,
                                      const uint8_t* packet, uint32_t len);
void clockwork_comms_secret_free(ClockworkCommsSecret* secret);

#ifdef __cplusplus
}
#endif
#endif /* CLOCKWORK_COMMS_SECRET_H */
