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

## The graphics layer, start to finish

Resolved 2026-07-28. Five separate bugs, each found by measurement rather than
inference. Recorded in order because the sequence matters: every one of them
was hidden behind the one before it.

1. **`r13` was never set.** It is the per-thread block pointer on Xbox 360,
   not a scratch register. Null meant every access through it read the zero
   page, so the GPU watchdog's clock was permanently 0 and its 5000-tick
   deadline could never be reached. The game spun forever without ever
   reaching its own timeout handler. Found from a three-instruction
   disassembly of guest 0x82B13200 plus a harness log line showing a read of
   guest 0x100.

2. **Video driver addresses are physical, not virtual.** `MmGetPhysicalAddress`
   is `addr & 0x1FFFFFFF`, and the game calls it before `VdInitializeRingBuffer`
   and `VdEnableRingBufferRPtrWriteBack`. We wrote the read pointer to guest
   0x0006023C instead of its alias at 0xA006023C — an unrelated page, which the
   harness then committed on first touch, making the mistake look deliberate.

3. **Nothing executed the command packets.** The game's fence rides on PM4
   opcode 0x58, which writes a value to an address. Two per frame: a progress
   pointer to Snooped+4 and a counter to Snooped. Unexecuted, the GPU fence sat
   at 1 while the CPU fence climbed.

4. **The ring size argument is a log2 dword count, not bytes.** Reading it as
   bytes put the capacity at a quarter of its real value; once the write
   pointer passed it, the consumer hit a bounds guard and returned *silently*
   while still publishing the read pointer as caught up. The fence froze at
   exactly 0x54f in two consecutive runs — that reproducibility was the tell,
   since a timing gap does not land on the same value twice.

5. **`NtWaitForMultipleObjectsEx` was an unimplemented stub.** Returning
   success to a *wait* inverts the call's timing semantics and turns a block
   into a busy-spin: 11.7 million calls per five seconds, lockstep with
   `NtSetEvent` and `NtReleaseMutant`.

Result: `ERR[D3D]: The GPU is hung!` no longer appears, the ring flows
continuously with the read pointer tracking a few dwords behind the write
pointer, and the import heartbeat fell from 58,317,635 calls per five seconds
to 9,763 — a factor of about 6,000.

**The recurring pattern, stated once more because it has now cost five runs:**
a stub that returns success to a wait is worse than one that returns an error,
and a bounds check that returns silently is worse than one that crashes. Both
produce a system that looks healthy in every log line while doing nothing.

## The graphics interrupt callback (guest 0x82AB9840)

Read 2026-07-28. Two findings, one of them a correction to earlier work in
this same log.

**It takes two arguments, not three.** The prologue is unambiguous:

    82AB984C  mr r31,r4         ; the context
    82AB9850  cmplwi cr6,r3,1   ; the source

So the signature is `(source, context)` with the context in r4. An earlier
change in this project altered it from two arguments to three on the theory
that the middle slot was a CPU number, which put **zero** in r4 — so every
access through r31 inside the callback read the zero page and the vblank
handler ran with a null device. That theory was never checked against the
code, and it silently disabled the callback for several runs.

**The vblank path is gated on a GPU register:**

    82AB98D8  lis  r11,32712       ; 0x7FC80000
    82AB98DC  lwz  r11,25924(r11)  ; register at 0x7FC86544
    82AB98E0  clrlwi. r11,r11,31   ; bit 0
    82AB98E4  beq  -> return       ; clear: not ours, do nothing
    82AB98EC  bl   0x82AC46C8      ; otherwise the real handler

Nothing wrote that register, so it read zero and the callback returned
immediately on every one of the 600+ vblanks per run. The register index is
arbitrary — there was no route to it except reading the function.

Source 1 is a different path entirely: it walks a table at [ctx+0x2A94],
traps if it finds the sentinel 0x0BADF00D, optionally calls through a
function pointer at [that+0x14], then clears a per-CPU bit under a spinlock
using `1 << [r13+0x10C]`. That is another consumer of the r13 block.

## The frame-present path (guest 0x82AC4E48)

Reading onward from the vblank handler found where frames are actually
submitted — and an unimplemented import sitting in the middle of it:

    82AC5030  bl __imp__VdGetSystemCommandBuffer   ; r3 = &sp[208], r4 = &sp[116]
    82AC5034  lwz r11,21532(r31)                   ; if [ctx+0x541C] != 0 ...
    82AC5040  lwz r11,116(r1)                      ;   ... use the r4 out-value
    82AC5048  stw r11,8(r10)                       ;   store into [ctx+0x2A90]+8
    ...
    82AC5094  bl __imp__VdSwap

`VdGetSystemCommandBuffer` is not implemented, so it is a generated stub that
writes nothing to either out-parameter. The caller then reads sp[116],
sp[208], sp[212] and sp[216] — all uninitialised stack. This is the same
shape as the NtQueryInformationFile bug recorded earlier in this log: an
out-parameter stub that reports success and fills in nothing.

`VdSwap` is reached from here, which is the call that has never fired in any
run so far.

## KeResumeThread — fixed, but not the signaller

2026-07-28. `KeResumeThread` was unimplemented, so guest thread 4106 (entry
0x829F4C80) was created with CREATE_SUSPENDED, resumed through a stub that did
nothing, and never ran in any run before this. Implementing it works: the run
now shows `[thread] 4106 starting at guest 0x829F4C80`, the thread adopts two
further guest-embedded events (0x82F7700C, 0x82F76FFC), and returns.

**It does not signal 0x4083FDCC.** The hypothesis that it might came from four
KeSetEvent call sites sitting near its entry point; that was proximity, not
evidence, and it was wrong. The graphics threads remain parked at
sub_82ACECF0 +0x4CA. Recorded because the negative result narrows the search:
whatever signals that event, it is not this thread.

What the KeSetEvent xrefs actually give (13 sites total):

    829F40C0  829F482C  829F4D30  829F4E48   near thread 4106's entry
    82ACEED8  82ACF404                       inside sub_82ACECF0 itself
    82B1486C  82B1492C  82B14A78  82B14AE4  82B14BB8
    82B1C0FC  82B1D0E0

