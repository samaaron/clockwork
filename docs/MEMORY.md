<!-- SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial -->
<!-- Copyright (c) 2026 Sam Aaron -->
# All of clockwork's memory

Clockwork's memory, as one model: what every area is, who writes and
reads it, how fast it is, how long it lives. Proposed 2026-10-14,
implemented 2026-10-14 and 15; this is the record.

Part 1 is what existed before, on every platform, from a read of the code
with every claim cited. Part 2 is what was wrong with it. Part 3 is the
model. Part 4 is what was done, step by step, with what each step found.

Sizes are hand-computed from the constants, not measured.

## Part 1: what exists

### 1.1 The platforms

| platform | process model | memory | status |
|---|---|---|---|
| **web** | one wasm linear memory in an AudioWorklet, shared with the page and workers as a SharedArrayBuffer (or private to the worklet in postMessage mode) | one tier; only the top of memory can grow | shipping (Sonic Pi Web) |
| **native** | the engine's process; other processes map a shared segment when a UDP port is set | one tier; virtual memory | shipping (Sonic Pi) |
| **plugin** (clockwork inside a DAW, `clockwork_embed_attach`) | the host's process; one engine per process | one tier | API and tests only; no product wraps it |
| **embedded** (ESP32-S3, Teensy 4.1) | one process, no OS segment | **two tiers** on the ESP32 (512 KiB internal SRAM, 8 MiB PSRAM); Teensy has fast DTCM and slower OCRAM, which clockwork treats as one tier | profiles and allocator exist; **no embedded build of clockwork exists in the tree**. The out-of-tree ESP32 and Teensy hosts run the old SuperSonic engine. Nothing below is proven on a device. |

### 1.2 Every area, on every platform

"Engine thread" is whichever thread calls `process_audio`: the device
callback, the worklet, the DAW's process thread, or an embedded task.

