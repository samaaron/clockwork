// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_client.cpp — the client boundary, over the primitives the engine
 * already uses.
 *
 * There is deliberately very little arithmetic here. The ring mechanics live
 * in RingBufferWriter (write) and clockwork_drain_ring (read), which is the
 * whole point: a client and the engine's own threads run the SAME code over
 * the same bytes, so there is no second implementation to drift. What this
 * file adds is a handle, a way to reach the regions by name, and the argument
 * checking a public boundary owes its callers.
 */
#include "clockwork_client.h"
#include "clockwork_prefix.h"   // clockwork_sys_claims: whose message this is
#include "owner.h"

#include "shared_memory.h"
#include "metrics_history.h"
#include "clockwork_config.h"
#include "in_ring_door.h"        // the in ring's boundary
#include "lanes/ring_drain.h"
#include "workers/RingBufferWriter.h"
#include "shm_tap_stream.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#if !defined(__EMSCRIPTEN__)
#  ifdef _WIN32
#    include <process.h>
#  else
#    include <unistd.h>
#  endif
#endif

#if !defined(__EMSCRIPTEN__)
#  include "shm_segment.hpp"
#  include "shm_attach.hpp"
#endif

namespace {

// Everything a handle needs, and nothing that belongs to one transport.
//
// `mapping` is the only place the transports differ after open: a shm
// handle unmaps on close, an in-process one hands the memory back untouched.
struct ClientImpl {
    uint8_t* base   = nullptr;
    uint32_t bytes  = 0;
    // Where every region is: the arena's own table (clockwork_arena.h),
    // whichever way the handle was opened. Nothing below addresses the arena
    // through a constant.
    ShmReaderLayout L;

    // Sequence-gap tracking, which is per handle. The ring's read CURSOR is
    // not here: it lives in the ring cursors and is shared, because polling
    // takes from the ring. A reader that wants its own cursor opens a tap.
    ClockworkDrainState drain{};      // the OUT ring: the audio thread's replies
    ClockworkDrainState nrtDrain{};   // the NRT-out ring: the gateway's replies and broadcasts

    // An open send reservation, between begin and commit. One handle is for
    // one thread, so this needs no protection of its own — and the ring's
    // write lock is held for as long as it is set.
    RingBufferWriter::Reservation pending{};

#if !defined(__EMSCRIPTEN__)
    // The published segment, when this handle mapped it: read-only, so
    // nothing below writes through `base` on such a handle.
    std::unique_ptr<detail_shm_segment::shm_segment_client> mapping;
    // The command segment, when this handle is the peer (docs/SEGMENTS.md):
    // its plane is where send writes and poll takes from. `lease` holds the
    // plane for as long as the handle lives, when the hand-off was ours.
    std::unique_ptr<detail_shm_segment::shm_command_client> commands;
    shm_attach::command_lease lease;
    ClockworkDrainState  peerDrain{};      // the plane's reply ring
    std::vector<uint8_t> peerBatch;        // the last poll's frames, copied out of the ring
#endif

    // A reader: a segment handle without the command plane. It reads, and
    // nothing else; every call that would write says so.
    bool reader() const {
#if !defined(__EMSCRIPTEN__)
        return mapping && !commands;
#else
        return false;
#endif
    }
#if !defined(__EMSCRIPTEN__)
    ShmPeerPlaneHeader* plane() const { return commands ? commands->get_peer_plane() : nullptr; }
#endif

    RingCursors* control() const {
        return reinterpret_cast<RingCursors*>(base + L.control_offset);
    }
};

ClockworkStatus fail(ClockworkStatus* out, ClockworkStatus s) {
    if (out) *out = s;
    return s;
}

// A lane as the arena's table places it, for a handle in the engine's own
// process (an embedder's, the web's): the table names the address when the
// lanes are the engine's own memory. Absent when they are nowhere, or in a
// command segment this handle has no mapping of.
ClockworkStatus laneInProcess(const uint8_t* base, uint32_t area, int writable, ClockworkRegion* out) {
    const ClockworkArenaAreaEntry* a = clockwork_arena_area(arenaHeader(base), area);
    if (!a || a->space != CLOCKWORK_SPACE_PROCESS || a->bytes_lo == 0) return CLOCKWORK_E_ABSENT;
    const auto at = static_cast<uintptr_t>((uint64_t(a->base_hi) << 32) | a->base_lo);
    *out = { reinterpret_cast<uint8_t*>(at), a->bytes_lo, writable };
    return CLOCKWORK_OK;
}

#if !defined(__EMSCRIPTEN__)
// This process, for the plane's owner stamp (shm_peer_attach).
long clockwork_client_pid() {
#  ifdef _WIN32
    return static_cast<long>(_getpid());
#  else
    return static_cast<long>(::getpid());
#  endif
}
#endif

// The arena's table, and which status a refusal is. Memory too small to
// hold a header, or a table whose regions do not fit, is the caller's
// argument (E_ARG); a header that is not an arena, or a version this build
// does not read, is the engine's (E_VERSION).
ClockworkStatus readTable(const uint8_t* base, uint32_t bytes, ShmReaderLayout& L) {
    if (bytes < sizeof(ClockworkArenaHeader)) return CLOCKWORK_E_ARG;
    const auto* h = arenaHeader(base);
    if (h->magic != CLOCKWORK_ARENA_MAGIC || h->version != CLOCKWORK_ARENA_VERSION)
        return CLOCKWORK_E_VERSION;
    const char* why = "";
    return ShmReaderLayout::from_arena(base, bytes, L, &why) ? CLOCKWORK_OK : CLOCKWORK_E_ARG;
}

} // namespace

