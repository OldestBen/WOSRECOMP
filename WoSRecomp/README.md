# WoSRecomp

The runtime application — this is the hand-written PC-side code that gives
the recompiled PPC code (from `WoSRecompLib/ppc/`) something to actually run
against: graphics, audio, input, filesystem, and OS shims.

See [`docs/03-runtime-architecture.md`](../docs/03-runtime-architecture.md)
for the full design rationale and [`docs/04-roadmap.md`](../docs/04-roadmap.md)
for build order. Subdirectories:

| Dir | Purpose |
|---|---|
| [`kernel/`](kernel/) | Xbox 360 kernel/XEX import shims: threading, memory, sync primitives |
| [`gpu/`](gpu/) | Xenos → modern graphics backend, wiring up XenosRecomp shader output |
| [`apu/`](apu/) | Audio decode + mixing replacement |
| [`os/`](os/) | Filesystem, save-data, settings shims |
| [`ui/`](ui/) | Any native UI outside the game's own (launcher, config, etc.) |
| [`install/`](install/) | First-run flow, asset/XEX validation |
| [`patches/`](patches/) | Game-specific bug fixes / QoL patches on top of translated code |

All currently stubs — nothing has been implemented yet. Start with
`kernel/` per the roadmap.