The two inside sub_82ACECF0 are the interesting pair: +0x1E8 (before the wait
at +0x4CA, so both graphics threads have already passed it) and +0x714 (after
it, so neither has reached it). That is the shape of two threads meant to
hand off to each other, with both stuck on the receiving side — which points
at a third party that should signal first, or at an event our adoption is
tracking as a different object than the game thinks it is.

## sub_82ACECF0 — the graphics thread body, read at last

2026-07-28. This is the function both graphics threads block in, and the one
piece of the chain that had never been dumped. The disassembly answers the
structural questions; it does not, on its own, say which thread is where, and
that gap is what the next run is instrumented to close.

What the code does, read directly:

    82ACECFC  mr    r26,r3              ; r26 = context pointer, the thread argument
    82ACED10  > lis r24,-32256          ; top of the main loop
    82ACED14  lis   r11,-5
              ori   r11,r11,27680       ; r11 = 0xFFFFFFFFFFFB6C20
    82ACED18  lwz   r25,0(r26)          ; r25 = device, from [ctx+0x00]
    82ACED1C  lwz   r10,4(r26)          ; r10 = [ctx+0x04]
    82ACED20  addi  r27,r1,80           ; r27 = &timeout on the local stack
    82ACED24  std   r11,80(r1)          ; store the timeout
    82ACED28  lwz   r11,376(r25)        ; r11 = [device+0x178]
    82ACED2C  cmplw cr6,r10,r11
    82ACED30  beq   cr6,+8
    82ACED34  li    r27,0               ; ...otherwise r27 = NULL, i.e. INFINITE
    82ACED38  > lwz r11,60(r26)         ; [ctx+0x3C]
    82ACED3C  lwz   r10,56(r26)         ; [ctx+0x38]
    82ACED44  bne   cr6,0x82acee3c      ; queue non-empty -> skip the wait, do work
    82ACED6C  mr    r7,r27              ; r7 = timeout pointer (or NULL)
    82ACED7C  mr    r3,r28              ; r28 = ctx+0x20 — the KEVENT
    82ACED80  bl    __imp__KeWaitForSingleObject
    ...
    82ACEE34  > cmplwi cr6,r3,258       ; 258 = 0x102 = STATUS_TIMEOUT
    82ACEE38  beq   cr6,0x82aceda4      ; -> reaches 82ACEDD8: bl 0x82AC4E48

Four things follow.

**The event is at context+0x20.** r28 is ctx+0x20 and that is what r3 holds at
the wait. With the two known context pointers 0x4083FD5C and 0x4083FDAC, the
events are 0x4083FD7C and 0x4083FDCC — exactly the pair the wait diagnostics
have been naming since they were added. That is independent confirmation that
the two blocked graphics threads are both in *this* loop, rather than merely
somewhere in this function.

**The timeout is ~30 ms, and only sometimes.** `lis r11,-5; ori r11,r11,27680`
sign-extends to 0xFFFFFFFFFFFB6C20 = -302048 in 100 ns units = 30.2 ms
relative. But r27 is replaced with NULL when `[ctx+0x04] != [device+0x178]`, so
one of the two threads waits with a 30 ms timeout and the other waits INFINITE
depending on a device field. This matches the observed stacks — one thread in
`wait_for`, the other in `Cnd_wait`.

**Both threads are inside the wait**, which means `[ctx+0x3C] == [ctx+0x38]`
for both: the work queue this loop drains is empty. Neither thread is blocked
because it is busy; both are blocked because there is nothing to do.

**The frame-present branch is taken on STATUS_TIMEOUT.** 82ACEE34 compares the
wait result against 0x102 and branches into the region that calls 0x82AC4E48
only when it matches. So presentation does not require the event to be
signalled at all — it requires the wait to *time out*, which the 30 ms waiter
should be doing about thirty times a second.

That last point is the problem, because `KeWaitForSingleObject` already returns
`kStatusTimeout` on timeout, and the per-object counters already show timeouts
happening in bulk (`ev6(manual) 950/949/0` — 949 timeouts, zero signals). If
the reading above were the whole story, the present path would be firing. It is
not: `VdSwap` has still never been called.

So one of these is true, and the disassembly cannot distinguish them:

1. The timing-out waiter is not the one at 82ACED80 — some other wait site in
   this large function accounts for those timeouts.
2. The branch at 82ACEE38 is reached from a path that is itself gated on
   something else, so the comparison never runs.
3. 0x82AC4E48 *is* being called and bails out before VdSwap.

Guessing between them is exactly the move that has cost this project its worst
turns. Two instruments were added instead.

## Two instruments: wait call-site census, and guest-function tripwires

2026-07-28.

**Wait call-site census** (`kernel/sync.cpp`). XenonRecomp emits
`ctx.lr = 0x<return address>` immediately before every `bl`, so inside an
import implementation `ctx.lr` is the guest call site plus four. Every wait
entry point now records (call site, object, timeout, timed-out?) into a bounded
table, printed by the heartbeat as:

    [waitsites] N site(s):
        bl@0x82ACED80 -> obj 0x4083FD7C  30ms      1421 call(s), 1421 timeout(s)

This answers, per run and without inference, which guest instruction each wait
comes from, on which object, with which timeout, and whether it is timing out.
It settles possibility 1 above directly, and it generalises: every future
"who is waiting on this" question is now a lookup rather than a backtrace.

**Guest-function tripwires** (`kernel/trace_guest.cpp`, new). The recompiler
emits every function twice — the body as `PPC_FUNC_IMPL(__imp__sub_XXXXXXXX)`
and a weak alias `sub_XXXXXXXX` — and both direct calls and the indirect
dispatch table go through the weak name. A strong definition of that name in
our own code therefore intercepts the call, and can forward to the untouched
original. This is the only way to observe that control reached an address whose
code calls no imports.