extern "C" {

uint32_t clockwork_client_abi_version(void) { return CLOCKWORK_CLIENT_ABI_VERSION; }

const char* clockwork_client_status_text(ClockworkStatus s) {
    switch (s) {
        case CLOCKWORK_OK:           return "ok";
        case CLOCKWORK_E_ARG:        return "invalid argument";
        case CLOCKWORK_E_VERSION:    return "engine layout is a version this build does not read";
        case CLOCKWORK_E_NOT_FOUND:  return "no engine there";
        case CLOCKWORK_E_PERM:       return "engine found but not permitted";
        case CLOCKWORK_E_ABSENT:     return "this engine has no such region";
        case CLOCKWORK_E_FULL:       return "ingress ring full; retry";
        case CLOCKWORK_E_TOO_BIG:    return "message larger than the ring can hold";
        case CLOCKWORK_E_CLOSED:     return "engine closed";
        case CLOCKWORK_E_NOMEM:      return "out of memory";
        case CLOCKWORK_E_NO_INSTANCE: return "no instance: the engine is between guests; wait for setup";
        case CLOCKWORK_E_BUSY:       return "another peer holds the command plane";
    }
    return "unknown status";
}

ClockworkClient* clockwork_client_open_memory(void* base, uint32_t bytes,
                                              ClockworkStatus* out_status) {
    if (!base || bytes == 0) { fail(out_status, CLOCKWORK_E_ARG); return nullptr; }

    // The arena's own table, even in-process: an embedder's client may be
    // built from a different tree than the engine it was handed.
    ShmReaderLayout L;
    const ClockworkStatus st = readTable(static_cast<const uint8_t*>(base), bytes, L);
    if (st != CLOCKWORK_OK) { fail(out_status, st); return nullptr; }
    std::unique_ptr<ClientImpl> c(new (std::nothrow) ClientImpl());
    if (!c) { fail(out_status, CLOCKWORK_E_NOMEM); return nullptr; }
    c->base  = static_cast<uint8_t*>(base);
    c->bytes = L.blob_size;
    c->L     = L;
    fail(out_status, CLOCKWORK_OK);
    return reinterpret_cast<ClockworkClient*>(c.release());
}

#if !defined(__EMSCRIPTEN__)
ClockworkClient* clockwork_client_open_shm_handle(intptr_t native_handle,
                                                  ClockworkStatus* out_status) {
#ifdef _WIN32
    const auto h = reinterpret_cast<detail_shm_segment::shm_native_handle>(native_handle);
#else
    const auto h = static_cast<detail_shm_segment::shm_native_handle>(native_handle);
#endif
    std::unique_ptr<ClientImpl> c(new (std::nothrow) ClientImpl());
    if (!c) {
        detail_shm_segment::shm_close_native(h);
        fail(out_status, CLOCKWORK_E_NOMEM);
        return nullptr;
    }
    try {
        // Takes the handle on every path, so nothing leaks on a refusal.
        c->mapping = std::make_unique<detail_shm_segment::shm_segment_client>(h);
    } catch (const std::exception&) {
        // The segment is truncated, unpublished, or built by an engine whose
        // layout this build does not recognise. shm_segment_client refuses
        // all three by throwing; a boundary in C reports rather than unwinds.
        fail(out_status, CLOCKWORK_E_NOT_FOUND);
        return nullptr;
    }
    // The mapping is read-only and this handle never writes through `base`
    // on a segment handle: every writing call asks reader() first and the
    // peer's writes go to its own command segment. The cast is for the one
    // pointer type the handle keeps for every way of opening.
    c->base         = const_cast<uint8_t*>(c->mapping->get_base());
    c->L            = c->mapping->layout();   // the ENGINE's layout, not this build's
    c->bytes        = c->L.blob_size;
    fail(out_status, CLOCKWORK_OK);
    return reinterpret_cast<ClockworkClient*>(c.release());
}

ClockworkClient* clockwork_client_open_shm(const char* endpoint, const uint8_t* secret,
                                           ClockworkStatus* out_status) {
    if (!endpoint || !*endpoint) { fail(out_status, CLOCKWORK_E_NOT_FOUND); return nullptr; }
    if (!secret) { fail(out_status, CLOCKWORK_E_ARG); return nullptr; }
    std::string err;
    const auto h = shm_attach::receive(endpoint, secret, &err);
    if (!detail_shm_segment::shm_handle_valid(h)) {
        // Refused: an engine answered and did not accept the secret. Else no
        // engine at the endpoint, or not ours: nothing to attach to.
        fail(out_status, err.rfind("refused by", 0) == 0 ? CLOCKWORK_E_PERM : CLOCKWORK_E_NOT_FOUND);
        return nullptr;
    }
#ifdef _WIN32
    const auto raw = reinterpret_cast<intptr_t>(h);
#else
    const auto raw = static_cast<intptr_t>(h);
#endif
    return clockwork_client_open_shm_handle(raw, out_status);
}

ClockworkClient* clockwork_client_open_shm_peer_handles(intptr_t reader_handle, intptr_t command_handle,
                                                        ClockworkStatus* out_status) {
    const auto command = detail_shm_segment::shm_handle_from_intptr(command_handle);
    ClockworkClient* handle = clockwork_client_open_shm_handle(reader_handle, out_status);
    if (!handle) {
        detail_shm_segment::shm_close_native(command);
        return nullptr;
    }
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    try {
        // Takes the handle on every path, so nothing leaks on a refusal.
        c->commands = std::make_unique<detail_shm_segment::shm_command_client>(command);
    } catch (const std::exception&) {
        clockwork_client_close(handle);
        fail(out_status, CLOCKWORK_E_NOT_FOUND);
        return nullptr;
    }
    // Claim the plane as its peer: the pid stamp, the generation, a stale
    // writer lock reset, replies to a dead predecessor skipped.
    shm_peer_attach(c->plane(), static_cast<uint32_t>(clockwork_client_pid()));
    fail(out_status, CLOCKWORK_OK);
    return handle;
}

ClockworkClient* clockwork_client_open_shm_peer(const char* endpoint, const uint8_t* secret,
                                                ClockworkStatus* out_status) {
    if (!endpoint || !*endpoint) { fail(out_status, CLOCKWORK_E_NOT_FOUND); return nullptr; }
    if (!secret) { fail(out_status, CLOCKWORK_E_ARG); return nullptr; }
    std::string err;
    const auto reader = shm_attach::receive(endpoint, secret, &err);
    if (!detail_shm_segment::shm_handle_valid(reader)) {
        fail(out_status, err.rfind("refused by", 0) == 0 ? CLOCKWORK_E_PERM : CLOCKWORK_E_NOT_FOUND);
        return nullptr;
    }
    shm_attach::command_lease lease = shm_attach::receive_command_plane(endpoint, secret, &err);
    if (!lease.valid()) {
        detail_shm_segment::shm_close_native(reader);
        // The hello says which: no plane on this host, held by another peer,
        // or the secret refused.
        ClockworkStatus st = CLOCKWORK_E_NOT_FOUND;
        if (err.rfind("no shared-memory command plane", 0) == 0) st = CLOCKWORK_E_ABSENT;
        else if (err.rfind("another peer holds", 0) == 0)       st = CLOCKWORK_E_BUSY;
        else if (err.rfind("refused by", 0) == 0)               st = CLOCKWORK_E_PERM;
        fail(out_status, st);
        return nullptr;
    }
    // The segment handle goes to the client; the lease stays here, held for
    // the handle's life.
    const auto command = lease.handle;
    lease.handle = detail_shm_segment::shm_invalid_handle;
    ClockworkClient* handle = clockwork_client_open_shm_peer_handles(
        detail_shm_segment::shm_handle_to_intptr(reader),
        detail_shm_segment::shm_handle_to_intptr(command), out_status);
    if (!handle) return nullptr;   // the lease releases as it goes out of scope
    reinterpret_cast<ClientImpl*>(handle)->lease = std::move(lease);
    return handle;
}

uint32_t clockwork_client_default_endpoint(uint32_t port, char* buf, uint32_t cap) {
    const std::string ep = shm_attach::default_endpoint(port);
    const auto need = static_cast<uint32_t>(ep.size());
    if (buf && cap > need) std::memcpy(buf, ep.c_str(), need + 1);
    else if (buf && cap > 0) buf[0] = '\0';
    return need;
}
#else
ClockworkClient* clockwork_client_open_shm_peer(const char*, const uint8_t*, ClockworkStatus* out_status) {
    fail(out_status, CLOCKWORK_E_NOT_FOUND);
    return nullptr;
}
ClockworkClient* clockwork_client_open_shm_peer_handles(intptr_t, intptr_t, ClockworkStatus* out_status) {
    fail(out_status, CLOCKWORK_E_NOT_FOUND);
    return nullptr;
}
ClockworkClient* clockwork_client_open_shm(const char*, const uint8_t*, ClockworkStatus* out_status) {
    // No segment in a browser: the engine's arena IS this heap, so a client
    // there opens it by address.
    fail(out_status, CLOCKWORK_E_NOT_FOUND);
    return nullptr;
}
ClockworkClient* clockwork_client_open_shm_handle(intptr_t, ClockworkStatus* out_status) {
    fail(out_status, CLOCKWORK_E_NOT_FOUND);
    return nullptr;
}
uint32_t clockwork_client_default_endpoint(uint32_t, char* buf, uint32_t cap) {
    if (buf && cap > 0) buf[0] = '\0';
    return 0;
}
#endif

void clockwork_client_close(ClockworkClient* handle) {
    gsl::owner<ClientImpl*> c = reinterpret_cast<ClientImpl*>(handle);   // NOLINT(cppcoreguidelines-owning-memory): the caller's handle, given back here
    delete c;
}

ClockworkStatus clockwork_client_info(ClockworkClient* handle, ClockworkClientInfo* out) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out || out->struct_bytes < sizeof(uint32_t) * 4) return CLOCKWORK_E_ARG;

    // Fill only as far as the caller's struct reaches: an older client asking a
    // newer library gets what it knows about and nothing written past its end.
    ClockworkClientInfo v{};
    v.struct_bytes    = out->struct_bytes;
    v.abi_version     = CLOCKWORK_CLIENT_ABI_VERSION;
    v.engine_version  = CLOCKWORK_COMMIT_WORD;   // which Clockwork (clockwork_config.h): abi_version is what to check
    v.features        = CLOCKWORK_FEATURE_WINDOW
