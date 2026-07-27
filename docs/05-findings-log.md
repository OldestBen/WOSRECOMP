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

## XEX section sources can point past the decrypted image (2026-07-26)

First execution of the harness faulted while mapping sections:

```
=== CRASH: access violation (0xC0000005) ===
tried to READ address 000001DFC6833000
that is OUTSIDE the guest reservation (000001DFC6840000 .. 000001E04AA985C8)
```

A read, below the guest base, with the next phase marker unprinted — so the
fault was the memcpy **source** inside the section loop, on a section past
`.XBLD`.

### Mechanism

`XenonUtils/xex.cpp:293` maps every section as:

```cpp
image.Map(name, section.VirtualAddress, section.Misc.VirtualSize, flags,
          image.data.get() + section.VirtualAddress);
```

No bounds check. And the buffer's real size need not equal `image.size`:

| Compression | Buffer allocated with | `image.size` set to |
|---|---|---|
| `NONE` | `security->imageSize` | `security->imageSize` |
| **`BASIC`** | **sum of block `dataSize + zeroSize`** (`xex.cpp:177-183`) | `security->imageSize` (`xex.cpp:261`) |
| `NORMAL` | `security->imageSize` | `security->imageSize` |

For BASIC compression the two are computed independently and are only equal
by convention. Where the header value is larger, `image.size` overstates the
allocation, and sections near the top of the image resolve to addresses past
its end.

### Consequence for the recompiler, not just the harness

XenonRecomp reads section data through the same pointers, so an
out-of-range section would feed it unmapped memory too. It has not crashed,
which suggests the sections it actually walks (`.text`, `.pdata`) are well
inside the buffer — but that is luck, not a guarantee.

### What the harness does now

Prints the decrypted buffer's true bounds and the section count; prints each
section's name, guest base, size and source pointer *before* touching it; then
zero-fills sources wholly outside the buffer, clamps those running past the
end, and skips sections extending beyond the guest reservation — reporting
each case rather than trusting the loader.

### Section names are also unterminated

`IMAGE_SECTION_HEADER::Name` is an 8-byte field with no terminator when all
8 bytes are used, and XenonUtils builds a `std::string` from it via the
`const char*` constructor. Hence `BINKDATA` reading as `BINKDATAh=` and
`.XBMOVIE` dragging in a newline. Harmless for `.pdata`/`.text` lookups
(both under 8 chars, so NUL-padded), but any exactly-8-character section
name is wrong. The harness sanitises at print time via `CleanName()`.

## Guest address space is 4 GiB, not 2.06 GiB (2026-07-26)

First execution of recompiled game code faulted reading guest `0x93010000`.

The harness had been reserving only `PPC_IMAGE_BASE + PPC_IMAGE_SIZE +
PPC_CODE_SIZE*2` = `0x842585C8` — enough for the image and the indirect-call
table, and nothing else. But the generated code's memory accessors do no
masking and no bounds checking:

```c
#define PPC_LOAD_U32(x)  __builtin_bswap32(*(volatile uint32_t*)(base + (x)))
#define PPC_MEMORY_SIZE  0x100000000ull
```

so any 32-bit guest address is a valid host address of `base + x`. The Xbox
360 maps physical memory through aliases from `0x80000000` upward, and
`0x93010000` is one of them — an entirely normal address for the game to
touch, roughly 304 MiB into physical RAM.

**The reservation is now `PPC_MEMORY_SIZE` (4 GiB).** That costs address
space, not committed memory.

### Diagnostic lesson

The crash reporter classified this as "host-side bug, not the recompiled
game touching bad memory" — confidently, and wrongly. The rule it applied
(outside the reservation ⇒ host-side) was only valid if the reservation
covered the whole guest space, which it didn't. A classifier is only as good
as the invariant it assumes; that invariant is now enforced by
`static_assert` and by reserving the full range.

### Commit-on-first-touch

Nothing pre-commits guest memory, and the guest allocates across the whole
space, so the harness registers a vectored exception handler that commits the
enclosing 64 KiB on fault and resumes. Without it the game stops within a few
thousand instructions — long before it calls anything we want to observe.