Two are placed: `sub_82AC4E48` (the frame-present path, three known callers)
and `sub_82ACECF0` (the graphics thread body, which also prints its context
pointer on entry). Together they settle possibility 3: if the present tripwire
fires and VdSwap still does not, the fault is inside 0x82AC4E48; if it never
fires, the branch is never taken and the fault is upstream.

If the file ever fails to link with an unresolved `__imp__sub_XXXXXXXX`, that
address is not a function boundary the analyser found — delete that tripwire
rather than forcing it. Nothing depends on the file.

## --disasm now stops at the .pdata boundary

2026-07-28. `--disasm 0x82ACECF0` ran to the full 16384-instruction cap again,
after the `isCallInsn` fix. That fix was correct but incomplete: the walk stops
at a terminator only once no branch seen so far still targets an address past
it, and a single forward branch to a distant handler — or a tail call emitted
as a plain `b` — keeps that horizon permanently ahead of the cursor.

`.pdata` is the authority here; it is the same table XenonRecomp derives its
function list from. The walk now also stops at the end of the `.pdata` record
covering the start address, and the footer says which of the two rules ended
it, so a dump truncated at an unwind-record split is visible as such rather
than passing for a complete function.

## The census pays off, and corrects a method I had been trusting

2026-07-28, run 20260728-032051. First run with the wait call-site census and
the guest tripwires.

**Two distinct wait sites in sub_82ACECF0, not one.** Thread 4102 (context
0x4083FD5C) passes through 0x82ACED80 exactly once and then settles into a
loop on `bl@0x82ACEE14` — 30 ms timeout, 794 calls, 794 timeouts, about 33 Hz,
which is the 30 ms cycle exactly. Thread 4103 (context 0x4083FDAC) is parked at
0x82ACED80 with a NULL timeout on 0x4083FDCC and has never returned; its single
recorded "timeout" is our own five-second warning probe before it re-enters
`Wait(-1)`. So the loop is alive on one thread and permanently blocked on the
other, and the earlier reading of a single wait at 0x82ACED80 was wrong.

**Host stack offsets are not reliable attribution — correcting an earlier
method, not just an earlier fact.** Both graphics threads report
`__imp__sub_82ACECF0 +0x4CA`, and the census proves they are at different guest
instructions. clang tail-merged two identical `KeWaitForSingleObject` call
sequences into one host call site, so dbghelp cannot tell them apart. Every
`sub_XXXXXXXX +0xNNN` offset quoted earlier in this log is suspect as a
*position* claim; the function identity is still sound. `ctx.lr` is the only
trustworthy call-site attribution we have.

**sub_82AC4E48 is never entered.** The tripwire is live — the link resolved
`__imp__sub_82AC4E48`, and the `sub_82ACECF0` tripwire printed both thread
entries — and `[trace] present sub_82AC4E48` appears zero times in the run.
794 timeouts at the site whose result feeds `82ACEE34 cmplwi cr6,r3,258` do
not produce a single call. So either there is a guard between the branch
target 0x82ACEDA4 and the call at 0x82ACEDD8, or the branch is not the one I
read it to be. That region has never been disassembled.

**The main thread is not blocked. It is spinning in guest code.**

    #0   __imp__sub_82AC0C10 +0x79
    #1   __imp__sub_82ABA260 +0x2D5
    #2   __imp__sub_82ABAD58 +0x127
    #3   __imp__sub_829254C8 +0xC1
    #4   __imp__sub_826B9EC0 +0x5B
    ...  __imp___xstart, main

This is the first time the main thread has been seen doing anything specific.
It calls no imports at all, which is precisely why it has been invisible for
the whole project — the import trace and the heartbeat are both blind to it,
and only the watchdog's all-thread dump can see it. The addresses are in the
same 0x82AB/0x82AC band as the rest of the D3D layer.

**The GPU fence stopped at 0x0D while the ring kept flowing.** *(WRONG — see
the correction in the device-probe entry below. The fence never stopped; the
`op=0x58` log line is capped at twelve occurrences in video.cpp, so the log
went quiet while the writes continued. The reasoning below is left in place
because the mistake is instructive: a diagnostic that stops printing is not
evidence that the thing it prints about stopped happening.)* The last
`op=0x58` write puts 0x0000000D at 0xA0060200; no fence packet appears after
that for the remaining ~25 seconds. Meanwhile `CP_RB_WPTR` climbs steadily by
about 0x300 dwords per poll and the read pointer tracks it, trailing by exactly
six dwords every time.

The constant six-dword lag is worth its own look later: it is too regular to be
noise, and it suggests the consumer stops just short of a trailing packet
rather than at an arbitrary point.

**Incidental.** `KeDelayExecutionThread` was called 28,068,783 times in the
first five seconds and 3,969,995 in the next, then vanished from the report
entirely — a transient during archive loading, not a steady-state spin.
Handle numbering shifted again between runs (0x00010050/0x00010024 last run,
0x00010058/0x0001002C this run), confirming handles are as unstable as the
ev-numbers and must never be used as identifiers across runs.

## The frame loop stops on three bytes in the D3D device

2026-07-28. Both halves of the stall reduce to fields in one guest structure,
and the structure is reachable from a fixed global, so all of it is directly
observable.

**The present is gated on a flag nothing sets.** The region between the
STATUS_TIMEOUT branch target and the present call had never been read. It is:

    82ACEDA4  > lwz     r11,4516(r24)      ; r24 = 0x82000000 -> [0x820011A4]
    82ACEDA8    lwz     r30,0(r11)         ; r30 = the D3D device
    82ACEDAC    addi    r29,r30,14944      ; device+0x3A60, a critical section
    82ACEDB4    bl      __imp__RtlEnterCriticalSection
    82ACEDB8    lbz     r11,10942(r30)     ; [device+0x2ABE]
    82ACEDBC    rlwinm. r11,r11,0,30,30    ; test bit 0x02
    82ACEDC0    beq     0x82aceddc         ; CLEAR -> skip both present calls
    82ACEDC4    mr      r3,r30
    82ACEDC8    bl      0x82ac4e40
    82ACEDD0    addi    r4,r30,14844
    82ACEDD8    bl      0x82ac4e48         ; the present
    82ACEDDC  > mr      r3,r29
    82ACEDE0    bl      __imp__RtlLeaveCriticalSection

