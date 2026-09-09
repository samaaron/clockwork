<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2025-2026 Sam Aaron -->
# Command transports

How OSC commands reach clockwork, and how replies get back.

**One at a time.** The server installs a single transport at startup
(`engine.setTransport`). Passing more than one of the flags below is an error,
not a merge:

```
ERROR: pick at most one of --tcp / --uds / --uds-dgram / --pipe / --shm-commands
```

Choosing an alternative *replaces* the UDP command port. The cue server and
outbound OSC are unaffected, and `-u` still numbers the shared-memory segment.

| flag | transport | shape |
|---|---|---|
| *(none)* | UDP on `-u <port>` | datagram, network |
| `--tcp <port>` | TCP, length-prefixed OSC (respects `-B`) | stream, network |
| `--uds <path>` | Unix socket, file mode 0600 | stream, local |
| `--uds-dgram <path>` | Unix socket, file mode 0600 | datagram, local |
| `--pipe <name>` | Named pipe, owner-only DACL, no remote clients | stream, local |
| `--shm-commands` | the segment's peer command plane | ring, local |

`--uds` and `--uds-dgram` are macOS and Linux; `--pipe` is Windows. They are
the same idea — local IPC addressed by a path rather than an IP — on the two
platform families.

Both socket files are created `0600`. There is a bind-then-chmod window, so put
the socket in a `0700` directory rather than relying on the mode alone. The
named pipe is built with an SDDL DACL admitting only SYSTEM and the current
user's SID, and rejects remote clients.

**Stream or datagram** is the other axis, and it decides what the transport has
to do. A stream is a connection carrying a byte sequence, so it needs an accept
loop, an admission cap (`--max-connections`, default 4, clamped to 1-1024) and
length-prefixed
framing to find message boundaries. A datagram arrives already delimited: no
connection, no accept, no framing, and `--max-connections` means nothing. TCP,
UDS-stream and the named pipe share `StreamOscTransport` for that reason; UDP
and UDS-dgram do not.

## The transports are a client library, not the engine's

An engine has no sockets. It has one reply door, `IOscTransport`, and the
in-process `CallbackTransport` behind it by default — which is all the
worklet and the NIF ever have. The transports described in this file live in
`src/comms/` (`clockwork_comms.h`, CMake target `clockwork_comms`, Rust crate
`clockwork-comms`) and are linked by a HOST that wants a wire: SuperSonic's
own main (clockwork-supersonic/host/), a GUI that embeds the engine and opens
a port for its language server. The origin registry and the
notify audiences live there too, on the client's side of the door, because
"who is on the other end" was never the engine's question.

That is the same arrangement as the web, where the JavaScript client owns the
wire and the worklet knows nothing of it. A host that embeds clockwork in its
own process needs none of this: it calls the client API directly and the lane
is plain memory.

The Rust half of the library is carried in the native umbrella under the
`comms` cargo feature, so a host linking `clockwork_comms` finds the symbols;
an engine-only build leaves the feature off and has no socket transport at
all. What stays in the engine is what the ENGINE needs a socket for: the cue
server and outbound OSC, in `clockwork-osc-net`.

## UDP is the default, and forgiving

A UDP bind failure is logged but does not stop the server — deliberately, for
scsynth compatibility. UDP also starts receiving *before* the engine is
initialised and queues what arrives, flushing the queue once boot completes, so
nothing a client sends during a slow device open bounces as ICMP — a device
open can take many seconds.

Every other transport starts *after* `clockwork_init`, because the ingress ring
has to exist before a packet can be written to it.

## --shm-commands

Not a socket: a command ring inside the shared-memory segment, for one trusted
co-located peer. It needs `-u > 0` — not to bind anything, but because a
non-zero port is what makes the engine create the segment. The peer reaches
the segment the way every reader does, through the attach endpoint below.

It is also the one transport with no receive wiring in the host: the engine's
control pass drains the plane's command ring — on the engine's gateway thread,
or the host's own when the host drives control — rather than a recv thread
calling in.

## The shared-memory attach endpoint

The segment itself has no name. The engine creates it anonymously (`memfd` on
Linux, an unlinked POSIX object on macOS, an unnamed file mapping on Windows),
so nothing can open it by guessing, nothing another user can plant stands in
its way, and nothing is left behind when the engine dies — the pages go with
the last mapping. A reader is *given* the segment instead: it connects to the
attach endpoint and receives a duplicate of the engine's handle, as `SCM_RIGHTS`
ancillary data over a Unix socket, or, on Windows, duplicated straight into
the client process over a named pipe (`shm_attach.hpp`).

That is a separate endpoint from the command transport because only a local
socket or pipe can carry an OS handle. A host that talks TCP to its engine
(Sonic Pi's daemon does, so the same code path serves every platform) still
needs its GUI to reach the segment, and the GUI is not the process that
connected.

`--shm-endpoint <path|pipe>` names it. Unset, it is derived from the port:
`$XDG_RUNTIME_DIR/clockwork-shm-<port>.sock`, else `$TMPDIR/…`, else a
uid-tagged path under `/tmp`; on Windows `\\.\pipe\clockwork-shm-<port>`. Both
sides check the peer's uid (owner-only DACL on Windows), so a reader that only
knows the port still meets its own engine and nobody else's. The C client API
does the whole hand-off: `clockwork_client_open_shm(endpoint)`, with
`clockwork_client_default_endpoint(port)` for the derived name.

## Origins

Every command carries an **origin** in its ring `Message` header, and a reply is
addressed to it. `dsp_api.h` calls it `origin`; the well-known constants are
named `*_ORIGIN_TOKEN`.

The transport mints it on the way in and resolves it on the way out — same
object, both ends:

* the socket transports intern the sender's `(ip, port)` to a stable token
  (`OriginTable`), so a token survives a scheduled event's delay rather than
  churning under traffic;
* the shm plane uses one constant, `SHM_PEER_ORIGIN_TOKEN`, because it has
  exactly one peer;
* `0` is the in-process caller, and broadcast when replying.

Nothing between those two points inspects the value: `OscEgress::dispatchEgress`
takes a `uint32_t` and hands it to `IOscTransport::send`. It is a **reply
address, not a client identity** — a single client needs it just as much as
several would, because the answer still has to find its way back.

A DSP therefore keeps no client registry: it echoes back the `origin` it was
given, and clockwork routes the reply.
