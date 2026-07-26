# Findings Log — Web of Shadows binary data

This is the persistent record of everything discovered about *Web of
Shadows*'s actual XEX — paste `import_dump.sh`/`xex_info` output and any
addresses found here so they survive context resets/new sessions, instead of
living only in chat history.

**What goes here vs. elsewhere:**
- **This file** — human-readable findings *with provenance* (what we found,
  how we found it, why). The working record.
- [`WoSRecompLib/config/WoS_config.toml`](../WoSRecompLib/config/WoS_config.toml) —
  the machine-readable result. XenonRecomp reads this one; it doesn't explain
  itself.
- [`PROGRESS.md`](../PROGRESS.md) — project/tooling changelog (what code
  changed and why), not game data.

Nothing here should ever be copyrighted content (no disassembly dumps, no
code, no asset data) — only addresses, sizes, names, and structural metadata,
same rule as everywhere else in this repo. See [`private/README.md`](../private/README.md).

---

## Dump info

Ingested 2026-07-26 via `tools/import_dump.sh` on the Windows workstation.
First real run of that script — previously only exercised against synthetic
fixtures.

- **Region/edition:** TBD (NTSC-U retail / Platinum Hits / PAL / JP)
- **`default.xex` size:** **14,528,512 bytes** (0xDDB000)
- **`default.xexp` present:** **No.** No title update in this dump, so the
  config's `[main]` section should omit `patch_file_path` /
  `patched_file_path` and point `file_path` straight at the retail XEX.
  Worth revisiting later — if a TU exists for WoS it may fix retail bugs.
- **Layout:** flat XISO-style — `default.xex` at the dump root, no nested
  GOD-style content folders.
- **`xex_info` output** (captured 2026-07-26):
  ```
  Base address: 0x82000000
  Entry point:  0x82B15E38
  Image size:   0x1000000
  Sections (12):
    name             base         size         flags
    .rdata           0x82001000   0x27D4E8
    .pdata           0x8227F000   0x3A7B0
    BINKBSS          0x822BA000   0x2920
    .text            0x822C0000   0x91C9AC     CODE
    BINK             0x82BDD000   0xF2E4       CODE
    .data            0x82BF0000   0x3951FC
    .tls             0x82F86000   0x11
    BINKDATA         0x82F87000   0x3D68
    .XBMOVIE         0x82F8B000   0xC
    .idata           0x82F90000   0x3FA
    .XBLD            0x82FA0000   0xA0
    .reloc           0x82FA1000   0xCCC80
  ```

### What the layout tells us

- **`.text` is 0x91C9AC = 9,554,860 bytes** ≈ **2.39 million PPC
  instructions**. Large. Expect the generated C++ to be big and the
  eventual compile to be the memory-hungry step flagged in `PROGRESS.md`.
- **`BINK` is a second CODE section.** The Bink Video library (RAD Game
  Tools) is statically linked into the executable, not just used for the
  `.bik` files. It will be recompiled along with everything else, so the
  runtime has to either satisfy or stub whatever it calls out to.
- **`.pdata` is 0x3A7B0.** On Xbox 360 this is the exception-unwind
  function table; at 8 bytes per record that's roughly **29,900 function
  entries** — effectively a ready-made list of function boundaries.
  Potentially very useful for cross-checking XenonAnalyse's detection and
  for populating `functions = [...]` overrides. **Not yet exploited.**
- Entry point `0x82B15E38` sits inside `.text`
  (`0x822C0000`–`0x82BDC9AC`), as expected.

### XenonAnalyse

First run completed **cleanly with no output or errors**, writing
`WoSRecompLib/config/WoS_switch_tables.toml`: **12,327 lines, 498 switch
tables** detected across ~2.39M instructions. That density is plausible for
a game this size and gives no reason to suspect the analyser struggled —
though the real test is whether the recompile trips over control flow it
got wrong.

### Notable non-executable files

Not needed for recompilation, but they map the engine's shape and will
matter for the runtime's filesystem/asset layer later:

| File | Size | Notes |
|---|---|---|
| `amalga.toc` | 54,316 | Almost certainly the engine's master archive/table-of-contents. "Amalga" is likely the internal engine name. Asset loading starts here — first thing to reverse when building the I/O layer. |
| `game_shared.ini` | 45 | Tiny config; trivial to inspect. |
| `movies/*.bik` | ~1.3 GB total | **Bink Video** (RAD Game Tools). Runtime will need Bink playback or a substitute. `credits.bik` alone is 652 MB. |
| `$SystemUpdate/` | 7,262,208 | Standard Xbox 360 disc system-update payload. Irrelevant to us. |

`shaba.bik` and `treyarch.bik` confirm Shaba Games (developer, now defunct)
and Treyarch involvement — consistent with the "no public RE work on this
engine" assessment in the odds discussion.

## Register save/restore function addresses

**Resolved 2026-07-26** via `xex_info --helpers`, which scans the decrypted
image (the patterns don't exist in the raw XEX — it's encrypted and
compressed). See [`docs/02-config-guide.md`](02-config-guide.md#register-saverestore-functions--required-wos-specific).

| Field | Address | How found | Notes |
|---|---|---|---|
| `restgprlr_14_address` | `0x82B24710` | `xex_info --helpers` | block size 0x50 = 20 insns ✓ |
| `savegprlr_14_address` | `0x82B246C0` | `xex_info --helpers` | 18 std r14-r31 + std LR + blr |
| `restfpr_14_address` | `0x82B24AEC` | `xex_info --helpers` | block size 0x4C = 19 insns ✓ |
| `savefpr_14_address` | `0x82B24AA0` | `xex_info --helpers` | 18 stfd f14-f31 + blr |
| `restvmx_14_address` | `0x82B25448` | `xex_info --helpers` | block size 0x94 = 37 insns ✓ |
| `savevmx_14_address` | `0x82B251B0` | `xex_info --helpers` | 18 * (li+stvx) + blr |
| `restvmx_64_address` | `0x82B254DC` | `xex_info --helpers` | mirrors save side (0x94) ✓ |
| `savevmx_64_address` | `0x82B25244` | `xex_info --helpers` | block 0x204 = 129 insns (64 regs) ✓ |

**Confidence: high.** All eight matched unambiguously (single candidate
each), all lie inside `.text`, and — the strong evidence — every inter-block
gap equals the theoretical instruction count for what that helper must
contain. A wrong address would not produce five independently consistent
block sizes. Written into `WoS_config.toml`.

## setjmp / longjmp

| Field | Address | How found |
|---|---|---|
| `longjmp_address` | **not located** | Optional. Look for calls to `RtlUnwind`; `setjmp` usually sits just after. Omitted from the config for the first pass — if the game uses them, the symptom is broken control flow on error/exception paths, not a recompile failure. |
| `setjmp_address` | **not located** | as above |

## First recompile — 2026-07-26

**`XenonRecomp` ran to 100% and exited 0**, emitting C++ into
`WoSRecompLib/ppc/`. It is *not* correct yet: two classes of diagnostic came
out, both expected at this stage.

### Class 1 — switch cases jumping outside their function (2,123 errors)

`ERROR: Switch case at <site> is trying to jump outside function: <target>`,
across **123 distinct switch sites**.

**First hypothesis — WRONG, recorded so it isn't retried.** I assumed a
logical function was split across several consecutive `.pdata` records, with
switches jumping between the pieces. `--fix-switches` measured it directly:

```
Loaded 29942 .pdata function records.
Loaded 498 switch tables.
Switch sites whose cases escape their .pdata function: 0
Switch sites with no containing .pdata record:        112
```

**Zero** sites overflow their record. The split-function theory is dead.

**Actual cause:** `.pdata` does not describe every function — 112 of 498
switch sites sit in the *gaps between* records (leaf functions that never
unwind don't need an entry). Those functions are therefore not registered
from `.pdata` at all; XenonRecomp falls back to its heuristic branch scan
(`recompiler.cpp`, scanning for `bl` targets and calling `Function::Analyze`),
and that heuristic sizes them too small — so their switch targets land
outside. 112 unmapped sites vs 123 erroring sites lines up closely.

**Fix:** `functions = [...]` overrides. For sites inside a record, widen the
record. For sites in a gap, recover the function start by disassembling
backwards to the previous function's terminator (`blr` / `bctr` /
unconditional `b`), skipping padding, clamped to the preceding record's end
so an inferred function can never overlap a known one; the end is clamped to
the next record's start for the same reason. Config entries are registered
before `.pdata`, so overrides win.

### Class 2 — unrecognized instructions (265 sites, 15 distinct opcodes)

| Opcode | Count | Notes |
|---|---|---|
| `vrfip128` | 145 | VMX128 round to +inf |
| `vcmpgtsw.` | 39 | vector compare > signed word, record form |
| `bso` | 23 | branch if summary overflow |
| `vcmpgtsh` | 18 | vector compare > signed halfword |
| `vsrh` | 9 | vector shift right halfword |
| `vspltish` | 8 | vector splat immediate signed halfword |
| `vsel128` | 8 | VMX128 select |
| `vslh` | 6 | vector shift left halfword |
| `frsqrte` | 2 | reciprocal sqrt estimate |
| `bns` | 2 | branch if not summary overflow |
| `vsrah` | 1 | vector shift right algebraic halfword |
| `vnor128` | 1 | VMX128 nor |
| `vcfpuxws128` | 1 | VMX128 convert fp→uint word saturate |
| `dcbst` | 1 | data cache block store — almost certainly a safe no-op |
| `bsolr` | 1 | branch to LR if summary overflow |

**All 15 implemented 2026-07-26** in our vendored `recompiler.cpp`:

| Opcode | Implementation |
|---|---|
| `vrfip`/`vrfip128` | `simde_mm_round_ps` with `TO_POS_INF` (mirrors existing `vrfin`/`vrfiz`) |
| `vcmpgtsw` | `simde_mm_cmpgt_epi32`, record form sets `cr6` via `setFromMask(...,0xF)` |
| `vcmpgtsh` | `simde_mm_cmpgt_epi16`; 8 lanes so the mask is `movemask_epi8` → `0xFFFF` |
| `vsrh`/`vslh`/`vsrah` | per-lane over 8 halfwords, `& 0xF` shift count (mirrors word versions) |
| `vspltish` | `simde_mm_set1_epi16` (mirrors `vspltisb`/`vspltisw`) |
| `vsel128` | added to the existing `vsel` case — disasm table gives it the same `{VD,VA,VB,VC}` layout |
| `vnor`/`vnor128` | OR then XOR with all-ones; simde has no NOR |
| `vcfpuxws128` | per-lane clamp to `[0, 2^32-1]` then convert; simde has no unsigned float→int |
| `frsqrte` | `1.0 / sqrt(x)` — exact beats the hardware's ~1/4096 estimate |
| `dcbst` | no-op, alongside `dcbf` (cache maintenance is meaningless here) |
| `bso`/`bns` | `printConditionalBranch(false/true, "so")` — `PPCCRRegister` already has `.so` |
| `bsolr` | `if (cr.so) return;` (mirrors `bltlr`/`bgtlr`) |

**Verified against the game 2026-07-26:** recompile ran to 100% with
**zero `Unrecognized instruction` lines** — all 265 sites across 15 opcodes
now handled. Beforehand the emitted C++ was also rendered and compiled
against the real `ppc_context.h`, confirming field names, simde intrinsics,
brace escaping and both `setFromMask` overloads type-check.

Remaining diagnostics in that run are the 33 known switch sites only.

## Second recompile — 2026-07-26 (after boundary overrides)

Ran to **100%, no hang**, with 106 generated `functions` entries.

| | 1st run | 2nd run |
|---|---|---|
| Switch error lines | 2,123 | **466** |
| Distinct switch sites failing | 123 | **33** |
| Unrecognized instructions | 265 | 265 (untouched) |

78% of the switch errors gone. **33 sites remain**, and the cause is now
understood and different again:

For site `822EF278` the declared function is
`{ address = 0x822EF24C, size = 0xC4 }`, i.e. `0x822EF24C..0x822EF310`.
The rejected labels are `0x822EF2FC` and `0x822EF308` — **both inside it**.
The bounds test (`recompiler.cpp:618`) is
`label < fn.base || label >= fn.base + fn.size`, so the `fn` being
recompiled is *not* our declared function: a second, smaller function
covering the same code is also being recompiled.

**Where the duplicate comes from.** `SymbolTable::find` is a *range* lookup,
so the `.pdata` loop and the `bl`-target scan both correctly skip addresses
already inside a declared function. The linear walk does not:

```cpp
auto fnSymbol = image.symbols.find(base);
if (fnSymbol != end && fnSymbol->address == base && ...)   // EXACT start required
    base += fnSymbol->size;
else
    functions.emplace_back(Function::Analyze(data, dataEnd - data, base));
```

It requires a function starting **exactly** at `base`. The walk advances by
each known function's size from the section start, so if it lands *inside* a
declared function rather than on its first byte, it synthesises an
overlapping one — and that shorter duplicate is what reports the errors.

So the remaining failures are an **alignment** problem: our inferred starts
don't coincide with where the walk arrives. Fixing it properly means either
predicting the walk's tiling, or starting each function at the preceding
`.pdata` record's end — but the latter would swallow any genuine functions
in between (the `bl`-scan would then skip them, folding their code into ours
and breaking calls to them). **Not attempted; diminishing returns versus the
265 unrecognized instructions.**

## First successful compile — 2026-07-26

**The recompiled code builds.** `tools/build_ppc.sh` on the Windows
workstation:

```
==> Jobs: 14  (cores: 32, RAM: 31 GiB -> memory cap 14)
-- WoSRecompLib: 200 generated source file(s)
[201/202] Linking CXX static library WoSRecompLib.lib
Built in 27s with 14 job(s).
  192M  WoSRecompLib/build/WoSRecompLib.lib
```

| | |
|---|---|
| Generated translation units | **200** |
| Build time | **27 s** at -j14 |
| Output | **192 MB** static library |
| Compile errors | **zero** |

### What this proves

The generated C++ is **valid and complete enough to compile**, including
all 15 instructions added this session. ~2.4M PPC instructions are now
native x86-64 object code.

### What it does *not* prove

Compiling is not running. The 33 known-bad switch sites emit `// ERROR:`
comments in place of jumps — that code compiles fine and is simply *wrong*
at runtime. Nothing calls into this library yet.

**It also does not prove the library is internally consistent** — see the
next section. Archiving object files into a `.lib` never checks whether two
of them define the same symbol; only linking an executable does. "Zero
errors" here was a weaker result than it looked.

### Predictions that were wrong

- **Build time.** I said "minutes, not seconds"; it took **27 seconds**.
  The 9950X3D is much faster at this than I assumed.
- **Memory pressure.** I capped at ~1 job per 2 GiB expecting large
  translation units. 200 files finishing in 27 s at -j14 implies real
  headroom — the cap is probably conservative. `JOBS=24` or higher is
  likely fine and worth trying if rebuild time ever matters.
- **Compile errors.** I expected some, particularly around the broken
  switch sites. There were none.

## First host link — duplicate symbols from stale generated files (2026-07-26)

First attempt to link `WoSRecomp.exe` against `WoSRecompLib.lib`. All 229
targets compiled; the link failed:

```
lld-link: error: duplicate symbol: __declspec(dllimport) _sub_82BD6F50
>>> defined at WoSRecompLib\ppc\ppc_recomp.194.cpp:1649
>>>            WoSRecompLib.lib(ppc_recomp.194.cpp.obj)
>>> defined at WoSRecompLib.lib(ppc_recomp.197.cpp.obj)
```

~20 of these, all in `0x82BD6F50..0x82BD7050` at 8/16-byte spacing, then
`too many errors emitted, stopping now`.

### Cause

`Recompiler::Recompile` emits its output through `SaveCurrentOutData()`,
which:

1. names each chunk `ppc_recomp.{cppFileIndex}.cpp` and increments, and
2. hash-compares against the existing file and skips the write if identical,
   so unchanged chunks don't trigger a C++ recompile.

Neither step deletes anything. So whenever a run emitted **fewer** chunks
than the run before it — which happens on any config change that reduces the
function count — the previous run's surplus files stayed in
`WoSRecompLib/ppc/`, holding the same functions under the old boundaries.
`WoSRecompLib/CMakeLists.txt` globs the directory, so they were compiled in.

The address clustering is the diagnostic tell. `.text` ends at `0x82BDC9AC`,
and chunks are written in ascending base order, so the last chunk of any run
holds the highest addresses. Leftovers are therefore *always* top-of-`.text`
duplicates. A duplicate arising from an analysis bug would be scattered.

### Why it stayed hidden

`llvm-lib`/`ar` do not diagnose duplicate symbols across members — that is
the linker's job, and nothing had ever been linked. The library had been
built successfully several times with this already broken.

### What it was *not*

Initially suspected `Recompiler::Analyse`'s `functions` vector, which is
sorted by base at `recompiler.cpp:254` and **never deduplicated** (no
`std::unique` or `erase` exists in the file). That's true but irrelevant
here: three of the four sites appending to `functions` are guarded by
`image.symbols.find()`, and `config.functions` — the unguarded one — had no
repeated addresses. Worth recording separately:

> `SymbolTable::find(address)` (in `XenonUtils/symbol_table.h`) resolves via
> `equal_range(address)`, so it matches an **exact** start address, not
> containment. A function spanning `[X, X+N)` does not shadow a `bl` target
> at `X+4`. Overlapping functions are consequently normal in XenonRecomp
> output and are not, by themselves, a defect.

### Confirmation (second attempt, same day)

The first fix attempt never ran — `build_tools.sh` aborted before rebuilding
XenonRecomp (see the patching note below), so the link failed identically.
That run did, however, settle the diagnosis arithmetically:

XenonRecomp's first progress line prints
`static_cast<float>(i + 1) / functions.size() * 100.0f`, and reported
`0.0019879923` at `i = 0`:

| | |
|---|---|
| `functions.size()` | `100 / 0.0019879923` = **50,302** |
| Functions per chunk | 256 (`if ((i % 256) == 0) SaveCurrentOutData()`) |
| Chunks this run | `ceil(50302 / 256)` = **197** (indices 0..196) |
| Files on disk | **199** |
| Therefore stale | `ppc_recomp.197.cpp`, `ppc_recomp.198.cpp` |

`.197` is precisely the file the linker named. The earlier run must have had
50,689–50,944 functions, a drop of ~400–600 — consistent with the 106
boundary overrides absorbing smaller inferred functions.

### Detection: why counting files cannot work

An initial guard checked that chunk indices were contiguous from 0. That is
useless here and passed with two stale files present: **both runs number
from zero, so leftovers are always the tail of an unbroken sequence.** There
is no gap to find. The only reliable signal is the count the recompiler
itself wrote, so it now prints `Wrote N chunk file(s).` and `recompile.sh`
compares that against the files present.

### Fix

`patches/XenonRecomp/0001-wos-recompiler-fixes.patch` now also deletes
`ppc_recomp.N.cpp` for N counting up from the final `cppFileIndex` until one
is missing, printing each removal. Upstream's incremental behaviour is
untouched. **`build_ppc.sh` does not do this — the prune happens inside
`recompile.sh`, so a stale tree needs a regenerate, not just a rebuild.**

## Kernel/OS imports — the runtime to-do list (2026-07-26)

From `xex_info private/default.xex --imports`. **214 imported functions
across 12 subsystems.** This is the complete surface the runtime must cover,
enumerated from the game rather than guessed.

| Subsystem | Count | Notes |
|---|---:|---|
| Xam (system/UI) | 37 | profiles, content/saves, message boxes, **input** |
| Nt (kernel objects) | 27 | files, events, memory, threads |
| Ke (kernel core) | 26 | threading, TLS, synchronisation, timing |
| other | 36 | networking (15), STFS, CRT (`sprintf`/`_snprintf`), `XGetLanguage` |
| Rtl (runtime library) | 20 | mostly maps onto the C library |
| Vd (video driver) | 20 | **the hard one** — ring buffer, EDRAM, `VdSwap` |
| Xe (GPU/crypto) | 12 | actually XeCrypt (MD5/SHA) + Xex module queries |
| Io (file I/O) | 9 | device layer |
| Audio | 8 | XAudio render driver + XMA decode |
| Mm (memory manager) | 7 | physical memory |
| Ex (executive) | 6 | pools, `ExCreateThread` |
| Ob (object manager) | 6 | handles, symbolic links |

### Things worth noticing

- **Input is three functions.** `XamInputGetState`, `XamInputSetState`,
  `XamInputGetCapabilities`. Mapping an XInput pad onto these is nearly
  trivial, and the 360 controller layout maps 1:1.
- **There is no graphics API to implement.** The 360 talks to the GPU by
  writing command buffers, so the `Vd*` functions are ring-buffer and display
  plumbing, not draw calls. Rendering means interpreting the Xenos command
  stream — the single biggest piece of work in the project.
- **Networking (15 `NetDll_*`) can almost certainly be stubbed** to
  "no network" for a single-player port.
- **`RtlUnwind` is at 0x82BDC94C.** `docs/02-config-guide.md` says the way to
  find `longjmp` is to look for `RtlUnwind` callers, with `setjmp` usually
  adjacent — so this is the lead for the still-unlocated
  `longjmp_address`/`setjmp_address` config fields.
- **`CurlOpenTitleBackingFile`** is not a standard Xbox 360 export. Either a
  Shaba/Activision addition or a mis-resolved ordinal — worth a look before
  trusting it.
- **`StfsCreateDevice`/`StfsControlDevice`** — STFS is the 360 save/content
  package format, so these matter for save handling.

### Why a static list isn't enough

Unimplemented imports fail **silently**: XenonUtils rewrites each thunk to
`nop/nop/nop/blr`, so a missing function returns immediately and the game
misbehaves without saying why. `xex_info --emit-stubs` therefore generates a
logging stub per import, and the host prints them in call order — turning
214 alphabetical names into the game's actual boot sequence, which is what
tells you *which* to implement next.

## Explicit function boundary overrides

Running log of `functions = [...]` entries added to the config and *why*
(what error/crash led to adding each one). Append, don't rewrite — this is
a history, and the reasoning matters more than the raw TOML (which already
lives in the config file itself).

- **2026-07-26 — batch 1 (107 entries): REVERTED, hung the recompiler.**
  Entries ended at "furthest switch target + 4", an arbitrary address rather
  than a function boundary. `recompiler.cpp`'s linear walk advances `base` by
  each function's size and calls `Function::Analyze` where none starts;
  `Analyze` starts at size 0 and returns 0 if its block stack empties at
  once, so `base += 0` spins forever. Symptom: no output whatsoever, not even
  the first progress line.
- **2026-07-26 — batch 2 (106 entries): applied, in place now.** Ends snap
  forward to just past a terminator (`blr`/`bctr`/unconditional `b`), so a
  declared function ends where a function plausibly ends. Written by
  `xex_info --fix-switches --write-config`. Cut switch errors 2,123 -> 466
  and failing sites 123 -> 33.

## Invalid instruction skips

Running log of `invalid_instructions = [...]` entries and what was found at
each address (padding, exception handler data, etc.).

*(none yet)*

## Mid-asm hooks

Running log of hooks added, what they're for, and their runtime
implementation status (stubbed / implemented / working).

*(none yet)*

## Open questions / blockers

*(none yet)*