#if CLOCKWORK_SCHEDULER
                      | CLOCKWORK_FEATURE_SCHEDULER
#endif
                      | CLOCKWORK_FEATURE_TAPS
                      | CLOCKWORK_FEATURE_SCOPE;
    // The geometry, from the words the engine wrote once at init
    // (init_memory, audio_processor.cpp) — the same ones a metrics reader
    // finds at the schema's audioSampleRate..audioInputChannels, read here
    // so a client asking "what am I talking to" need not know the offsets.
    const auto* pm = reinterpret_cast<const PerformanceMetrics*>(c->base + c->L.metrics_offset);
    v.sample_rate     = pm->audio_sample_rate.load(std::memory_order_relaxed);
    v.block_frames    = pm->audio_block_size.load(std::memory_order_relaxed);
    v.output_channels = pm->audio_output_channels.load(std::memory_order_relaxed);
    v.input_channels  = pm->audio_input_channels.load(std::memory_order_relaxed);
    const size_t n = out->struct_bytes < sizeof(v) ? out->struct_bytes : sizeof(v);
    std::memcpy(out, &v, n);
    return CLOCKWORK_OK;
}

ClockworkStatus clockwork_client_send(ClockworkClient* handle,
                                      const uint8_t* osc, uint32_t bytes,
                                      uint32_t origin) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !osc || bytes == 0) return CLOCKWORK_E_ARG;
    if (c->reader()) return CLOCKWORK_E_PERM;
