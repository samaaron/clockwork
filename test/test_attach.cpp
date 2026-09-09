// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_attach.cpp — more than one host in one process.
 *
 * The engine is a process singleton, so the hazard is not a crash. A second
 * host calling clockwork_init would be OBEYED: it would reconfigure the first
 * host's engine underneath it, and the only symptom would be the first host's
 * rate or block changing for no reason it could see. That is what this file
 * pins — mostly by asserting that things DON'T change.
 *
 * A BINARY OF ITS OWN, and ONE TEST CASE, both for the same reason: there is
 * one engine per process and one attach count, so the protocol has to be
 * walked in order. Catch2 randomises case order, so splitting this into cases
 * would make it depend on the seed.
 */
#include "lanes/lanes.h"
#include "shared_memory.h"
#include "clockwork_ports.h"
#include "clockwork_port_bus.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {
alignas(16) uint8_t gArena[1 << 20];
constexpr double   kRate  = 48000.0;
constexpr uint32_t kBlock = 128;
}

TEST_CASE("attach: boot, join, refuse, and what a joiner may not change",
          "[attach]") {
    REQUIRE(clockwork_attached() == 0);

    // ── The first host boots it ──────────────────────────────────────────────
    REQUIRE(clockwork_attach(kRate, kBlock, /*in*/ 2, /*out*/ 8, /*verbosity*/ 0,
                             gArena, sizeof gArena, nullptr, 0, 0)
            == CLOCKWORK_ATTACH_BOOTED);
    REQUIRE(clockwork_attached() == 1);
    REQUIRE(clockwork_sample_rate() == kRate);
    const uint32_t booted_block = clockwork_block_size();
    REQUIRE(booted_block == kBlock);

    // ── A second host the engine can serve JOINS ────────────────────────────
    // Fewer channels than are open is served: "enough" is the test, not
    // "identical". Block 0 means "whatever is running".
    REQUIRE(clockwork_attach(kRate, /*block*/ 0, /*in*/ 1, /*out*/ 2, 0,
                             nullptr, 0, nullptr, 0, 0)
            == CLOCKWORK_ATTACH_JOINED);
    REQUIRE(clockwork_attached() == 2);

    // A JOINER'S ARGUMENTS ARE NOT APPLIED. This is the whole point: the
    // engine still has the first host's geometry, not the joiner's narrower
    // one, and not a null arena.
    REQUIRE(clockwork_sample_rate() == kRate);
    REQUIRE(clockwork_block_size()  == booted_block);
    REQUIRE(clockwork_lanes_base()  != nullptr);

    // ── A host it cannot serve is REFUSED, and changes nothing ──────────────
    REQUIRE(clockwork_attach(44100.0, kBlock, 2, 2, 0, nullptr, 0, nullptr, 0, 0)
            == CLOCKWORK_ATTACH_REFUSED);
    REQUIRE(clockwork_attached() == 2);            // not counted
    REQUIRE(clockwork_sample_rate() == kRate);     // not applied

    // More channels than are open is also refused — a host that asked for 32
    // outputs must not silently render into 8.
    REQUIRE(clockwork_attach(kRate, kBlock, 2, 32, 0, nullptr, 0, nullptr, 0, 0)
            == CLOCKWORK_ATTACH_REFUSED);
    REQUIRE(clockwork_attached() == 2);

    // ── clockwork_init REFUSES while two hosts hold the engine ──────────────
    // The regression this file exists for. Before the attach protocol this
    // call was obeyed, and the first host's engine was reconfigured under it.
    clockwork_init(44100.0, 512, 2, 2, 0, gArena, sizeof gArena, nullptr, 0, 0);
    REQUIRE(clockwork_sample_rate() == kRate);         // unchanged
    REQUIRE(clockwork_block_size()  == booted_block);  // unchanged
    REQUIRE(clockwork_attached() == 2);

    // ── Detaching is what makes it safe again ───────────────────────────────
    clockwork_detach();
    REQUIRE(clockwork_attached() == 1);

    // With a single host, reconfiguring is allowed again — that is a device
    // switch, and it is routine.
    clockwork_init(kRate, 256, 2, 8, 0, gArena, sizeof gArena, nullptr, 0, 0);
    REQUIRE(clockwork_block_size() == 256);

    clockwork_detach();
    REQUIRE(clockwork_attached() == 0);

    // Detaching past zero is harmless rather than an underflow.
    clockwork_detach();
    REQUIRE(clockwork_attached() == 0);
}

// ── A rebuild does not forget what is bound ─────────────────────────────────
//
// This is what the old announcement mechanism needed a re-announce sweep for:
// every attachment was replayed after dsp_new so a rebuilt guest was caught
// up. The map needs no replay — it is state, not a message — but it must not
// be TRAMPLED by the device widths a rebuild brings, which is the one way the
// same bug could come back.
//
// Here rather than in clockwork_tests because it re-initialises the engine,
// and this binary is the one that already does that on purpose.

TEST_CASE("channel map: a rebuild keeps the streams bound across it", "[attach][channel-map]") {
    static unsigned char arena2[1 << 20];
    REQUIRE(clockwork_attach(kRate, kBlock, 2, 8, 0, arena2, sizeof arena2, nullptr, 0, 0)
            == CLOCKWORK_ATTACH_BOOTED);

    const ClockworkChannelMapState* m = clockwork_channel_map();
    REQUIRE(m != nullptr);
    clockwork_port_bus_detach_all();

    const uint32_t at = 5;   // above the two device inputs
    const ClockworkPort p = clockwork_port_open("survivor", kClockworkPortSource, 2, 1024);
    REQUIRE(p != CLOCKWORK_PORT_NONE);
    REQUIRE(clockwork_port_bus_attach(p, at) == 1);
    REQUIRE(channelKind(m->in[at].load(std::memory_order_acquire)) == CLOCKWORK_CH_PORT);

    // A device switch: same engine, different widths.
    clockwork_init(kRate, kBlock, /*in*/ 4, /*out*/ 4, 0, arena2, sizeof arena2, nullptr, 0, 0);

    CHECK(m->device_in.load(std::memory_order_acquire)  == 4);
    // The device took channels 0-3 and said so...
    CHECK(channelKind(m->in[0].load(std::memory_order_acquire)) == CLOCKWORK_CH_DEVICE);
    // ...without trampling the stream that was already on 5.
    CHECK(channelKind(m->in[at].load(std::memory_order_acquire)) == CLOCKWORK_CH_PORT);

    clockwork_port_bus_detach_all();
    clockwork_port_close_all();
    clockwork_detach();
}
