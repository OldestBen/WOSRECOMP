# Progress Log

Living devlog for this project. **Read this file first** in any new session —
it's kept up to date after every meaningful change so you don't have to dig
through commit history to know where things stand.

Format: a **Current State** summary (rewritten in place, always reflects
"now"), then a reverse-chronological **Log** of dated entries (append-only,
never edited after the fact — corrections get a new entry).

## How the records are layered

Read top-down, stop when you know enough. **This file's Current State
section is the single authority on what's true now** — if anything else
contradicts it, this wins and the other is stale history.

| Layer | Where | Content |
|---|---|---|
| 1. Orientation | **This file** § Current State | What's true right now. Rewritten in place. |
| 2. Durable record | **This file** § Log · [`docs/05-findings-log.md`](docs/05-findings-log.md) | Dated project changes; WoS technical data with provenance. Appended. |
| 3. Detail | [`docs/sessions/`](docs/sessions/) | Per-session blow-by-blow, incl. dead ends and unverified claims. One file per session. |

You should never need layer 3 to get oriented — it's for digging into a
specific past episode. Its conventions (and why dead ends matter more than
successes there) are in [`docs/sessions/README.md`](docs/sessions/README.md).

---

## Current State

- **Stage:** **BOOTS, RUNS A STEADY RENDER LOOP, LOADS SOME ASSETS.** 79
  imports reached, 76 implemented. Twelve threads. The command processor
  consumes the ring continuously, the GPU fence tracks the CPU fence four
  behind, and the game's own D3D layer is satisfied — no hang report, runs for
  minutes. **Nothing is drawn: there is no renderer and no window.**
- **One blocker, precisely located.** Two threads — `sub_829677D0` (the game's
  own "Game Master") and `sub_82A7CD00` — wait forever on five events that
  nothing ever signals. The whole chain is mapped: every event is created at
  guest `0x82B16368` and set from `0x82B16C58`; the only code that would
  signal these five is five instructions at `0x82965534`, guarded by two
  branches at `0x829654F8`/`0x82965504`, reached from a request-completion
  function at `0x82965488` with four callers, none of which has ever appeared
  on a stack. The request objects stay in state 1 instead of moving to 3.
- **The async read works. Nobody consumes the result.** `game.XEPACK` is read,
  the completion APC is delivered correctly, and `sub_82968498` marks the file
  request complete — confirmed by `[req+0x20] = 0x80000`, the byte count it
  writes only on the success path. `sub_82968498` is a state setter, not a
  signaller; it was never meant to reach `sub_82965488`. The consumer that
  would signal has four call sites and none has ever run. Request object [0] is
  sitting in state 1, which is the state that signals immediately, and the
  handle it would set — `[0x82F719DC] = 0x00010030` — is ev4, one of the exact
  events the Game Master is blocked on. Everything is armed; nothing pumps it.
- **Asset loading works as far as it gets:** `game_shared.ini`, `amalga.toc`
  and 774 KB of `SOUNDSRC_RVB.PCK` all load correctly.
- **The ring size encoding is still unconfirmed.** The raw argument is 0xE; a
  modelled 0x4000 dwords is too small, the capacity grows once to 0x8000 and
  is clamped at the allocation edge. No wrap has been observed, which is what
  would settle it.

Full evidence for each of these is in
[`docs/05-findings-log.md`](docs/05-findings-log.md).

## Next Steps (in order)

1. **Find who pumps `sub_82965488`.** Tripwires are in on all four of its
   callers (`sub_82965D58`, `sub_829660C0`, `sub_82966C58`, `sub_82966D90`).
   If one fires, the argument is wrong; if none fires, walk up their callers or
   find the thread that should be running them. The request is armed and the
   signal handle resolves to a blocked event, so this is the last link.
2. **Then the rest of asset loading.** `game.XEPACK` past its first 0x80000,
   plus whatever the unblocked loader threads ask for next. Expect the import
   count to move past 76 for the first time in a dozen runs.
3. **Checkpoint here.** A game that boots, streams its assets and drives a
   coherent command stream is the right foundation to start a renderer
   against, and it is a different thing from where this was.
4. **Then the big one: the renderer.** Host backend, PM4 command translation,
   shader recompilation via XenosRecomp, vertex/texture formats, EDRAM and
   resolves. This is the majority of the remaining work by a wide margin.
5. Input, then audio.
6. Revisit the 33 known-bad switch sites; locate `setjmp`/`longjmp` if error
   paths misbehave.

---

## Log

### 2026-07-28 (25) — The deadlock fully mapped; graphics detour corrected

The session's real output is a diagnosis, not a fix. Two threads block on five
events nobody signals, and the entire chain is now known by measurement:
creation site, set site, the five instructions that would signal, the two
branches guarding them, and the completion function upstream. What is not yet
known is why that completion never runs.

**Corrections that cost the most time, recorded because the pattern repeated:**

- The main thread was never deadlocked. It runs a wraparound-safe fence wait
  that completes continuously; it merely spends most of its time there.
- The GPU fence never froze. A log line was capped at twelve occurrences and
  the silence read as the fence stopping.
- There is no `Sleep()` spin. The call-site counter is frozen after startup.
- Three separate times an instrumentation gap read as a result — a capped log
  line, a function-scope `static bool`, and a report on the wrong side of a
  branch. Same shape each time: absence of output taken as absence of the
  condition. Every self-suppressing diagnostic now announces its suppression.

**Real fixes this session:** the string conversions (first bulk asset load),
proper WaitAny (the "first object only" shortcut was a permanent block), the
adaptive ring capacity (a refusal to consume was making the game declare the
GPU hung), and completion-APC delivery.

**Tooling:** `--field` finds who touches a struct field, with `--context N` to
show the value each store writes — the question `--xrefs` structurally cannot
answer. `LogCallSite` attributes any hot import to its guest call site.
Guest-function tripwires observe that control reached an address whose code
calls no imports. The event report now lists every event with its creation and
last-set call sites.

### 2026-07-28 (24) — Asset loading unblocked; the GPU hang was ours

Two real fixes and three corrections.

**RtlMultiByteToUnicodeN / RtlUnicodeToMultiByteN were never implemented.**
Out-parameter stubs, so the game built a filename out of uninitialised stack
and opened `"B<garbage>"`. Implementing them produced the first bulk asset
load of the project — 774 KB out of `SOUNDSRC_RVB.PCK`, under a name the
game round-trips through both conversions. Lengths are in bytes not
characters and the guest's UTF-16 is big-endian; both are silent when wrong
and both are stated in the code.

**The ring capacity guard was hanging the GPU.** A run reached write pointer
0x4003 against a modelled 0x4000 and refused to consume, which froze the
fence, and the game's D3D layer declared the GPU hung within seconds. A
write pointer cannot exceed the ring it indexes, so that is proof the number
is wrong, not a reason to stop. It now grows and reports; the wrap will
settle the encoding by measurement.

**Three things believed and recorded that measurement then overturned:** the
main thread was never deadlocked, the GPU fence never froze, and there is no
`Sleep()` spin. Each looked convincing from a log and dissolved on contact
with a counter. The pattern is consistent enough to be worth naming — a
diagnostic that stops printing is not evidence that the thing it reports
stopped happening.

**Tooling:** `xex_info --field <disp> [--stores] [--context N]` finds who
touches a struct field, which `--xrefs` structurally cannot do, and shows the
value each store writes. `wos::LogCallSite` attributes any hot import to its
guest call site via `ctx.lr`. Guest-function tripwires in
`kernel/trace_guest.cpp` observe that control reached an address whose code
calls no imports.

### 2026-07-28 (23) — sub_82ACECF0 read; present gated on a timeout that already happens

Dumped the graphics thread body, the one function in the chain never looked
at. It answers the structural questions cleanly: the KEVENT is at
context+0x20 (so 0x4083FD7C and 0x4083FDCC — matching the wait diagnostics
exactly), one thread waits ~30 ms and the other INFINITE depending on
`[ctx+0x04] == [device+0x178]`, both are inside the wait so the work queue is
empty, and the branch to the present path at 82ACEE38 is taken on
`STATUS_TIMEOUT`, not on a signal.