#if !defined(__EMSCRIPTEN__)
    // The peer's road: the plane's command ring, which the engine's control
    // pass drains into the same door every other sender meets. A frame that
    // cannot fit an empty ring will never fit.
    if (ShmPeerPlaneHeader* plane = c->plane()) {
        if (bytes + sizeof(Message) > SHM_PEER_CMD_RING_SIZE) return CLOCKWORK_E_TOO_BIG;
        return RingBufferWriter::write(shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
                                       &plane->cmd_head, &plane->cmd_tail,
                                       &plane->cmd_sequence, &plane->cmd_write_lock,
                                       osc, bytes, origin)
                   ? CLOCKWORK_OK : CLOCKWORK_E_FULL;
    }
#endif

    // A frame that cannot fit an empty ring will never fit: say so once rather
    // than let a caller retry forever.
    if (bytes + sizeof(Message) > c->L.in_ring_size) return CLOCKWORK_E_TOO_BIG;

    // A control verb of clockwork's, while the engine serves the control
    // ring: written there, where no audio block is between it and its
    // answer (docs/RESTARTS.md, "The control ring"). The verbs the audio
    // thread performs, and the guest's, ride the in ring. The same rule as
    // clockwork_ingress_write's, read from the same bytes.
    const uint32_t flags = c->control()->status_flags.load(std::memory_order_acquire);
    if ((flags & STATUS_CONTROL_SERVED) && c->L.control_ring_size > 0
        && clockwork_sys_control_takes(osc, bytes)) {
        auto* ctl = reinterpret_cast<ControlRingCursors*>(c->base + c->L.control_ring_offset);
        const bool ok = RingBufferWriter::write(
            c->base + c->L.control_ring_offset + CONTROL_RING_HEADER_SIZE,
            c->L.control_ring_size - CONTROL_RING_HEADER_SIZE,
            &ctl->head, &ctl->tail, &ctl->sequence, &ctl->write_lock,
            osc, bytes, origin);
        return ok ? CLOCKWORK_OK : CLOCKWORK_E_FULL;
    }

    // No guest instance to take it: refused at the boundary (in_ring_door.h),
    // rather than written to a ring whose next reader is an instance it was
    // not addressed to. The generation word is in the arena's header, so a
    // client in another process reads the same word.
    switch (in_ring_write(c->base, c->base + c->L.in_ring_offset, c->L.in_ring_size,
                          c->control(), osc, bytes, origin)) {
        case InRingWrite::Written:    return CLOCKWORK_OK;
        case InRingWrite::NoInstance: return CLOCKWORK_E_NO_INSTANCE;
        case InRingWrite::Full:       break;
    }
    return CLOCKWORK_E_FULL;
}

uint8_t* clockwork_client_send_begin(ClockworkClient* handle, uint32_t max_bytes,
                                     ClockworkStatus* out_status) {
    const auto fail = [out_status](ClockworkStatus s) -> uint8_t* {
        if (out_status) *out_status = s;
        return nullptr;
    };
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || max_bytes == 0) return fail(CLOCKWORK_E_ARG);

    // A second begin would take the lock a second time from the thread that
    // already holds it, which is a hang rather than an error message. Refuse.
    if (c->pending.valid()) return fail(CLOCKWORK_E_ARG);
    if (c->reader()) return fail(CLOCKWORK_E_PERM);

#if !defined(__EMSCRIPTEN__)
    if (ShmPeerPlaneHeader* plane = c->plane()) {
        if (max_bytes + sizeof(Message) > SHM_PEER_CMD_RING_SIZE) return fail(CLOCKWORK_E_TOO_BIG);
        c->pending = RingBufferWriter::reserve(
            shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
            &plane->cmd_head, &plane->cmd_tail, &plane->cmd_write_lock, max_bytes);
        if (!c->pending.valid()) return fail(CLOCKWORK_E_FULL);
        if (out_status) *out_status = CLOCKWORK_OK;
        return c->pending.payload;
    }
#endif

    if (max_bytes + sizeof(Message) > c->L.in_ring_size)
        return fail(CLOCKWORK_E_TOO_BIG);

    c->pending = RingBufferWriter::reserve(
        c->base + c->L.in_ring_offset, c->L.in_ring_size,
        &c->control()->in_head, &c->control()->in_tail,
        &c->control()->in_write_lock, max_bytes);

    if (!c->pending.valid()) return fail(CLOCKWORK_E_FULL);
    if (out_status) *out_status = CLOCKWORK_OK;
    return c->pending.payload;
}

ClockworkStatus clockwork_client_send_commit(ClockworkClient* handle,
                                             uint32_t bytes, uint32_t origin) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c) return CLOCKWORK_E_ARG;
    if (!c->pending.valid()) return CLOCKWORK_E_ARG;

#if !defined(__EMSCRIPTEN__)
    // The peer's reservation is in the plane's ring; the door's question is
    // the engine's to ask when its pass takes the frame.
    if (ShmPeerPlaneHeader* plane = c->plane()) {
        if (bytes > c->pending.max) {
            RingBufferWriter::abort(&plane->cmd_write_lock);
            c->pending = {};
            return CLOCKWORK_E_ARG;
        }
        RingBufferWriter::commit(shm_peer_cmd_ring(plane), SHM_PEER_CMD_RING_SIZE,
                                 &plane->cmd_head, &plane->cmd_sequence,
                                 &plane->cmd_write_lock, c->pending, bytes, origin);
        c->pending = {};
        return CLOCKWORK_OK;
    }