| area | holds | writer → reader | web (Sonic Pi Web) | native (Sonic Pi) | plugin (attach) | embedded (ESP32) |
|---|---|---|---|---|---|---|
| **arena** `ring_buffer_storage` | the table, state, scope, taps, message rings, guest window, config, persist (1.3) | per region | 6.4 MB static in the wasm data segment, seen by every JS thread | 52 MiB; in the segment when `udpPort > 0`, else static | 52 MiB static; no segment, so no other process can see it | 205 KB static, the whole block in PSRAM (`COLD_BSS`) |
| **scheduler store** | timed messages | engine thread | 790 KB static | 594 KB static | 594 KB | 135 KB static, in **internal SRAM** (no `COLD_BSS`, though the comments say otherwise) |
| **audio staging** `static_audio_bus/in` | the block's channel buffers | engine thread | 2 × 64 KB | 2 × 512 KB | 2 × 512 KB | 2 × 512 B |
| **clockwork heap** (TLSF pool, `rust/clockwork-heap`) | everything the guest takes through `DspHost::alloc_bytes`, plus clockwork's own | engine thread, lock-free; foreign threads get system memory | 76 MiB, carved from the placement arena; may grow **once**, by 4 MiB, inside it | 64 MiB from `malloc` + a 16 MiB spare, re-armed by the NRT thread | 64 MiB + 16 MiB | 80 KiB from fast RAM, clamped; a 256 KiB spare from PSRAM, re-armed never (no NRT thread) |
| **placement arena** `clockwork::mem` | the heap's backing | engine thread | 80 MiB, one `malloc` at first boot (sized by SuperSonic from scsynth's `-m` + 16 MiB) | none (`set_arena` is web-only) | none | none |
| **guest working memory** `DspConfig::arena` | whatever the guest carves at `dsp_new`; zeroed every rebuild | guest only | 4 MiB, a fixed slice of wasm memory; **unused** by SuperSonic (its pool doesn't fit) | 32 MiB `new[]`, always, **not** in the segment; holds scsynth's 8 MiB pool, 24 MiB idle | NULL unless the guest declares a want; scsynth declares 0 | from fast RAM only if the guest declares a want; else NULL (the 64 KiB default in the profile is never read) |
| **guest bulk memory** `memory_bulk` | cold guest state | guest only | NULL | NULL | NULL | 2 MiB from PSRAM, always |
| **assets** | client → guest bulk: **the sample pool** | client writes; guest reads in place; **and writes** (1.4) | 4 MiB at boot, grows in 32 MiB steps to 768 MiB (96 MiB on a phone); the top of wasm memory | 512 MiB of address space, launch-sized (clockwork's default is 8 MiB); in the segment, or `new[]` without one | **none**; an asset commit is refused | none unless the host passes one |
| **returns** | guest → client bulk | guest writes; client reads | 4 MiB, **unused** | 8 MiB; `/b_write` staging only | none | none |
| **segment header + peer plane** | where the blob is; two OSC rings for one trusted peer | creator; peer ↔ NRT thread | — | 256 B + 393 KB, only with `udpPort > 0` | — | — |
| **plugin-track bridge segment** | send/return audio rings, three message rings, a 4 MB JSON mirror | engine ↔ the bridge child process | — | 5.3 MB, one named segment per engine pid | present but silent (no lanes reserved) | — |
| **asset table** | id → committing client | engine thread | 48 KB static | 48 KB | 48 KB | 48 KB |
| **system malloc** | Rust sink queues and port rings, `ClockworkProcessor` buffers, the C client handle, **scsynth's synthdefs** (`new` on the engine thread) — *as surveyed; step 6 in Part 4 moved the queues to the rust heap and the synthdefs to the clockwork heap* | various | yes | yes | yes | yes, **not tier-routed** |

Total committed at boot, Sonic Pi Web: 118 MiB, of which the arena is 6.4,
the heap 76, the guest slice 4, the returns area 4 and the assets area 4. Everything
that fills up in a session (samples) lands in the assets area.

### 1.3 Inside the arena

| region | bytes web / native | owner | audience today | touched |
|---|---|---|---|---|
| header + table | 4 KB | creator | all | boot |
| metrics | 208 | clockwork | published | **every block** |
| native stats | 24 | clockwork | published | per callback (never on the web) |
| clock anchors | 16 | host | published | read every block |
| clock state | 40 | clockwork | published + guest | every change |
| sample clock | 32 | clockwork | published | **every block** |
| channel map | 6 KB | clockwork | published + guest | hot half read at use |
| scope | 750 KB / 3 MB | clockwork | published | **every block** |
| track taps | 0 / 16 MiB | clockwork | published | every block, native with tracks |
| control (ring cursors, ingress lock, status flags) | 56 | clockwork + clients | transport | **every block** |
| node id counter | 4 | clockwork's JS client | transport | per channel |
| in ring | 1 MiB / 768 KB | clients | transport | **every block** |
| out ring | 128 KB | clockwork | transport | **every block** |
| NRT out ring | 64 KB | clockwork | transport | control rate |
| guest window | 96 KB | guest | published | per node-tree change |
| taps | 4 MiB / 32 MiB | guest (really clockwork, 1.4) | published | **every block** with scopes |
| guest config | 1 KB | host | guest + host | boot |
| guest persist | 64 KB | guest | guest | rare |

Natively 98% of the arena is the three audio rings. The table, clocks and
message rings are about 1 MB.

### 1.4 Where SuperSonic's things actually are

| thing | web | native |
|---|---|---|
| scsynth's real-time pool (synth and FX state, LocalBuf, node graphs) | clockwork heap: the guest slice is too small | guest working memory |
| a loaded sample (`/b_allocRead`) | assets, written by the page, bound by `/b_allocPtr`, read in place | assets, written by the front, bound by an asset commit, read in place |
| an empty buffer (`/b_alloc`: recording, delay lines) | **assets**: JS rewrites `/b_alloc` to `/b_allocPtr` (sent through `sendOSC` or in a bundle it isn't rewritten and lands on the heap) | **clockwork heap** |
| `/b_read`, `/b_write` | fail | work, staged through the assets area/returns |
| synthdefs | system `malloc`, on the engine thread | same |
| node-tree mirror | arena: guest window | same |
| scopes | arena: taps, written by clockwork through `tap_write` | same |
| the notify table | arena: guest persist | same |
| recording | reads the scope from the page (SAB only) | the front's recorder thread reads the scope |

Every buffer in the assets area is **written by the guest**: `/b_zero`, `/b_set`,
`/b_fill`, `/b_gen`, `RecordBuf`, `BufWr`, FFT and the rest. SuperSonic
casts the `const` away (`SC_MiscCmds.cpp:219`). On the web that is every
buffer; natively only loaded samples. BOUNDARY.md says the guest must copy
before it mutates; nothing in SuperSonic says it doesn't.

scsynth's "non-real-time" command stages all run on the engine thread
(`mRealTime = false` → `CallEveryStage`), so a `/b_alloc` zeroes its whole
buffer inside the tick, and `/b_zero` or `/b_gen` on a large assets buffer
is bounded only by the buffer's size. Nothing is time-sliced.

### 1.5 Platform differences that nothing forces

The platforms must differ in these ways: wasm grows only at the top and
has no processes; a DAW owns the audio thread and allows one engine per
process; the ESP32 has two speeds of RAM. These differences are not forced
by anything:

| difference | web | native | stated reason |
|---|---|---|---|
| where `/b_alloc` goes | assets, via a JS rewrite | heap | none for the divergence |
| where the RT pool lives | heap | guest memory | falls out of the sizes |
| how a sample is bound | `/b_allocPtr` with a guarded range | asset commit with a payload offset | "the web client's spelling" |
| guard frames | every buffer | loaded samples only | none |
| assets sizing | grows 4 → 768 MiB | 512 MiB reserved up front | none; native reserves address space too |
| pool bookkeeping | inside the assets area (SAB) | the client's own memory | none |
| `/b_read`, `/b_write` | fail | work | none; the lanes exist on the web |
| who intercepts buffer verbs | `send()` only | every transport | — |
| after a rebuild | buffers restored | all slots freed | — |
| `/supersonic/buffer/freed` | carries a raw pointer | the same, a process address sent to clients | — |

### 1.6 The names

- **"arena"** means five things: the shared span, the guest's working
  memory, the placement span, the scheduler's store, and two `clockwork_init`
  parameters that mean two of those.
- **"lanes"** means four: the ABI, the assets area and returns, reserved channels,
  and the bridge's audio channels.
- **"tap"** names the scope in `CLOCKWORK_TAP_OUT/IN` and `CLOCKWORK_TAP_CHANNELS`,
  and the tap streams everywhere else.
- **"assets"** is a place samples live for as long as a buffer does, not a
  place things arrive.
- **"heap"** is clockwork's pool on every platform, and its file says "the
  native growable heap".
- **"embedded"** means a microcontroller in `memory_profile.h` and a
  plugin or app in `clockwork_embed.h`, ARENA.md and THREADS.md.
- **`SHM_`** prefixes regions that are not shared on three of four platforms.

### 1.7 Findings outside the layout

Recorded here so they aren't lost; not part of the proposal.

- The heap **does** grow on the engine thread (it adopts a pre-armed spare)
  while `lanes.h` and `dsp_api.h` say it never does. On the ESP32 the spare
  is PSRAM, and a hot pool that overflows into it is the measured
  distortion case (`memory_profile.h:86-90`).
- The web's one boot-time check compares `sbrk` against the guest slice,
  not the returns area below it, so later `malloc` can overrun the returns area
  unchecked. Nothing checks after boot.
- A second `clockwork_embed_attach` is refused; the header promises it joins.
- `clockwork_heap_free` of a pointer the guest got from the assets area writes a
  link word into the assets area and later `free()`s it. Only a comment stops it.
- The web's two growth sites test `grow() === -1`; the API throws.
- The ESP32 fast-arena default is never read; the scheduler sits in SRAM
  against the comment; track taps take a third of the embedded arena
  against the comment; `CLOCKWORK_RT_POOL_GROWTH_SIZE` has no reader.

## Part 2: what is wrong

1. **There is no model.** Each area was added with its own vocabulary and
   its own rule for who may touch it. Only the arena's regions say who
   writes and who reads. The guest's memory, the assets area, the returns area, the
   heap, the bridge segment and the statics say nothing.
2. **The biggest memory is the least described.** The assets area holds what
   fills a session and has three jobs at once (sample store, every web
   buffer, native staging) and two writers, against its documented rule.
3. **The guest's memory is wrong-sized on every platform.** Natively 32 MiB
   with 24 idle; on the web 4 MiB, idle; nothing on the plugin path. The
   pool it exists for lives in clockwork's heap instead, which is sized for
   it by a guest-specific calculation in SuperSonic's JS.
4. **Speed is not an axis.** On the ESP32 the arena's placement is one
   attribute on one array, and the hot regions in it (metrics, cursors,
   rings, scope) go to slow RAM. The Teensy's two speeds are invisible.
5. **Web and native diverged where they need not** (1.5), so the same
   `/b_alloc` lands in different memory with different rules, and the
   web guest writes memory that the native rule says it may not.
6. **The names don't say what things are** (1.6).

## Part 3: the model

### 3.1 Every region carries five facts

Whatever the area, the same questions, and the answers go in a table a
tool can read, not in comments:

| fact | values | today |
|---|---|---|
| **owner**: who writes | clockwork, guest, client, host | arena only |
| **audience**: who reads | public (any client, by hand), transport (client code only), guest, host | arena only |
| **cadence**: how often the engine thread touches it | every block, on change, boot | nowhere |
| **tier**: what speed it needs, derived from cadence | fast, bulk | one attribute per static array |
| **lifetime**: when it is reset | never, cold boot, rebuild, each call | comments |

Growth is a sixth, per area rather than per region: fixed, or reserved
and committed on demand.

### 3.2 The areas, named for what they hold

Seven kinds of memory, on every platform. The names are the ones a
memory map shows and the code uses; the old names are in brackets.

| area | what it is | owner → audience | tier | grows |
|---|---|---|---|---|
| **clockwork-state** | metrics, stats, clock anchors, clock state, sample clock, channel map | clockwork/host → public | fast | no |
| **clockwork-audio** | scope (device out and in), taps (guest and track slots in one region) | clockwork → public | fast | no |
| **clockwork-transport** | ring cursors, node id counter, in ring, RT out ring, NRT out ring | clients/clockwork → transport | fast | no |
| **guest-public** | the guest window | guest → public | bulk | no |
| **guest-private** | config, persist, **and the guest's working memory** (today's `DspConfig::arena`) | guest (config: host) → guest | working memory: fast; persist: bulk | no |
| **assets** | bulk the client hands the guest, read in place | client → guest, **read-only** | bulk | reserved, committed on commit |
| **returns** | bulk the guest hands the client | guest → client | bulk | no |
| **guest-memory-fast** [`DspConfig::memory`] | the guest's fast memory: its hot per-block state, laid out by hand at `dsp_new` | guest → guest | fast | no |
| **guest-memory-bulk** [`DspConfig::memory_bulk`] | the guest's bulk memory: everything it allocates as it runs, through clockwork's allocator (a Rust guest's Box and Vec by themselves) | guest, through the pool → guest | bulk | once, by the spare |
| **static-data**, **stack**, **malloc** | the program's own: its statics (natively the arena inside), its stack, what `malloc`/`new` took (on the web the engine's memory inside); placed by the web, unknown natively until taught | the program | — | malloc: by sbrk |
| **rust-heap** | one block the RT router serves: what clockwork's Rust subsystems allocate inside an engine entry point (a sink's or a port's queues); taken once at boot, for the life of the process | clockwork | fast | no |
| **engine-memory** | on the web, the one block the arena and all of the above are carved from | clockwork | — | no |

The first five are the arena today, and stay one span with one table.
`assets`, `returns` and the guest's two spans are outside it on every platform (the
segment natively, slices of wasm memory on the web). The table gains
entries for them too, so a client finds every area the same way, by id,
and the map draws all of memory from one table.

### 3.3 The three unifications

**A. One place for a buffer.** An empty buffer (`/b_alloc`) comes from the
guest's bulk memory on every platform (step 12; when this was written, from
the engine heap, as native did then). Then the assets area
has one writer, the client, and the `const` in `DspConfig` becomes true:
the guest never writes an asset. A loaded sample that a synth then records
into would have to be copied to the guest's memory first, which SuperSonic doesn't do
today; that is the cost, and BOUNDARY.md already requires it.

