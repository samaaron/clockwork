<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2025-2026 Sam Aaron -->
# Command transports

How OSC commands reach clockwork, and how replies get back.

**One at a time.** An engine has one transport, installed by its host
(`ClockworkEngine::setTransport`). The flags below are SuperSonic's host's
(`host/EngineHost.cpp` in that repository), which takes exactly one of them
and refuses to start with none, or with more than one:

```
pick exactly one of --tcp / --uds / --pipe / --shm-commands
```

The cue server and outbound OSC are unaffected, and that host's `-u` still
numbers the shared-memory segment.

| flag | transport | shape |
|---|---|---|
| `--tcp <port>` | TCP, length-prefixed OSC (respects `-B`) | stream, network |
| `--uds <path>` | Unix socket, file mode 0600 | stream, local |
| `--pipe <name>` | Named pipe, owner-only DACL, no remote clients | stream, local |
| `--shm-commands` | the segment's peer command plane | ring, local |

There is no datagram command port. UDP and the Unix datagram socket were
removed because a datagram cannot carry the secret once per connection: every
datagram would have to carry it, and one captured datagram would give it
away. A client that spoke to scsynth over UDP does not reach clockwork.

**The stream transports ask for a secret.** TCP, the Unix stream socket and
the named pipe start only with the host's per-boot secret: 32 random bytes
that whoever launches the host gets to it by a path other processes cannot
read, never the command line or the environment; where the host reads them
is the host's decision (`clockwork_comms_secret_from_bytes`). A client's first
frame on a connection must be `/clockwork/auth` with the 32 bytes as a blob.
Nothing is delivered before it, and a connection that sends anything else
first, or nothing within 2 s, is closed and counted. The secret crosses TCP in
the clear, so TCP binds loopback unless `-B` names an address, and the log
says so once when it binds anything else.

The local transports are owner-only by file mode or DACL as well. `--uds` is
macOS and Linux; `--pipe` is Windows. They are the same idea — local IPC
addressed by a path rather than an IP — on the two platform families.

The socket file is created `0600`. There is a bind-then-chmod window, so put
the socket in a `0700` directory rather than relying on the mode alone. The
named pipe is built with an SDDL DACL admitting only SYSTEM and the current
user's SID, and rejects remote clients.

**One client.** A stream server accepts one connection that presented the
secret and one still presenting it; anything more is closed at accept, and
nothing displaces a connection, so an unauthenticated connection occupies the
server for 2 s at most and never the client's place. How many things stand
behind the one client is the host's. TCP, the UDS stream socket and the named
pipe share `StreamOscTransport`: an accept loop, that admission, and
length-prefixed framing to find message boundaries.

## The transports are a client library, not the engine's

An engine has no sockets. It has one reply door, `IOscTransport`, and the
in-process `CallbackTransport` behind it by default. The NIF puts its own
there, `NifTransport` (`src/nif/clockwork_nif.cpp`), which hands each reply to
the registered Erlang processes. The transports described in this file live in
`src/comms/` (`clockwork_comms.h`, CMake target `clockwork_comms`, Rust crate
`clockwork-comms`) and are linked by a HOST that wants a wire: SuperSonic's
own main (its `host/`), a GUI that embeds the engine and opens
a port for its language server. Which connection the client is lives there
too, on the client's side of the door, because "who is on the other end" was
never the engine's question. What the client asked to be told is the core's
to keep (`clockwork_client_wants`, `lanes.h`), the same on every host.

That is the same arrangement as the web, where the JavaScript client owns the
wire and the worklet knows nothing of it. A host that embeds clockwork in its
own process needs none of this: it calls the client API directly and the lane
is plain memory.

The Rust half of the library is carried in the native umbrella under the
`comms` cargo feature, so a host linking `clockwork_comms` finds the symbols;
an engine-only build leaves the feature off and has no socket transport at
all. What stays in the engine is what the ENGINE needs a socket for: the cue
server and outbound OSC, in `clockwork-osc-net`.