#endif

    // More than was reserved would run past the room the fit test allowed, so
    // the reservation is given back rather than published — a wrong length is
    // a caller's bug, and wedging the ring on top of it helps nobody.
    if (bytes > c->pending.max) {
        RingBufferWriter::abort(&c->control()->in_write_lock);
        c->pending = {};
        return CLOCKWORK_E_ARG;
    }
    // The boundary's question, asked now the message is there to read, under
    // the lock the reservation holds (in_ring_door.h): a send by the other
    // road is refused the same way.
    if (!in_ring_takes(c->base, c->pending.payload, bytes)) {
        RingBufferWriter::abort(&c->control()->in_write_lock);
        c->pending = {};
        return CLOCKWORK_E_NO_INSTANCE;
    }

    RingBufferWriter::commit(
        c->base + c->L.in_ring_offset, c->L.in_ring_size,
        &c->control()->in_head, &c->control()->in_sequence,
        &c->control()->in_write_lock, c->pending, bytes, origin);
    c->pending = {};
    return CLOCKWORK_OK;
}

void clockwork_client_send_abort(ClockworkClient* handle) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !c->pending.valid()) return;
#if !defined(__EMSCRIPTEN__)
    if (ShmPeerPlaneHeader* plane = c->plane()) {
        RingBufferWriter::abort(&plane->cmd_write_lock);
        c->pending = {};
        return;
    }
#endif
    RingBufferWriter::abort(&c->control()->in_write_lock);
    c->pending = {};
}

uint32_t clockwork_client_poll(ClockworkClient* handle,
                               ClockworkClientMessage* out, uint32_t max) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out || max == 0) return 0;
    if (c->reader()) return 0;   // nothing to take from: a reader's segment has no egress for it

#if !defined(__EMSCRIPTEN__)
    // The peer takes from its plane's reply ring. The frames are copied out
    // before the ring's tail moves: the plane has one tail and no separate
    // read cursor, so handing out pointers into the ring would let the
    // engine write over bytes the caller is still reading. The copies live
    // until the next poll, as the contract says. The metrics are the
    // engine's and this mapping of them is read-only, so nothing is counted
    // here; the engine counts its own side.
    if (ShmPeerPlaneHeader* plane = c->plane()) {
        c->peerBatch.clear();
        std::vector<std::array<uint32_t, 4>> heads;   // offset, length, origin, sequence
        clockwork_drain_ring(
            shm_peer_rep_ring(plane), SHM_PEER_REP_RING_SIZE,
            &plane->rep_head, &plane->rep_tail,
            c->peerDrain, ClockworkDrainMetrics{}, max,
            [&](uint32_t sourceId, const uint8_t* payload, uint32_t len, uint32_t seq) {
                heads.push_back({ static_cast<uint32_t>(c->peerBatch.size()), len, sourceId, seq });
                c->peerBatch.insert(c->peerBatch.end(), payload, payload + len);
                return ClockworkDrainVerdict::Consume;
            });
        for (size_t i = 0; i < heads.size(); ++i) {
            out[i].bytes    = c->peerBatch.data() + heads[i][0];
            out[i].length   = heads[i][1];
            out[i].origin   = heads[i][2];
            out[i].route    = CLOCKWORK_ROUTE_REPLY;
            out[i].sequence = heads[i][3];
        }
        return static_cast<uint32_t>(heads.size());
    }
#endif

    // The same counters the engine's own drain keeps: reply and notification
    // volume, and ring health. They live in the segment so an observer outside
    // this process can see them, which is why a client bumps them rather than
    // keeping a private tally.
    auto* pm = reinterpret_cast<PerformanceMetrics*>(c->base + c->L.metrics_offset);
    ClockworkDrainMetrics dm;
    dm.received  = &pm->osc_in_messages_received;
    dm.bytes     = &pm->osc_in_bytes_received;
    dm.corrupted = &pm->osc_in_corrupted;
    dm.seqGaps   = &pm->messages_sequence_gaps;

    // Egress frames are [route:u32][osc]; the route is peeled here so a
    // client sees the address it sent to, not a length-prefixed blob.
    ClockworkClientMessage* next = out;   // where the next message goes
    auto take = [&](uint32_t sourceId, const uint8_t* payload, uint32_t len, uint32_t seq) {
        if (len < EGRESS_ROUTE_SIZE) return ClockworkDrainVerdict::Consume;
        uint32_t route = 0;
        std::memcpy(&route, payload, sizeof(route));
        next->bytes    = payload + EGRESS_ROUTE_SIZE;
        next->length   = len - EGRESS_ROUTE_SIZE;
        next->origin   = sourceId;
        next->route    = route;
        next->sequence = seq;
        ++next;
        return ClockworkDrainVerdict::Consume;
    };
    const auto taken = [&] { return static_cast<uint32_t>(next - out); };
    // THE LAST BATCH'S SPACE GOES BACK FIRST, AND THIS ONE'S NOT YET. The
    // messages point into the ring, so each ring is read against its read
    // cursor, and its tail — the one the writer measures free space against
    // — is brought up to the read cursor only here, at the next poll. A writer
    // that fills the ring in between is refused, as any full ring refuses it,
    // rather than let on to bytes the caller is still reading. Both cursors
    // live in the ring's cursors, so a rebuild resets them together and
    // a client that comes back after a crash simply polls on.
    RingCursors* ctl = c->control();
    clockwork_ring_give_back(&ctl->out_tail, &ctl->out_read);
    clockwork_ring_give_back(&ctl->nrt_out_tail, &ctl->nrt_out_read);

    // TWO RINGS, ONE READ. The audio thread answers on the OUT ring; the NRT
    // gateway — the subsystems, the state changes, every broadcast — on the
    // NRT-out ring. A client does not care which; it takes from both, OUT
    // first, and a ring's sequence numbers stay its own.
    clockwork_drain_ring(
        c->base + c->L.out_ring_offset, c->L.out_ring_size,
        &ctl->out_head, &ctl->out_read,
        c->drain, dm, max, take);
    if (taken() < max && c->L.nrt_out_ring_size > 0)
        clockwork_drain_ring(
            c->base + c->L.nrt_out_ring_offset, c->L.nrt_out_ring_size,
            &ctl->nrt_out_head, &ctl->nrt_out_read,
            c->nrtDrain, dm, max - taken(), take);
    return taken();
}