The web rewrite (`/b_alloc` → `/b_allocPtr`) goes, and with it the
divergence in replies, completion messages, guard frames and ordering.
`/b_read` and `/b_write` then work on the web through the same staging
native uses.

**B. One place for the guest's pool.** The guest declares its working
memory (`memory_bytes_wanted`), scsynth from `-m`, and clockwork gives it
exactly that, from fast RAM, on every platform. SuperSonic's JS stops
sizing clockwork's heap for it. The engine heap shrinks to what is
actually allocated at run time (buffers, World tables, synthdefs) and is
sized by the host, not by one guest's arithmetic.

**C. One way to bind.** A sample is an asset on every platform: the client
writes it, commits it by offset, the guest reads it in place and releases
it. `/b_allocPtr` and `/supersonic/buffer/freed` (which sends a raw
pointer) go. The web client takes clockwork's asset pool, which exists but
is not compiled for wasm today, and keeps its bookkeeping in its own
memory as the native front does.

### 3.4 Tiers, modelled

Tier follows cadence, per region, and the table records it. On the ESP32:

- fast: clockwork-state, clockwork-transport, clockwork-audio's scope, the
  scheduler store, the staging buses, the guest's working memory;
- bulk: taps, the guest window, persist, assets, the guest's bulk memory and the spare.

