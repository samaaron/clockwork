# The two segments

How a process other than the engine reaches its memory, and what it may do
there. Natively the engine shares two anonymous segments, each with one
job, and hands them out by role over the attach endpoint
(`src/shm_attach.hpp`). This document is the contract; `src/shm_segment.hpp`
and `src/shm_attach.hpp` implement it, and the tests named at the end hold
it.

## The two segments

| segment | holds | who writes | who maps it | how many |
|---|---|---|---|---|
| **published** | the segment header and the arena blob (`shared_memory.h`): clock, metrics, scopes, taps, node tree, the guest's window, the engine's rings and cursors | the engine, and nobody else | every reader, **read-only**: a GUI, a clock reader, a monitor, a test | any number |
| **command** | the peer plane (`shm_peer_plane.h`: a command ring and a reply ring) and the guest's bulk lanes (the assets area a client fills, the returns area the guest fills) | one peer and the engine, one writer per ring and per lane | **one peer**, read-write | one at a time |

The published segment exists whenever the engine has a segment at all
(`Config::shmSegment`, `-u > 0` on the server). The command segment exists
only when the host asked for the shared-memory command plane
(`Config::shmCommands`, `--shm-commands`). Without that flag there is no
command segment: nothing to hand out, no memory spent, and the bulk lanes
live in the engine's own process as they do with no segment.

The web has one process and one trust domain, so it has one arena and no
segments; embedded targets have no OS segment. Nothing here changes either.

## Why two

One segment with the plane inside it made every reader a potential command
writer. `clockwork_client_send` on any attached handle wrote the arena's in
ring and took its `in_write_lock`, the spinlock the engine's own transports
take; a reader that died holding it stalled the engine's ingest. Two pollers
moved one shared read cursor and took each other's replies. "One command
peer" and "one writer per ring" were comments.

Now a reader cannot write (the kernel refuses the mapping), a second peer
cannot attach (the server refuses the hand-off), and the arena's rings are
written only inside the engine's process on every host. The properties the
crash-safety story rests on — one producer per ring, no shared lock the
engine can be stalled on — no longer depend on every attacher behaving.

## Read-only, enforced

The engine keeps a read-write descriptor of the published segment for itself
and makes a **read-only descriptor of the same object** to hand out:

| platform | how |
|---|---|
| Linux | `memfd_create`, then `open("/proc/self/fd/<fd>", O_RDONLY)` |
| macOS | `shm_open` the throwaway name a second time with `O_RDONLY`, then unlink |
| Windows | `DuplicateHandle` with `FILE_MAP_READ` as the access |

A process holding the read-only descriptor can map the segment `PROT_READ`
(`FILE_MAP_READ`) and nothing else: `mmap` with `PROT_WRITE` fails with
`EACCES`, `MapViewOfFile` with write access fails, and `mprotect` cannot add
what the descriptor does not grant. The reader's class (`shm_segment_client`)
maps read-only and exposes only `const` views. An in-process reader
(a test, an embedding host's GUI) gets the same read-only descriptor from
`shm_segment_creator::reader_handle()`.

## The attach protocol

One connection, one segment. The client connects to the endpoint and sends:

1. the auth frame: a 4-byte big-endian length, then `/clockwork/auth` with the
   32-byte per-boot secret as a blob (56 bytes) — the same first frame the
   stream transports take;
2. the **role**, one 4-byte big-endian word: `1` reader, `2` peer.

The server answers with one `hello`, 40 bytes, native byte order:

```
u32 magic       "CWAT"
u32 version     4
u64 size        the segment's byte length, 0 when nothing is handed over
u64 handle      Windows: the duplicated HANDLE's value in the client; POSIX: 0
u32 role        the role granted, as asked
u32 status      0 ok · 1 no command segment · 2 held · 3 unknown role
u64 wake_event  a peer on Windows: the engine's wake event, duplicated into the client; 0 elsewhere
```

| the client asked for | the server |
|---|---|
| anything, with a wrong or missing auth frame | closes without a hello within 2 s, and counts it (`refused`): a secret probe learns nothing |
| reader | sends `hello` with the **read-only** descriptor alongside (`SCM_RIGHTS`; on Windows the handle duplicated into the client), then closes |
| peer, on a server with no command segment | sends `hello` with status 1 and no descriptor, then closes |
| peer, while another peer holds the command segment | sends `hello` with status 2 and no descriptor, then closes |
| peer, otherwise | sends `hello` with the **read-write** command descriptor alongside, and **keeps the connection open: it is the lease** |
| an unknown role | sends `hello` with status 3, then closes |

Role refusals are told apart from secret refusals on purpose: a client with
the secret is trusted and gets the reason; a client without it gets nothing.
They are counted separately (`declined`).

### The lease

The peer's attach connection stays open for as long as it holds the command
segment. Closing it — on purpose, or because the process died — is the
release, and the next peer's attach succeeds. No pid is tracked and nothing
polls: the kernel closes a dead process's sockets. The server reads nothing
from the lease and the peer sends nothing on it.

The peer plane's own attach (`shm_peer_attach`: pid stamp, generation bump,
writer-lock reset, stale replies skipped) stays as it is and runs after the
hand-off; the lease is what guarantees it runs with no live producer.