That last one should have been the answer, and it is not: we already return
0x102 on timeout and the counters already show ~950 timeouts against zero
signals, yet `VdSwap` has still never been called. Three explanations remain
and the disassembly cannot choose between them — the timeouts may come from a
different wait site in the same large function, the comparison may sit behind
another gate, or 0x82AC4E48 may be running and bailing before VdSwap.

Rather than pick one, added two measurements that decide it:

- **Wait call-site census** (`kernel/sync.cpp`). XenonRecomp sets
  `ctx.lr` to the return address before every `bl`, so an import
  implementation can name its own guest call site for free. Every wait now
  records (call site, object, timeout, timed-out?) and the heartbeat prints
  it. This is a general capability, not a one-off probe.
- **Guest-function tripwires** (`kernel/trace_guest.cpp`, new). A strong
  definition of `sub_XXXXXXXX` overrides the recompiler's weak alias and can
  forward to `__imp__sub_XXXXXXXX`, so we can observe that control reached an
  address whose code calls no imports. Placed on `sub_82AC4E48` and
  `sub_82ACECF0`.

Also fixed `--disasm` running to its 16384-instruction cap again. The earlier
`isCallInsn` fix was right but incomplete: one forward branch to a distant
handler keeps the "has anything branched past here" horizon permanently ahead
of the cursor. It now also stops at the `.pdata` record boundary, and the
footer says which rule ended the walk.

### 2026-07-26 (21) — Watchdog delivers; graphics interrupt called with wrong arguments

- The all-thread watchdog worked and produced the one stack that mattered:
  ```
  main thread
    #0   sub_82B13200 +0x19        <- 25 bytes in: a tiny spin loop
    #1   sub_82AC0C10 +0x1B7
    #2   sub_82ABA260 +0x2D5
    #3   sub_82ABAD58 +0x127
    ...  _xstart
  ```
- The callers place it squarely in the **graphics layer**: the interrupt
  callback is `0x82AB9840` and the graphics threads are `0x82ACECF0`, so
  `0x82ABA260` / `0x82ABAD58` / `0x82AC0C10` are its neighbours.
- Full picture, with every thread now accounted for: five JQ workers idle on
  `ev0`; the master idle; **both graphics threads blocked on their own
  `ctx+0x20` events**; and the main thread spinning in the graphics layer
  waiting for those threads. On hardware, the **graphics interrupt** is what
  wakes them.
- **The bug is mine.** The Xbox 360 graphics interrupt callback takes three
  arguments — `(source, cpu, userdata)` — and I was passing two, so
  `userdata` went into the `cpu` slot and `r5` held whatever happened to be
  there. The callback then used a non-pointer as its context.
- **That also retrospectively explains the unexplained reads.** Guest
  `0x59000000` and `0x66020000` were logged early and never accounted for;
  neither is a heap or physical-alias address. A callback dereferencing a
  garbage context is exactly the shape of thing that produces them.
- Fixed: `r3 = source`, `r4 = cpu`, `r5 = userdata`.
- Worth noting how this was found. Three rounds of inference about the main
  thread produced one wrong answer and two dead ends; the watchdog produced
  the answer on its first run. The cost of building the general tool was
  about the same as one more guess.

### 2026-07-26 (20) — Build broke on MSVC only; stale-binary trap closed

- **`main.cpp` failed to compile on Windows**: `no type named 'mutex' in
  namespace 'std'`. The thread registry used `std::mutex` without including
  `<mutex>`.
- **My cross-compile check did not catch it, and that is the real finding.**
  libstdc++ pulls `<mutex>` in transitively via `<thread>`; MSVC's STL does
  not. Every syntax check this session has been "clean on Linux and mingw",
  and for *transitive includes* that guarantee is worth less than it looks.
- Added an **include-what-you-use audit** over `WoSRecomp/**` that maps each
  `std::` facility used to the header that must be included (following one
  level of our own headers). It reports clean now, and unlike the compilers
  it does not depend on which STL happens to be in use.
- **The run that followed the failed build used the previous binary.** The
  chained `build && run` printed a compile error, then produced output that
  looked like a perfectly normal result from new code — no watchdog lines,
  because the watchdog was not in that executable. Nothing about the run
  output said "this is stale".
- `build_host.sh` now deletes the executable *before* building. A failed
  build leaves nothing to run, so a stale binary cannot masquerade as a
  result. Cheap, and it removes a whole category of wasted round trip.

### 2026-07-26 (19) — Deadlock ruled out; a watchdog that backtraces every thread

- **No `[lock]` warning fired.** The critical-section deadlock hypothesis is
  wrong: the main thread is not blocked on a lock. A clean negative result,
  and worth as much as a positive one — it eliminates my own synchronisation
  code as the cause.
- So the main thread is **executing guest code while calling no imports at
  all**. Nothing in the existing instrumentation can see that: the import
  trace, the heartbeat and the wait statistics are all keyed on imports.
- **Stopped guessing and built the general tool instead.** I have now
  inferred this thread's location twice and been wrong once, so:
  `DumpAllThreadStacks` registers every guest-executing thread (main plus
  each `ExCreateThread` trampoline) and, on demand, suspends each one,
  captures its `CONTEXT`, walks it with `StackWalk64` and symbolises the
  result.
- `CaptureStackBackTrace` cannot do this — it only ever walks the *calling*
  thread, which is precisely why the earlier diagnostics could describe the
  waiters and never the one thread that mattered.
- A watchdog fires it automatically after **three consecutive heartbeats with
  no new imports** (15 s). "Busy" is deliberately defined as *new imports
  appearing*, not calls happening: a game polling the same three waits
  forever is not progressing, whatever the call rate says.
- This generalises beyond the current question. Any future hang — a worker
  stuck mid-job, a thread lost in mis-recompiled control flow — now names
  itself without another round of instrumentation.

### 2026-07-26 (18) — Backtrace corrects me; hunting the invisible main thread

- **Correction to entry (17).** I said the long wait was the main thread. The
  backtrace says otherwise:
  ```
  #2   __imp__sub_82ACECF0 +0x4CA
  #3   GuestThreadMain +0x15D          <- our ExCreateThread trampoline
  #4   std::thread::_Invoke<...>
  ```
  `GuestThreadMain` only appears on threads created through `ExCreateThread`;
  the main thread would show `main` / `__scrt_common_main_seh`. So the
  blocked waiter is one of the two `0x82ACECF0` threads, waiting on **its
  own** `ctx+0x20` event. The inference was wrong; the measurement was right.
  This is the second time today a confident claim about *who* was doing
  something has been overturned by an actual backtrace.
- **So where is the main thread?** It makes *no import calls whatsoever* —
  every count in the heartbeat is attributable to thread 4102's polling loop
  (`RtlEnterCriticalSection`, `RtlLeaveCriticalSection` and
  `KeWaitForSingleObject` all ~158 per 5 s, in lockstep). A thread that calls
  nothing is either spinning in pure guest code or blocked inside a host
  primitive.
- **The prime suspect is my own critical-section implementation.** A thread
  blocked in `RtlEnterCriticalSection` makes no further import call while it
  waits, so it is invisible to the heartbeat *by construction* — and a
  `std::recursive_mutex::lock()` would hide a deadlock forever. If thread
  4103 holds a section and then blocks on its event (which it does), any
  other thread wanting that section is stuck silently.
- Critical sections are now `std::recursive_timed_mutex` with a bounded
  attempt: 5 seconds, then report the blocked thread, the section address,
  **the guest thread that holds it**, and a backtrace — then carry on
  waiting, because the report is the point rather than giving up.
- Added `t_guestThreadId`, a thread-local set in the trampoline, so
  diagnostics can say "main thread" or "guest thread 4103" rather than
  leaving the reader to infer it. That inference is exactly what went wrong
  above.

