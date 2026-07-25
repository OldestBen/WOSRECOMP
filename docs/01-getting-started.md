# Getting Started

This walks through going from a fresh clone to your first successful run of
`XenonAnalyse` and `XenonRecomp` against *Web of Shadows*.

## 1. Prerequisites

| Tool | Minimum version | Notes |
|---|---|---|
| CMake | 3.20 | |
| Clang  | 18 | XenonRecomp explicitly does not recommend other compilers — it leans on Clang-specific intrinsics/optimizations. |
| Git | any (with submodule support) | |
| A 360 dumping tool | — | e.g. a modded/JTAG 360, or existing extraction tooling for your legally owned disc/digital copy. Out of scope for this repo. |

Install Clang 18+ and CMake for your platform before continuing. On Ubuntu/Debian:

```bash
sudo apt install cmake clang-18 lld-18
```

## 2. Clone with submodules

If you already cloned this repo without `--recursive`:

```bash
git submodule update --init --recursive
```

This pulls down `tools/XenonRecomp` and `tools/XenosRecomp`, each with their own
nested submodules (e.g. `simde` for VMX intrinsics on non-x86, capstone-derived
disassembly bits, etc.).

## 3. Build the recompiler tools

```bash
./tools/build_tools.sh
```

This builds, from `tools/XenonRecomp`:
- `XenonAnalyse` — scans a XEX and detects jump tables
- `XenonRecomp` — takes a config TOML + `ppc_context.h` and emits C++
- `XenonTests` — the project's own PPC instruction test suite (useful for
  sanity-checking your toolchain, not WoS-specific)

and from `tools/XenosRecomp`:
- The shader recompiler (needed later, once we're pulling Xenos shaders out of
  the game's compiled shader blobs and turning them into HLSL)

Binaries land in `tools/XenonRecomp/build/` and `tools/XenosRecomp/build/` (or
wherever your CMake build directory is configured — see the script).

## 4. Get your XEX

You need to dump *your own* legally owned Xbox 360 copy of Web of Shadows. The
disc/package will contain a `default.xex` (the main executable) and possibly a
title update package containing a `default.xexp` (a patch XEX with bug fixes —
worth checking if one exists, since it may fix issues in the base retail code).

Place them here (this directory is gitignored, nothing here gets committed):

```
private/
├── default.xex
└── default.xexp        # optional, if you have a title update
```

How you dump the XEX from your disc/console is outside this repo's scope —
that's general Xbox 360 modding/dumping knowledge, not specific to
recompilation.

## 5. First analysis pass

```bash
tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse \
    private/default.xex \
    WoSRecompLib/config/WoS_switch_tables.toml
```

This produces a TOML of detected jump/switch tables. Expect to revisit this —
the analyser's heuristics aren't perfect for every game, and WoS may need
manual corrections (see [`docs/02-config-guide.md`](02-config-guide.md)).

## 6. First recompile pass

Once [`WoSRecompLib/config/WoS_config.toml`](../WoSRecompLib/config/WoS_config.toml)
is filled in (see the config guide — this is the part that actually requires
reverse-engineering work specific to WoS):

```bash
tools/XenonRecomp/build/XenonRecomp/XenonRecomp \
    WoSRecompLib/config/WoS_config.toml \
    tools/XenonRecomp/XenonUtils/ppc_context.h
```

Output C++ lands in `WoSRecompLib/ppc/` (as configured by `out_directory_path`
in the TOML).

**This will not "just work" on the first try.** Getting a clean recompile
that the C++ compiler accepts, and then getting that compiled binary to
actually boot, are two separate long efforts. See
[`docs/04-roadmap.md`](04-roadmap.md) for the realistic path from here.

## 7. Next steps

- [`docs/02-config-guide.md`](02-config-guide.md) — how to fill in the WoS-specific
  addresses (register save/restore functions, setjmp/longjmp, mid-asm hooks).
- [`docs/03-runtime-architecture.md`](03-runtime-architecture.md) — what you're
  building in `WoSRecomp/` once the PPC code compiles.
- [`docs/04-roadmap.md`](04-roadmap.md) — the phased plan.