void clockwork_client_count_undelivered(ClockworkClient* handle, uint32_t n) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || n == 0) return;
#if !defined(__EMSCRIPTEN__)
    if (c->mapping) return;   // a segment's metrics are the engine's: this mapping is read-only
#endif
    auto* pm = reinterpret_cast<PerformanceMetrics*>(c->base + c->L.metrics_offset);
    pm->egress_frames_undelivered.fetch_add(n, std::memory_order_relaxed);
}

ClockworkStatus clockwork_client_region(ClockworkClient* handle, ClockworkRegionId id,
                                        ClockworkRegion* out) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out) return CLOCKWORK_E_ARG;

    switch (id) {
        case CLOCKWORK_REGION_METRICS:
            *out = { c->base + c->L.metrics_offset, c->L.metrics_bytes, 0 };
            return CLOCKWORK_OK;
        case CLOCKWORK_REGION_WINDOW:
            *out = { c->base + c->L.window_offset, c->L.window_bytes, 0 };
            return CLOCKWORK_OK;
        case CLOCKWORK_REGION_TAPS:
            *out = { c->base + c->L.tap_offset,
                     c->L.tap_header_bytes + c->L.tap_max * c->L.tap_slot_bytes, 0 };
            return CLOCKWORK_OK;
        case CLOCKWORK_REGION_SCOPE:
            *out = { c->base + c->L.audio_offset,
                     c->L.audio_slot_count * c->L.audio_slot_bytes, 0 };
            return CLOCKWORK_OK;

        // The bulk lanes (docs/SEGMENTS.md). The peer sees them in its command
        // segment, at offsets its header names; a handle in the engine's own
        // process finds them where the arena's table says they are. The
        // assets area is the one region a client WRITES: it is the client's
        // lane, and clockwork_asset_pool.h is how a client carves it. A
        // reader's segment has no lanes and a reader may not have them.
        case CLOCKWORK_REGION_ASSETS:
#if !defined(__EMSCRIPTEN__)
            if (c->commands) {
                *out = { c->commands->get_assets(),
                         static_cast<uint32_t>(c->commands->get_assets_size()), 1 };
                return CLOCKWORK_OK;
            }
            if (c->mapping) return CLOCKWORK_E_PERM;
#endif
            return laneInProcess(c->base, CLOCKWORK_AREA_ASSETS, 1, out);
        case CLOCKWORK_REGION_RETURNS:
#if !defined(__EMSCRIPTEN__)
            if (c->commands) {
                *out = { const_cast<uint8_t*>(c->commands->get_returns()),
                         static_cast<uint32_t>(c->commands->get_returns_size()), 0 };
                return CLOCKWORK_OK;
            }
            if (c->mapping) return CLOCKWORK_E_PERM;
#endif
            return laneInProcess(c->base, CLOCKWORK_AREA_RETURNS, 0, out);

        // The rings. Writable names who may put bytes in: an in-process
        // client writes the ingress ring through clockwork_client_send, and
        // reads the egress one. The ingress ring is the engine process's: a
        // segment handle, reader or peer, is refused it.
        case CLOCKWORK_REGION_INGRESS:
#if !defined(__EMSCRIPTEN__)
            if (c->mapping) return CLOCKWORK_E_PERM;
#endif
            *out = { c->base + c->L.in_ring_offset, c->L.in_ring_size, 1 };
            return CLOCKWORK_OK;
        case CLOCKWORK_REGION_EGRESS:
            *out = { c->base + c->L.out_ring_offset, c->L.out_ring_size, 0 };
            return CLOCKWORK_OK;

        // What the device layer measures about its own callback. Written by
        // clockwork, read as a block like the metrics; a client draws a load
        // meter from it and a test reads overruns from it.
        case CLOCKWORK_REGION_NATIVE_STATS:
            *out = { c->base + c->L.native_stats_offset, c->L.native_stats_bytes, 0 };
            return CLOCKWORK_OK;
    }
    return CLOCKWORK_E_ARG;
}

uint32_t clockwork_client_metrics(ClockworkClient* handle, uint32_t* out, uint32_t max) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out || max == 0) return 0;
    const uint32_t have = c->L.metrics_bytes / 4u;
    const uint32_t n    = max < have ? max : have;
    const auto* src = reinterpret_cast<const std::atomic<uint32_t>*>(c->base + c->L.metrics_offset);
    for (uint32_t i = 0; i < n; ++i) out[i] = src[i].load(std::memory_order_relaxed);
    return n;
}

ClockworkStatus clockwork_client_clock(ClockworkClient* handle, ClockworkClientClock* out) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out || out->struct_bytes < sizeof(uint32_t)) return CLOCKWORK_E_ARG;

    const auto snap = readClockworkClock(
        reinterpret_cast<const ClockworkClockState*>(c->base + c->L.clock_state_offset));

    ClockworkClientClock v{};
    v.struct_bytes      = out->struct_bytes;
    v.bpm               = snap.bpm;
    v.beat_origin_ntp   = snap.beat_origin_ntp;
    v.is_playing_at_ntp = snap.is_playing_at_ntp;
    v.is_playing        = snap.is_playing ? 1 : 0;
    v.flags             = snap.flags;
    v.meter_num         = snap.meter_num;
    v.meter_den         = snap.meter_den;
    const size_t n = out->struct_bytes < sizeof(v) ? out->struct_bytes : sizeof(v);
    std::memcpy(out, &v, n);
    return CLOCKWORK_OK;
}