### 2026-07-26 (17) — Adoption works; whole game is quiescent; backtrace the waiters

- Adoption landed and the addresses were immediately meaningful:
  ```
  ev5 adopted at 0x4082FD7C   thread 4102 ctx 0x4082FD5C  -> ctx + 0x20
  ev6 adopted at 0x4082FDCC   thread 4103 ctx 0x4082FDAC  -> ctx + 0x20
  ```
  Both are events embedded at **offset 0x20 inside the per-thread context
  struct** of the two threads at entry `0x82ACECF0` — created right after the
  ring buffer and `KiApcNormalRoutineNop`, so almost certainly graphics-side.
- Current state, and it is a clean one to reason about: **everything waits,
  nothing signals.**
  | event | waits | timeouts | signals | who |
  |---|---:|---:|---:|---|
  | `ev0` | 25,576 | 25,511 | 3 | 5 JQ workers, idle |
  | `ev5` | 3,167 | 3,166 | 0 | thread 4102, polling ~32/s |
  | `ev6` | 3 | 2 | 0 | **main thread, blocked infinitely** |
- The long-wait warning fired on `ev6` exactly as designed:
  `still waiting on event 0x4082FDCC after 5s`.
- **Stopped guessing.** Several stories fit (a GPU fence, an APC never
  delivered — `KiApcNormalRoutineNop` is suggestive — a producer thread that
  never starts), and picking one to implement speculatively is how the
  earlier rounds would have gone wrong. Each guest thread runs on its own
  host stack, so a `dbghelp` backtrace taken *inside* the wait names the
  recompiled functions that led into it.
- Long waits now print that backtrace. `PrintGuestStack` exposed from
  `main.cpp` via `guest.h`; the existing symbolisation already works, as the
  `KeBugCheck` backtrace proved.

### 2026-07-26 (16) — Wait statistics name the blocker: guest-embedded events

- The counters answered it on their first run:
  ```
  ev0(manual) 25517/25508/3     <- waits / timeouts / signals
  ev1(manual)     1/    0/3
  ev3(auto)       1/    0/0
  ```
- **`ev0`: waited 25,517 times, signalled 3 times, all at startup.** Exactly
  the pattern the counters were built to expose. `ev0` accounts for *every*
  `NtWaitForSingleObjectEx` call (1,276 per 5 s in both), i.e. 255/s across
  five JQ workers — the pool is idle, waiting for jobs the main thread never
  posts.
- **The more useful finding is what is missing from that table.** The main
  thread's `KeWaitForSingleObject` runs at 128/s, but `ev1` and `ev3` show a
  single wait each. So the Ke waits target an object that is **not in our
  object table at all** — and the 128/s rate is my own unknown-object
  fallback (a 1 ms sleep, ~7.8 ms real at Windows' default timer
  resolution). The arithmetic identifies the fallback as the thing setting
  the rate, which is a strong signal it was being hit every time.
- **Cause: Xbox 360 dispatcher objects need not come from `NtCreateEvent`.**
  A game can embed a `KEVENT` in one of its own structures and initialise it
  in place; `Ke*` calls then operate on that guest address directly. We never
  see such an object created, so every lookup failed and the main thread's
  wait was answered by a sleep that signalled nothing.
- Now adopted on first touch: an unrecognised guest pointer reaching
  `KeWaitForSingleObject`/`KeSetEvent`/`KeResetEvent` gets a real event
  created at that address, so a later `KeSetEvent` on the same address wakes
  the same object. Announced once per object as
  `[sync] KeWaitForSingleObject: adopting guest-embedded event at 0x... as evN`.
- Chose **manual-reset** as the default for adopted events: an auto-reset
  event we invented could silently consume a signal a real waiter needed.
  Worth revisiting if a woken thread appears to miss wakeups.

### 2026-07-26 (15) — Spin fixed (5M/s -> 510/s); now idle-waiting

- The kernel-mode wait implementations worked exactly as intended:
  | | before | after |
  |---|---:|---:|
  | total calls/sec | ~10,000,000 | **510** |
  | `KeWaitForSingleObject` | 5,000,000/s | 127.6/s |
  | `KeResetEvent` | 5,000,000/s | 127.6/s |
  | `NtWaitForSingleObjectEx` | 254/s | 255/s (unchanged) |
- The remaining rates are suspiciously exact: `NtWaitForSingleObjectEx` is
  **precisely 2.000x** `KeWaitForSingleObject`. Regular polling, not chaos.
- **But the game is idle, not working.** Held stable for 5+ minutes across
  60 heartbeats with no new imports, and **`VdSwap` has never been called** —
  so despite the vblank ticking, no frame has ever been presented. The main
  loop is blocked somewhere before it renders.
- Added per-event **waits / timeouts / signals** counters, reported each
  heartbeat. The diagnostic pattern being looked for is an event with waits
  ≈ timeouts and **zero signals**: something waiting on a thing nothing in
  the runtime ever wakes. That names the missing piece precisely rather than
  by elimination.
- Deliberately did not guess further. Candidate explanations (a missing XAM
  notification, an unsignalled GPU fence, a job queue nobody feeds) are all
  plausible and the wait statistics will distinguish them in one run — which
  is cheaper than implementing any of them speculatively.

### 2026-07-26 (14) — Heartbeat finds a 5-million-per-second busy-spin

- The heartbeat earned its keep on its first run:
  ```
  KeWaitForSingleObject   x24,801,146 per 5 s   = 5.0 million/sec
  KeResetEvent            x24,801,146 per 5 s   = 5.0 million/sec
  NtWaitForSingleObjectEx x1,271      per 5 s   = 254/sec
  ```
- **Exactly equal counts for wait and reset** — that is one loop body:
  wait, reset, check, repeat. And both were still stubs.
- **A stub that returns success to a *wait* inverts the function's purpose.**
  It does not merely return the wrong value; it converts a blocking call into
  a busy-spin, burning a core at five million iterations a second. The
  properly implemented `NtWaitForSingleObjectEx` sitting at 254/sec in the
  same trace is the control group.
- New variant of the recurring lesson, and worth stating separately: the
  earlier cases were stubs that returned *wrong data*. This one returned
  plausible data but destroyed the call's *timing semantics*.
- Implemented the kernel-mode family in `sync.cpp`: `KeWaitForSingleObject`,
  `KeWaitForMultipleObjects`, `KeSetEvent`, `KeResetEvent`, `KePulseEvent`.
  Ke* takes an object *pointer* where Nt* takes a handle, and `ObjectFromAny`
  already accepts both, so the object handling is shared.
- Added a long-wait warning: an infinite wait is first attempted with a 5 s
  bound, and if that expires it says so once before continuing to block. A
  deadlock should name itself rather than going quiet.
- **Stated shortcut:** `KeWaitForMultipleObjects` waits on the first object
  only, rather than implementing WaitAll/WaitAny across the array. That at
  least blocks instead of spinning, which was the failure that mattered; if
  the game later behaves as though the wrong object woke it, this is the
  first place to look.

### 2026-07-26 (13) — It boots. The render loop is running at 60 Hz.

- ```
  [video] first vblank interrupt delivered
  [video] 600 vblanks (~10 s) — the render loop is turning over
  ```
  **Spider-Man: Web of Shadows boots and runs as native x86-64.** Kernel,
  memory, threads, files, synchronisation and the video driver are all
  carrying real load. Nothing is drawn — there is no renderer — but the game
  is alive and looping.
- Everything fixed last round confirmed by absence: `unknown handle
  0xFFFFFFFE`, `RtlNtStatusToDosError`, `DbgPrint` and `_vsnprintf` all
  vanished from the trace. The pseudo-handles resolved, and the game no
  longer reaches its error-formatting path at all.
- Ring buffer facts: buffer at guest `0x000914E0`, 2^14 = 16 KiB; read
  pointer writeback at `0x0006023C`; GPU registers at `0x7FC80000` with the
  game's only init-time write landing on `0x7FC80714` — CP_RB_WPTR.
