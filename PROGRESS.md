# Progress Log

Living devlog for this project. **Read this file first** in any new session —
it's kept up to date after every meaningful change so you don't have to dig
through commit history to know where things stand.

Format: a **Current State** summary (rewritten in place, always reflects
"now"), then a reverse-chronological **Log** of dated entries (append-only,
never edited after the fact — corrections get a new entry).

---

## Current State

- **Stage:** Scaffold + toolchain verified. No game-specific work started yet.
- **Toolchain:** `XenonAnalyse`, `XenonRecomp`, `XenosRecomp` all build and run
  cleanly via `./tools/build_tools.sh` (verified on Ubuntu 24.04, Clang 18.1.3,
  CMake 3.28, in the cloud sandbox — not yet verified on the user's own
  machine/OS).
- **Config:** `WoSRecompLib/config/WoS_config.toml` is still the placeholder
  copied from XenonRecomp's Sonic Unleashed example — every address in it is
  `0x00000000` or a Unleashed-specific value and **must** be replaced with
  values found in WoS's own `default.xex`. Nothing has been recompiled yet.
- **Game files:** Not yet in hand. User is extracting their own legally-owned
  Xbox 360 disc. Blocking next step: get `default.xex` (+ any title update)
  into `private/`, confirm region/edition.
- **Blocked on:** disc extraction (user, in progress) → address-hunting in
  the XEX (docs/02-config-guide.md) can't start until we have the binary.

## Next Steps (in order)

1. Get `default.xex` (and `default.xexp` if a title update exists) into
   `private/`. Confirm region/edition (NTSC-U retail vs. Platinum Hits vs.
   PAL vs. JP — these can differ in binary layout).
2. Run `XenonAnalyse` against it to produce the first switch-table TOML.
3. Find the 8 register save/restore function addresses (byte-pattern search,
   see `docs/02-config-guide.md`) and fill in `WoS_config.toml`.
4. First `XenonRecomp` pass — expect errors, iterate on `functions =`
   overrides and `invalid_instructions =` entries.
5. Only after CPU code recompiles cleanly: start on the `WoSRecomp/` runtime
   (kernel/OS shims first, then GPU via XenosRecomp, then APU/input/UI).

---

## Log

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