ClockworkStatus clockwork_client_sample_clock(ClockworkClient* handle,
                                              ClockworkClientSampleClock* out) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out || out->struct_bytes < sizeof(uint32_t)) return CLOCKWORK_E_ARG;

    const auto snap = read_sample_clock(c->base + c->L.sample_clock_offset);
    if (!snap.valid) return CLOCKWORK_E_ABSENT;

    ClockworkClientSampleClock v{};
    v.struct_bytes          = out->struct_bytes;
    v.sample_rate           = snap.sample_rate;
    v.engine_frames         = snap.engine_frames;
    v.dac_ntp               = snap.dac_ntp;
    v.output_latency_frames = snap.output_latency_frames;
    const size_t n = out->struct_bytes < sizeof(v) ? out->struct_bytes : sizeof(v);
    std::memcpy(out, &v, n);
    return CLOCKWORK_OK;
}

ClockworkStatus clockwork_client_metrics_history(ClockworkClient* handle, uint32_t level, uint32_t buckets,
                                                 uint32_t metric_offset, ClockworkClientMetricWindow* out) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out || out->struct_bytes < sizeof(uint32_t)) return CLOCKWORK_E_ARG;
    if (c->L.metrics_history_bytes < clockwork::metrics_history::kRegionBytes) return CLOCKWORK_E_ABSENT;

    const auto w = clockwork::metrics_history::window(c->base + c->L.metrics_history_offset, level, buckets, metric_offset);
    ClockworkClientMetricWindow v{};
    v.struct_bytes = out->struct_bytes;
    v.covered      = w.covered;
    v.low          = w.low;
    v.high         = w.high;
    v.last         = w.last;
    v.delta        = w.delta;
    const size_t n = out->struct_bytes < sizeof(v) ? out->struct_bytes : sizeof(v);
    std::memcpy(out, &v, n);
    return CLOCKWORK_OK;
}

ClockworkStatus clockwork_client_metrics_history_bucket(ClockworkClient* handle, uint32_t level, uint32_t back,
                                                        uint32_t metric_offset, ClockworkClientMetricBucket* out) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out || out->struct_bytes < sizeof(uint32_t)) return CLOCKWORK_E_ARG;
    if (c->L.metrics_history_bytes < clockwork::metrics_history::kRegionBytes) return CLOCKWORK_E_ABSENT;

    const auto b = clockwork::metrics_history::bucket(c->base + c->L.metrics_history_offset, level, back, metric_offset);
    ClockworkClientMetricBucket v{};
    v.struct_bytes = out->struct_bytes;
    v.recorded     = b.recorded ? 1u : 0u;
    v.low          = b.low;
    v.high         = b.high;
    v.last         = b.last;
    const size_t n = out->struct_bytes < sizeof(v) ? out->struct_bytes : sizeof(v);
    std::memcpy(out, &v, n);
    return CLOCKWORK_OK;
}

ClockworkStatus clockwork_client_audio_tap_open(ClockworkClient* handle, uint32_t slot,
                                            ClockworkAudioTapReader* out) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !out) return CLOCKWORK_E_ARG;
    uint8_t* at = c->L.tapSlotAt(c->base, slot);
    if (!at) return CLOCKWORK_E_ABSENT;

    out->struct_bytes = sizeof(ClockworkAudioTapReader);
    out->slot         = slot;
    out->_internal    = at;
    out->_ring_frames = c->L.tap_ring_frames;
    return CLOCKWORK_OK;
}

int clockwork_client_audio_tap_valid(const ClockworkAudioTapReader* r) {
    if (!r || !r->_internal) return 0;
    shm_tap_stream_reader rd(static_cast<shm_tap_stream*>(r->_internal), nullptr,
                               r->_ring_frames);
    return rd.valid() ? 1 : 0;
}

uint32_t clockwork_client_audio_tap_activations(const ClockworkAudioTapReader* r) {
    if (!r || !r->_internal) return 0;
    return shm_tap_stream_reader(static_cast<shm_tap_stream*>(r->_internal), nullptr,
                                   r->_ring_frames).activations();
}

uint64_t clockwork_client_audio_tap_audible_end(ClockworkClient* handle,
                                            const ClockworkAudioTapReader* r) {
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !r || !r->_internal) return 0;
    shm_tap_stream_reader rd(static_cast<shm_tap_stream*>(r->_internal), nullptr,
                               r->_ring_frames);
    // The AUDIBLE edge, not the write head: the engine writes a buffer ahead of
    // what has left the speakers, so a window ending at the write position
    // shows audio nobody has heard yet and the drawing runs ahead of the sound.
    return read_sample_clock(c->base + c->L.sample_clock_offset).audible_end(rd);
}

uint32_t clockwork_client_audio_tap_read(ClockworkClient* handle, const ClockworkAudioTapReader* r,
                                     uint64_t end, uint32_t frames,
                                     float* out, uint32_t* out_channels) {
    (void)handle;
    if (!r || !r->_internal || !out || frames == 0) {
        if (out_channels) *out_channels = 0;
        return 0;
    }
    shm_tap_stream_reader rd(static_cast<shm_tap_stream*>(r->_internal), nullptr,
                               r->_ring_frames);
    if (!rd.valid()) {
        // The slot is not live — never claimed, or released since the reader
        // was opened. The ring still holds whatever was last written, and the
        // header promises silence rather than that: `out` is always fully
        // written, so a caller never draws a dead slot's last window.
        const uint32_t channels = rd.channels();
        if (out_channels) *out_channels = channels;
        std::memset(out, 0, static_cast<size_t>(frames) * channels * sizeof(float));
        return 0;
    }
    return rd.copy_window(end, frames, out, out_channels);
}

double clockwork_client_beat_at(const ClockworkClientClock* clock, double ntp_seconds) {
    if (!clock) return 0.0;
    return clockwork::beatAt(ntp_seconds, clock->beat_origin_ntp, clock->bpm);
}

double clockwork_client_ntp_at_frame(const ClockworkClientSampleClock* clock, uint64_t frame) {
    if (!clock || clock->sample_rate == 0) return 0.0;
    // The distance as an exact integer first: two frame counts near 2^53
    // would lose it as doubles.
    const double frames = frame >= clock->engine_frames
                              ? static_cast<double>(frame - clock->engine_frames)
                              : -static_cast<double>(clock->engine_frames - frame);
    return clock->dac_ntp + frames / static_cast<double>(clock->sample_rate);
}