The graphics thread reaches 82ACEDA4 about 33 times a second — 794 timeouts in
the run — and takes the `beq` every single time. So `[device+0x2ABE] & 0x02` is
the "a frame is ready to present" flag, and nothing in the run sets it. This is
why the tripwire on sub_82AC4E48 never fired despite the branch being taken
constantly: the branch is taken, and then the call is skipped.

Note `rlwinm. rA,rS,0,30,30` is mask 0x02 in big-endian bit numbering, not
0x40000000. The neighbouring byte uses `clrlwi r11,r11,31` (mask 0x01) for a
different flag, which confirms the encoding rather than leaving it to memory.

**The main thread is a GPU wait predicate.** sub_82AC0C10 returns 1 for "keep
waiting" and 0 for "done":

    82AC0C24    lwz     r29,0(r31)         ; r31 = wait state, [r31+0] = device
    82AC0C28  > db16cyc x8, repeated x4    ; a deliberate stall, not a nop
    82AC0C50    lbz     r11,10941(r29)     ; [device+0x2ABD]
    82AC0C54    rlwinm. r11,r11,0,30,30    ; test bit 0x02
    82AC0C58    bne     0x82ac0cd4         ; SET -> return 0, the clean exit
    82AC0C5C    lwz     r11,10896(r29)     ; [device+0x2A90] -> the polled counter
    82AC0C60    lwz     r10,256(r13)       ; KPCR -> current thread
    82AC0C68    lwz     r8,0(r11)          ; the counter's value
    82AC0C6C    lwz     r30,88(r10)        ; thread+0x58 — the tick r13 provides
    82AC0C70    cmplw   cr6,r9,r8          ; changed since [r31+8]?
    82AC0C7C    stw     r30,12(r31)        ;   yes -> reset the deadline
    82AC0CAC    cmplwi  cr6,r11,5000       ; 5000 ticks with no progress
    82AC0CC0    bl      0x82acb050         ;   -> the "GPU is hung" handler
    82AC0CB4  > li      r3,1               ; otherwise keep waiting

This is the same 5000-tick watchdog that produced `ERR[D3D]: The GPU is hung!`
before r13 was implemented. Two things it settles: `[device+0x2A90]` holds a
*pointer* to the counter being polled, not the counter itself; and the whole
loop calls no imports, which is exactly why the main thread has been invisible
to the import trace and the heartbeat for the entire project. Only the
all-thread watchdog dump can see it.

**The device is at *(*(0x820011A4)).** `lis r24,-32256` is 0x82000000 and
`lwz r11,4516(r24)` is [0x820011A4]; the device is what that points to. Both
sub_82ACECF0 and sub_82AC1690 resolve it the same way, so this is the game's
single global device slot rather than a local convention.

## A 200 us watch on the device flags

2026-07-28. Added `kernel/d3d_probe.cpp`: a thread that resolves the device
from the global slot and samples `[device+0x2ABE]`, `[device+0x2ABD]`, and the
counter behind `[device+0x2A90]`, reporting from the heartbeat.

Two design points worth keeping. It ORs each observed byte into a sticky mask,
because a flag set and cleared inside one frame is invisible to a five-second
sample — "never set once in thirty seconds" is only a claim worth making if the
sampling can actually catch a transient. And it samples at 200 us rather than
on the heartbeat for the same reason. The fence value is tracked as
min/max/current so a counter that moves and then stops is distinguishable from
one that never moved.

What the next run decides: if bit 1 of +0x2ABE is never seen, the frame is
never marked ready and the question becomes what should set it. If it is seen,
the graphics thread is missing a window and the fix is on our side.

## The device probe answers it, and corrects me on the fence

2026-07-28, run 20260728-122056.

**The device is 0x4083D080 — the same pointer VdSetGraphicsInterruptCallback
was given as user data.** The graphics interrupt context and the D3D device
are one object. That was never stated before and it ties the interrupt path
and the frame loop to the same structure.

**Both gates are dead, and they are dead differently.**

    +2ABE (present gate)  ever=0x14   bit1 never
    +2ABD (wait exit)     ever=0x00   bit1 never

`0x14` is bits 2 and 4, so that byte is live — the game writes it, just never
the bit the present call needs. `+2ABD` is never written at all in a
thirty-second run, so the main thread's clean exit from sub_82AC0C10 has never
been available to it.

**CORRECTION: the GPU fence never stopped.** The previous entry concluded it
froze at 0x0D because the `[gpu] op=0x58 write` lines stopped appearing. They
stopped because that printf is capped at twelve occurrences in video.cpp. The
probe reads the counter directly and it is climbing steadily:

    value 0x0000027D -> 0x000004FB -> 0x00000777 -> 0x000009F3

in increments of 2, about 64 per second — the same +2 pattern as the twelve
logged writes, continuing uninterrupted. The ring, the command processor and
the fence are all healthy.

This was self-inflicted, and the lesson is worth more than the fact: a
diagnostic that stops printing is not evidence that the thing it reports
stopped happening. The cap now announces itself when it engages.

**Which means the main thread will spin forever by design.** sub_82AC0C10
resets its deadline whenever the polled counter changes, and the counter
changes 64 times a second, so the 5000-tick timeout is unreachable. It is not
hung and it is not going to report itself as hung — it is correctly waiting
for `[device+0x2ABD] & 0x02`, which nothing sets. The stack offsets differ
between runs (`sub_82AC0C10 +0x79` then `+0x285`), confirming it is live and
moving rather than parked.

**Supporting reads.** `[device+0x2A88] = 0x40001000`, which is the main
thread's thread block — the value guest 0x82B13200 returns, stored by
sub_82AC0F68 (`bl 0x82b13200; stw r3,10888(r31)`). `[device+0x2AFC] = 0`, so
the second deadline-reset path at 82AC0C94 never runs either.

## --field: finding who touches a struct field