The trade-off is real: a genuinely wild pointer now reads zeros and lets the
game continue rather than stopping at the mistake. Mitigations: every region
is logged (first 48 individually), a 512 MiB ceiling stops runaway commits,
and `WOS_NO_AUTOCOMMIT=1` restores hard faults for when the fault location
matters more than progress.

## Boot order — the first 15 imports the game actually calls (2026-07-26)

The point of the whole harness. This is *Web of Shadows* asking for things in
its own order, not 214 names sorted alphabetically. Implement top-down.

| # | Import | Notes |
|---|---|---|
| 1 | `NtAllocateVirtualMemory` | **First thing it wants.** Currently returns nothing, so the game has no heap. |
| 2 | `HalReturnToFirmware` | Reboot/shutdown. Reached this early = something already went wrong, or it is a probe. |
| 3 | `RtlEnterCriticalSection` | |
| 4 | `RtlLeaveCriticalSection` | |
| 5 | `XexCheckExecutablePrivilege` | |
| 6 | `XGetAVPack` | A/V pack detection — display capability query |
| 7 | `ExGetXConfigSetting` | System config (language, region, ...) |
| 8 | `KeTlsAlloc` | |
| 9 | `KeTlsSetValue` | |
| 10 | `KeTlsGetValue` | TLS trio — the CRT needs these working |
| 11 | `KeQuerySystemTime` | |
| 12 | `RtlInitializeCriticalSection` | Note: called *after* Enter/Leave above |
| 13 | `RtlInitAnsiString` | |
| 14 | `KeBugCheck` | **Kernel panic.** The game is trying to die. |
| 15 | `KeGetCurrentProcessType` | |

Then the guest stack ran away (see below).

### Reads that reveal garbage returns

Auto-commit logged first touches at guest `0x00000014` and `0xFFFFFFFD`.
Neither is a plausible allocation. Import thunks are rewritten to
`nop/nop/nop/blr` and our stubs only log, so **`r3` keeps whatever the caller
left in it** — the game reads a stale register as a return value and
dereferences it. `0x14` is a field offset from a null pointer; `0xFFFFFFFD`
is `-3`.

Also logged: guest `0x93010000`, `0x59000000` and `0xAD000000` — physical
aliases, plausible enough to be real, but with no allocator behind them they
are equally likely to be garbage.

## Runaway guest stack after import 15 (2026-07-26)

After `KeGetCurrentProcessType`, auto-commit logged 26 consecutive 64 KiB
regions descending from `0x81FB0000` to `0x81E20000`, each first touched near
the top of the region by a **write**. That is a stack pointer walking
downward — unbounded recursion, roughly 15,000 frames deep at the observed
frame sizes.

The process then died with **no crash report at all**. Cause: recompiled
guest functions are ordinary C++ functions, so guest call depth *is* host
call depth, and the default 1 MiB host stack overflowed. A stack overflow
leaves no stack on which to run an exception filter, so the process is
terminated without one — losing the entire trace.

Two changes, so the failure reports itself:

- **256 MiB host stack** (`/STACK:268435456`, reserved not committed), so the
  guest-stack runaway detector wins the race against host stack exhaustion.
- **Runaway detection** in the page committer: growth below `kStackTop`
  within `kStackRegionSpan` is accumulated, and past 2 MiB it prints a
  `dbghelp`-symbolised backtrace of the recompiled functions on the stack,
  dumps the import trace, and exits. Repeated `sub_XXXXXXXX` names in that
  backtrace *are* the cycle.

The predicate is unit-tested against the regions from the real run:
`0x93010000`, `0x59000000`, `0x00000000`, `0xAD000000` are correctly not
counted as stack; `0x81FB0000`, `0x81F00000`, `0x81E20000` are. It fires at
32 commits; the real run reached 26 before dying.

## The recursion: KeBugCheck must not return (2026-07-26)

The symbolised backtrace named the cycle on its first outing:

```
  #8   __imp__sub_82B31A48 +0x176
  #9   __imp__sub_82B31B88 +0xBD
  #10  __imp__sub_82B31A48 +0x1CE
  #11  __imp__sub_82B31B88 +0xBD      ... repeating to the bottom
```

Two mutually recursive functions. The call counts then identified *what* they
are, with no guessing required:

| Import | Calls | Per iteration |
|---|---:|---:|
| `RtlInitAnsiString` | 57,204 | **6.00** |
| `KeBugCheck` | 19,068 | **2.00** |
| `KeGetCurrentProcessType` | 9,534 | 1.00 |
| `RtlEnterCriticalSection` | 9,539 | 1.00 |
| `RtlLeaveCriticalSection` | 9,539 | 1.00 |

Exact integer ratios — a deterministic loop, not corruption. Six string
initialisations and two bugchecks per pass is a **panic handler formatting a
message**.

### Cause

`KeBugCheck` is `DECLSPEC_NORETURN` on the Xbox 360: it halts the console.
Our stub logged the call and returned, so the panic handler returned into the
code that had just panicked, which panicked again — forever.

The same applies to `HalReturnToFirmware` (reboot/return to dashboard), which
the trace reached as import #2.

### Why it was panicking at all

`NtAllocateVirtualMemory` is import #1, called once, and returned nothing —
the stub left `r3` holding whatever the caller had put there. With no heap,
CRT startup fails, and the game bugchecks. Everything after that, including
the reads of guest `0x00000014` and `0xFFFFFFFD`, is downstream of a failed
first allocation.

### The import override mechanism

Generated stubs are now emitted as:

```cpp
#ifndef WOS_IMPL_KeBugCheck
PPC_FUNC(__imp__KeBugCheck) { WOS_IMPORT_STUB("KeBugCheck"); }
#endif
```

`WoSRecomp/kernel/kernel_overrides.h` lists what is implemented for real, so
implementing a function does not require regenerating the stub file, and
forgetting to list it fails at link time rather than silently keeping the
stub. Verified by partial-link test: every import is defined exactly once,
implementations displacing their stubs and stubs covering everything else.

## The allocator works; FP exception masks were being cleared (2026-07-26)

With `NtAllocateVirtualMemory` implemented, the bugcheck cascade vanished
entirely — `KeBugCheck` and `HalReturnToFirmware` are no longer called at
all, confirming both were downstream of the failed first allocation. The
trace reordered and reached new ground:

```
[mem] alloc guest 0x40000000, 0x100000  bytes  (type 0x60002000 = MEM_RESERVE|LARGE_PAGES|16MB_PAGES)
[mem] alloc guest 0x40000000, 0x10000   bytes  (type 0x60001000 = MEM_COMMIT, at the reserved base)
[mem] alloc guest 0x40100000, 0x2010000 bytes  (type 0x00003000 = MEM_COMMIT|MEM_RESERVE)  ~32 MiB
[mem] alloc guest 0x42110000, 0x40000   bytes
...
[import  12] MmQueryStatistics
[import  13] MmAllocatePhysicalMemoryEx
```

The reserve-then-commit pair is the guest doing exactly what the flags say,
and honouring a requested base handled it correctly by accident rather than
design — worth revisiting when reserve and commit need to differ.

### The new crash: EXCEPTION_FLT_INEXACT_RESULT (0xC000008F)

`PPCContext ctx{}` value-initialises, so `ctx.fpscr.csr` starts at **0**. The
first `enableFlushMode()` then does:

```cpp
csr |= FlushMask;   // 0 | 0x8040
setcsr(csr);        // MXCSR = 0x8040
```

MXCSR bits 7–12 are the FP exception **masks**, where 1 means *masked*. The
host default is `0x1F80` (all masked). Writing `0x8040` clears every one, so
the next inexact result traps instead of rounding.

| | MXCSR | Masks |
|---|---|---|
| Host default | `0x1F80` | all masked |
| `0 \| FlushMask` (the bug) | `0x8040` | **all clear — every FP op can trap** |
| `loadFromHost() \| FlushMask` | `0x9FC0` | all masked, FTZ+DAZ set |