// ── Watching a ring ─────────────────────────────────────────────────────────
//
// A tap walks the same frames the consumer walks, with the same code, and the
// only difference is whose cursor moves. The ring's real cursor is left alone;
// the tap keeps its own and hands the drain a local atomic standing in for it.
//
// Being lapped is ordinary here, so the drain's resynchronise-to-head recovery
// is the wanted behaviour rather than an error path: the writer overwrote what
// this reader had not reached, and the newest data is what remains.

struct TapImpl {
    ClientImpl* client = nullptr;
    uint8_t*    buffer = nullptr;
    uint32_t    size   = 0;
    std::atomic<int32_t>* head = nullptr;
    int32_t     cursor = 0;
    bool        peelRoute = false;   // egress frames carry a route word
    bool        owns_storage = true;
    uint64_t    missed = 0;
    ClockworkDrainState drain{};
};

size_t clockwork_client_tap_sizeof(void) { return sizeof(TapImpl); }

namespace {

// Point a tap at a ring and start it wherever the writer has got to.
bool tapBind(TapImpl* t, ClientImpl* c, uint32_t ring) {
    if (ring == CLOCKWORK_REGION_INGRESS) {
        t->buffer    = c->base + c->L.in_ring_offset;
        t->size      = c->L.in_ring_size;
        t->head      = &c->control()->in_head;
        t->peelRoute = false;
    } else if (ring == CLOCKWORK_REGION_EGRESS) {
        t->buffer    = c->base + c->L.out_ring_offset;
        t->size      = c->L.out_ring_size;
        t->head      = &c->control()->out_head;
        t->peelRoute = true;
    } else {
        return false;
    }
    t->client = c;
    t->cursor = t->head->load(std::memory_order_acquire);
    return true;
}

} // namespace

ClockworkClientTap* clockwork_client_tap_open_in(void* storage, size_t storage_bytes,
                                                 ClockworkClient* handle, uint32_t ring,
                                                 ClockworkStatus* out_status) {
    const auto fail = [out_status](ClockworkStatus s) -> ClockworkClientTap* {
        if (out_status) *out_status = s;
        return nullptr;
    };
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c || !storage || storage_bytes < sizeof(TapImpl))
        return fail(CLOCKWORK_E_ARG);

    auto* t = new (storage) TapImpl();   // NOLINT(cppcoreguidelines-owning-memory): the caller's storage
    if (!tapBind(t, c, ring)) {
        t->~TapImpl();
        return fail(CLOCKWORK_E_ARG);
    }
    t->owns_storage = false;
    if (out_status) *out_status = CLOCKWORK_OK;
    return reinterpret_cast<ClockworkClientTap*>(t);
}

ClockworkClientTap* clockwork_client_tap_open(ClockworkClient* handle, uint32_t ring,
                                              ClockworkStatus* out_status) {
    const auto fail = [out_status](ClockworkStatus s) -> ClockworkClientTap* {
        if (out_status) *out_status = s;
        return nullptr;
    };
    auto* c = reinterpret_cast<ClientImpl*>(handle);
    if (!c) return fail(CLOCKWORK_E_ARG);

    std::unique_ptr<TapImpl> t(new (std::nothrow) TapImpl());
    if (!t) return fail(CLOCKWORK_E_NOMEM);
    if (!tapBind(t.get(), c, ring)) return fail(CLOCKWORK_E_ARG);
    if (out_status) *out_status = CLOCKWORK_OK;
    return reinterpret_cast<ClockworkClientTap*>(t.release());
}

void clockwork_client_tap_close(ClockworkClientTap* handle) {
    gsl::owner<TapImpl*> t = reinterpret_cast<TapImpl*>(handle);   // NOLINT(cppcoreguidelines-owning-memory): the caller's handle, given back here
    if (!t) return;
    if (t->owns_storage) delete t;
    else                 t->~TapImpl();
}

uint32_t clockwork_client_tap_poll(ClockworkClientTap* handle,
                                   ClockworkClientMessage* out, uint32_t max) {
    auto* t = reinterpret_cast<TapImpl*>(handle);
    if (!t || !out || max == 0) return 0;

    // The ring's cursor stays where it is; this one stands in for it, so the
    // drain advances the tap and nothing else.
    std::atomic<int32_t> cursor{t->cursor};

    // Only the gap counter is wanted. The engine's message and byte counters
    // belong to whoever actually consumed the frame, and a watcher adding to
    // them would report traffic that never happened.
    std::atomic<uint32_t> gaps{0};
    ClockworkDrainMetrics dm;
    dm.seqGaps = &gaps;

    uint32_t n = 0;
    clockwork_drain_ring(
        t->buffer, t->size, t->head, &cursor, t->drain, dm, max,
        [&](uint32_t sourceId, const uint8_t* payload, uint32_t len, uint32_t seq) {
            const uint32_t skip = t->peelRoute ? EGRESS_ROUTE_SIZE : 0u;
            if (len < skip) return ClockworkDrainVerdict::Consume;
            uint32_t route = 0;
            if (t->peelRoute) std::memcpy(&route, payload, sizeof(route));
            out[n].bytes    = payload + skip;
            out[n].length   = len - skip;
            out[n].origin   = sourceId;
            out[n].route    = route;
            out[n].sequence = seq;
            ++n;
            return ClockworkDrainVerdict::Consume;
        });

    t->cursor = cursor.load(std::memory_order_relaxed);
    t->missed += gaps.load(std::memory_order_relaxed);
    return n;
}

uint64_t clockwork_client_tap_missed(const ClockworkClientTap* handle) {
    auto* t = reinterpret_cast<const TapImpl*>(handle);
    return t ? t->missed : 0;
}

}  // extern "C"
