/**
 * @name Blocking, allocating or thread-starting call reachable from the audio thread
 * @description The audio thread owns a deadline a few milliseconds wide and
 *   nothing on it may wait, allocate, touch a file or start a thread: a lock
 *   that is contended, a malloc that takes a page fault, a write that blocks,
 *   each is a dropout the listener hears. The rule holds on every platform
 *   and is stated in docs; this query walks the call graph from the audio
 *   entry points and reports every path to a call that breaks it.
 * @kind path-problem
 * @problem.severity warning
 * @precision high
 * @id clockwork/audio-thread-forbidden-call
 * @tags real-time
 *       correctness
 */

import cpp

/** Where the audio thread enters Clockwork: the device callback and the
 *  block render it drives, and the engine tick a host's own callback calls. */
class AudioEntry extends Function {
  AudioEntry() {
    this.hasName("audioDeviceIOCallbackWithContext") or
    this.hasName("renderAudioBlock") or
    this.hasName("clockwork_tick")
  }
}

/** A function the audio thread must never reach, and why. Standard-library
 *  names are matched by type and name rather than full qualification, since
 *  libc++ keeps them in an inline namespace. */
predicate forbidden(Function f, string why) {
  f.hasGlobalName(["malloc", "calloc", "realloc", "free", "posix_memalign", "aligned_alloc",
                   "valloc", "strdup"]) and why = "allocates"
  or
  // operator new, but not placement new, which constructs in storage the
  // caller already holds
  f instanceof OperatorNewAllocationFunction and
    not f.getParameter(1).getType().getUnspecifiedType() instanceof VoidPointerType and
    why = "allocates"
  or
  f instanceof OperatorDeleteDeallocationFunction and why = "frees"
  or
  f.getName() = ["lock", "lock_shared"] and f.getDeclaringType().getName().matches("%mutex%")
    and why = "may block on a lock"
  or
  f.getName() = ["wait", "wait_for", "wait_until"] and
    f.getDeclaringType().getName().matches("condition_variable%") and why = "waits"
  or
  f.hasGlobalName(["pthread_mutex_lock", "pthread_cond_wait", "pthread_cond_timedwait",
                   "pthread_join", "sem_wait", "flock"]) and why = "may block"
  or
  f.hasGlobalName(["sleep", "usleep", "nanosleep", "Sleep", "WaitForSingleObject",
                   "WaitForMultipleObjects"]) and why = "sleeps"
  or
  f.getName() = ["sleep_for", "sleep_until", "yield"] and f.getNamespace().getName() = "this_thread"
    and why = "sleeps"
  or
  f.hasGlobalName(["pthread_create", "CreateThread", "_beginthreadex", "fork", "system",
                   "popen", "dlopen", "LoadLibraryA", "LoadLibraryW"]) and why = "starts a thread or process"
  or
  f instanceof Constructor and f.getDeclaringType().getName() = ["thread", "jthread"] and
    f.getNumberOfParameters() > 0 and why = "starts a thread"
  or
  f.hasGlobalName(["fopen", "freopen", "fclose", "fread", "fwrite", "fflush", "fseek", "ftell",
                   "fprintf", "vfprintf", "printf", "vprintf", "puts", "fputs", "fputc", "putchar",
                   "perror", "open", "close", "read", "write", "pread", "pwrite", "unlink",
                   "rename", "stat", "fstat", "mkdir", "opendir", "readdir", "closedir",
                   "CreateFileA", "CreateFileW", "ReadFile", "WriteFile"]) and why = "does file IO"
  or
  f.hasGlobalName(["getenv", "setenv", "putenv", "localtime", "gmtime", "strerror"])
    and why = "takes a hidden lock in the C runtime"
}

/** Code the graph does not walk into: the reference host (src/host) is a
 *  program of its own that defines the engine's log hook over stderr, so a
 *  call resolved to its definition is not the engine's; and the standard
 *  library's std::function machinery, through which a call resolves to
 *  every callable of that signature in the program, which is not a call
 *  graph but a cross product. A std::function the audio thread does call
 *  must have its target reached another way: by being an entry point. */
predicate outside(Function f) {
  // a symbol the host defines over the engine's (its own clockwork_log) has
  // a definition in both; it is the host's only when every definition is
  exists(FunctionDeclarationEntry e | e = f.getADeclarationEntry() and e.isDefinition() |
    e.getFile().getRelativePath().matches("src/host/%")) and
  not exists(FunctionDeclarationEntry e | e = f.getADeclarationEntry() and e.isDefinition() |
    not e.getFile().getRelativePath().matches("src/host/%"))
  or
  f.getFile().getBaseName() = ["function.h", "invoke.h", "functional", "__functional_base"]
}

/** Functions the audio thread never runs although its graph calls them,
 *  each with the reason: the engine's own ordering rules them out, which a
 *  walk of calls cannot see. Stated here once, in review, rather than
 *  dismissed alert by alert. */
predicate neverOnTheAudioThread(Function f) {
  // The logger's no-ring path, taken only while memory_initialized is false.
  // The audio thread never runs then: the device callback is attached after
  // init_memory builds the ring and its device closed before teardown_memory,
  // and process_audio returns before logging when there is no ring.
  f.hasName("clockwork_log_no_ring")
}

/** A call edge, with virtual dispatch widened to every override: a call
 *  through a base class may land on any of them. */
predicate calls(Function caller, Function callee) {
  not outside(caller) and not outside(callee) and not neverOnTheAudioThread(callee) and
  exists(Call c | c.getEnclosingFunction() = caller |
    callee = c.getTarget() or
    callee.(MemberFunction).overrides+(c.getTarget()))
}

/** On the audio thread's path: an entry point, or anything one calls. */
predicate onAudioThread(Function f) {
  f instanceof AudioEntry or
  exists(Function g | onAudioThread(g) and calls(g, f))
}

/** Clockwork's own code: a file under the source root. */
predicate inTree(Function f) { exists(f.getFile().getRelativePath()) }

/** `f` is forbidden itself, or is library code that leads to a forbidden
 *  function `bad`: the walk continues only through library functions, so a
 *  call from our code into a library is judged by everything behind it. */
predicate leadsTo(Function f, Function bad, string why) {
  forbidden(f, why) and bad = f
  or
  not inTree(f) and exists(Function g | calls(f, g) and leadsTo(g, bad, why))
}

/** An edge of the path shown: from a function on the audio thread to one it
 *  calls that is also on it. */
query predicate edges(Function a, Function b) {
  onAudioThread(a) and calls(a, b) and onAudioThread(b)
}

// Reported at the last call in our own code, since a finding placed inside
// a library header would be outside the source archive and never shown:
// the constructor of a lock_guard is where the audio thread takes the mutex,
// and an emplace_back is where it allocates.
from Call c, Function target, Function bad, string why, AudioEntry entry, Function enclosing
where
  enclosing = c.getEnclosingFunction() and
  inTree(enclosing) and
  not c.getFile().getRelativePath().matches("src/host/%") and
  onAudioThread(enclosing) and
  (target = c.getTarget() or target.(MemberFunction).overrides+(c.getTarget())) and
  leadsTo(target, bad, why) and
  edges*(entry, enclosing)
select c, entry, enclosing, "On the audio thread: $@ " + why + ".", bad, bad.getQualifiedName()