Which means the arena stops being one static array with one placement
attribute, and becomes a fast part and a bulk part, each placed by its
tier. On desktop, web and plugin there is one tier and the labels change
nothing, but they are stated, and the map shows them.

The Teensy's DTCM and OCRAM are two tiers by this definition; today the
profile calls it single-tier. A later decision.

### 3.5 The names, in code

- The shared span keeps **arena**. The other four uses are renamed:
  `DspConfig::arena` → `DspConfig::memory` (the guest's working memory);
  the placement span → `clockwork::mem` pool; the scheduler's store →
  store; the init parameters follow.
- Regions are named for their half, as Sam proposed: `clockwork-metrics`,
  `clockwork-scope`, `clockwork-taps`, `clockwork-in-ring`,
  `clockwork-rt-out-ring`, `clockwork-nrt-out-ring`, `clockwork-ring-cursors`,
  `guest-window`, `guest-config`, `guest-persist`, `guest-memory`;
  outside the arena `assets`, `returns`, `heap`. C spells them
  `CLOCKWORK_ARENA_CLOCKWORK_TAPS`; JS and Rust `CLOCKWORK_TAPS`.
- **published** → **public**. **lanes** stays for the ABI only. **tap**
  stops naming the scope. **`SHM_`** goes. **"embedded"** means a
  microcontroller; the in-process API is "embed".
- Ids in memory order, format version 2, as the arena v2 work had it.

### 3.6 What this costs

- clockwork: the table (new fields, new entries), the allocator (the
  guest's memory sized by want on every platform), the arena split by
  tier, the web layout (`/b_alloc` on the heap, asset pool compiled for
  wasm), the renames, every reader (C, JS, Rust), tests, docs.
- SuperSonic: declare the pool want; drop the `/b_alloc` rewrite,
  `/b_allocPtr` and the raw-pointer reply; bind through assets on the web;
  copy before recording into a loaded sample; the front unchanged.
- string_lid: the tools read the new table; nothing inferred.
- Measurements before sizing anything: how much of the 64 MiB pool a
  heavy Sonic Pi session uses; whether the boot-time memsets commit the
  tap pages; the cost of a 20 MB assets copy on the worklet in postMessage
  mode.

## Part 4: what was done

Sam approved the model on 2026-10-14 and the five steps were taken as one
change, in this order. The decisions in the earlier Part 4 were taken as
recommended, with one change at step 5, noted there.

1. **Renames.** The arena v2 work folded in (ids in memory order, one name
   per region, format version 2), then: raw scope → **scope**, out ring →
   **RT out ring**, control → **ring cursors** (`RingCursors`, the global
   `cursors`), published → **public**, the block and the guest region →
   **clockwork's half** and **the guest's half**, every region named for its
   half (`clockwork-metrics`, `guest-window`; in C `CLOCKWORK_ARENA_CLOCKWORK_METRICS`).
2. **The table.** The header carries an **area table** (the five areas in
   the arena as offsets; assets, returns and the heap placed in a *space*
   when the engine knows where they are; the guest's working memory listed
   beside them so a map can draw it) and every region entry carries the
   **five facts**: owner, audience, cadence, tier, lifetime. The taps are one
   region in clockwork's half (the guest's slots, then the track slots). The
   node id counter is owned by the clients. `clockwork_arena_check` refuses a
   table whose areas and facts disagree. ARENA.md is this table's contract.
3. **The pool.** `DspConfig::arena` is `DspConfig::memory`, the guest's
   working memory, and clockwork reserves it from its fast tier on every
   platform, sized to the guest's static want plus what its configuration
   adds (`dsp_memory_bytes`, new; scsynth: its real-time pool from `-m`).
   The native engine's fixed 32 MiB block and the web layout's fixed 4 MiB
   slice are gone (`guestMemorySize` defaults to 0), and SuperSonic's
   `dsp_heap_bytes` asks only for what the heap holds beyond the pool.
4. **Buffers and binding.** `/b_alloc` goes to the engine as sent on every
   platform, from the heap. A sample is an **asset** everywhere: the web
   client stages it in the assets area and sends `/clockwork/asset/commit`, hears
   `/clockwork/asset/committed` and `/released`; `/b_allocPtr` and the
   raw-pointer reply `/supersonic/buffer/freed` are gone. `/b_read` and
   `/b_readChannel` work on the web through `/supersonic/buffer/read`, as
   natively; `/b_write` publishes the frames and hands the product a WAV
   (`'buffer:written'`). The guest never writes an asset.
5. **Tiers.** Every region's tier is in the table. The arena stays **one
   span, in fast RAM** on a tiered device: its hot regions are what matter,
   and a hot region in PSRAM was the measured distortion case. The tiered
   profiles keep the arena's bulk-tier regions small instead (no track-tap
   slots, a 4 KB persistent store), so the whole span fits beside the heap
   and the audio task's stack. The guest's working memory is fast, its bulk
   span PSRAM, the heap's spare PSRAM, as before. A true split of the arena
   into two spans would use the area table's *space* (an area placed in the
   process rather than at an arena offset) and change every reader's
   address arithmetic; not done, because no embedded build exists to measure
   it against and the arena on a device is about 80 KB.

