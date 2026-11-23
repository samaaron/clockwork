------------------------------ MODULE Ring ------------------------------
(* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial *)
(* Copyright (c) 2026 Sam Aaron                                              *)
(*                                                                           *)
(* A message ring with several writers and one reader, the word the reader   *)
(* sleeps on, and the handshake that pauses the reader. It restates, one     *)
(* shared-memory read or write to a step:                                    *)
(*                                                                           *)
(*   src/workers/RingBufferWriter.h   reserve, commit, abort, the lock       *)
(*   src/ring/ring.h                  ring_fits and the arithmetic under it  *)
(*   src/lanes/ring_drain.h           clockwork_drain_ring                   *)
(*   src/workers/wait_word.{h,cpp}    wait, bump, notify                     *)
(*   src/workers/RingReader.cpp       run, pause, resume, stop               *)
(*                                                                           *)
(* TLC tries every order of those steps. A change to one of those files is   *)
(* a change here.                                                            *)
(*                                                                           *)
(* WHAT IS TAKEN AS GIVEN, AND NOT CHECKED HERE.                             *)
(*                                                                           *)
(*  - Every step is seen by every thread at once. Whether the memory orders  *)
(*    in the code (relaxed, acquire, release) are strong enough for that is  *)
(*    not a question TLA+ can ask.                                           *)
(*  - No writer stops for good while it holds the lock (clockwork_client.h,  *)
(*    ONE CLIENT). One that did would stop the ring, by design.              *)
(*  - Nobody but the writers and the reader touches the ring's memory. What  *)
(*    the drain does with damaged bytes is the fuzzer's (test/fuzz).         *)
(*  - Nothing wakes the reader but a writer, a pause and a stop. The audio   *)
(*    thread's wake every block is left out, so that a wake lost here is     *)
(*    not hidden by the next block.                                          *)
(*                                                                           *)
(* SLOTS. The ring is Size slots of four bytes. A frame is a header slot     *)
(* and its payload slots; the real header is four slots, which changes no    *)
(* rule below. A slot holds a frame's header, a frame's payload, a padding   *)
(* marker, the zeros after one, or whatever was there before.                *)
EXTENDS Integers, Sequences, FiniteSets

CONSTANTS
    Size,        \* slots in the ring
    MaxLen,      \* the largest frame, in slots: at most half the ring (ring_max_message)
    Writers,     \* the threads that write
    Pausers,     \* the threads that pause the reader
    MaxWrites,   \* how many writes are begun, so the model ends
    MaxPauses,   \* how many pauses are begun
    Retains,     \* whether the reader's callback may leave a frame in the ring
    Timeouts,    \* whether a pause may give up waiting (its two-second deadline)
    Stops        \* whether the reader's thread is ever stopped

ASSUME /\ Size >= 2
       /\ MaxLen >= 1
       /\ MaxLen <= Size \div 2

VARIABLES
    \* The ring, in shared memory.
    lock, head, tail, seq, mem,
    \* The reader's wake word: its value, and how many wait on it.
    wake, waiters,
    \* The pause handshake, and the stop.
    pauseReq, parkedAt, stop,
    \* Each writer: where it is, the slots it reserved and will commit (0: it
    \* gives the reservation back), where its frame starts, the sequence it took.
    wpc, wres, wuse, woff, wseq, wid, started,
    \* The reader: where it is, the wake value it sleeps on, the head and the
    \* frame length it read, the pause request it read.
    rpc, rseen, rhead, rlen, rask,
    \* Each pauser: where it is, the request it made, whether the reader parked for it.
    ppc, pask, pgot, pauses,
    \* Whoever stops the reader.
    spc,
    \* Not in the code: every frame published and every frame consumed, in
    \* order; whether a writer wrote where the reader may read or the reader
    \* was handed part of a frame; whether the reader met a ring it calls damaged.
    log, delivered, torn, corrupt

ring    == <<lock, head, tail, seq, mem>>
writer  == <<wpc, wres, wuse, woff, wseq, wid, started>>
reader  == <<rpc, rseen, rhead, rlen, rask>>
pauser  == <<ppc, pask, pgot, pauses>>
ghosts  == <<log, delivered, torn, corrupt>>
vars    == <<ring, wake, waiters, pauseReq, parkedAt, stop, writer, reader, pauser, spc, ghosts>>

Slots == 0..(Size - 1)

Junk         == [kind |-> "junk"]
Zero         == [kind |-> "zero"]
Padding      == [kind |-> "padding"]
Header(n, s, id) == [kind |-> "header", len |-> n, seq |-> s, id |-> id]
Payload(id)  == [kind |-> "payload", id |-> id]

\* ring.h: ring_used_bytes, ring_free_bytes, ring_fits. A full ring keeps one
\* slot, and a frame lies in one piece: before the end, or at the front
\* before the tail.
Used(h, t)    == (h - t + Size) % Size
Free(h, t)    == Size - Used(h, t) - 1
Fits(h, t, n) == /\ n <= Free(h, t)
                 /\ \/ n <= Size - h
                    \/ n <= (IF t > 0 THEN t - 1 ELSE 0)

\* Whether slot `i` lies between the tail and the head: published, and not
\* yet given back.
Readable(i) == (i - tail + Size) % Size < Used(head, tail)

Init ==
    /\ lock = 0 /\ head = 0 /\ tail = 0 /\ seq = 0
    /\ mem = [i \in Slots |-> Junk]
    /\ wake = 0 /\ waiters = 0
    /\ pauseReq = 0 /\ parkedAt = 0 /\ stop = FALSE
    /\ wpc = [w \in Writers |-> "idle"]
    /\ wres = [w \in Writers |-> 0] /\ wuse = [w \in Writers |-> 0]
    /\ woff = [w \in Writers |-> 0] /\ wseq = [w \in Writers |-> 0]
    /\ wid = [w \in Writers |-> 0] /\ started = 0
    \* RingReader::start reads the wake word, then the thread begins.
    /\ rpc = "top" /\ rseen = 0 /\ rhead = 0 /\ rlen = 0 /\ rask = 0
    /\ ppc = [p \in Pausers |-> "idle"]
    /\ pask = [p \in Pausers |-> 0] /\ pgot = [p \in Pausers |-> FALSE] /\ pauses = 0
    /\ spc = "idle"
    /\ log = <<>> /\ delivered = <<>> /\ torn = FALSE /\ corrupt = FALSE

\* An empty ring takes the largest message, wherever its head is.
ASSUME EmptyTakesTheLargest == \A h \in Slots : Fits(h, h, Size \div 2)

--------------------------------------------------------------------------
\* The wake word (wait_word.cpp). A waker changes the value and then looks
\* at the count; with a waiter counted it makes the system call, which wakes
\* a reader that is asleep.

Bump     == wake' = wake + 1
WakeLook == rpc' = IF waiters # 0 /\ rpc = "asleep" THEN "wait_look" ELSE rpc

--------------------------------------------------------------------------
\* A writer (RingBufferWriter.h; in_ring_door.h for the reservation given back).

WStart(w) ==
    /\ wpc[w] = "idle" /\ started < MaxWrites
    /\ \E r \in 1..MaxLen : \E c \in 0..r :
          /\ wres' = [wres EXCEPT ![w] = r]
          /\ wuse' = [wuse EXCEPT ![w] = c]
    /\ started' = started + 1
    /\ wid' = [wid EXCEPT ![w] = started + 1]
    /\ wpc' = [wpc EXCEPT ![w] = "lock"]
    /\ UNCHANGED <<ring, wake, waiters, pauseReq, parkedAt, stop, woff, wseq, reader, pauser, spc, ghosts>>

\* lock(): the compare and exchange that succeeds.
WLock(w) ==
    /\ wpc[w] = "lock" /\ lock = 0
    /\ lock' = 1
    /\ wpc' = [wpc EXCEPT ![w] = "reserve"]
    /\ UNCHANGED <<head, tail, seq, mem, wake, waiters, pauseReq, parkedAt, stop,
                   wres, wuse, woff, wseq, wid, started, reader, pauser, spc, ghosts>>

\* reserve(): the head is the writers' own and they hold the lock; the tail
\* is read once. No room gives the lock back.
WReserve(w) ==
    /\ wpc[w] = "reserve"
    /\ IF ~Fits(head, tail, wres[w])
          THEN wpc' = [wpc EXCEPT ![w] = "unlock"] /\ UNCHANGED woff
          ELSE IF wres[w] > Size - head
                  THEN wpc' = [wpc EXCEPT ![w] = "pad"] /\ UNCHANGED woff
                  ELSE /\ wpc' = [wpc EXCEPT ![w] = "fill"]
                       /\ woff' = [woff EXCEPT ![w] = head]
    /\ UNCHANGED <<ring, wake, waiters, pauseReq, parkedAt, stop, wres, wuse, wseq, wid, started,
                   reader, pauser, spc, ghosts>>

\* reserve(), at the end of the ring: a padding marker and zeros to the end,
\* written before the head moves, and the frame restarts at slot 0.
WPad(w) ==
    /\ wpc[w] = "pad"
    /\ mem' = [i \in Slots |-> IF i = head THEN Padding ELSE IF i > head THEN Zero ELSE mem[i]]
    /\ torn' = (torn \/ \E i \in Slots : i >= head /\ Readable(i))
    /\ woff' = [woff EXCEPT ![w] = 0]
    /\ wpc' = [wpc EXCEPT ![w] = "fill"]
    /\ UNCHANGED <<lock, head, tail, seq, wake, waiters, pauseReq, parkedAt, stop,
                   wres, wuse, wseq, wid, started, reader, pauser, spc, log, delivered, corrupt>>

\* The caller writes its payload where it will lie, into as much as it
\* reserved. Then it commits what it used, or gives the reservation back.
WFill(w) ==
    /\ wpc[w] = "fill"
    /\ LET at == {i \in Slots : woff[w] < i /\ i < woff[w] + wres[w]} IN
          /\ mem' = [i \in Slots |-> IF i \in at THEN Payload(wid[w]) ELSE mem[i]]
          /\ torn' = (torn \/ \E i \in at : Readable(i))
    /\ wpc' = [wpc EXCEPT ![w] = IF wuse[w] = 0 THEN "unlock" ELSE "header"]
    /\ UNCHANGED <<lock, head, tail, seq, wake, waiters, pauseReq, parkedAt, stop,
                   wres, wuse, woff, wseq, wid, started, reader, pauser, spc, log, delivered, corrupt>>

\* commit(): the sequence is taken, and the header written.
WHeader(w) ==
    /\ wpc[w] = "header"
    /\ wseq' = [wseq EXCEPT ![w] = seq]
    /\ seq' = seq + 1
    /\ mem' = [mem EXCEPT ![woff[w]] = Header(wuse[w], seq, wid[w])]
    /\ torn' = (torn \/ Readable(woff[w]))
    /\ wpc' = [wpc EXCEPT ![w] = "publish"]
    /\ UNCHANGED <<lock, head, tail, wake, waiters, pauseReq, parkedAt, stop,
                   wres, wuse, woff, wid, started, reader, pauser, spc, log, delivered, corrupt>>

\* commit(): the head moves past the frame, by what was used.
WPublish(w) ==
    /\ wpc[w] = "publish"
    /\ head' = (woff[w] + wuse[w]) % Size
    /\ log' = Append(log, wid[w])
    /\ wpc' = [wpc EXCEPT ![w] = "unlock_written"]
    /\ UNCHANGED <<lock, tail, seq, mem, wake, waiters, pauseReq, parkedAt, stop,
                   wres, wuse, woff, wseq, wid, started, reader, pauser, spc, delivered, torn, corrupt>>

\* unlock(). A writer that published goes on to wake the reader.
WUnlock(w) ==
    /\ wpc[w] \in {"unlock", "unlock_written"}
    /\ lock' = 0
    /\ wpc' = [wpc EXCEPT ![w] = IF wpc[w] = "unlock" THEN "idle" ELSE "bump"]
    /\ UNCHANGED <<head, tail, seq, mem, wake, waiters, pauseReq, parkedAt, stop,
                   wres, wuse, woff, wseq, wid, started, reader, pauser, spc, ghosts>>

\* RingReader::wake(), after the write.
WBump(w) ==
    /\ wpc[w] = "bump"
    /\ Bump
    /\ wpc' = [wpc EXCEPT ![w] = "look"]
    /\ UNCHANGED <<ring, waiters, pauseReq, parkedAt, stop, wres, wuse, woff, wseq, wid, started,
                   reader, pauser, spc, ghosts>>

WLook(w) ==
    /\ wpc[w] = "look"
    /\ WakeLook
    /\ wpc' = [wpc EXCEPT ![w] = "idle"]
    /\ UNCHANGED <<ring, wake, waiters, pauseReq, parkedAt, stop, wres, wuse, woff, wseq, wid, started,
                   rseen, rhead, rlen, rask, pauser, spc, ghosts>>

WStep(w) == WLock(w) \/ WReserve(w) \/ WPad(w) \/ WFill(w) \/ WHeader(w) \/ WPublish(w)
            \/ WUnlock(w) \/ WBump(w) \/ WLook(w)

--------------------------------------------------------------------------
\* The reader (RingReader::run, wait_word.cpp, clockwork_drain_ring).

Reader(to) == rpc' = to

RKeep == UNCHANGED <<ring, wake, waiters, pauseReq, parkedAt, stop, writer, pauser, spc, ghosts>>

\* while (!stop.stop_requested())
RTop ==
    /\ rpc = "top"
    /\ Reader(IF stop THEN "exited" ELSE "wait_check")
    /\ RKeep /\ UNCHANGED <<rseen, rhead, rlen, rask>>

\* wait(): the value has already changed, and there is nothing to wait for.
RWaitCheck ==
    /\ rpc = "wait_check"
    /\ Reader(IF wake # rseen THEN "reread" ELSE "wait_count")
    /\ RKeep /\ UNCHANGED <<rseen, rhead, rlen, rask>>

\* wait(): the waiter counts itself before it looks.
RWaitCount ==
    /\ rpc = "wait_count"
    /\ waiters' = waiters + 1
    /\ Reader("wait_look")
    /\ UNCHANGED <<ring, wake, pauseReq, parkedAt, stop, writer, pauser, spc, ghosts, rseen, rhead, rlen, rask>>

\* wait(): the look, and the system call, which sleeps only if the value is
\* still the one waited on. The kernel compares and sleeps in one step.
RWaitLook ==
    /\ rpc = "wait_look"
    /\ Reader(IF wake = rseen THEN "asleep" ELSE "wait_leave")
    /\ RKeep /\ UNCHANGED <<rseen, rhead, rlen, rask>>

\* A wait may return for no reason. Nothing relies on it: it is given no
\* fairness below, so no frame is delivered because of it.
RSpurious ==
    /\ rpc = "asleep"
    /\ Reader("wait_look")
    /\ RKeep /\ UNCHANGED <<rseen, rhead, rlen, rask>>

RWaitLeave ==
    /\ rpc = "wait_leave"
    /\ waiters' = waiters - 1
    /\ Reader("reread")
    /\ UNCHANGED <<ring, wake, pauseReq, parkedAt, stop, writer, pauser, spc, ghosts, rseen, rhead, rlen, rask>>

\* Read before the pass: a wake that lands while the pass runs is a change
\* the next wait sees at once.
RReread ==
    /\ rpc = "reread"
    /\ rseen' = wake
    /\ Reader("stop_check")
    /\ RKeep /\ UNCHANGED <<rhead, rlen, rask>>

RStopCheck ==
    /\ rpc = "stop_check"
    /\ Reader(IF stop THEN "exited" ELSE "ask")
    /\ RKeep /\ UNCHANGED <<rseen, rhead, rlen, rask>>

\* An odd count is a pause asked for. The count is read again after each
\* park, so a pause asked for again before the pass parks the reader again.
RAsk ==
    /\ rpc = "ask"
    /\ rask' = pauseReq
    /\ Reader(IF pauseReq % 2 = 1 THEN "park" ELSE "pass_check")
    /\ RKeep /\ UNCHANGED <<rseen, rhead, rlen>>

\* After the parks, before the pass: a stop ends a park, and the reader
\* leaves without a pass.
RPassCheck ==
    /\ rpc = "pass_check"
    /\ Reader(IF stop THEN "exited" ELSE "drain")
    /\ RKeep /\ UNCHANGED <<rseen, rhead, rlen, rask>>

\* The acknowledgement, between passes.
RPark ==
    /\ rpc = "park"
    /\ parkedAt' = rask
    /\ Reader("parked")
    /\ UNCHANGED <<ring, wake, waiters, pauseReq, stop, writer, pauser, spc, ghosts, rseen, rhead, rlen, rask>>

\* wait(mPauseRequests, ask): asleep until the count moves on. The same wait
\* as the one spelled out above; here only what it waits for. Then the count
\* is looked at again and, with no pause standing, a pass runs: the wake
\* that brought the reader round to the park may have been a writer's too,
\* and rseen already holds it, so going back to the wait would sleep on a
\* frame still in the ring.
RParked ==
    /\ rpc = "parked"
    /\ pauseReq # rask
    /\ Reader("ask")
    /\ RKeep /\ UNCHANGED <<rseen, rhead, rlen, rask>>

\* clockwork_drain_ring: the head is read, and an empty ring ends the pass.
RDrain ==
    /\ rpc = "drain"
    /\ rhead' = head
    /\ Reader(IF head = tail THEN "top" ELSE "look")
    /\ RKeep /\ UNCHANGED <<rseen, rlen, rask>>

\* What the drain calls damage: it gives up what is pending and stops.
Damaged ==
    /\ corrupt' = TRUE
    /\ tail' = rhead
    /\ Reader("top")
    /\ UNCHANGED <<rlen, log, delivered, torn>>

\* The slot at the tail is read. A padding marker is followed to slot 0; a
\* header's length is held to the ring and to what is published.
RLook ==
    /\ rpc = "look"
    /\ LET slot == mem[tail] IN
          CASE slot.kind = "padding" ->
                  IF tail = 0 \/ rhead > tail
                     THEN Damaged
                     ELSE /\ tail' = 0
                          /\ Reader("drain")
                          /\ UNCHANGED <<rlen, ghosts>>
            [] slot.kind = "header" ->
                  IF slot.len > Size - tail \/ slot.len > Used(rhead, tail)
                     THEN Damaged
                     ELSE /\ rlen' = slot.len
                          /\ Reader("deliver")
                          /\ UNCHANGED <<tail, ghosts>>
            [] OTHER -> Damaged
    /\ UNCHANGED <<lock, head, seq, mem, wake, waiters, pauseReq, parkedAt, stop, writer, pauser, spc,
                   rseen, rhead, rask>>

\* The callback is handed the frame where it lies. It consumes it, or (on a
\* ring whose reader may) leaves it and ends the pass.
Whole == \A i \in 1..(rlen - 1) : mem[tail + i] = Payload(mem[tail].id)

RDeliver ==
    /\ rpc = "deliver"
    /\ torn' = (torn \/ ~Whole)
    /\ \/ /\ delivered' = Append(delivered, mem[tail].id)
          /\ Reader("advance")
       \/ /\ Retains
          /\ Reader("top")
          /\ UNCHANGED delivered
    /\ UNCHANGED <<ring, wake, waiters, pauseReq, parkedAt, stop, writer, pauser, spc, log, corrupt,
                   rseen, rhead, rlen, rask>>

\* The tail moves only after the callback returns.
RAdvance ==
    /\ rpc = "advance"
    /\ tail' = (tail + rlen) % Size
    /\ Reader("drain")
    /\ UNCHANGED <<lock, head, seq, mem, wake, waiters, pauseReq, parkedAt, stop, writer, pauser, spc,
                   ghosts, rseen, rhead, rlen, rask>>

RStep == RTop \/ RWaitCheck \/ RWaitCount \/ RWaitLook \/ RWaitLeave \/ RReread \/ RStopCheck
         \/ RAsk \/ RPassCheck \/ RPark \/ RParked \/ RDrain \/ RLook \/ RDeliver \/ RAdvance

--------------------------------------------------------------------------
\* A pauser (RingReader::pause, then what it paused for, then resume).

PKeep == UNCHANGED <<ring, waiters, parkedAt, stop, writer, spc, ghosts>>

PStart(p) ==
    /\ ppc[p] = "idle" /\ pauses < MaxPauses
    /\ pauses' = pauses + 1
    /\ ppc' = [ppc EXCEPT ![p] = "load"]
    /\ PKeep /\ UNCHANGED <<wake, pauseReq, reader, pask, pgot>>

\* The count is read; an even one is made odd below, and an odd one is
\* taken as this pause's own.
PLoad(p) ==
    /\ ppc[p] = "load"
    /\ pask' = [pask EXCEPT ![p] = pauseReq]
    /\ ppc' = [ppc EXCEPT ![p] = IF pauseReq % 2 = 0 THEN "add" ELSE "bump"]
    /\ PKeep /\ UNCHANGED <<wake, pauseReq, reader, pgot, pauses>>

\* fetch_add: the ask is the count it makes.
PAdd(p) ==
    /\ ppc[p] = "add"
    /\ pauseReq' = pauseReq + 1
    /\ pask' = [pask EXCEPT ![p] = pauseReq + 1]
    /\ ppc' = [ppc EXCEPT ![p] = "bump"]
    /\ PKeep /\ UNCHANGED <<wake, reader, pgot, pauses>>

\* wake(): a reader waiting for work comes round to the park.
PBump(p) ==
    /\ ppc[p] = "bump"
    /\ Bump
    /\ ppc' = [ppc EXCEPT ![p] = "look"]
    /\ PKeep /\ UNCHANGED <<pauseReq, reader, pask, pgot, pauses>>

PLook(p) ==
    /\ ppc[p] = "look"
    /\ WakeLook
    /\ ppc' = [ppc EXCEPT ![p] = "await"]
    /\ PKeep /\ UNCHANGED <<wake, pauseReq, rseen, rhead, rlen, rask, pask, pgot, pauses>>

\* The reader parked for this ask.
PParked(p) ==
    /\ ppc[p] = "await" /\ parkedAt = pask[p]
    /\ pgot' = [pgot EXCEPT ![p] = TRUE]
    /\ ppc' = [ppc EXCEPT ![p] = "holding"]
    /\ PKeep /\ UNCHANGED <<wake, pauseReq, reader, pask, pauses>>

\* The deadline passed first: the caller goes on beside the reader.
PGaveUp(p) ==
    /\ Timeouts
    /\ ppc[p] = "await"
    /\ pgot' = [pgot EXCEPT ![p] = FALSE]
    /\ ppc' = [ppc EXCEPT ![p] = "holding"]
    /\ PKeep /\ UNCHANGED <<wake, pauseReq, reader, pask, pauses>>

\* resume(): an odd count is moved on.
PResume(p) ==
    /\ ppc[p] = "holding"
    /\ pauseReq' = IF pauseReq % 2 = 1 THEN pauseReq + 1 ELSE pauseReq
    /\ ppc' = [ppc EXCEPT ![p] = "idle"]
    /\ PKeep /\ UNCHANGED <<wake, reader, pask, pgot, pauses>>

PStep(p) == PLoad(p) \/ PAdd(p) \/ PBump(p) \/ PLook(p) \/ PParked(p) \/ PResume(p)

--------------------------------------------------------------------------
\* The stop (RingReader::stop: request_stop, then wakeForStop).

SKeep == UNCHANGED <<ring, waiters, parkedAt, writer, pauser, ghosts>>

SStop ==
    /\ Stops /\ spc = "idle"
    /\ stop' = TRUE
    /\ spc' = "bump"
    /\ SKeep /\ UNCHANGED <<wake, pauseReq, reader>>

SBump ==
    /\ spc = "bump"
    /\ Bump
    /\ spc' = "look"
    /\ SKeep /\ UNCHANGED <<pauseReq, stop, reader>>

SLook ==
    /\ spc = "look"
    /\ WakeLook
    /\ spc' = "resume"
    /\ SKeep /\ UNCHANGED <<wake, pauseReq, stop, rseen, rhead, rlen, rask>>

SResume ==
    /\ spc = "resume"
    /\ pauseReq' = IF pauseReq % 2 = 1 THEN pauseReq + 1 ELSE pauseReq
    /\ spc' = "done"
    /\ SKeep /\ UNCHANGED <<wake, stop, reader>>

SStep == SBump \/ SLook \/ SResume

--------------------------------------------------------------------------

Next ==
    \/ \E w \in Writers : WStart(w) \/ WStep(w)
    \/ RStep \/ RSpurious
    \/ \E p \in Pausers : PStart(p) \/ PStep(p) \/ PGaveUp(p)
    \/ SStop \/ SStep

\* Every thread that has begun something goes on with it. Beginning a write,
\* a pause or a stop is nobody's duty, and neither is giving up or waking
\* for no reason.
Fair ==
    /\ \A w \in Writers : WF_vars(WStep(w))
    /\ WF_vars(RStep)
    /\ \A p \in Pausers : WF_vars(PStep(p))
    /\ WF_vars(SStep)

Spec == Init /\ [][Next]_vars /\ Fair

--------------------------------------------------------------------------
\* WHAT IS ALWAYS SO.

InLock == {"reserve", "pad", "fill", "header", "publish", "unlock", "unlock_written"}

\* One writer at a time, and the lock is held exactly while one is inside.
OneWriter ==
    /\ Cardinality({w \in Writers : wpc[w] \in InLock}) <= 1
    /\ (lock = 1) <=> (\E w \in Writers : wpc[w] \in InLock)

\* No writer writes a slot the reader may be reading, and the reader is
\* never handed part of a frame.
NothingTorn == ~torn

\* While the writers keep the rules, the reader never meets a ring it calls damaged.
NothingDamaged == ~corrupt

\* Every frame consumed was published, once, in the order it was published.
InOrder ==
    /\ Len(delivered) <= Len(log)
    /\ \A n \in 1..Len(delivered) : delivered[n] = log[n]

\* Sequences run with the order of publishing, and none is skipped: one is
\* taken only by a frame that is then published.
NoHoles == \A w \in Writers : wpc[w] = "publish" => wseq[w] = Len(log)

\* The ring reads as empty exactly when everything published has been given back.
GivenBack == Len(delivered) - (IF rpc = "advance" THEN 1 ELSE 0)
EmptyMeansEmpty == (head = tail) <=> (Len(log) = GivenBack)


\* While a pause that was acknowledged is held, no pass runs. The reader is
\* parked, or was stopped and is on its way out. True of one pauser, which
\* is all the engine has: a pause is asked only inside a restart, and one
\* restart runs at a time. With two at once it is not: the second finds the
\* count odd or makes it even, and the reader leaves its park under the
\* first.
Quiet == rpc \in {"parked", "exited"} \/ (stop /\ rpc \in {"top", "ask", "pass_check", "park"})
PauseMeansPaused == \A p \in Pausers : (ppc[p] = "holding" /\ pgot[p]) => Quiet

\* WHAT COMES TO PASS.

\* Every frame published is consumed, unless the reader was stopped. No wake
\* is lost, with a pause or without. The pass after a park is what makes it
\* true with one: send RParked back to "top" and a wake that lands as the
\* reader comes round to park is taken by the park, and the reader then
\* sleeps on the value it last read with the frame still in the ring.
AllDelivered == <>[](stop \/ Len(delivered) = Len(log))

\* A reader that is stopped leaves.
StopLeaves == stop ~> (rpc = "exited")

\* A pause is acknowledged. For a model with no deadline, where a pause that
\* is never acknowledged shows as one that never ends. True with no stop. A
\* pause that meets a stop is not acknowledged, and waits out its deadline.
PauseAnswered == \A p \in Pausers : (ppc[p] = "await") ~> (ppc[p] = "holding")
==========================================================================
