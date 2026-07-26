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

**Cause (established, not guessed):** `recompiler.cpp` registers one function
per `.pdata` unwind record, sized `FunctionLength * 4`. A single logical
function can be split across several consecutive `.pdata` records, so a
switch jumping between the pieces looks out of bounds.

**Fix:** `functions = [...]` overrides that span the whole function.
Automated by `xex_info --fix-switches`, which finds the `.pdata` record
containing each switch site and widens it to reach the furthest case target,
absorbing the intervening records. Config entries take priority over
`.pdata` (they're registered first), so the override wins.

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

These need implementing in XenonRecomp's `recompiler.cpp` (upstream code we
vendor). Mostly bounded, mechanical work: the branch forms (`bso`/`bns`/
`bsolr`) are condition-register variants, `dcbst` is a cache hint that can be
a no-op, `frsqrte` maps to a math intrinsic, and the vector ops map onto
`simde`. **Not yet addressed.**

## Explicit function boundary overrides

Running log of `functions = [...]` entries added to the config and *why*
(what error/crash led to adding each one). Append, don't rewrite — this is
a history, and the reasoning matters more than the raw TOML (which already
lives in the config file itself).

- **2026-07-26 — pending first batch.** 123 switch sites need widened
  functions (see above). Entries to be generated by
  `xex_info --fix-switches` and pasted in; the tool reports the original
  `.pdata` size and how many records each override absorbs, so the diff is
  auditable.

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
