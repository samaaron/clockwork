// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_server_shm_security.cpp — the public shm segment is a trust boundary
 * on a multi-user machine. test_shm_segment.cpp pins its creation (anonymous,
 * close-on-exec, size-checked); these two pin the scope readers over it: they
 * hold the client mapping alive, and clamp corrupt slot geometry rather than
 * read past it.
 *
 * POSIX-only; the Windows mapping path is exercised through the engine tests.
 */
#if !defined(_WIN32)

#include <catch2/catch_test_macros.hpp>
#include "shm_segment.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

using detail_shm_segment::shm_segment_creator;
using detail_shm_segment::shm_segment_client;
using detail_shm_segment::shm_dup_handle;

TEST_CASE("shm-security: scope readers pin the client mapping (no dangling reads)",
          "[shm][security]") {
    shm_segment_creator creator;
    creator.publish();

    // A GUI hands reader copies to long-lived widgets, then remaps on engine
    // cold swap by destroying its client. The reader must keep the old mapping
    // alive: 2026-07-11 this exact sequence was a GUI segfault (poll timer →
    // atomic load through the munmapped slot pointer).
    auto client = std::make_unique<shm_segment_client>(shm_dup_handle(creator.native_handle()));
    auto reader = client->get_scope_stream_reader(0);
    client.reset();

    float scratch[64 * 2] = {};
    (void)reader.valid();
    (void)reader.write_position();
    (void)reader.copy_window(reader.write_position(), 64, scratch);
    SUCCEED("reader survived client teardown without touching unmapped memory");
}

TEST_CASE("shm-security: scope reader clamps corrupt slot geometry",
          "[shm][security]") {
    shm_segment_creator creator;
    creator.publish();
    shm_segment_client client(shm_dup_handle(creator.native_handle()));
    auto reader = client.get_scope_stream_reader(0);

    // channels/capacity_frames are re-read from shared memory on every
    // copy_window and index into the inline data array; a corrupt or hostile
    // slot must clamp to the compile-time ring geometry rather than push
    // reads out of bounds (or divide by a zero capacity).
    auto* slot = reinterpret_cast<shm_scope_stream*>(
        creator.get_base() + SHM_SCOPE_START + SHM_SCOPE_HEADER_SIZE);
    slot->state.store(1, std::memory_order_release);
    slot->channels = 0xFFFFu;
    slot->capacity_frames = 0;
    slot->write_position.store(1u << 20, std::memory_order_release);

    CHECK(reader.channels() == SHM_SCOPE_STREAM_CHANNELS);
    CHECK(reader.capacity_frames() == SHM_SCOPE_RING_FRAMES);
    std::vector<float> scratch(1024 * SHM_SCOPE_STREAM_CHANNELS, 0.0f);
    (void)reader.copy_window(reader.write_position(), 1024, scratch.data());
    SUCCEED("copy_window stayed inside the inline ring under corrupt geometry");

    slot->capacity_frames = 0x7FFFFFFFu;  // absurdly large
    (void)reader.copy_window(reader.write_position(), 1024, scratch.data());
    SUCCEED("oversized capacity clamped");
}

#endif  // !_WIN32