- **Suspected current stall, and a correction to my own code:** the vblank
  thread was holding the ring buffer read pointer at 0. For a ring buffer
  that means "the GPU is still at the start", so once the game advances its
  write pointer it waits for a read pointer that never moves. That fits
  exactly what was observed — a loop that runs but calls no further imports.
  Now mirrors CP_RB_WPTR, modelling a GPU that consumes instantly.
- Added a **5-second heartbeat**. The import log only prints on *first*
  call, so a steady state prints nothing and a running game is
  indistinguishable from a hung one on a silent terminal. The heartbeat
  reports calls-since-last-tick and the busiest imports, which answers the
  only question that matters: working, or spinning?

### 2026-07-26 (12) — Past the bugcheck; GPU init reached; vblank implemented

- **The bugcheck is gone.** Physical memory and the filled-in statistics were
  the blocker; with them the game runs straight past the abort it had hit in
  every previous run and on to **56 imports**.
- Physical allocations are unmistakably GPU buffers: `0x30000` x4, then
  `0x50000` x2, then **4 MiB, 4 MiB and 16 MiB**. It also writes to
  `0x7FC80000` — the Xbox 360 GPU register window.
- The new imports are almost entirely graphics: `XGetVideoMode`,
  `VdInitializeEngines`, `VdSetGraphicsInterruptCallback`,
  `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`,
  `VdQueryVideoMode`, `VdQueryVideoFlags`, `VdRetrainEDRAM`,
  `VdIsHSIOTrainingSucceeded`, plus `DbgPrint` and `_vsnprintf`.
- **It then hangs**, and the reason is structural rather than a bug: the
  console's graphics driver calls back into the title on every vertical
  blank, and the render loop waits on that callback. With no GPU there is no
  vblank, so the wait never ends.
- `kernel/video.cpp` supplies the *shape* of a GPU without drawing anything:
  a 720p widescreen progressive video mode, ring-buffer calls accepted, and
  a **host thread standing in for the display's 60 Hz heartbeat**, invoking
  the registered interrupt callback on its own guest stack. The ring
  buffer's read-pointer writeback is kept at "fully caught up" for the same
  reason — a frozen read pointer is another way to wait forever.
- `kernel/debug.cpp` implements `DbgPrint`/`_vsnprintf`, so the engine can
  tell us what it is doing in its own words. **Stated limitation:**
  `_vsnprintf` copies the format string through without substituting
  arguments. Walking a PowerPC va_list properly (eight GPR slots, then the
  stack, with separate float registers) is fiddly enough that getting it
  subtly wrong would produce plausible but false log messages — worse than
  none. Literal messages come through perfectly; the rest arrive with their
  `%s` and `%d` visible, which is unmistakably a limitation rather than a
  lie.
- **Pseudo-handles.** `ObReferenceObjectByHandle: unknown handle 0xFFFFFFFE`
  appeared twice — that is NT's "current thread" constant (and `0xFFFFFFFF`
  is "current process"), which need no allocation and were being looked up
  as ordinary handles. Now backed on first reference.
- Link-tested: 64 imports implemented, none defined twice.

### 2026-07-26 (11) — Config file read; physical memory and clocks implemented

- **`game_shared.ini` read in full** — 45 bytes, size query correct, no more
  2 GB allocation. `RtlNtStatusToDosError` disappeared from the trace
  entirely, which is the clean confirmation that the file path stopped
  failing.
- **The thread pool named itself**, via the decoded `SetThreadName`
  exceptions: `JQ worker 0 (CPU 2)`, `1 (CPU 5)`, `2 (CPU 3)`, `3 (CPU 1)`,
  `4 (CPU 4)`, and `Game Master`. A job-queue pool pinned across the 360's
  six hardware threads. Worth recording as structural knowledge about the
  engine, not just a log curiosity.
- **Same bugcheck, same backtrace, same place** — main thread, via
  `sub_8290B1D0 -> sub_829395C0 -> sub_8290BC28 -> sub_82515E60 ->
  sub_82515430 -> sub_82514AC8 -> sub_82B27EB8`. So the file fix, while
  correct, was not on the critical path to the abort.
- **Four more out-parameter stubs found — the same failure shape for the
  fourth time today.** `MmQueryStatistics` (×6) takes an `MM_STATISTICS*`
  and filled in nothing; `KeQuerySystemTime` (×2) takes a `LARGE_INTEGER*`
  and filled in nothing; `KeQueryPerformanceFrequency` returned 0, which is
  a *divisor* in the caller; `KeGetCurrentProcessType` returned 0 (idle)
  rather than 1 (title).
- **And `MmAllocatePhysicalMemoryEx` returned NULL every time (×4).** That
  is very likely the real story behind the unexplained reads at
  `0xAD000010`, `0xAE010000` and `0x59000000` — the Xbox 360 aliases RAM
  into `0xA0000000..0xBFFFFFFF`, and the game was dereferencing physical
  memory it had asked for and never received. `0x59000000` sits outside
  that window, so it may be a separate garbage pointer rather than the same
  cause; noting the uncertainty rather than assuming.
- Implemented in the new `kernel/system.cpp`: a physical allocator in the
  uncached alias window, `MmGetPhysicalAddress`, a filled-in `MM_STATISTICS`
  reporting 512 MiB with the title owning three quarters, real system time
  in Windows 100 ns ticks, and the 50 MHz timebase frequency.
- Link-tested again: every import defined exactly once, seven new
  implementations displacing their stubs. Both builds compile.

### 2026-07-26 (10) — Six threads run concurrently; first real file opened

- **Guest threads are real and running.** Six host threads executing
  recompiled game code at once, five of them sharing entry `0x82963840`
  (a worker pool) plus one at `0x829677D0`. All created suspended and
  released by `NtResumeThread`, exactly as the release-gate design intended.
