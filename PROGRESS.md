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

- **Stage:** Scaffold + toolchain verified, dump-ingestion tooling built. No
  game-specific work started yet.
- **Toolchain:** `XenonAnalyse`, `XenonRecomp`, `XenosRecomp`, `xex_info`, and
  `extract-xiso` all build cleanly via `./tools/build_tools.sh` on **both**:
  - Ubuntu 24.04 / Clang 18.1.3 / CMake 3.28 (cloud sandbox)
  - **Windows / VS 2026 / clang-cl 22.1.3 / CMake 4.3.1 / Ninja 1.13.2 —
    the user's own workstation, verified 2026-07-26, exit 0 in 22s.**
    Required three fixes: select `clang-cl` over `clang`, pin `CMAKE_AR` to
    `llvm-lib`, and wipe stale build trees on toolchain change. Only
    remaining noise is `strerror` deprecation warnings from extract-xiso
    (upstream 2003-era C; harmless).
- **Dump ingestion:** `tools/import_dump.sh <path>` is ready — finds
  `default.xex`/`default.xexp` in a raw dump (flat or nested layouts, or a
  `.iso` via `extract-xiso`), copies into `private/`, and prints a
  non-copyrighted summary (directory tree + `xex_info` output) for sharing.
  Tested against synthetic fixtures; not yet run against a real WoS dump.
- **Config:** `WoSRecompLib/config/WoS_config.toml` is still the placeholder
  copied from XenonRecomp's Sonic Unleashed example — every address in it is
  `0x00000000` or a Unleashed-specific value and **must** be replaced with
  values found in WoS's own `default.xex`. Nothing has been recompiled yet.
- **Game files:** Not yet in hand. User has extracted their disc into a local
  folder (`wos/`) on their own machine — not yet run through
  `import_dump.sh` or shared back into this session.
- **Platform:** **Windows workstation, native, toolchain building green.**
  See [`docs/06-windows-setup.md`](docs/06-windows-setup.md). WSL2 was tried
  first and abandoned (no outbound network from the distro; details in the
  session log).
- **Blocked on:** running `tools/import_dump.sh` against the `wos` folder
  and sharing the tree + `xex_info` output → address-hunting in the XEX
  (docs/02-config-guide.md) starts there. **This is now the only thing
  standing between us and real work.**
- **Findings log:** `docs/05-findings-log.md` added as the durable place to
  record dump info/addresses (separate from this file — see note above).
  Still empty; nothing pasted in yet.
- **User's machines:**
  - *Primary (Windows workstation)* — Ryzen 9 9950X3D (16C/32T), RTX 5090,
    **32 GB RAM**. Intended target for the heavy work. Disk capacity not
    yet stated.
  - *Currently available (Mac)* — no Homebrew, using `tools/docker/`
    instead. Fine for ingest/analyse; not the machine for bulk compiles.
  - **Forward-looking concern:** 32 GB is the likely constraint, not the
    CPU. Compiling the generated PPC C++ is many parallel Clang jobs on
    large TUs; a naive `-j32` on 32 GB can thrash or OOM. When a build for
    `WoSRecompLib/ppc/` exists, cap parallelism (`-j12`–`-j16`) and tune
    upward from measurements rather than defaulting to `nproc`. Not
    actionable yet — `tools/build_tools.sh` only builds the toolchain
    itself, which is small.

## Next Steps (in order)

1. User runs `tools/import_dump.sh <path-to-wos-folder>` on macOS (Homebrew
   `llvm@18`), pastes the resulting directory tree + `xex_info` output into
   chat *and* into `docs/05-findings-log.md`, and notes region/edition
   (NTSC-U retail vs. Platinum Hits vs. PAL vs. JP — these can differ in
   binary layout).
2. Run `XenonAnalyse` against it to produce the first switch-table TOML.
3. Find the 8 register save/restore function addresses (byte-pattern search,
   see `docs/02-config-guide.md`) and fill in `WoS_config.toml`.
4. First `XenonRecomp` pass — expect errors, iterate on `functions =`
   overrides and `invalid_instructions =` entries.
5. Only after CPU code recompiles cleanly: start on the `WoSRecomp/` runtime
   (kernel/OS shims first, then GPU via XenosRecomp, then APU/input/UI).

---

## Log

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
