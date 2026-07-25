# Roadmap

A phased, realistic plan. Treat estimates loosely — Unleashed Recompiled
(the reference project for this whole approach) took a small team roughly
two years. This is a marathon, tackled in the same order that project
followed.

## Phase 0 — Toolchain (scaffold status: mostly done here)

- [x] Import `XenonRecomp` / `XenosRecomp` as submodules
- [x] Repo structure + docs
- [ ] Build `XenonAnalyse`, `XenonRecomp`, `XenosRecomp` locally
- [ ] Run `XenonTests` to sanity-check your Clang/CMake setup works correctly
      before touching real game code

## Phase 1 — Acquire & analyse

- [ ] Dump your legally owned Xbox 360 copy of Web of Shadows
- [ ] Locate `default.xex` (+ `default.xexp` title update, if one exists —
      worth checking, TUs often fix retail bugs that would otherwise need
      workarounds)
- [ ] Run `XenonAnalyse` for an initial switch-table TOML
- [ ] Disassemble the XEX (IDA/Ghidra/Xenia symbol DBs) far enough to locate
      the register save/restore helper functions and fill in
      `WoS_config.toml` (see `docs/02-config-guide.md`)

## Phase 2 — First clean recompile

- [ ] Run `XenonRecomp` and get it to emit C++ without erroring
- [ ] Get the *generated* C++ to actually compile under Clang — this alone
      surfaces the first wave of `functions[]` / `invalid_instructions[]`
      overrides you'll need
- [ ] No runtime behavior expected yet — success here just means "valid C++
      exists," not "it runs"

## Phase 3 — Minimal kernel shims, link & boot to first crash

- [ ] Stand up the smallest possible `WoSRecomp/kernel/` — enough fake
      NT/XEX-import surface for the entry point to run without immediately
      faulting (memory allocation, thread creation, basic sync primitives)
- [ ] Get the recompiled game linking into a runnable executable
- [ ] Expect an immediate crash or hang — that's the starting line, not a
      setback. Debugging from here is: crash → identify PPC address →
      config/hook fix → rebuild → repeat

## Phase 4 — Filesystem + first frame

- [ ] `WoSRecomp/os/` filesystem shim so the game can open its own asset
      packages
- [ ] Stand up `WoSRecomp/gpu/` far enough to create a device/swapchain and
      present *something* (even a blank/garbage frame) — this validates the
      graphics plumbing independent of shader correctness
- [ ] Begin running shaders through `XenosRecomp` as they're encountered

## Phase 5 — Iterative bring-up

- [ ] Push further into boot: main menu rendering, first level load
- [ ] Expect a long tail of mid-asm hooks and `functions[]` overrides
      accumulating in `WoS_config.toml`
- [ ] Audio (`WoSRecomp/apu/`) and input once visuals are stable enough to
      make them testable
- [ ] Save data (`WoSRecomp/os/`) — WoS's save format needs to be understood
      before this can be faked convincingly

## Phase 6 — Playability & polish

- [ ] Full playthrough stability
- [ ] Game-specific patches/QoL fixes (`WoSRecomp/patches/`) — bug fixes,
      widescreen/UI corrections, performance work
- [ ] Packaging/installer flow (`WoSRecomp/install/`), mirroring Unleashed
      Recompiled's first-run asset-validation approach

## Immediate next action

Right now, the actionable next step is **Phase 0**: run `tools/build_tools.sh`
and confirm `XenonAnalyse`/`XenonRecomp`/`XenosRecomp` all build cleanly on
your machine, before anything WoS-specific begins.