6. **Nothing on the engine thread reaches malloc** (added after the first
   map of a Sonic Pi Web boot showed scsynth's synthdefs and the Rust sink
   and port queues in plain `malloc`, outside every area). The RT router
   (`rust/clockwork-heap/src/router.rs`, written for this and never
   installed) is now `clockwork-native`'s `#[global_allocator]`; the host
   arms it at heap init on one block from the fast tier
   (`CLOCKWORK_ROUTER_BYTES`: 1 MB on desktop and the web, 64 KB on the
   ESP32-S3, none on the Teensy), listed as the **rust-heap** area; the sink
   and port FFI entry points enter engine scope, so their queues come from
   it. scsynth's synth definitions (`SC_GraphDef.cpp`: every array, the param
   spec table and its storage) and the piano wavetable come from the guest's
   heap (`supersonic_guest_alloc_bytes`) instead of `new`. The program's own
   three areas — **static-data**, **stack** (on the web the audio worklet's:
   the worklet instantiates the module), **malloc** (to the frontier at the
   end of boot, the guest built) — are placed by a wasm engine so a map
   draws them; a native engine says unknown until taught.

7. **Every byte of boot accounted for.** Measuring malloc's frontier at each
   stage of boot found a regression from step 6: on the web the router's
   block came out of the placement arena the heap's spare was reserved
   from, so the spare no longer fit and the heap had lost its one growth.
   The router's block is the arena's first allocation now, taken before the
   heap is sized, so the heap sees what is left and the reserve is the
   spare's alone (a reserve that counted the router as well left a megabyte
   of the arena free after the spare). The spare is listed as
   the **heap-spare** area. What `dsp_new` grew the frontier by was listed
   for an afternoon as a "guest-malloc" area and taken out again: an area
   named for a measurement, not for what it holds. Measuring it (652 KB,
   576 KB of them floats that never change) found scsynth's FFT window and
   cosine tables, built once by `InitializeFFTTables()`; those are static
   storage now, so the symbol map names them, and what `dsp_new` still
   takes with plain malloc is a few small objects.
   The heap's and the rust heap's census — bytes in use, the peak, growths,
   fallbacks — are metrics 53–57 (`js/lib/metrics_schema.js`, the slots the
   buffer pool's metrics once held), written at
   the metrics flush, so a map reads the heap's use beside its size.
8. **Static data, by symbol.** The web build writes the linker's map beside
   the module (`CLOCKWORK_WEB_LINK_MAP`) and `scripts/static-map.mjs` reduces
   it to `<module>.symbols.json`: the sections and every symbol in rodata,
   data and bss, demangled. A memory map reads it beside the arena table and
   names static memory as the compiler did; the table says nothing about it,
   since it is not clockwork's.

9. **The guest's fixed state is its working memory.** Sam, seeing scsynth's
   sine and FFT tables named in static data: "it really feels like the
   implementation of the guest is starting to bleed out to the clockwork dev
   tools". The rule was already the model's — a guest owns its half of the
   arena, its working memory and what it takes through `alloc_bytes` —
   and scsynth broke it with ~800 KB of globals and load-time mallocs. Its
   lookup tables (the sine tables, the polar LUTs, the FFT windows and
   cosine tables: `scsynth_tables.cpp`) are now built at the start of its
   working memory on every `World_New`, and `dsp_memory_bytes` adds
   `SCSYNTH_TABLES_BYTES` for them. `dsp_api.h` states a guest's memory in
   three sentences. Clockwork cannot stop a guest keeping globals or calling
   malloc, and such a guest runs; what it can do is construct its own layout
   (next) and count what a guest does outside the doors. Moving the tables
   found a reload bug on the web: a start-over of the placement arena
   (`Clockwork#reload`) forgot the heap but kept the guest's span, so the
   rebuilt heap's backing was handed the arena's start, where the guest's
   memory already was; the two overlapped, and the first heap allocation
   in `World_New` trapped. A start-over now forgets the owned spans too,
   and the RT router's block — process-lifetime, in that arena — is the
   arena's first allocation, taken again first after a reset and checked to
   be where it was.

10. **The guest's mallocs, counted.** The ~30 KB left in the malloc box
    after step 9 was scsynth's library registration: `new` inside
    `dsp_new`, once per process, kept forever. Routing a guest's raw mallocs
    into the heap, as first proposed, would have put once-per-process state
    in a pool a cold swap tears down — so the honest version is to count,
    not route. Every call into the guest is now marked (`guest_malloc.h`:
    `GuestCall::Boot` around `dsp_describe`, `dsp_memory_bytes`, `dsp_new`
    and `dsp_free`; `GuestCall::Processing` around `dsp_process`, `dsp_osc`
    and `dsp_asset`), and where the link lets clockwork stand between the
    program and its allocator — wasm-ld, `-Wl,--wrap=malloc,calloc,realloc,
    free` (`CLOCKWORK_WEB_COUNT_GUEST_MALLOC`, on) — every allocator call
    made while a guest call is open is counted as the guest's, host
    callbacks included. Sized by `malloc_usable_size`, so the number is what
    the allocator gave. Two metrics (58, 59; the context's slots moved up
    two, `GUEST_METRICS_BASE` 71): **guestMallocBytes**, what the guest
    holds from malloc now, and **guestMallocsProcessing**, allocator calls
    inside the processing entry points since boot — the number a
    well-behaved guest keeps at 0. A verbose boot prints "guest malloc at
    boot: N calls, F freed, B bytes held". Where the link cannot wrap
    (Apple's ld) nothing is counted and both stay 0 by absence, which the
    schema says. Nothing changes what any allocation does. Measured on
    SuperSonic the first time: scsynth holds 74,512 bytes from malloc after
    boot, makes 3 mallocs on the audio thread for every synthdef it loads
    (`/d_recv`, inside `dsp_osc`) and none for a note; a cold swap leaves
    15.5 KB more held than before it.

11. **The engine's memory is one block.** On the web the arena was static
    (`arena_storage`, 6 MB of `.bss`) and the placement arena a malloc at
    boot; the reload bug in step 9 was that shape failing, with spans
    remembered across a start-over. Now the worklet claims one block before
    anything else is allocated (`clockwork_memory_claim(memArenaSize)`, the
    first thing after the linker's heap base; `get_arena_base` claims the
    default size for a caller that skips it): the arena is its first
    `ARENA_SIZE` bytes and the placement arena the rest, so the RT router's
    block, the guest's working memory, the heap and its spare are carved
    from the same block on every boot, in the same order. A reload finds
    the block where it was (the globals survive the new module) and carves
    it again; nothing has to be remembered. The table lists it as
    **`CLOCKWORK_AREA_ENGINE_MEMORY`** (15), placed on every boot, and
    `STATIC_DATA` on the web now holds only the program's globals. Natively
    nothing moved: `arena_storage` stays static and `clockwork::mem` stays
    the system allocator, reached through `clockwork_local_arena()`; the
    tiered profiles keep the arena in fast RAM by construction. Giving a
    desktop engine the same one block is a change of its own, with the
    heap's growth past the spare to decide first. Found on the way: a
    `clockwork_boot_fail` is invisible to a web page. The first build left
    `clockwork_memory_claim` out of the module's export list, the worklet
    skipped it, the default 32 MB was claimed, and Sonic Pi Web's 64 MB
    pool did not fit — the engine recorded the failure and the page showed
    an engine that never ticked, with nothing in its log. The export is in;
    the worklet now refuses a module without it; surfacing a recorded boot
    error to the page is still to do.

12. **The guest's memory is two spans, one per speed, and nothing else.**
    Sam, on the heap: "I don't understand why we need separate guest memory
    and buffer memory segments" — and there was no reason beyond history:
    clockwork was where the allocator was, so the guest's run-time
    allocations went to a region clockwork owned and named as its own. Now a
    guest gets exactly two spans from clockwork (`dsp_api.h`): `memory`,
    fast, which it lays out by hand (its hot per-block state), and
    `memory_bulk`, bulk, which clockwork runs its allocator over
    (`src/clockwork_pool.h`, the pool crate the heap was) and from which
    everything the guest allocates as it runs comes. Both are given on every
    host — a single-RAM host hands two slices of one block, so a guest is
    written once and only the sizes change per target — and each is sized
    as the larger of what the guest declares (`memory_bytes_wanted` /
    `memory_bulk_bytes_wanted`, plus `dsp_memory_bytes` /
    `dsp_memory_bulk_bytes` for what its configuration adds; `dsp_heap_bytes`
    is gone) and the host profile's default (`CLOCKWORK_GUEST_MEMORY_FAST_BYTES`,
    0 everywhere but the ESP32's 80 KB; `CLOCKWORK_GUEST_MEMORY_BULK_BYTES`,
    64 MB on a desktop, what is left of the engine's memory on the web, 2 MB
    of PSRAM on the ESP32; a host may raise the bulk floor with
    `clockwork_set_guest_bulk_bytes`). A want that does not fit refuses the
    boot with both figures. The bulk span may grow once, into the spare
    (`CLOCKWORK_GUEST_SPARE_BYTES`, the heap's growth block renamed), never on
    the audio thread. The C doors `alloc_bytes` / `free_bytes` stay and now
    mean the guest's own span. A Rust guest needs no door: clockwork's Rust
    global allocator (the RT router, `rust/clockwork-heap/src/router.rs`)
    serves every allocation made while a guest entry point is open from the
    guest's pool, through hooks the pool installs at boot and a guest scope
    `guest_malloc.h`'s call scope opens, so a Rust guest's `Box` and `Vec`
    land in its own memory by writing ordinary Rust; an allocation the pool
    cannot meet falls back to the system and is counted
    (`clockwork_router_guest_fallback_allocs`). Areas 8 and 14 are
    **`GUEST_MEMORY_BULK`** and **`GUEST_SPARE`**, 9 is
    **`GUEST_MEMORY_FAST`**; metrics 53–55 are the guest's pool's census
    (`guestPoolUsedBytes`, `guestPoolPeakBytes`, `guestPoolGrowths`). The
    web client sizes the engine's memory as the guest's two declared spans
    plus the engine's own overhead beside them (`MemoryLayout.engineOverhead`:
    the router's block, the spare, the headers), since a declared bulk want
    is now checked rather than taken from whatever the heap had left.
    SuperSonic: `dsp_memory_bulk_bytes` declares the 16 MB headroom
    `dsp_heap_bytes` did, its doors are unchanged, and scsynth's pool and
    tables stay in the fast span. What a guest's memory story reads like
    now: you get a fast span and a bulk span; say how much of each you
    need; hot things fast, by hand; everything else from the allocator,
    which in Rust is just `Box`; samples arrive read-only; there is nothing
    else.

13. **The engine's memory is one block natively too.** A desktop engine
    took the guest's two spans and the RT router's block as separate
    mallocs; now it claims one block (`g_engine_block`, the web's name)
    sized to exactly the build's two spans — the fast span, then the bulk
    span at its figure — and cuts it in two by hand. `clockwork::mem` is
    not involved: natively it stays the system allocator, as it always was.
    (A first version installed the block as the placement arena, as the
    web does; a test fixture that boots one engine for the whole process
    then had every later test's "system" allocation come out of the
    engine's block and freed as the system's. CI's one-process shuffled
    run and its TSan job found it on the first push, 2026-10-15, and a
    test now holds the allocator to the system while an engine runs and
    after it goes.) Two things stay out of the block, each for a reason
    the model states:
    the arena, which natively is static storage or a shared-memory segment
    other processes map, and the guest's memory must not be in a segment
    any process can reach; and the router's block, which is the process's
    for life while the block is a build's — a build whose guest wants more
    than the block holds takes a new one (everything in it is per-build and
    already gone) and the router's block stays where it was. The table
    lists it as `ENGINE_MEMORY` on every host; a tiered device has real
    regions to place into and no block. The last structural difference
    between the web and native layouts is gone: what differs now is where
    the arena lives, and that is forced.

14. **scsynth's three mallocs per synthdef, and its 15.5 KB per reload.**
    The census (step 10) counted three malloc calls on the audio thread
    for every synthdef loaded. They were three temporary `std::vector`s in
    the parser (`SC_GraphDef.cpp`: the index map `calcParamSpecs` sorts and
    the two stacks of the buffer colouring), on the system allocator; now
    on the guest's heap through a standard allocator (`GuestAllocator`),
    and the count is 0 per synthdef and per note. The 15.5 KB held more
    after every reload was not a leak in scsynth's accounting: a web reload
    never calls `dsp_free` (the old engine is gone, not torn down), so an
    instance's per-instance system allocations — the definition table, the
    client lists, the allocator object, the random generators, the two
    locks, the instance struct, the node tree's index arrays — were never
    freed, and they summed to exactly the figure. The clean fix is the
    model's: everything an instance owns is made in the guest's own memory
    (`dsp/scsynth/guest_heap.h`: `guest_new`, `guest_new_array`,
    `GuestAllocator`, `GuestMalloc`, one header the server's files share),
    so abandoning the guest's memory reclaims the instance by construction
    and nothing of it is ever on the system allocator. Held from malloc
    after boot is now the once-per-process library registration alone, and
    a reload adds nothing.

15. **A refused boot reaches the page.** The engine records why a build has
    no guest (`clockwork_boot_error`) and goes on answering its own verbs,
    so the arena is up, the metrics show an engine that never ticks, and
    the log line went to a console the page cannot see. The worklet now
    reads the recorded reason right after `clockwork_init` and posts it to
    the client as the init's error, so `init()` rejects with the engine's
    own words ("the engine refused to boot: FATAL: guest wants …") and the
    client is left as it was before init. The export is in the module's
    list, the worklet refuses a module without it, and the boot spec boots
    the dummy into a block too small for its fast want and reads the
    reason back.

16. **Assets and returns, in code.** The two bulk lanes kept their old
    names in code (`DspConfig::inbox`, `writeInbox`, `CLOCKWORK_REGION_INBOX`,
    `maxInboxSize`, SuperSonic's `supersonic_inbox_contains`) while the
    table called the areas assets and returns, so the map said one thing
    and the code another. Renamed throughout clockwork, SuperSonic,
    string_lid's tools and Sonic Pi Web's one option (`maxAssetsSize`):
    `DspConfig::assets` and `returns`, `writeAssets`, `readReturns`,
    `growAssets`, `memory.assetsSize` / `maxAssetsSize` / `returnsSize`,
    the region ids, the segment's lane geometry, the OSC-side helpers. In
    prose "the inbox" is "the assets area" and "the outbox" "the returns
    area". Nothing published is a contract yet (README); the names now say
    what the areas hold.

17. **Every malloc after boot, counted.** Sam: "shouldn't malloc take
    nothing over a long session — shouldn't all memory allocations be with
    the rust allocator?" The honest answer was that nothing measured it, so
    the wrap that counts the guest's calls now counts every call, any
    scope, from the moment the engine is ready (metrics 60
    `mallocsAfterBoot`, 61 `mallocBytesAfterBoot`; the map's "unused" box
    reads them). The worklet's own scratch through `clockwork_worklet_alloc`
    (its client handle, poll array and tap, made once after the engine is
    ready) is the host's and left out. Measured on SuperSonic over a
    session of 250 notes, four synthdef loads, a sample load and a reload:
    **0 calls, 0 bytes**. Nothing on the engine side reaches malloc after
    boot, so the two allocators the model names — the rust heap for
    clockwork's Rust subsystems inside their doors, the guest's pool for
    the guest — are already the only two in use, and routing the rest of
    clockwork's C++ and Rust through the first (one scope around the whole
    engine, a C++ `new` override) would be machinery for traffic that does
    not exist. Not done, by measurement. What the count buys instead is a
    reading that stays 0 and says so the day something changes, and the
    ground to size the web's `wasmHeapSize` from what boot actually takes
    rather than from a budget for run-time malloc.

18. **The spare is gone.** Sam, on the map's "spare" and "unused": "just
    feels wrong". It was: a block kept armed so the guest's pool could grow
    once if the guest had declared too little, which is a hedge against the
    declaration being wrong, sat beside a model in which the declaration is
    the budget, and cost a block, an area, a knob, a growth path, a metric
    and a line on the map. Removed: the pool never grows, a pool that fills
    answers NULL loudly, and the fix is the guest's declaration or the
    host's floor, both of which exist. Area 14 is now `ENGINE_MEMORY`;
    metric 55 is a reserved pad; `CLOCKWORK_GUEST_SPARE_BYTES` is gone from
    every profile; the web's `engineOverhead` is 2 MB (the router's block
    and the headers). "Unused" stays for now: it is the web layout's
    `wasmHeapSize` budget for run-time malloc, which the count in step 17
    shows is nothing, and sizing it from what boot actually takes is a
    measurement Sam has deferred.

19. **What the pool made visible, and the tool for the next one.** The
    first push failed SuperSonic's macOS native jobs only: the one-process
    shuffled run aborted in `World_Cleanup` with "mutex lock failed:
    Invalid argument". A destroyed or zeroed pthread mutex still locks on
    Linux and refuses on macOS, which is why Linux, Windows and ASan were
    green on the same seed, and why ASan could not see it at all: the
    corruption was inside the guest's pool, which ASan does not watch. The
    World's locks now live in that pool, so a corruption of it surfaced at
    the lock rather than passing silently, as it had under the old heap.
    Hunting it: the pool refuses and counts a double free (none here);
    then a walk of the whole pool after every engine-thread operation
    (`CLOCKWORK_POOL_CHECK=1`, `clockwork_heap_pool_check`) stopped at the
    first broken header and named the allocation before it — scsynth's
    audio bus, 64 channels by the embedded test's option, under a host
    handing it 80 in and 80 out (lanes a previous test had reserved, which
    the fixture's 2-in 2-out could not know about), overrun by every block.
    scsynth now widens its bus to the host's channels and says so, as the
    contract asks ("size buffers against these in dsp_new"). Two guest
    globals that outlived their instance were tidied on the way (the piano
    table, the node tree's indices). The check stays, off by default, as
    the way to find the next one at its source.

**Still to measure:** how much of its pool a heavy Sonic Pi session uses;
whether the boot-time memsets commit the tap pages; the cost of a 20 MB
copy on the worklet in postMessage mode; the embedded profiles on a device.