PowerPC leaves FP traps disabled via `MSR[FE0,FE1]`, so the game never
expects them. Fixed by calling `ctx.fpscr.loadFromHost()` before entering the
guest, which seeds `csr` from the real MXCSR. Verified by direct test.

### Why the crash report was empty

The reporter printed `=== import trace ===` and then nothing. `DumpImportLog`
computes `100.0 * reached / total` for the percentage — floating point, with
the masks still cleared, so it trapped *again* inside the crash handler.

All three reporting paths now call `RestoreHostFpState()` first. A diagnostic
that can be killed by the condition it is diagnosing is not a diagnostic.

## Guest filesystem layout, and the 2 GB allocation (2026-07-26)

First successful file open:

```
[file] game root: C:/Users/benro/Downloads/wos
[file] opened "D:\game_shared.ini" -> C:/Users/benro/Downloads/wos\game_shared.ini
```

**The game addresses its disc as `D:\`**, not `game:\` or
`\Device\Harddisk0\...`. Stripping everything up to and including the first
`:` and treating the remainder as relative to the game root is correct for
this title.

Confirmed disc layout (top level, from a real dump):

| Entry | |
|---|---|
| `amalga.toc` | table of contents — almost certainly the archive index |
| `game_shared.ini` | first file the game opens |
| `packs/` | bulk game data |
| `sound/` | audio |
| `movies/` | Bink video (matches the `BINK`/`BINKDATA` sections in the XEX) |
| `$SystemUpdate/` | title update, not needed |

### The 2 GB allocation

The sequence was: open `game_shared.ini`, ask its size, allocate a buffer,
read, close. Step two was a stub that returned `STATUS_SUCCESS` and wrote
**nothing** to its out-parameter, so the game read uninitialised guest memory
as the size and got `0x82010000` — which is not a size at all, it is an
address (the image base is `0x82000000`). It then asked for 2.03 GB, the
allocator refused, and the config load failed.

**A stub that reports success without filling in its out-parameter is worse
than one that returns an error**, because the caller has no way to tell. Now
implemented: `FileStandardInformation`, `FilePositionInformation`,
`FileNetworkOpenInformation`, plus `NtReadFile` and `NtSetInformationFile`.

### RtlRaiseException ×6 was never an error

Exactly one raise per thread created read as six failures. It is not:
**`0x406D1388` is the `SetThreadName` convention**, where the exception is
merely a carrier for a name string a debugger is meant to intercept.
`ExceptionInformation[1]` is the name pointer, `[2]` the thread id. Decoded
now, so the trace prints thread names instead of six alarming lines.

## Engine structure: the thread pool (2026-07-26)

Decoding the `SetThreadName` exceptions gave the engine's own names for its
threads:

| Thread | Name |
|---|---|
| 0x1000 | `JQ worker 0 (CPU 2)` |
| 0x1001 | `JQ worker 1 (CPU 5)` |
| 0x1002 | `JQ worker 2 (CPU 3)` |
| 0x1003 | `JQ worker 3 (CPU 1)` |
| 0x1004 | `JQ worker 4 (CPU 4)` |
| — | `Game Master` |

A **job-queue worker pool**, five workers explicitly pinned across the Xbox
360's six hardware threads (CPU 1–5, leaving CPU 0 for the master/system),
plus a `Game Master` thread. Five workers share entry `0x82963840`; the
master is `0x829677D0`.

This is structural knowledge about the engine, not just a log curiosity: it
says the game is genuinely parallel, that work is dispatched through a queue,
and that CPU affinity is meaningful to it. `KeSetAffinityThread` is currently
a no-op, which is fine while nothing depends on *which* core runs what — but
worth remembering if timing-dependent misbehaviour turns up later.

## Out-parameter stubs: the recurring failure (2026-07-26)

Four separate times in one day, the same bug shape stopped progress:

| Function | What the stub did | What the caller then did |
|---|---|---|
| `KeBugCheck` | returned | panicked again, forever (~19,000 deep) |
| *all stubs* | left `r3` untouched | read a stale register as the return value |
| `NtQueryInformationFile` | returned success, wrote nothing | read `0x82010000` as a file size, asked for 2 GB |
| `MmQueryStatistics` | returned success, wrote nothing | read uninitialised memory as free-memory figures |
| `KeQuerySystemTime` | returned, wrote nothing | uninitialised timestamp |
| `KeQueryPerformanceFrequency` | returned 0 | zero *divisor* |

**A stub that reports success without filling in its out-parameter is worse
than one that returns an error**, because the caller has no way to detect it
and the damage surfaces far from the cause. The generated stubs now zero
`r3`, which makes them deterministic, but any import with an out-parameter
still has to be implemented properly before its caller can be trusted.

## Physical memory (2026-07-26)

`MmAllocatePhysicalMemoryEx` returns a **guest pointer**, not an NTSTATUS, so
the stub's zero meant "out of memory" on all four calls. The Xbox 360 aliases
physical RAM into `0xA0000000..0xBFFFFFFF` (uncached) and `0x80000000..`
(cached), which is almost certainly what the unexplained reads at
`0xAD000010` and `0xAE010000` were: memory the game had asked for and never
received.

Now allocated from a bump region at `0xA0000000`, kept well clear of the
`0x40000000` virtual heap so the two can never be confused in a log.

**Still unexplained:** the read at `0x59000000`, immediately before the
bugcheck. That is outside both the virtual heap and the physical alias
window, so it may be a genuinely stray pointer rather than the same cause.
Recorded as open rather than assumed solved.

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

## Reading the guest's own code (`--disasm` / `--xrefs`)

Added 2026-07-27, after guessing wrong twice in a row about what the idle
threads are doing.

The runtime can say *where* a thread is (`sub_82AC0C10 +0x212`) but not
*what it is doing there*, so every conclusion about the wait/signal layer so
far has been inference from behaviour. Two of those inferences were wrong:
the graphics interrupt callback was being called with two arguments instead
of three, and the "long wait" was attributed to the main thread when the
backtrace showed a spawned one.

`Image::ParseImage` already decrypts and decompresses the XEX, and XenonUtils
ships the same PowerPC disassembler XenonRecomp uses, so reading the actual
instructions is a thin wrapper rather than new machinery:

```
xex_info private/default.xex --disasm 0x82AC0C10        # to end of function
xex_info private/default.xex --disasm 0x82AC0C10 200    # fixed count
xex_info private/default.xex --xrefs  0x82AB9840        # who calls/references it
```

`--disasm` marks branch targets with `>` so loops are visible, annotates a
`bl` with its symbol (an import thunk therefore names the kernel call), and
flags backward branches. With no count it walks to a terminator, but only
once no branch seen so far still targets past it — functions have several
`blr`s and stopping at the first truncates. It says explicitly whether it
ended on a terminator, so a truncated dump can't be mistaken for a whole
function.

`--xrefs` scans every code section for direct branches to an address and
every section for a stored 4-byte pointer to it (vtables, callback tables).
Pointed at an import thunk it lists every call site of that kernel function
— which is the way to answer "what is supposed to signal this event" rather
than continuing to guess.

Neither mode prints data bytes or strings: addresses, mnemonics and operands
only, the same class of structural metadata the other modes emit.

## Open questions / blockers

- **The game is stable but idle.** Eight guest threads run, the vblank
  interrupt fires at 60 Hz, ~24 MB of GPU buffers are allocated, but `VdSwap`
  has never been called and no frame has ever been presented. Every thread
  waits; nothing signals. The two graphics threads wait on events at
  `ctx+0x20` (logged as `ev5`/`ev6`); both show 0 signals.
- **The main thread spins at `sub_82AC0C10 +0x212`.** Fixing the interrupt
  callback's argument count advanced it from `+0x1B7` (calling into
  `sub_82B13200`) to `+0x212`, 91 bytes further into the same function —
  progress, not a fix. What it polls there is the next thing to read with
  `--disasm`, not to infer.
