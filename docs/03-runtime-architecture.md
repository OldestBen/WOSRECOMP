# Runtime Architecture

XenonRecomp only gets you translated PPC-as-C++. It calls out into a runtime
*you* write for everything the Xbox 360 OS/kernel and hardware used to
provide. This is the majority of the actual project effort, and it's what
lives under [`WoSRecomp/`](../WoSRecomp/).

This layout mirrors [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp)'s
split between `UnleashedRecompLib` (generated PPC + shared headers) and
`UnleashedRecomp` (the actual runtime app), adapted here as `WoSRecompLib/`
and `WoSRecomp/`.

## Why a runtime is needed at all

The recompiled game code still makes calls that, on real hardware, went to:
- the Xbox 360 **kernel** (threading, memory allocation, file I/O, XAM/XUI
  system calls)
- the **GPU** (Xenos — a D3D9-like unified shader architecture, but with its
  own quirks: EDRAM, tiling, predicated tiling, etc.)
- the **APU** (XMA-encoded audio, XAudio-style mixing)
- **peripherals** (controller input, save devices)

None of that exists on a PC. The runtime's job is to intercept those calls —
largely through the mid-asm hooks and imported-function stubs described in
[`docs/02-config-guide.md`](02-config-guide.md) — and reimplement their
*behavior* using modern PC APIs.

## Planned directory layout (`WoSRecomp/`)

```
WoSRecomp/
├── kernel/     # Xbox 360 kernel import shims: threads, memory, XEX imports,
│               # synchronization primitives (critical sections, events),
│               # the "fake" NT-style APIs the recompiled code expects to call.
├── gpu/        # Xenos → modern graphics backend (D3D12 and/or Vulkan).
│               # This is where XenosRecomp-translated shaders (HLSL) get
│               # wired up, plus command-buffer / state translation.
├── apu/        # Audio: XMA decode replacement + a modern mixer/backend
│               # (e.g. via a cross-platform audio library).
├── os/         # Filesystem shims, save-game (STFS-style) emulation,
│               # settings storage, OS-level window/input plumbing.
├── ui/         # Any native UI needed outside the game's own UI (e.g. a
│               # launcher, config screen, input remapping — see
│               # UnleashedRecomp's approach for prior art).
├── install/    # First-run flow: pointing the app at your dumped game files,
│               # asset/XEX validation, TU detection.
└── patches/    # Game-specific bug fixes / QoL patches applied on top of the
                # otherwise mechanically-translated retail code (again, mirroring
                # Unleashed Recompiled's "patches" concept — most patches there
                # were mid-asm hooks or targeted function replacements).
```

Each of these is currently a stub with just a `README.md` placeholder — see
[`docs/04-roadmap.md`](04-roadmap.md) for build order.

## `WoSRecompLib/`

```
WoSRecompLib/
├── config/    # WoS_config.toml, WoS_switch_tables.toml — hand-maintained
├── ppc/       # generated output of XenonRecomp — NOT hand-edited, NOT committed
└── shaders/   # XenosRecomp output (HLSL) once shader work begins
```

Treat `ppc/` as a build artifact, same as `.o` files — it's regenerated from
the config + your XEX, never edited directly, and gitignored.

## Graphics: the hardest part

Xenos is a D3D9-generation GPU with several 360-specific behaviors (EDRAM
render targets, memory export, predicated tiling, specific texture tiling
formats) that have no 1:1 modern equivalent. Unleashed Recompiled built a
D3D12 backend (with follow-on community work toward Vulkan) that reimplements
Xenos semantics on top of modern APIs. Realistically, this repo should:

1. Start by referencing Unleashed Recompiled's `gpu/` implementation as prior
   art for the *category* of problems (not copy-paste — WoS's renderer,
   post-processing stack, and shader usage will differ).
2. Get XenosRecomp building and extracting WoS's shaders early, even before
   full gameplay boots, so shader translation issues surface sooner rather
   than later.

## Suggested build order (see roadmap for full detail)

1. Kernel shims minimal enough that the game's entry point runs and gets past
   initial engine bring-up (memory allocators, thread creation) without
   crashing.
2. Filesystem shim (game needs to read its own asset packages).
3. Bring up a black window via the GPU backend — i.e., get *a* frame
   presenting, even if garbage — to validate the graphics pipeline plumbing.
4. Iterate: fix crashes/hangs address-by-address, filling in
   `WoS_config.toml` and the runtime stubs as you go.
5. Audio, input, save data — once the game is visibly running.
6. Game-specific patches/QoL once it's stable enough to play through.