2026-07-28. `--xrefs` answers "who reaches this address" and structurally
cannot answer "who touches this field", because a field access encodes no
address at all — only a base register and a 16-bit displacement. That is
exactly the question left: nothing in the ~1600 instructions dumped by hand
writes bit 1 of either gate byte.

`xex_info --field <disp> [--stores]` scans every code section for D-form
loads and stores whose displacement matches, and names the containing function
from .pdata. The match is exact rather than heuristic — the displacement is
the low 16 bits and the primary opcode the top 6 — but it cannot know the base
register's *type*, so hits against unrelated structures with the same offset
are expected and the output says so. For this game the D3D code clusters in
0x82AB..0x82AD, which makes the real hits easy to pick out.

Known limitation: DS-form `ld`/`std` (primary 58/62) use the low two bits as
an opcode extension rather than displacement, and the floating-point forms are
not covered. Neither matters for a byte-sized flag; both would need handling
before trusting this for 64-bit fields.

## sub_82AB99E0 sets bit 5, not bit 1 — and why I keep guessing

2026-07-28. `--field 0x2ABD --stores` flagged a store at 82AB9A00 with no
containing .pdata function, sitting in the same address band as the graphics
interrupt callback. I called it a lead. It is not one:

    82AB99E0  lwz  r10,16728(r3)     ; [ctx+0x4158]
    82AB99E4  lbz  r9,10941(r3)      ; [ctx+0x2ABD]
    82AB99E8  addi r11,r10,4800      ; r10 + 0x12C0
    82AB99EC  ori  r9,r9,32          ; bit 5 (0x20), NOT bit 1
    82AB99F0  addi r8,r11,-160
    82AB99F4  stw  r11,52(r3)        ; [ctx+0x34]
    82AB99F8  stw  r10,48(r3)        ; [ctx+0x30]
    82AB99FC  stw  r8,56(r3)         ; [ctx+0x38]
    82AB9A00  stb  r9,10941(r3)
    82AB9A04  blr

A five-instruction leaf that repoints the command-buffer cursor, base and
limit at [ctx+0x4158]+0x12C0 and flags that it has done so. [ctx+0x30] is the
same field every packet emitter walks with `stwu r11,4(r3)`, and 0x20 is the
bit tested at 82AC1550 and 82AC20D0. Nothing to do with the wait.

The proximity argument was worthless, and it is the third time this session
that "it is near the right address" has produced a wrong answer. The reason it
keeps happening is a real gap in the tooling rather than a lapse of care: the
`--field` output gives the *location* of a store and nothing about the *value*
it writes, and for a flags byte the value is the entire question. `ori r9,r9,32`
and `ori r9,r9,2` are indistinguishable in that listing.

So `--field` now takes `--context N` and prints the N instructions that
produced the stored register. Twenty candidate sites become one dump that
answers which of them touches bit 1, with no inference in between.

## The wait-exit bit is an abort flag, and the present bit has no setter

2026-07-28. `--field --context` over both gate bytes, 35 stores with the
instructions that computed each value. Two results, read directly off the
listing.

**[device+0x2ABD] bit 1 has exactly two setters.**

    82ABABAC  bl   0x82aba260
    82ABABB0  lbz  r11,10941(r31)
    82ABABB4  ori  r11,r11,2         ; sub_82ABAAD8+0xE0
    82ABABB8  stb  r11,10941(r31)

    82ACB0BC  ori  r10,r10,3         ; sub_82ACB050+0x74 — bits 0 and 1
    82ACB0C4  stb  r10,10941(r31)

sub_82ACB050 is what the wait predicate calls on timeout (82AC0CC0
`bl 0x82acb050`). So bit 1 is an **abort** flag: set when the GPU is declared
hung, to break the wait out. It is the failure path, and it can never fire in
our runs because the fence advances 64 times a second and the 5000-tick
deadline is unreachable.

**This corrects my reading of the wait.** sub_82AC0C10 returning 0 on that bit
is not "the frame is done", it is "give up". The normal exit is not in the
predicate at all, which means the loop condition lives in the caller —
sub_82ABA260, frame #1 on the main thread's stack, never dumped.

The other setter is downstream, not upstream: sub_82ABAAD8 sets the bit
*after* calling sub_82ABA260. (sub_82ABAAD8 is the command-buffer flush —
every `bl 0x82abaad8` in the dumped code is guarded by
`[r31+0x30] > [r31+0x38]`, cursor past limit.) Caveat: r10's load at 82ACB0BC
falls outside the six-instruction window, so what is recorded here is the `ori`
itself, not the provenance of the value.

**[device+0x2ABE] bit 1 has no setter at all.** Fifteen stores at that
displacement; the values are 0x04 (82AB9B78, 82ACD8DC), 0x10 (82AC51DC,
82AD1348), 0x40 (82ACCC78), and `rlwimi` inserts into bits 0 (82AC287C), 3
(82ACC9E0), 5 (82AB9080) and 7 (82ACC658). Clears cover bits 3, 4, 5, 6 and 7.
Not one `ori ...,2` anywhere in the image.

Two possibilities remain, and they are distinguishable rather than a matter of
opinion: a wider store overlapping the byte — a `stw` or `sth` at displacement
10940 covers 0x2ABC..0x2ABF — or the field being reached through a form the
scan does not cover (`stbx`, or a base register plus computed offset).

**Dead lead, recorded.** sub_82AB99E0 was flagged because it stores to
0x2ABD with no .pdata function and sits near the graphics interrupt callback.
It is `ori r9,r9,32` — bit 5 — and the function is a five-instruction leaf
that repoints the command-buffer cursor, base and limit at [ctx+0x4158]+0x12C0.
Proximity was not evidence. The listing now shows the value, which is what
made this checkable in one dump rather than by argument.

## sub_82ABA260 is a fence wait, and its guarantee is one we can break