- **First real file I/O.** `D:\game_shared.ini` resolved to the host dump and
  opened first try — so the game addresses its disc as `D:\`, and stripping
  to the first `:` is the right mapping for this title.
- `NtWaitForSingleObjectEx` x14 with no hang: the event and mutant
  implementations are carrying real cross-thread synchronisation.
- **The abort was `NtQueryInformationFile`.** Still a stub, it returned
  `STATUS_SUCCESS` while writing nothing to its out-parameter, so the game
  read uninitialised memory as the file size — `0x82010000`, an address, not
  a size — and asked for 2.03 GB. Implemented `FileStandardInformation`,
  `FilePositionInformation`, `FileNetworkOpenInformation`, `NtReadFile` and
  `NtSetInformationFile`.
- **Correction to entry (9): `RtlRaiseException` x6 was never a failure.**
  I read "one exception per thread" as six errors. `0x406D1388` is the
  `SetThreadName` convention — the exception is just a carrier for a name
  string. Now decoded, so the trace prints thread names. The thread-handle
  diagnosis in (9) was still right, but this part of the evidence was not
  evidence.
- Standing lesson, third instance today: **a stub that reports success
  without filling in its out-parameter is worse than one that fails.** Same
  shape as `KeBugCheck` returning when it must not, and as stubs leaving
  `r3` untouched.

### 2026-07-26 (9) — 31 imports; kernel objects, threads and files implemented

- The FP fix worked. The run reached **31 of 214 imports** and produced a
  single clean bugcheck with a full backtrace through `_xstart` to
  `__scrt_common_main_seh` — no cascade, exactly as intended.
- **The counts localised the failure precisely.** `ExCreateThread` ×6,
  `KeSetAffinityThread` ×6, `NtResumeThread` ×6 — and `RtlRaiseException`
  ×6. One raised exception per thread created. `RtlNtStatusToDosError` ×13
  says it was busy converting failures into error codes. The game was
  creating six threads, getting garbage handles back, and throwing each
  time; the bugcheck came from `sub_82B27EB8`, `0x12080` past `_xstart`, so
  a CRT abort/assert handler.
- Implemented the kernel object model, which is what all of that needed:
  - `object.cpp` — handle table. Objects are registered **twice**: under a
    handle, and under a small guest-visible block whose address is the
    "object pointer" `ObReferenceObjectByHandle` hands back. The guest mixes
    handles and pointers freely, so lookups accept either.
  - `sync.cpp` — events (manual and auto-reset), mutants,
    `NtWaitForSingleObjectEx` with real timeout conversion from the guest's
    100 ns `LARGE_INTEGER`, `NtClose`, and the `Ob*` reference calls.
  - `thread.cpp` — **guest threads are now real host threads.** Recompiled
    functions are ordinary C++ functions, so a fresh `PPCContext` plus a
    fresh guest stack is all one needs. `CREATE_SUSPENDED` parks the thread
    on a release gate rather than deferring creation, so its handle is valid
    immediately. Affinity and priority are accepted and ignored.
  - `file.cpp` — `NtCreateFile` reads the `OBJECT_ATTRIBUTES` -> `ANSI_STRING`
    name, maps device/drive paths onto a host directory (`WOS_GAME_ROOT` or
    `private/game/`), and **reports every path requested** whether or not it
    can open it. Knowing what the game asks for is the immediate value.
- Note on the new thread trampoline: it calls `ctx.fpscr.loadFromHost()`.
  Every thread has its own MXCSR, so a value-initialised context there
  reproduces the FP-exception-mask bug from entry (8) — but only on worker
  threads, which would have been considerably nastier to find.
- **Generated stubs now zero `r3`.** Leaving it untouched means the caller
  reads whatever it happened to leave there as the return value; that is how
  a run came to dereference guest `0x14` and `0xFFFFFFFD`. Zero is
  `STATUS_SUCCESS` for the many NTSTATUS imports and a null handle for the
  rest — wrong sometimes, but wrong identically every run.
- Re-verified by partial link that every import is defined exactly once,
  with the 12 new implementations displacing their stubs and the rest still
  covered. Both Win32 and POSIX builds compile.

### 2026-07-26 (8) — Allocator works, panic gone; FP exception masks fixed

- **The kernel implementations paid off immediately.** `KeBugCheck` and
  `HalReturnToFirmware` are no longer called *at all* — both were purely
  downstream of the failed first allocation, which confirms the previous
  entry's diagnosis outright rather than by inference.
- The game now allocates and keeps going: 1 MiB reserve then a 64 KiB commit
  at the reserved base (`MEM_RESERVE` `0x60002000` then `MEM_COMMIT`
  `0x60001000`), a ~32 MiB block, then 256 KiB — and reaches
  `MmQueryStatistics` and `MmAllocatePhysicalMemoryEx`, two imports never
  seen before.
- **Note the reserve/commit pair works by accident.** `NtAllocateVirtualMemory`
  honours a requested base, so the commit landed on the reserved address
  correctly, but the allocator does not actually distinguish the two. That
  needs fixing before the guest does anything more sophisticated.
- **New stop: `EXCEPTION_FLT_INEXACT_RESULT` (0xC000008F).** `PPCContext ctx{}`
  value-initialises `fpscr.csr` to 0, so the first `enableFlushMode()` writes
  MXCSR = `0 | FlushMask` = `0x8040`. Bits 7–12 are the FP exception *masks*
  (1 = masked), so that clears every one and the next inexact result traps.
  The host default is `0x1F80`. Fixed with `ctx.fpscr.loadFromHost()` before
  entering the guest, giving `0x9FC0` — masks preserved, FTZ+DAZ set. PowerPC
  disables FP traps via `MSR[FE0,FE1]`, so the game never expects them.
- **The crash reporter was killed by the thing it was reporting.** It printed
  `=== import trace ===` and stopped, because `DumpImportLog` computes
  `100.0 * reached / total` — floating point, masks still cleared, trap again
  inside the handler. All three reporting paths now call
  `RestoreHostFpState()` first. Also added the seven FP exception codes to
  the reporter's name table, so `0xC000008F` reads as "FP inexact result"
  rather than "unknown".
- Verified by direct test rather than argument: zero-init `| FlushMask` gives
  `0x8040` with masks clear; `loadFromHost() | FlushMask` gives `0x9FC0` with
  all six masked and FTZ+DAZ set.

### 2026-07-26 (7) — Recursion identified; first real kernel implementations

- **The backtrace named the cycle immediately:** `sub_82B31A48 +0x1CE` ⇄
  `sub_82B31B88 +0xBD`, repeating all the way down. dbghelp symbolisation
  worked first time.
- **The call counts settled the diagnosis without any guessing.** Per
  iteration, exactly: 1× `KeGetCurrentProcessType`, 2× `KeBugCheck`, 6×
  `RtlInitAnsiString`, 1× `RtlEnter/LeaveCriticalSection` — 57204/19068/9534
  are exact integer multiples, so this is a deterministic loop, not chaos.
  That shape is a **panic handler formatting a message**.
- **Cause: `KeBugCheck` is `DECLSPEC_NORETURN` on hardware — it halts the
  console.** Stubbed as an ordinary returning function, the panic handler
  returns into the code that panicked, which panics again. ~19,000 nested
  bugchecks and a runaway stack follow directly.
- Added an **import override mechanism**: generated stubs are now wrapped in
  `#ifndef WOS_IMPL_<name>`, and `kernel/kernel_overrides.h` lists what is
  implemented for real. No regeneration needed per function, and forgetting
  the `#define` fails loudly at link rather than silently doing the wrong
  thing. Verified by a partial-link test: every import defined exactly once,
  implementations displacing their stubs, stubs covering the rest.
- First real kernel code in `WoSRecomp/kernel/`:
  - `panic.cpp` — `KeBugCheck`, `KeBugCheckEx`, `HalReturnToFirmware` now
    terminate, printing the bugcheck code (named where known), the guest
    backtrace and the import trace.
  - `memory.cpp` — bump allocator behind `NtAllocateVirtualMemory`, honouring
    a requested base, respecting `MEM_LARGE_PAGES` alignment, committing at
    the harness's 64 KiB granularity. `NtFreeVirtualMemory` accepts and
    ignores.
  - `thread.cpp` — TLS slots (global allocation, `thread_local` values) and
    critical sections keyed by guest address using `std::recursive_mutex`,
    so the guest's own structure layout never has to be matched. `std::map`
    rather than `unordered_map` deliberately: a rehash would relocate a mutex
    another thread is blocked on.
  - `rtl.cpp` — `RtlInitAnsiString`, writing a real big-endian `ANSI_STRING`.
- CMake now rejects a pre-override `imports_generated.cpp` with an
  explanatory error instead of letting it produce a wall of duplicate-symbol
  failures.

### 2026-07-26 (6) — 15 imports reached; runaway stack made self-reporting

- **The import trace exists.** Commit-on-first-touch worked: the first fault
  (guest `0x93010000`) was committed and execution resumed straight into
  `NtAllocateVirtualMemory`, then 14 more imports. Full ordered list in the
  findings log — that is the runtime to-do list, in the game's own priority
  order, which was the entire point of building the harness.
- Two logged first-touches give away the underlying problem: guest
  `0x00000014` and `0xFFFFFFFD`. Import thunks are `nop/nop/nop/blr` and our
  stubs only log, so `r3` keeps whatever the caller left. The game reads a
  stale register as a return value and dereferences it.
- `KeBugCheck` at #14 is the kernel-panic call. The game was already trying
  to die before the stack ran away.
- **The runaway itself:** 26 consecutive 64 KiB regions descending from
  `0x81FB0000` to `0x81E20000`, each first touched near the top by a write.
  A stack pointer walking down, ~15,000 frames deep.
- **It died with no crash report**, which is itself a finding: recompiled
  guest functions are ordinary C++ calls, so guest depth is host depth, and
  the default 1 MiB host stack overflowed. A stack overflow leaves no stack
  to run an exception filter on, so the process dies silently and takes the
  trace with it.
