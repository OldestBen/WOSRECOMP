# Project status — a complete account

Written 2026-09-23, at commit `d7f6d93`.

This is the one document that tries to say *everything*: what the project is,
what actually works, what does not, what has been ruled out, and how far the
remaining work runs. [`PROGRESS.md`](../PROGRESS.md) stays the authority on
"what is true right now" and is rewritten in place;
[`docs/05-findings-log.md`](05-findings-log.md) holds the evidence for every
claim here. This file is the orientation layer between them, for someone
arriving cold or returning after a gap.

---

## 1. What this is

A native PC port of **Spider-Man: Web of Shadows** (Xbox 360), built by static
recompilation with [hedge-dev/XenonRecomp](https://github.com/hedge-dev/XenonRecomp).
The existing PC port runs badly on modern hardware; this is an attempt at a
better one.

**How static recompilation works here, in one paragraph.** XenonRecomp reads
the game's PowerPC executable and emits C++ — one function per guest function.
Those become ordinary C++ functions, so guest call depth *is* host stack depth
and guest threads *are* real host threads. Guest memory is a flat 4 GiB
reservation where guest address `x` lives at `base + x`, big-endian, with no
masking or bounds checks. What the recompiler cannot translate is the console's
*kernel* — every `NtCreateFile`, `KeWaitForSingleObject` and `VdSwap` the game
calls. Those are rewritten to `nop/nop/nop/blr` stubs, and re-implementing them
against Windows is the bulk of the work.

**The critical consequence of that stub design**, which has produced four
separate bugs in this project: an unimplemented import does not fail. It
returns with `r3` untouched — still holding its *first argument*. For a
function returning NTSTATUS that is usually a pointer, which reads back as a
large non-zero error; for one whose first argument happens to be zero, it reads
back as **success**. Either way the game is told something confident and wrong.

---

## 2. What works, verified

| Area | State |
|---|---|
| **Boot** | Runs to 77 distinct kernel imports, twelve threads, for minutes without a hang report |
| **Window** | 1280×720, Win32 + D3D11 swapchain, presenting at 60 Hz from the vblank thread |
| **Memory** | Virtual heap, physical alias window (0xA0000000–0xBFFFFFFF, full 512 MiB), demand commit |
| **Threads** | Real host threads, per-thread KPCR through `r13`, priorities and affinity accepted and ignored |
| **Sync** | Events (including guest-embedded KEVENTs adopted on first touch), mutants, WaitAny, thread-handle waits |
| **Files** | Path translation, reads, `NtQueryInformationFile`, completion APCs |
| **Graphics init** | Ring buffer, command processor consuming continuously, GPU fence tracking the CPU fence, vblank and swap interrupts delivered |
| **Assets** | `game_shared.ini`, `amalga.toc`, 774 KB of `SOUNDSRC_RVB.PCK`, first 512 KB of `game.XEPACK` |
| **Audio registration** | `XAudioRegisterRenderDriverClient` succeeds; the game now takes a code path it never reached before |
| **Controller** | XInput wired through `XamInputGetState` — **written, never tested** |

85 imports have real implementations across 21 source files in
`WoSRecomp/kernel/` and `WoSRecomp/gpu/`.

---

## 3. What does not work

**Nothing of the game is drawn.** The window shows a generated gradient.
`VdSwap` has never been called in any run, so the presenter has never been
handed a front buffer. There is no PM4 translation, no shader recompilation, no
texture handling, no EDRAM emulation.

**Asset loading stops** after the first 512 KB of `game.XEPACK`.

**Two threads wait forever.** `sub_82A7CD00` (thread 4104) on three events
`{0x00010050, 0x00010048, 0x0001004C}`, and one graphics thread on the
guest-embedded event `0x4083FDCC`. None of those has ever been signalled.

**The loader only moves because of a stand-in.** See §4.

---

## 4. The stand-in, stated plainly

The async read of `game.XEPACK` completes correctly and *nothing consumes the
result*. Calling the consumer directly — `sub_82965488(0x00000080)` at
APC-delivery time — makes the guest's own state machine run to completion
unaided:

- request object [0] goes state 1 → 3 → 4
- its handle is recycled to `0x80000080`, high bit set, meaning **freed**
- ev4 (`0x00010030`) is signalled for the first time
- the Game Master leaves the wait it had been stuck in and goes round its loop
- a five-million-per-second `KeDelayExecutionThread` spin stops

**The guest freeing the slot itself is what makes this a diagnosis rather than
a hack.** Six validation steps inside `sub_82965488` would have rejected a
wrong handle, and a wrong call would have left the object in a state nothing
advances. It did neither.

But we still do not know what makes that call on real hardware. Of the four
call sites, the two that run are on the submission side; the two that would
consume a completion have never executed. It is on by default
(`WOS_NO_COMPLETE_IO` disables it) because staying deadlocked finds nothing,
and it is one line to remove when the real mechanism turns up.

---

## 5. Ruled out — the negative results

These cost real time and are worth not repeating.

**The completion APC is not the mechanism.** Four rounds went into the theory
that the console delivers I/O completion as a user APC at an alertable wait.
A census settled it: **one alertable wait in sixty seconds**, out of a hundred
and twenty thousand.

```
NtWaitForSingleObjectEx      0 / 100686   <- never alertable
KeWaitForSingleObject        0 / 1749     <- never alertable
NtWaitForMultipleObjectsEx   1 / 1
KeDelayExecutionThread       0 / 17442560 <- never alertable
```

**The 15-million-per-second spin is `Sleep(0)`.** I claimed it was an infinite
sleep collapsing to a no-op through signed overflow on `INT64_MIN`. Logging the
actual values showed exactly one distinct interval ever passed at that site:
**zero**. A deliberate yield-spin in the game's retry loop. The fix I wrote
predicted the count would fall to nothing; it went *up*, which should have been
treated as falsifying immediately.

**Neither early-exiting thread is the missing I/O worker.** Both were read.
`0x829F4C80` is the XAudio mixer, `0x82A25CE8` a sound worker.

**`sub_82968498` was never supposed to reach `sub_82965488`.** They sit on
opposite sides of a producer/consumer boundary. The first is a state setter.

**The vblank path is correct.** The interrupt callback at `0x82AB9840` is gated
on `[0x7FC86544]` bit 0; we set it, the argument count is right, and a tripwire
confirms `sub_82AC46C8` runs at call #1, #10, #100, #1000. It simply does not
signal the context events.

**The graphics threads are not a deadlocked pair.** One is working:

```
bl@0x82ACEE14 -> obj 0x4083FD7C  30ms      789 call(s), 789 timeout(s)
bl@0x82ACED80 -> obj 0x4083FDCC  INFINITE    1 call(s),   1 timeout(s)
```

Thread 4102 loops correctly on a 30 ms timeout, several hundred times, and the
present branch is taken on the *timeout* rather than on a signal — so it is
doing exactly what it is designed to do. 4103 is the stuck one, on a different
wait in the same function. And the working thread still never reaches
`sub_82AC4E48`, whose tripwire has never fired, so presentation is gated
between the timeout branch and the present call.

---

## 6. Bugs found in our own runtime

Every one of these was silent. None produced an error message.

| Bug | Effect |
|---|---|
| `XamInputGetState` unimplemented | Returned `r3` = user index = 0 = SUCCESS, with the state structure never written. The game read uninitialised guest memory as controller state, every frame, every run. |
| `XAudioRegisterRenderDriverClient` unimplemented | Returned its first argument — a pointer — as an NTSTATUS. The game was told audio registration failed on every run and correctly declined to start its pipeline. |
| Thread-handle waits | Fell through to "unknown object, return success", telling callers a thread had exited while it was still starting. |
| `KeDelayExecutionThread` absolute deadlines | Yielded instead of waiting, justified by a comment claiming there was no absolute clock. There is — it is what `KeQuerySystemTime` returns. |
| `KeDelayExecutionThread` negation | `-interval` on `INT64_MIN` is signed overflow. Correct fix; wrong diagnosis attached to it. |
| Ring buffer capacity | Modelled 0x4000 dwords; the write pointer exceeded it. Adaptive growth now handles it, clamped at the physical allocation edge. |
| `RtlMultiByteToUnicodeN` lengths | In bytes, not characters; guest UTF-16 is big-endian. Fixing it produced the first bulk asset load. |
| `NtWaitForMultipleObjectsEx` | Waited on the first object only and returned 0 unconditionally, i.e. "handle 0 signalled". |

---

## 7. Tooling built

The diagnostic tooling is a substantial part of the work and is why the
remaining questions are answerable at all.

**`tools/xex_info`** — the project's own analysis tool.
- `--func <addr>` disassembles the whole function *containing* an address.
  Everything from `--xrefs` or a runtime return address is an interior address,
  and starting a dump there hides the prologue.
- `--xrefs <addr>` finds every branch and stored pointer, annotated with the
  containing function.
- `--field <disp> [--stores] [--context N]` finds who touches a struct field.
  `--xrefs` structurally cannot answer this: a field access encodes no address.
- `--disasm`, `--imports`, `--emit-stubs`, `--fix-switches`.

**`tools/ask.sh`** — runs several `xex_info` queries and copies *all* the output
at once. `clip` overwrites, so four separate queries used to leave only the
fourth.

**`tools/run_game.sh`** — runs for a fixed duration, stops itself, filters, and
copies the digest. One command instead of run / Ctrl-C / grep.

**`tools/check_syntax.sh`** — compiles every hand-written kernel source against
a stand-in for the generated recompiler header. No MSVC, no XEX, about a
second. Known blind spot: `#ifdef _WIN32` code is invisible when it runs on
Linux, so anything in `gpu/` is only checked by the real Windows build.

**In-runtime diagnostics** — import call census with per-call-site attribution,
wait census keyed on (call site, object), all-thread stack dumper, D3D device
field sampler, loader state dumper reading the guest's own globals, guest
function tripwires, and a PM4 opcode/register census.

---

## 8. Method notes

Four recurring failure patterns, all learned the expensive way.

**A stub that returns success to a *wait* inverts the call's timing
semantics.** It does not fail — it deletes the blocking the caller depends on.
Hit at least four times in different places.

**A diagnostic that stops printing is not evidence that the thing stopped
happening.** A capped `printf`, a function-scope `static bool` shared across
threads, and a report inside the wrong branch each produced a confident wrong
conclusion.

**A diagnostic that is collected and filtered out is worse than one never
written**, because its absence reads as evidence. This happened twice in
consecutive rounds — `[alertable]`, then `[audio]` one round after writing a
comment about `[alertable]`. The digest filter is now a deny-list: keep
everything, name the few noisy things.

**A mechanism that predicts the wrong direction is refuted, not unexplained.**
The `INT64_MIN` theory predicted a spin count would fall to zero. It rose
twenty-fold. That should have ended the theory on the spot.

The governing constraint, from the user, mid-project: *"Let's not make
assumptions going forward and just read the code please."* Every conclusion
since has come from a disassembly, a counter, or a memory dump. It has worked —
the wrong turns above all predate it or violate it.

---

## 9. What is left, and how long

The single most valuable unknown right now: **does the command stream contain
any draw packets yet?** The PM4 census at `d7f6d93` answers it and **has never
been run**. Zero draws means the stream is still device setup and renderer work
cannot start in earnest; non-zero means there is something real to translate.

| Milestone | Remaining work | Scale |
|---|---|---|
| Frame path opens | Read `sub_82AC46C8`, `sub_82ACECF0`, `sub_82AC4E48`, `sub_82AC0C10` — all four in one batch | days |
| `VdSwap` fires | Follows the above; the presenter is already waiting for the address | days |
| Asset streaming completes | Resumes once the frame path moves | weeks |
| **Renderer** | PM4 → draw calls, XenosRecomp shader translation, vertex and texture formats, EDRAM, resolves | **months** |
| Audio output | Registration works; the mixer still retires immediately | weeks |
| Input | Written; needs a controller plugged in | hours |

**A recognisable frame: a few months. Controllable gameplay: 6–12 months.**

The renderer is not one item among six — it is the overwhelming majority of
what remains. Everything above it is measured in days or weeks; that one is
measured in months, and it is the only thing standing between "a window and a
coherent command stream" and "Spider-Man on screen".

Nothing found so far suggests this game is unusually hostile to recompilation.
Every blocker has turned out to be a missing or wrong kernel implementation,
and every one has yielded to reading the code. That is the normal shape of this
kind of port. It is tractable; it is just long.

---

## 10. What is deliberately not in this repository

Permanently, and enforced by `.gitignore`:

- `private/` — the XEX and any disc contents
- `WoSRecompLib/ppc/` — the generated translation of the game's code
- `WoSRecomp/kernel/imports_generated.cpp` — generated stubs
- `logs/*.log` — raw run logs

Only addresses, sizes, names and structural metadata are recorded in these
documents. The submodule pointer is never committed; `patches/` carries our
changes to upstream.