2026-07-28. The main thread's frame #1, finally read. 51 instructions, ends on
a terminator.

    82ABA26C  mr   r30,r4            ; r30 = target fence value
    82ABA270  mr   r31,r3            ; r31 = device
    82ABA278  cmplwi cr6,r30,0
    82ABA27C  beq  return            ; target 0 -> nothing to wait for
    82ABA280  lwz  r11,10896(r31)    ; [device+0x2A90] -> the GPU fence
    82ABA284  lwz  r10,10908(r31)    ; [device+0x2A9C] =  the CPU fence
    82ABA288  subf r9,r30,r10        ; r9  = cpu - target
    82ABA28C  lwz  r11,0(r11)        ; r11 = *gpu_fence
    82ABA290  subf r11,r11,r10       ; r11 = cpu - gpu
    82ABA294  cmplw cr6,r9,r11       ; UNSIGNED
    82ABA298  bge  cr6,return        ; done when (cpu-target) >= (cpu-gpu)
    ...
    82ABA2F4  bl   0x82ac0c10        ; the stall/timeout predicate
    82ABA2F8  cmpwi r3,0
    82ABA2FC  beq  0x82aba31c        ; predicate says give up -> leave
    82ABA300  > (the same comparison again)
    82ABA318  blt  cr6,0x82aba2f0    ; not reached yet -> loop

This is the textbook wraparound-safe fence wait: subtract both the target and
the completed value from the CPU counter and compare unsigned, so the test
stays correct across a 2^32 wrap. sub_82AC0C10 is not the loop condition at
all — it is only the stall-and-timeout check between iterations, which is why
its `[device+0x2ABD]` exit is an abort path.

There is also a deadlock guard at 82ABA2A4 that had not been seen: if the
target equals the *current* CPU fence — work that has not been submitted yet —
it calls sub_82ABAAD8 to flush the command buffer first, so the GPU can
actually reach the value. That explains why sub_82ABAAD8 sets +0x2ABD bit 1
after calling this: it is the submit path.

**The wraparound trick holds on exactly one assumption: the GPU fence never
runs ahead of the CPU fence.** If it does, `cpu - gpu` underflows to a huge
unsigned value, `(cpu - target)` stays small, the `bge` is never taken, and the
wait becomes unsatisfiable regardless of what the GPU does afterwards.

That is a failure *our side can cause*, which is what makes it worth measuring
rather than assuming. The guest advances the CPU counter only when it submits
work; our command processor advances the GPU counter by executing whatever
memory-write packets it finds in the ring. Any over-execution — re-running a
region, mis-parsing a packet boundary, following an indirect buffer twice —
shows up as a crossing. The observed fence climbing at a steady 64 Hz while the
game is blocked and submitting nothing is consistent with that, and also
consistent with the game still submitting; the two are distinguishable only by
reading the CPU counter.

The probe now reads `[device+0x2A9C]` alongside the GPU fence, reports the
signed difference, and latches the FIRST crossing rather than the latest —
once they diverge every later sample looks crossed, and the pair of values at
the moment it happened is what says how far we over-ran.

**Which also resolves the +0x2ABE puzzle.** No store in the image sets bit 1
of the present gate, and sub_82AC4E48 has three callers, only one of which is
the graphics thread. The graphics-thread path is a secondary, deferred present
gated on a flag the primary path sets; it is not the route to a frame. The
main thread is.

## No crossing — and the main thread was never stuck

2026-07-28, run 20260728-125032. The fence-crossing hypothesis is wrong.
`GPU-CPU` sits at a constant -4 and never inverts:

    GPU=0x0000027F  CPU=0x00000281  GPU-CPU=-2
    GPU=0x000004F9  CPU=0x000004FD  GPU-CPU=-4
    GPU=0x00000775  CPU=0x00000779  GPU-CPU=-4
    GPU=0x00000C6F  CPU=0x00000C73  GPU-CPU=-4

The wraparound comparison in sub_82ABA260 is sound. One run to establish it,
which is the right price.

**The larger correction: the main thread is not deadlocked, and never was.**
`[device+0x2A9C]` is the *CPU* fence — the guest increments it when it submits
work — and it climbs at about 128 per second alongside the GPU fence. Guest
code is therefore running continuously and the fence wait is completing over
and over. The main thread appears in every watchdog dump inside sub_82ABA260
because that is where it spends most of its time, not because it is stuck
there. The render loop turns over.

**The `rptr` trailing `wptr` by six was also nothing.** CommandProcessorThread
publishes `rptr = wptr` unconditionally from the same value it consumed, so the
two cannot disagree. The printed gap is the guest advancing the register
between the log line's two reads. Recorded because it was treated as a signal
across three entries and it never was one.

So a stall was being chased that does not exist. What is genuinely stuck is
asset loading, and the evidence has been in every run since the file layer
started working:

    [file] read 524288 of 0x80000 bytes from "D:\packs\game.XEPACK"
    [file] open FAILED "B<garbage>" (not found on the host)

One read of the archive, then nothing. A filename assembled from uninitialised
memory. Two threads — sub_829677D0 (the "Game Master") and sub_82A7CD00,
spawned immediately after the sound pack opens — blocked in
NtWaitForMultipleObjectsEx for the whole run.

## RtlMultiByteToUnicodeN / RtlUnicodeToMultiByteN were never implemented

2026-07-28. Both were generated stubs. The garbage filename appears in the log
directly after they are first reached:

    [import 63] RtlMultiByteToUnicodeN
    [import 64] RtlUnicodeToMultiByteN
    [file] open FAILED "B<garbage>" (not found on the host)

Same failure as NtQueryInformationFile and VdGetSystemCommandBuffer before it:
an out-parameter stub returns success-shaped nothing, and the caller reads
whatever was already in the destination buffer. A loader thread that cannot
open its file never signals the event the other threads wait on, which is a
complete account of both permanent blocks.

Two details that are silent when wrong, so both are stated in the code:

- Every length in these APIs is in **bytes**, never characters. A UTF-16
  destination of N bytes holds N/2 code units, and the returned count for the
  multibyte->unicode direction is `chars * 2`.
- The guest is **big-endian**, so each UTF-16 unit is byte-swapped through
  StoreU16/LoadU16. Writing them host-native would produce strings that look
  plausible in a hex dump and match nothing on disk.