- Fixed both ends: **256 MiB host stack** so the detector wins the race, and
  **runaway detection** that prints a `dbghelp`-symbolised backtrace of the
  recompiled functions on the stack. Repeated `sub_XXXXXXXX` names name the
  cycle directly.
- Stack-region predicate unit-tested against the actual regions from the
  run: the four non-stack commits are correctly excluded, the three stack
  commits included, boundaries checked. Fires at 32 commits; the real run
  reached 26 before dying.
- Deliberately did **not** change the stubs to zero `r3` this round. It is
  very likely part of the fix, but changing stub semantics at the same time
  as adding the backtrace would confound the one measurement worth taking.

### 2026-07-26 (5) — Recompiled code runs; guest space widened to the full 4 GiB

- **The game's own code executed for the first time.** Section validation
  worked: `.reloc` (guest `0x82FA1000`, size `0xCCC80`) was the out-of-range
  section, clamped to the `0x5F000` bytes actually present. Relocation data
  is unused at runtime, so zeroing the tail is harmless. Everything after
  that proceeded: 50,516 function-table entries, entry point `0x82B15E38`
  resolved, and the call made.
- It then faulted **inside recompiled code** — `+0x41993` from the entry
  function, same module — reading guest `0x93010000`.
- **My crash classifier said "host-side bug, not the recompiled game" and
  was wrong.** It treated anything outside the reservation as host-side,
  but the reservation only ran to `0x842585C8` while the guest addresses in
  32 bits. `PPC_LOAD_U32` is `*(uint32_t*)(base + x)` with no masking, and
  `ppc_context.h` declares `PPC_MEMORY_SIZE 0x100000000`. `0x93010000` is a
  perfectly ordinary Xbox 360 physical-memory alias.
- Now reserving the full **4 GiB** (`kGuestReserve = PPC_MEMORY_SIZE`).
  Reservation is address space, not memory — nothing is committed until
  touched. A `static_assert` keeps it covering the function table.
- Added **commit-on-first-touch** via `AddVectoredExceptionHandler`: a guest
  fault commits the enclosing 64 KiB and resumes. Without it the game dies
  within a few thousand instructions, long before asking the kernel for
  anything. The cost is that a wild pointer now reads zeros and wanders on,
  so: each region is logged (first 48), there's a 512 MiB ceiling, and
  `WOS_NO_AUTOCOMMIT=1` restores hard faults.
- Classifier rewritten: image / call-table / physical-alias / user-space /
  null-ish, with a specific message for a call through an empty call-table
  slot, which is the next failure mode worth expecting.
- Verified on Linux that a 4 GiB `PROT_NONE` reservation succeeds and that
  commit-on-fault-and-resume works (4 addresses incl. `0x93010000` -> 3
  commits, two sharing a 64 KiB region). The Windows VEH path is the same
  shape but different APIs, and is compile-checked only.

### 2026-07-26 (4) — Crash located: XEX section sources aren't bounds-checked

- The crash reporter fired and answered the question in one run:
  ```
  === CRASH: access violation (0xC0000005) ===
  tried to READ address 000001DFC6833000
  that is OUTSIDE the guest reservation (000001DFC6840000 .. 000001E04AA985C8)
    -> host-side bug, not the recompiled game touching bad memory
  ```
  A **read**, **below** the guest base, with the "committing indirect-call
  table" marker never printed — so it faulted inside the section-mapping
  loop, on a section after `.XBLD`, and the bad pointer was the memcpy
  *source*, not the destination.
- **Cause is in XenonUtils' XEX loader, and it's structural.**
  `Image::Map` is handed `image.data.get() + section.VirtualAddress` with no
  check that the result is inside the buffer. Those two numbers need not
  agree: for `XEX_COMPRESSION_BASIC`, `xex.cpp` recomputes `imageSize` as the
  sum of the compression blocks and allocates *that*, then line 261 sets
  `image.size = security->imageSize` — the header's value. When the header
  value is the larger of the two, `image.size` overstates the allocation and
  any section near the top of the image points past its end.
- The harness no longer trusts either number. It prints the decrypted
  buffer's real bounds and the section count, prints each section's name /
  guest base / size / source pointer **before** touching it, and then
  validates: a source wholly outside the buffer is zero-filled, one that
  runs off the end is clamped to what's actually there, and a section
  extending past the guest reservation is skipped. Each case is reported.
- Also added `CleanName()`. `IMAGE_SECTION_HEADER::Name` is 8 bytes and is
  not null-terminated when the name uses all 8, and XenonUtils builds a
  `std::string` from it with the `const char*` constructor — which is why
  `BINKDATA` printed as `BINKDATAh=` and `.XBMOVIE` dragged in a newline.
  Cosmetic, but a garbled name in a diagnostic is a garbled diagnostic.
- `CleanName` and the clamp arithmetic are unit-tested (8-char bleed,
  non-printable leader, empty name; wholly-inside, runs-past-end,
  starts-at-end, starts-before-begin, last-valid-byte). Both the Win32 and
  POSIX builds type-check.

### 2026-07-26 (3) — First execution: harness runs, maps the image, then faults

- **`WoSRecomp.exe` linked and ran for the first time.** It parsed the XEX,
  reserved 2.06 GiB of guest space and mapped all 12 sections at their
  virtual addresses (`.rdata`, `.pdata`, `BINKBSS`, `.text`, `BINK`,
  `.data`, `.tls`, `BINKDATA`, `.XBMOVIE`, `.idata`, `.XBLD`), then died
  with a segfault. No import stubs were reached.
- Confirmed the two stale chunk files were the whole link problem: deleting
  `ppc_recomp.197.cpp` and `.198.cpp` left 198 sources (197 chunks +
  `ppc_func_mapping.cpp`), exactly as predicted, and it linked first try.
- `build_tools.sh`'s new self-healing path worked on the real tree:
  `working tree does not match patches/XenonRecomp — resetting`, saved the
  superseded diff to `logs/`, applied cleanly, rebuilt XenonRecomp.
- **The stopping point in that run cannot be trusted.** Under MinTTY stdout
  is a pipe, so a native `.exe` gets *fully* buffered output — the last
  visible line is wherever the buffer last flushed, not where the fault
  happened. Whether ~870 bytes of section map should have been visible at
  all under full buffering is contradictory, which is itself a reason not
  to reason from it. Fixed at the source: `setvbuf(stdout, nullptr,
  _IONBF, 0)` in `main()`.
- Added a Windows unhandled-exception filter that reports the exception
  kind, the faulting instruction, whether it was a read or a write, and —
  because the guest space is a flat `base + guest_address` mapping — the
  **guest** address it corresponds to, classified as image / indirect-call
  table / unmapped low memory / outside the reservation entirely. Also
  added phase markers around the table commit and population.
- `DumpImportLogUnsafe()` added for the crash path: `try_to_lock` rather
  than `lock`, because a fault inside `LogImportCall` would leave the
  mutex held by the faulting thread and deadlock the reporter. A hang is
  worse than a slightly racy dump.
- **Windows-only code is now actually verifiable here.** Installed
  `g++-mingw-w64-x86-64` in the sandbox and cross-compiled the harness for
  `x86_64-w64-mingw32`; both the Win32 and POSIX paths type-check. Doesn't
  substitute for running it (no Wine), but it stops Windows-only code
  being pushed sight-unseen — which has bitten this project twice.

### 2026-07-26 (2) — Stale-file diagnosis confirmed by arithmetic; patching made self-healing

- **The fix from earlier today never ran.** `build_tools.sh` aborted with
  `patch does not apply and is not already applied`, so XenonRecomp was
  never rebuilt and every later step used the old binary. The link failed
  identically, byte for byte.
- **Why the patch wouldn't apply:** `apply_patches` decided per patch file —
  reverses cleanly means applied, applies cleanly means apply, neither is a
  hard error. Amending an already-applied patch file defeats that: the old
  hunks block a forward apply and the new hunks block a reverse apply. Every
  future patch amendment would have hit this.
