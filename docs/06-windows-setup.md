# Windows setup (native)

The native-Windows path. This is arguably the *best-trodden* route for this
toolchain — XenonRecomp ships a `CMakeSettings.json` for Visual Studio, and
Unleashed Recompiled (the reference port) is Windows-primary.

> **Status: unverified.** Nothing in this repo has actually been built on
> Windows yet — only Linux (Ubuntu 24.04 / Clang 18.1.3). This document is
> written from the toolchain's requirements, not from a successful run. If
> something here is wrong, fix the doc as part of fixing the problem.

## 1. Install Visual Studio 2022

The Community edition is free. In the installer, select the **"Desktop
development with C++"** workload, and confirm these under *Individual
components*:

- **C++ Clang compiler for Windows** (Clang 18+ — VS 2022 17.10 and later
  ship 18 or newer)
- **C++ CMake tools for Windows** (brings CMake **and** Ninja)
- **Windows 11 SDK** (or 10 SDK — Clang needs the platform headers/libs)

This single workload gives you Clang, CMake, Ninja, and the SDK together,
which is why it beats installing standalone LLVM — a bare LLVM install
still needs the Windows SDK from somewhere.

## 2. Install Git for Windows

<https://git-scm.com/download/win>

Take the defaults. The important part is **Git Bash**, which is what runs
this repo's `tools/*.sh` scripts. They're bash, and Git Bash provides a
real GNU userland (`find -printf`, `mapfile`, `sed -E`, `tee` all work).

## 3. Launch Git Bash *inside* the VS developer environment

**This step matters.** Clang on Windows needs `INCLUDE`/`LIB` pointing at
the MSVC toolchain and Windows SDK. Clang can often auto-detect these, but
inheriting them from a developer shell is far more reliable.

1. Start menu → **"x64 Native Tools Command Prompt for VS 2022"**
2. In that prompt, launch bash:

   ```cmd
   "C:\Program Files\Git\bin\bash.exe" -l
   ```

You now have a bash shell that carries the full MSVC environment. Do all
the steps below from *this* shell.

Quick sanity check:

```bash
clang++ --version      # expect 18 or newer
cmake --version        # expect 3.20+
ninja --version
```

If `clang++` isn't found, the VS "C++ Clang compiler for Windows" component
isn't installed, or you're not in the developer prompt.

## 4. Clone and build

```bash
cd ~
git clone https://github.com/OldestBen/WOSRECOMP.git
cd WOSRECOMP
git checkout claude/spiderman-web-shadows-recompile-ad7kn5
git submodule update --init --recursive

tools/run_logged.sh build -- ./tools/build_tools.sh
```

`build_tools.sh` handles Windows specifics itself: it selects **`clang-cl`**
(see below), reads core count from `NUMBER_OF_PROCESSORS` since Git Bash has
no `nproc`, and refuses to build with Clang older than 18 rather than
failing confusingly later.

### Why `clang-cl` and not `clang`

Visual Studio ships the same Clang under two drivers, side by side:

| Binary | Driver style | Use |
|---|---|---|
| `clang.exe` | GNU-like (`-Wall`, `-o`) | Unix-style builds |
| `clang-cl.exe` | MSVC-like (`/W4`, `/Fo`) | MSVC-ABI builds |

XenonRecomp's `CMakeLists.txt` sets `CMAKE_MSVC_RUNTIME_LIBRARY` and, under
policy `CMP0141`, `CMAKE_MSVC_DEBUG_INFORMATION_FORMAT`. Those are MSVC-ABI
abstractions that the GNU-style driver doesn't implement, so configuring
with plain `clang.exe` dies at the compiler-check stage with:

```
MSVC_DEBUG_INFORMATION_FORMAT value 'ProgramDatabase' not known for this C compiler.
CMake Error ... CMakeTestCCompiler.cmake:56 (try_compile):
  Failed to generate test project build system.
```

`clang-cl` is the *same compiler* with an MSVC-compatible driver, so those
settings apply cleanly. `build_tools.sh` now picks it automatically on
Windows; you shouldn't have to do anything.

If you ever need to override:

```bash
CC=clang-cl CXX=clang-cl ./tools/build_tools.sh
```

And if `clang-cl` itself ever causes trouble, the alternative is to disable
the policy rather than change compiler:

```bash
CMAKE_ARGS="-DCMAKE_POLICY_DEFAULT_CMP0141=OLD" ./tools/build_tools.sh
```

### The archiver has to match the driver too

Choosing `clang-cl` isn't sufficient on its own. LLVM ships **two**
archivers, and CMake's autodetection can pick the wrong one:

| Binary | Flag style | Matches |
|---|---|---|
| `llvm-ar.exe` | GNU (`-rcs`) | `clang.exe` |
| `llvm-lib.exe` | MSVC (`/nologo`, `/out:`) | `clang-cl.exe` |

With clang-cl, CMake emits the MSVC archive rule:

```
<CMAKE_AR> /nologo /out:<TARGET> <OBJECTS>
```

If `CMAKE_AR` resolved to `llvm-ar.exe`, that fails immediately:

```
llvm-ar.exe: error: unknown option /
```

`build_tools.sh` now pins `CMAKE_AR` to the `llvm-lib.exe` sitting beside
whichever `clang-cl` is on `PATH`, so this is handled. If your layout is
unusual and it can't find one, it warns and you can point it manually:

```bash
CMAKE_ARGS="-DCMAKE_AR=C:/path/to/llvm-lib.exe" ./tools/build_tools.sh
```

### Harmless warnings you can ignore

CMake 4.x emits deprecation warnings for vendored dependencies that declare
`cmake_minimum_required` below 3.10 (xxHash and one of XenonRecomp's own
subdirectories do):

```
CMake Deprecation Warning ... Compatibility with CMake < 3.10 will be removed
```

These are upstream's to fix and don't affect the build.

## 5. Paths in Git Bash

Git Bash uses POSIX-style paths. A Windows path like:

```
C:\Users\benro\wos
```

is written:

```
/c/Users/benro/wos
```

So ingesting a dump looks like:

```bash
tools/import_dump.sh /c/Users/benro/wos
```

Tab-completion works normally. If a path contains spaces, quote it.

## 6. Where to put the repo and the dump

Both can live anywhere with space — unlike WSL2, there's no
cross-filesystem performance penalty here, because it's all native NTFS.

Budget roughly: the disc dump (up to ~8 GB for dual-layer), the generated
C++ (several GB), and build artifacts (potentially tens of GB once the PPC
code compiles).

## If Clang can't find standard headers

Symptom: errors like `'stddef.h' file not found` or missing `<windows.h>`.

Cause: you're not in the developer environment (step 3). Verify with:

```bash
echo "$INCLUDE"
```

Empty means the MSVC environment wasn't inherited — relaunch bash from the
x64 Native Tools prompt.

## Why not WSL2?

It's a fine option and was tried first, but hit a networking failure (DNS
resolved, outbound TCP timed out — Windows itself had connectivity, so a
WSL-specific config issue, commonly firewall/AV or VPN interference with
the virtual adapter). Rather than sink time into diagnosing it, native
Windows is the better destination anyway: the eventual runtime needs to be
a native Windows binary, so you'd end up here regardless.

If you ever want to revisit WSL, the usual fixes are `wsl --shutdown`, then
a `%UserProfile%\.wslconfig` containing `[wsl2]` / `networkingMode=mirrored`.
