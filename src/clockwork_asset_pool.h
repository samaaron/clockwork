/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2026 Sam Aaron
 *
 * clockwork_asset_pool.h — the client's pool over the inbox lane.
 *
 * The lane (DspConfig::inbox; CLOCKWORK_REGION_INBOX to a client) is the
 * client's to carve. This pool hands out offsets into it and takes them back.
 * It holds no memory of its own and never touches the lane, so the same code
 * serves a client writing plain memory in its own process and one writing a
 * mapped segment — only the offsets cross to the engine, in
 * /clockwork/asset/commit. Offsets are 16-aligned (a slot's first byte is
 * where a guest will point a float array), freed slots coalesce with their
 * neighbours, and a free of something never allocated is refused.
 *
 * ONE THREAD AT A TIME. The pool is the client's bookkeeping; a client that
 * allocates from several threads serialises them itself.
 */
#ifndef CLOCKWORK_ASSET_POOL_H
#define CLOCKWORK_ASSET_POOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkAssetPool ClockworkAssetPool;

/* A pool over a lane of `bytes` bytes. NULL when bytes is 0 or memory is short. */
ClockworkAssetPool* clockwork_asset_pool_open(uint32_t bytes);
void                clockwork_asset_pool_close(ClockworkAssetPool* pool);

/* A slot of at least `bytes`, 16-aligned. 0 and the offset on success; non-zero
 * when nothing that size is free (or bytes is 0). */
int clockwork_asset_pool_alloc(ClockworkAssetPool* pool, uint32_t bytes, uint32_t* offset_out);

/* Give a slot back. 0 on success; non-zero when `offset` is not the start of
 * a live allocation — never freed twice, never freed from the middle. */
int clockwork_asset_pool_free(ClockworkAssetPool* pool, uint32_t offset);

/* Bytes not currently allocated (including alignment slack). */
uint32_t clockwork_asset_pool_free_bytes(const ClockworkAssetPool* pool);

#ifdef __cplusplus
}
#endif
#endif /* CLOCKWORK_ASSET_POOL_H */