- `apply_patches` now targets the exact state (`HEAD` + `patches/`) instead
  of reasoning per file. If the tree doesn't match, it resets just the files
  the patches touch and applies from clean, saving the discarded diff to
  `logs/<ts>-<name>-discarded.diff` first. Reproduced the exact failure
  state locally, confirmed both old checks fail on it, and confirmed the new
  path recovers to a byte-exact match.
- **The stale-file diagnosis is now confirmed numerically.** XenonRecomp's
  first progress line prints `1/functions.size()*100` = `0.0019879923`, so
  `functions.size() = 50302`. At 256 functions per chunk that is
  `ceil(50302/256) = 197` files, indices 0..196. There were **199** on disk.
  `ppc_recomp.197.cpp` and `.198.cpp` are leftovers — and `.197` is exactly
  the file the linker named.
- **The contiguity check added earlier was worthless and has been removed.**
  Leftovers are contiguous by construction: both runs number from zero, so
  strays are always the tail of an unbroken sequence. It passed happily with
  two stale files present. Replaced with a real check — the patched
  recompiler now prints `Wrote N chunk file(s).` and `recompile.sh` compares
  that against the file count, warning explicitly if the line is absent
  (which means the tool predates the patch).

### 2026-07-26 — First host link; duplicate symbols traced to stale generated files

- Ran the full pipeline for the first time: 214 import stubs emitted,
  `WoSRecompLib` already current, host configured and compiled all 229
  targets — then **failed at the final link** with ~20 `duplicate symbol`
  errors, e.g. `_sub_82BD6F50` defined in both `ppc_recomp.194.cpp` and
  `ppc_recomp.197.cpp`, all clustered in `0x82BD6F50..0x82BD7050` at 8/16
  byte spacing.
- **First hypothesis was wrong.** Suspected the recompiler's `functions`
  vector was never deduplicated by base — which is true (`recompiler.cpp`
  sorts by base and there is no `std::unique` anywhere) but is *not* what
  caused this. Walking the four sites that append to `functions`, three are
  guarded by `image.symbols.find()` and the fourth (`config.functions`) had
  no duplicate addresses. Reading `SymbolTable::find` also shows it matches
  on *exact* address via `equal_range`, so overlapping functions are
  routine here and don't collide.
- **Actual cause: nothing ever deletes generated output.**
  `SaveCurrentOutData()` writes `ppc_recomp.{cppFileIndex}.cpp` with a
  monotonically increasing index, and hash-compares before rewriting so
  unchanged chunks don't trigger recompilation. Neither step removes a
  file. Every config change that reduced the function count therefore left
  the *tail* of the previous, longer run in `WoSRecompLib/ppc/` — files
  holding the same high-address functions under the old boundaries.
  `ppc_recomp.194.cpp` was the current run's last chunk; `.197` was a
  survivor from an earlier one. The address clustering is the tell: chunks
  are emitted in ascending base order, so leftovers always contain the
  highest addresses.
- **Why it hid for so long:** `ar`/`llvm-lib` does not check for duplicate
  symbols across object files. The "192 MB library, zero errors" build was
  never evidence the output was consistent — only linking an executable
  tests that, which had never been done until now.
- Fixed in `patches/XenonRecomp/0001-wos-recompiler-fixes.patch`: after
  generation, `Recompile()` deletes `ppc_recomp.N.cpp` for N upward from
  the final `cppFileIndex` until one is missing, printing each removal.
  Keeps upstream's incremental-rebuild behaviour intact. Verified it
  compiles and links on Linux; the drift check reproduces the patch byte
  for byte.
- Patch renamed `0001-implement-missing-instructions.patch` ->
  `0001-wos-recompiler-fixes.patch`, since it now carries two unrelated
  fixes. They stay in one file deliberately: the drift check compares the
  concatenated patches against the submodule's whole `git diff HEAD`, which
  two patches touching the same file could not reproduce.
- Corrected the stale header comment on `WoSRecomp/CMakeLists.txt`, which
  still claimed undefined symbols would enumerate the missing imports.
  `main.cpp` had already been corrected; the CMake file hadn't.

### 2026-07-25 — Session-log layer added; record-keeping formalised

- Added `docs/sessions/` (one file per session) plus
  `docs/sessions/README.md` documenting conventions, and backfilled
  `docs/sessions/2026-07-25.md` for this session as the first real entry.
- Resolves an open question about whether to keep one rolling log or one
  file per session: **both, layered.** Per-session files alone would give
  isolation but destroy orientation (you'd read N files to reconstruct
  state); a single "refreshing" file alone would keep orientation but lose
  the history that stops you re-walking dead ends. So: layer 1 (this
  file's Current State) is rewritten and authoritative for *now*; layers 2
  and 3 are append-only history.
- Core rule established: **exactly one file is authoritative for the
  present**, everything else is self-evidently dated history and is never
  edited after the fact (corrections go in newer entries). Stale info that
  looks authoritative is the failure mode this whole system exists to
  prevent.
- Rationale for going beyond PR history: git records what changed *and
  succeeded*. It structurally cannot record dead ends, ruled-out
  approaches, or abandoned work — none of which ever gets committed — and
  those are the expensive things to rediscover. Session logs therefore
  weight **negative results** highest, and carry an explicit "unverified
  claims" section so untested assertions don't silently harden into
  assumed-good.

### 2026-07-25 — Docker dev environment added (user has no Homebrew)

- User doesn't have Homebrew on their Mac and has Docker Desktop instead.
  Added `tools/docker/Dockerfile` + `tools/docker/README.md`: a container
  built on Ubuntu 24.04 + Clang 18 + CMake + Ninja — the exact package
  combination already proven to build this project natively (this sandbox).
  Workflow: bind-mount an empty host folder to `/workspace` and the user's
  extracted `wos` dump (read-only) to `/wos`, clone+build+
  `import_dump.sh` inside the container, with all output persisting on the
  host via the bind mount (not lost when the container exits).
- **Honesty note on testing:** attempted to actually build the image in
  this sandbox to verify it end-to-end (got the Docker daemon itself
  running here, which worked), but the sandbox's egress policy blocks
  Docker Hub's CDN (`production.cloudfront.docker.com` — confirmed via
  `curl .../__agentproxy/status`, a 403 policy denial, not a bug). Per the
  proxy's own guidance, policy denials aren't something to route around, so
  the image build itself is untested by me. Confidence is still high since
  it's the identical package set verified natively, but this should be
  flagged as unverified until the user's own Docker Desktop (normal
  internet access) builds it successfully.
- Wired the Docker option into `docs/01-getting-started.md` (prerequisites
  section) and the root `README.md` file tree.

### 2026-07-25 — Findings log added; macOS confirmed as a valid dev platform

- Added `docs/05-findings-log.md`: the durable, committed place to record
  WoS-specific technical data (dump directory tree, `xex_info` output,
  region/edition, and — as they're found — the register save/restore
  addresses, setjmp/longjmp, function boundary overrides, invalid
  instruction skips, and mid-asm hooks). Distinct from this file (project/
  tooling changelog) and from `WoS_config.toml` (the machine-readable
  result, no provenance/reasoning). Chat history isn't durable across
  context resets, so anything pasted back needs a home in the repo, not
  just this conversation.
- Confirmed macOS is a legitimate dev platform for the toolchain/analysis
  phase: XenonRecomp/XenonAnalyse/xex_info are portable CMake+Clang with no
  Windows-specific dependencies (uses `simde` specifically for cross-arch
  VMX support). Checked the vendored `dxc-bin` submodule directly and
  confirmed it ships real macOS binaries (x64 and arm64 — `libdxcompiler.dylib`,
  `dxc-macos`, plus a `build-macos.sh`), so `XenosRecomp` should build on
  macOS too, though only Linux has actually been build-verified so far.
  Caveat flagged: needs real Homebrew LLVM/Clang 18, not Xcode's bundled
  `clang` (different versioning/behavior). User is testing on macOS now;
  their Windows workstation isn't available at the moment.