## Transports start after init

Every transport starts *after* `clockwork_init`, because the ingress ring has
to exist before a packet can be written to it.

## --shm-commands

Not a socket: a command ring in the engine's shared-memory COMMAND segment
(docs/SEGMENTS.md), for one trusted co-located peer. It needs `-u > 0` — not
to bind anything, but because a non-zero port is what makes the engine
create its segments — and the command segment exists only with this flag.
The peer reaches it through the attach endpoint below, as a peer; one holds
it at a time.

It is also the one transport with no receive wiring in the host: the engine's
control pass drains the plane's command ring — on the engine's gateway thread,
or the host's own when the host drives control — rather than a recv thread
calling in.

## The shared-memory attach endpoint

The engine has two segments (docs/SEGMENTS.md): the PUBLISHED one — the
arena: clock, metrics, scopes, taps, the node tree — which any number of
readers map read-only, and, with `--shm-commands`, the COMMAND one — the
peer plane and the bulk lanes — which one peer maps read-write. Neither has
a name. The engine creates them anonymously (`memfd` on Linux, an unlinked
POSIX object on macOS, an unnamed file mapping on Windows), so nothing can
open them by guessing, nothing another user can plant stands in their way,
and nothing is left behind when the engine dies — the pages go with the
last mapping. A client is *given* a segment instead: it connects to the
attach endpoint and receives a duplicate of the engine's handle, as
`SCM_RIGHTS` ancillary data over a Unix socket, or, on Windows, duplicated
straight into the client process over a named pipe (`shm_attach.hpp`). A
reader's duplicate is a READ-ONLY handle: the kernel refuses it a writable
mapping.

That is a separate endpoint from the command transport because only a local
socket or pipe can carry an OS handle. A host that talks TCP to its engine
(Sonic Pi's daemon does, so the same code path serves every platform) still
needs its GUI to reach the segment, and the GUI is not the process that
connected.

`--shm-endpoint <path|pipe>` names it. Unset, it is derived from the port:
`$XDG_RUNTIME_DIR/clockwork-shm-<port>.sock`, else `$TMPDIR/…`, else a
uid-tagged path under `/tmp`; on Windows `\\.\pipe\clockwork-shm-<port>`. Attaching
asks for the per-boot secret: a client's first frame on the endpoint is the
same auth frame the stream transports take, then a 4-byte role word (1 a
reader, 2 a peer). A client that sends anything else, or nothing within 2 s,
gets no hand-off and is counted. A peer where there is no command segment,
or while another peer holds it, gets a hello that says so and no segment;
a peer that is served keeps its connection open as the lease, and closing
it (or dying) releases the segment. Both sides also check the peer's uid
(owner-only DACL on Windows), so a client that only knows the port still
meets its own engine and nobody else's. The C client API does the whole
hand-off: `clockwork_client_open_shm(endpoint, secret)` for a reader,
`clockwork_client_open_shm_peer(endpoint, secret)` for the peer, with
`clockwork_client_default_endpoint(port)` for the derived name.

## The writer label

A ring frame's `Message` header has a `sourceId` field, and a frame written
into the ingress ring carries a **writer label** there: the last argument of
`clockwork_client_send` and `clockwork_ingress_write`. It is a label of the
sender's own choosing, stored in the frame's ring header for whoever watches
the ring (a tap, the web's traffic log). Clockwork never reads it, the guest
is not given it, and no answer carries it.

* the web labels each thread that writes the ingress ring (0 the main
  thread, 1 and up the workers), and its traffic log shows which thread sent
  what;
* the engine's own `ingest` writes 0;
* the shm plane labels the peer's frames `SHM_PEER_WRITER`, in the ingress
  ring and in the plane's reply ring;
* every frame on the egress rings carries 0.

An engine has one client, and everything on the egress is handed to it:
`IOscTransport` is one call, `deliver`. A client that must match an answer to
its request carries an id in its own OSC messages. A stream transport keeps
its client's connection id to itself.