The mapping is Latin-1 <-> UTF-16, which is exact for ASCII — what asset paths
are — and substitutes '?' for anything above 0xFF rather than pretending to
implement a codepage we have no table for.

## The string conversions were the archive blocker

2026-07-28, run 20260728-125548. Implementing RtlMultiByteToUnicodeN and
RtlUnicodeToMultiByteN removed the garbage filename and produced the first
bulk asset load of the project:

    [file] opened "D:\sound\SOUNDSRC_RVB.PCK" -> .../sound/SOUNDSRC_RVB.PCK
    [file] read 8 of 0x8 bytes ...
    [file] read 774194 of 0xBD032 bytes ...
    [file] opened "D:\sound\SOUNDSRC_RVB.PCK" -> ...

The uppercase name is the game round-tripping the path through both
conversions, which is what they are there for. Where the previous run showed
`open FAILED "B<garbage>"`, this one opens the file and reads 774 KB.

Still open after the fix: game.XEPACK is still one 0x80000 read and no more,
and threads 4101 (sub_829677D0) and 4104 (sub_82A7CD00) still block forever in
NtWaitForMultipleObjectsEx via sub_82B16CC8. Import count unchanged at 76.

## A sustained Sleep() spin, and call-site attribution for any import

2026-07-28. New signal in the same run:

    [heartbeat] 34328346 call(s) ... busiest: KeDelayExecutionThread x34325200
    [heartbeat] 34441329 call(s) ... busiest: KeDelayExecutionThread x34438190

6.8 million sleeps per second, *sustained*. Earlier runs showed a similar
number in the first heartbeat only, then a collapse to about ten thousand —
that was archive loading, transient. This one holds across every heartbeat
while every other import sits at its normal rate.

The all-thread dump cannot find it: every thread it can see is parked in a
genuine wait, and the spinner is somewhere it cannot name. This is the same
question the wait census answered for waits — which guest instruction is doing
this — so the mechanism is now general rather than another bespoke probe.

`wos::LogCallSite(name, ctx.lr, detail)` in import_log.cpp records
(import, guest call site, last argument) into a bounded table, reported by the
heartbeat as `[callsites]`. Any import can opt in with one line. The `detail`
field carries whatever number makes the call legible; for
KeDelayExecutionThread that is the requested delay in milliseconds, because a
spin on Sleep(0) and a spin on Sleep(1ms) are different bugs with different
causes.

Worth recording that implementing KeDelayExecutionThread properly did not end
this problem, it moved it. The unimplemented stub returned instantly and logged
45 M calls per five seconds; the correct implementation sleeps and still logs
34 M. The call count was never the bug — it was always a symptom of a loop
whose exit condition is not being met, and only the call site can say which.

## The ring capacity is still wrong, and refusing to consume hangs the GPU

2026-07-28, run 20260728-132051. The longest run yet — about ninety seconds —
and it ends in a real failure rather than an idle:

    [gpu] write pointer 0x4003 is past the ring capacity 0x4000 — not consuming.
    [game] ERR[D3D]: The GPU is hung!
    [game] CPU fence 0x1553, GPU fence 0x154f
    [game] CP_RB_RPTR: 0x00004003   CP_RB_WPTR: 0x00004003

The write pointer walked past 0x4000 monotonically — 0x3EA1, 0x4003, 0x4009 —
with no wrap. A ring index cannot exceed the ring it indexes, so a modelled
capacity of 0x4000 dwords is still too small. The log2-dword reading fixed the
earlier factor-of-four error but is not the whole encoding.

Two changes, neither of which guesses at the encoding:

- `VdInitializeRingBuffer` logs the **raw** argument as well as the derived
  size, so the encoding can be settled from evidence.
- `ConsumeRing` grows its capacity to the next power of two and continues,
  reporting once, instead of refusing to consume. Refusing is what turns this
  into a hang: the fence freezes, and the game's D3D layer declares the GPU
  hung within seconds. A wptr past capacity is proof our number is wrong, not
  a reason to stop.
- The wrap is now reported when it happens. The highest write pointer seen
  immediately before wptr drops IS the ring's true size, which settles the
  encoding by measurement rather than by argument.

## The Sleep spin has one call site

2026-07-28. `[callsites]` in the same run:

    bl@0x82B1A680 -> KeDelayExecutionThread  19418004 call(s), last arg 0xFFFFFFFF

One site, 19.4 million calls, and the count is *frozen* across every later
heartbeat — 19418004, then 19418006, 19418008, 19418010. So this is not an
ongoing spin at all: it burned 19 M calls during startup and then went quiet,
ticking over about twice per five seconds afterwards.

The sustained 34 M figure from the previous run was startup traffic landing
inside the sampling window, not a steady-state spin. The `last arg 0xFFFFFFFF`
is the sentinel this code writes for the absolute-time branch, so those calls
took the yield path.

Recorded because it closes a line of investigation cheaply: there is no Sleep
spin to fix. The call-site census earned its keep by ruling something out
rather than finding something, which is the more common and less satisfying
half of what a measurement does.

## Adapting the ring capacity removes the hang

2026-07-28, run 20260728-132643. The guard fires and the run continues:

    [gpu] write pointer 0x4003 exceeds modelled ring capacity 0x4000 — the size
          argument encoding is wrong. Growing to 0x8000 and continuing.
    [video] ring: CP_RB_WPTR=0x00005C77 rptr_writeback=0x00005C71  moved

No `ERR[D3D]: The GPU is hung!`, and the run passes the point that killed the
previous one. The fence keeps tracking at a steady -4 well past 0x4000, which
also says we are consuming real packets rather than reading rubbish — a
misparse would desynchronise the fence immediately.

The raw argument is 0xE. If the write pointer wraps at 0x8000 dwords then
`size_bytes = 8 << log2` (0x20000 = 128 KB for log2 14), and the ring fits
inside the 0xA0090000 physical block with room to spare: 0x914E0 to 0xC0000 is
0xBAC8 dwords of headroom. That is a prediction the next wrap will confirm or
refute; it is not yet established.