### 2026-07-25 — Dump-ingestion tooling: xex_info, extract-xiso, import_dump.sh

- Added `tools/xex_info`, a small standalone C++ tool (not upstream — ours)
  that prints a XEX's base address, entry point, and section layout by
  reusing XenonUtils' own `Image::ParseImage`/`Xex2LoadImage` loader.
  Deliberately built as its own standalone CMake project (pulls in
  `XenonUtils`'s sources directly from `tools/XenonRecomp/XenonUtils`)
  rather than sharing a CMake target with `tools/XenonRecomp`, to avoid
  cross-project target collisions and keep it buildable independently via
  the same `build_one` pattern `build_tools.sh` already used.
- **Bug found & fixed:** the upstream XEX loader trusts header-declared
  offsets/counts without validating them against the actual file size —
  feeding it a truncated/corrupted file segfaults instead of erroring.
  Added bounds validation in `xex_info/main.cpp` (checks `headerSize`,
  `securityOffset`, and `headerCount` against the file size before calling
  into the upstream parser) so a bad dump fails with a clear message
  instead of crashing. Verified via a synthetic truncated-XEX fixture
  (segfault → clean error after the fix).
- Added [`XboxDev/extract-xiso`](https://github.com/XboxDev/extract-xiso) as
  a submodule under `tools/extract-xiso` — unpacks Xbox `.iso`/XISO disc
  images. Checked its license first: modified BSD (permissive,
  redistributable) — appropriate to bundle.
- Added `tools/import_dump.sh <path>`: given a raw extracted folder or a
  `.iso`, finds `default.xex`/`default.xexp` regardless of layout (flat
  XISO-style or nested GOD-style content folders), copies them into
  `private/` (never touches the original), and prints a directory tree
  (filenames/sizes only) plus `xex_info` output — all safe, non-copyrighted
  metadata meant to be pasted back into chat. Tested against synthetic flat
  and nested fixtures, a truncated-XEX fixture, and a no-XEX-found case; all
  behave correctly. Cleaned up all test fixtures/artifacts afterward — no
  test data left in `private/` or `/tmp`.
- Updated `docs/01-getting-started.md`, `README.md`, `private/README.md` to
  document the new tools and recommend `import_dump.sh` as the default path
  for getting a dump into the repo, with manual placement kept as a
  fallback.

### 2026-07-25 — Toolchain build verified end-to-end

- Built `XenonAnalyse`, `XenonRecomp`, and `XenosRecomp` in the cloud sandbox
  via `tools/build_tools.sh`. All three compile and print correct `Usage:`
  output.
- **Bug found & fixed:** `git submodule add` does not recurse into nested
  submodules. The initial add of `tools/XenonRecomp` and `tools/XenosRecomp`
  left their `thirdparty/` dependencies (fmt, tomlplusplus, xxHash, simde,
  zstd, dxc-bin, etc.) uncloned, and the first configure failed with missing
  `CMakeLists.txt` errors. Fixed by running
  `git submodule update --init --recursive`. Docs already told *users* to run
  this — the scaffolding step itself had just skipped it.
- **Correction:** `XenosRecomp` (shader recompiler) was assumed to be
  Windows-only due to its DirectXShaderCompiler dependency. That's wrong — it
  built and ran cleanly on Linux via the bundled `dxc-bin` submodule.
  `CMakeLists.txt`'s `WOSRECOMP_BUILD_XENOS` option flipped from `OFF` to
  `ON` by default, and the misleading note in `tools/build_tools.sh` was
  corrected.
- No game files were involved — this only exercised the open-source
  toolchain build.

### 2026-07-25 — Initial scaffold created

- Imported `hedge-dev/XenonRecomp` and `hedge-dev/XenosRecomp` as git
  submodules under `tools/`.
- Built repository layout mirroring Unleashed Recompiled's structure:
  `WoSRecompLib/` (generated PPC output + hand-authored config),
  `WoSRecomp/` (runtime, split into `gpu/apu/kernel/os/ui/install/patches`),
  `private/` (gitignored — user's own dumped game files), `docs/` (numbered
  guides), `tools/build_tools.sh` and `tools/recompile.sh`.
- Wrote `WoSRecompLib/config/WoS_config.toml` as a placeholder using
  XenonRecomp's own example config (Sonic Unleashed addresses) — explicitly
  documented as needing full replacement with WoS-specific values.
- Wrote `docs/01-getting-started.md`, `docs/02-config-guide.md` (full field
  reference for the config TOML, including the byte-pattern table for
  finding the 8 register save/restore functions), `docs/03-runtime-architecture.md`,
  `docs/04-roadmap.md`.
- `.gitignore` set up to exclude `private/`, `*.xex`, `*.xexp`, and generated
  `WoSRecompLib/ppc/*` output — this repo must never contain copyrighted game
  files or the recompiled game's CPU code, only the runtime scaffold/tooling.

## 2026-07-28 — the graphics layer, end to end

Eight bugs, each hidden behind the one before it. Every one found by
measurement — disassembling the guest, or reading a counter — rather than by
reasoning about what ought to be true. The two occasions this session where a
theory was acted on without checking it against the code were the two most
expensive detours, and both were the same mistake in the same function.

**Runtime fixes, in the order they were needed:**

1. **`r13` was never set.** It is the per-thread block pointer on Xbox 360,
   not a scratch register. Null meant the GPU watchdog's clock read 0 forever,
   so its 5000-tick deadline was unreachable and it spun without ever calling
   its own timeout handler. New: `kernel/pcr.cpp`.
2. **Video driver addresses are physical.** `VdInitializeRingBuffer` and
   `VdEnableRingBufferRPtrWriteBack` receive physical addresses; we wrote the
   read pointer to a virtual address of the same number — an unrelated page,
   which the harness then committed on first touch, making the error look
   deliberate.
3. **Nothing executed the command packets.** The GPU fence rides on PM4
   opcode 0x58. Two per frame: a progress pointer and a counter.
4. **The ring size argument is a log2 dword count, not bytes.** Read as
   bytes, the capacity came out four times too small; once the write pointer
   passed it, consumption stopped *silently* while the read pointer was still
   published as caught up.
5. **`NtWaitForMultipleObjectsEx` was an unimplemented stub** returning
   success to a wait — 11.7 M calls per five seconds.
6. **`KeDelayExecutionThread` was unimplemented**, so every `Sleep()` in the
   game was a no-op: 45 M calls per five seconds.
7. **The physical window modelled a 256 MiB console.** The real uncached
   alias is 0xA0000000..0xBFFFFFFF — 512 MiB. A 290 MiB request during
   archive loading was being refused.
8. **The graphics interrupt callback takes two arguments, not three**, and
   its vblank path is gated on GPU register 0x7FC86544 bit 0, which nothing
   ever set. Correcting an earlier wrong change of ours; see the findings log.

**Tooling added:**

- `xex_info --disasm <addr> [count]` — disassemble guest code, marking branch
  targets and naming the symbol behind a `bl`. Stops at a real function end.
- `xex_info --xrefs <addr>` — every branch to, and stored pointer to, an
  address. Points it at an import thunk and it lists every call site.
- `kernel/format.cpp` — real printf-family formatting for `DbgPrint`,
  `_vsnprintf` and `sprintf`. This is what made the game's own D3D hang dump
  legible, and that dump is what identified the fence.
- A GPU command-processor thread that consumes the ring, follows indirect
  buffers, executes register writes and memory-write packets, and reports each
  new opcode once.
- A shared diagnostic lock, after a stack dump interleaved three threads'
  frames into something worse than useless.

**The pattern worth keeping.** A stub that returns success to a *wait*
inverts the call's timing semantics and turns a block into a busy-spin — that
one cost five separate runs. A bounds check that returns *silently* is worse
than one that crashes. Both produce a system that looks healthy in every log
line while doing nothing at all.
