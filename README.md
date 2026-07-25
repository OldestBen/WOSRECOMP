# Web of Shadows Recompiled (WoSRecomp)

An **in-progress, unofficial** effort to create a native PC port of the **Xbox 360**
version of *Spider-Man: Web of Shadows* through **static recompilation**, using
[hedge-dev/XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and
[hedge-dev/XenosRecomp](https://github.com/hedge-dev/XenosRecomp).

This project follows the structure and conventions established by
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp), the reference
implementation for a full XenonRecomp-based port.

> ⚠️ **Read this first:** This repo contains **no game code or assets**. It is a
> toolchain + runtime scaffold. You must legally own *Spider-Man: Web of Shadows*
> on Xbox 360 and dump your own files. See [Legal & Scope](#legal--scope).

---

## What this actually is

XenonRecomp reads the **PowerPC machine code** out of an Xbox 360 executable
(`default.xex`) and statically translates it into **C++**. That C++ is then compiled
natively for PC. It is *not* a patch for the existing (bad) PC port — it builds a
brand-new port from the 360 binary.

XenonRecomp only produces the translated CPU code. **It does not provide a runtime.**
Graphics, audio, input, filesystem, saves, and OS/kernel shims are all things *we*
write, in the [`WoSRecomp/`](WoSRecomp/) runtime layer. This is the bulk of the work.

```
   your legally-dumped                XenonRecomp                   our runtime
   default.xex  (PPC)   ──▶  analyse ──▶ recompile ──▶  ppc/*.cpp  ──▶  WoSRecomp app
   Xenos shaders        ──▶  XenosRecomp             ──▶  HLSL      ──▶  (GPU/APU/kernel/…)
```

## Repository layout

```
WOSRECOMP/
├── tools/
│   ├── XenonRecomp/        # submodule — the PPC→C++ recompiler + XenonAnalyse
│   ├── XenosRecomp/        # submodule — the Xenos shader → HLSL recompiler
│   ├── build_tools.sh      # builds XenonAnalyse / XenonRecomp / XenosRecomp
│   └── recompile.sh        # runs the analyse → recompile pipeline for WoS
├── WoSRecompLib/
│   ├── config/
│   │   └── WoS_config.toml  # THE config you hand-author (addresses, hooks, etc.)
│   ├── ppc/                 # generated C++ output (gitignored)
│   └── README.md
├── WoSRecomp/               # the runtime we write (see its README)
│   ├── gpu/  apu/  kernel/  os/  ui/  install/  patches/
│   └── README.md
├── private/                 # YOU drop default.xex here (gitignored)
├── docs/                    # start here 👇
│   ├── 01-getting-started.md
│   ├── 02-config-guide.md
│   ├── 03-runtime-architecture.md
│   └── 04-roadmap.md
└── CMakeLists.txt
```

## Quick start

Full detail lives in [`docs/01-getting-started.md`](docs/01-getting-started.md). The
short version:

```bash
# 1. Clone with submodules (or init them after the fact)
git submodule update --init --recursive

# 2. Build the recompiler tools (needs CMake 3.20+ and Clang 18+)
./tools/build_tools.sh

# 3. Dump your Xbox 360 copy, put default.xex (+ optional default.xexp) in private/

# 4. Fill in WoSRecompLib/config/WoS_config.toml (addresses are game-specific!)
#    See docs/02-config-guide.md — this is where the real work starts.

# 5. Run the pipeline
./tools/recompile.sh
```

## Prerequisites

- **CMake** 3.20 or later
- **Clang** 18 or later (other compilers are explicitly *not* recommended by XenonRecomp)
- A disc dumper / extractor to get `default.xex` from your Xbox 360 copy
- Patience: Unleashed Recompiled was a small team over ~2 years.

## Legal & Scope

- The **tools** are open source (MIT). This scaffold is MIT.
- You must **own** the game and **dump your own** files. This repo ships nothing
  copyrighted, and the [`.gitignore`](.gitignore) is configured to keep `.xex`/`.xexp`
  and generated `ppc/` code out of version control.
- **Do not** commit or distribute the XEX, the recompiled game code, or game assets.
  Only the *runtime source* is shareable — exactly the model Unleashed Recompiled uses.

## Status

🔴 **Scaffold stage.** Toolchain imported and verified building end-to-end. Structure
and docs in place. Nothing game-specific has been recompiled yet. See
[`PROGRESS.md`](PROGRESS.md) for the up-to-date current state and a running devlog —
**read that first** in any new session before digging through commit history. The
immediate next task is building the tools and producing a first analyse pass on a
real XEX — see [`docs/04-roadmap.md`](docs/04-roadmap.md).

## Credits & references

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) / [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) — hedge-dev and contributors
- [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) — reference port structure
- Inspired by [N64: Recompiled](https://github.com/N64Recomp/N64Recomp)