**A bound that still needs adding.** If the write pointer never wraps and keeps
climbing, ExecutePackets eventually reads past the ring's own allocation and
into the next one at 0xA00C0000, roughly 0xBAC8 dwords out. Growing the
capacity indefinitely trades a hang for silent corruption, which is the worse
of the two. The wrap is what keeps this correct, so if a run ever passes 0x8000
without wrapping, the capacity needs a hard ceiling at the allocation edge
rather than another doubling.

## A ceiling for the ring, at the allocation edge

2026-07-28. The adaptive capacity needed a bound, and the bound needed to come
from something we actually know rather than another guess.

Physical allocations were a bump allocator with no record of what it handed
out, so `MmAllocatePhysicalMemoryEx` now keeps the blocks and
`wos::PhysicalBlockEnd(addr)` answers "how far can I read from this pointer".
The ring's capacity is clamped to the end of the block it was allocated from.

Why it matters: growing without a ceiling turns "the write pointer went past
our modelled capacity" into the command processor walking out of the ring and
interpreting whatever was allocated next as PM4 packets. That is strictly
worse than the hang it replaced — a hang says where it stopped, corruption
does not. The ring sits at 0xA00914E0 inside the 0xA0090000+0x30000 block, so
the real ceiling is 0xBAC8 dwords.

Reaching that clamp would mean the size encoding is wrong in kind rather than
merely underestimated, so it reports once and says so instead of continuing
quietly. The span check after the clamp is not redundant: with a ceiling in
place the write pointer can still exceed the capacity, and at that point
refusing the read is correct.

## WaitAny waited on the first object only — a documented shortcut that was a deadlock

2026-07-28. `NtWaitForMultipleObjectsEx` carried this comment since it was
written:

    // WaitAll waits on every object in turn; WaitAny waits on the first.
    // Waiting on the first is a stated shortcut ... a game that expects to be
    // woken by the *second* handle will wake late. If something starts behaving
    // as though the wrong object signalled it, look here first.

That is what has been happening. Threads 4101 (sub_829677D0, the "Game Master")
and 4104 (sub_82A7CD00) have blocked there for every run since the file layer
started working, while ev0 and ev1 were signalled about sixteen hundred times
each. Waiting on handle[0] of a WaitAny is not a lateness bug — it is a
permanent block whenever the object that signals is not at index zero.

Two things were wrong, not one. The wait, and the **return value**: it returned
`kStatusSuccess` (0) unconditionally, which is `WAIT_OBJECT_0 + 0`. Even if the
right object had woken it, every caller was being told the first handle
signalled. A caller that switches on that result would take the wrong branch
every time.

**How the fix works.** Each EventObject has its own condition variable, which is
right for waiting on that object and useless for waiting on a set — there is no
way to block on several condition variables at once. So every `Set()` now bumps
one global generation counter and notifies one global condition variable. A
WaitAny consumes across its whole set, then sleeps on that channel, then
re-checks. The generation counter closes the check-then-sleep race: a `Set()`
landing in that window bumps it, so the sleep returns immediately instead of
missing the wake.

The cost is that an unrelated event wakes a WaitAny spuriously. That is a
re-check of a handful of booleans against signals running in the low thousands
per second, so it is not worth engineering around.

An INFINITE WaitAny is bounded internally at five seconds per iteration purely
so it can report itself once and print a guest stack before continuing to wait.
A wait that goes quiet forever is the failure this function exists to stop
hiding, and it should not be able to hide its own.

**Still shortcut, deliberately:** `KeWaitForMultipleObjects` has the same "first
object only" behaviour. Nothing is blocked there, its argument layout is not
confirmed, and changing two things at once is how the last several wrong turns
happened. It is recorded here rather than fixed.

## The import log serialises every guest thread

2026-07-28. Visible in the same stack dump, two threads caught inside our own
instrumentation:

    #0 malloc_base / #1 operator new / #2 wos::LogImportCall +0xAB
    #0 Mtx_lock / #1 wos::RecordWaitSite +0x2E

`LogImportCall` does `g_counts.try_emplace(name, 0)` on an
`unordered_map<std::string, uint64_t>` under a global mutex, for every import
call. That constructs a std::string and hashes it each time. During startup,
with 29 million KeDelayExecutionThread calls, every guest thread is funnelling
through one lock and one allocator.

Not fixed yet, and worth being explicit about why it matters beyond speed: this
is measurement apparatus distorting the thing it measures. The names are string
literals with stable addresses, so keying on the pointer would remove both the
allocation and the hash — the caveat being that dedup then depends on each
import name appearing as exactly one literal in the binary, which is true today
but is a property nothing enforces.

## Open questions / blockers

- **Three waits nobody signals.** Handles 0x00010050 and 0x00010024 via
  `NtWaitForMultipleObjectsEx` (threads entering at 0x82A7CD00 and 0x829677D0,
  both through `sub_82B16CC8`), and the guest-embedded event at 0x4083FDCC via
  `KeWaitForSingleObject`. These are now genuine blocks rather than spins, so
  the question is which code path is meant to signal them. `xex_info --xrefs`
  on the relevant thunk is the way to find out.
- **Archive loading stops after one read.** `game.XEPACK` is opened and the
  first 0x80000 bytes are read, then nothing further.
- **`VdSwap` has still never been called**, so no frame has been presented.
  Its call site is guest 0x82AC5094, immediately after
  `VdGetSystemCommandBuffer` — which is an unimplemented out-parameter stub.
  That is the next thing to implement.
- **Diagnostic output interleaves.** The all-thread stack dump and the blocked
  -wait reporter print from different threads without a shared lock, so their
  lines shred each other. Some frames also come back as `(no symbol)` mid-walk.
  Cosmetic, but it made the last run materially harder to read.
- **`[file] open FAILED "B<garbage>"`** — a filename built from wrong data,
  appearing right after `RtlMultiByteToUnicodeN`/`RtlUnicodeToMultiByteN` are
  first called. Points at a string conversion, not a missing file.
- **`\Device\Harddisk0\partition0` reports "no game root configured"** when a
  root *is* configured — the failure message is wrong even if the failure
  is not.