### Waking the engine

The engine's control pass sleeps until something wakes it; it has no timer.
The command segment's header names a wake word (`wake_offset`, a
`ClockworkWaitWord`: a 32-bit value and a 32-bit waiter count, 8 bytes). A
peer adds one to the value after each write to the command ring and wakes
the address: `futex(FUTEX_WAKE)` on Linux, `os_sync_wake_by_address_all`
with the shared flag on Apple platforms. Windows has no wait on an address
across processes, so the engine creates an auto-reset event with the
segment and the hello carries a duplicate the peer may only signal
(`EVENT_MODIFY_STATE`). The peer skips the system call when the waiter count
is zero: the engine is mid-pass and reads the new value before it sleeps
again.

A peer that writes and does not wake still has its command read, at the
next audio block, but no sooner.

The engine holds a peer's command back while the reply ring lacks room for
its answers. Before it does, it sets `rep_room_wanted` in the plane header. A
peer looks at that word after each read that moves `rep_tail`, and when it is
set, clears it and wakes the engine as above. A peer that sends more than its
reply ring holds, then only reads, gets every answer with no audio block.

### Clients

A peer needs the published segment too (the clock, the metrics, the arena's
table), so it attaches twice: once as a reader, once as a peer. The C ABI
does both in one call:

| call | gets | `send` / `poll` | `region(ASSETS/RETURNS)` |
|---|---|---|---|
| `clockwork_client_open_shm(endpoint, secret)` | the published segment, read-only | `CLOCKWORK_E_PERM` | `CLOCKWORK_E_PERM` |
| `clockwork_client_open_shm_handle(handle)` | the same, from a descriptor already received | `CLOCKWORK_E_PERM` | `CLOCKWORK_E_PERM` |
| `clockwork_client_open_shm_peer(endpoint, secret)` | both segments; the lease is held until `close` (`CLOCKWORK_E_BUSY` while another peer holds it, `CLOCKWORK_E_ABSENT` where there is no plane) | the peer plane's command ring and reply ring | the command segment's lanes |
| `clockwork_client_open_shm_peer_handles(reader, command, wake_event)` | the same, from two descriptors already received (and, on Windows, the wake event); no lease | as above | as above |
| `clockwork_client_open_memory(base, bytes)` | the arena in this process | the arena's rings, as before | absent |

Taps, metrics, metrics history, the clock, the sample clock and the scope
and window regions are reads and work on every handle. `region(INGRESS)` is
refused on both segment handles: the arena's in ring is the engine
process's. A segment handle counts nothing in the metrics (`poll`,
`count_undelivered`): they are the engine's, and the mapping is read-only.

In C++, `shm_attach::receive` is the reader's hand-off and
`shm_attach::receive_command_plane` the peer's; the latter returns a
`command_lease` that holds the plane until it is released or destroyed. The
mappings are `shm_segment_client` (read-only, `const` views) and
`shm_command_client` (the plane and the lanes).

The Rust `clockwork-client` crate mirrors the three opens, and the Rust
`clockwork_comms::shm::ShmPeer` (the transport probe's peer) speaks the same
protocol for the command segment alone, holding its lease for the life of
the mapping.

## The arena's table

The arena's table (`clockwork_arena.h`) still names the bulk lanes as areas
outside the arena. Their *space* says where: `CLOCKWORK_SPACE_COMMAND_SEGMENT`
(an offset from the command segment's base) when the command segment exists,
`CLOCKWORK_SPACE_PROCESS` when the lanes are the engine's own memory, and
`CLOCKWORK_SPACE_UNKNOWN` when there are none. A reader that sees
`COMMAND_SEGMENT` and is not the peer cannot reach them, which is the point.

## Invariants

1. A reader cannot write the published segment. Kernel-enforced on every
   native platform.
2. The command segment exists only with `Config::shmCommands`; asking for it
   otherwise is declined with status 1.
3. At most one peer holds the command segment; its connection is the lease;
   a dead peer's lease is released by the kernel.
4. Nothing outside the engine's process writes the arena's rings or takes
   `in_write_lock`.
5. The web arena and the embedded profiles are unchanged by any of this.

## Tests

- `test/test_shm_segment.cpp` — two creators, two headers; the reader handle
  is read-only (a writable mapping is refused); the lanes are in the command
  segment and disjoint; the arena's table says where.
- `test/test_shm_attach.cpp` — the protocol: role word, 40-byte hello, a
  reader's descriptor is read-only, a peer's is not; status 1, 2 and 3; the
  lease released by close; secret refusals still silent and counted
  separately; a peer's command answered with no audio block, an idle engine
  running no pass, and a peer that sends more than its reply ring holds,
  then only reads, answered in full.
- `test/test_shm_peer_crash.cpp` + `test/peer_shm/main.cpp` — a peer killed
  mid-traffic releases the lease and the next peer attaches.
- `test/test_client_abi.cpp` — `send`/`poll`/`region` on a reader handle are
  `E_PERM`; a peer handle round-trips through the plane.
