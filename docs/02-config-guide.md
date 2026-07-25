# Config Guide — `WoS_config.toml`

This is the single most important file in the project. It tells XenonRecomp
*how* to read the PPC binary correctly, and it is entirely game-specific — the
addresses below are **placeholders copied from XenonRecomp's own example
(Sonic Unleashed / SWA)** and must be replaced with values found in *Web of
Shadows*'s `default.xex`.

The template lives at
[`WoSRecompLib/config/WoS_config.toml`](../WoSRecompLib/config/WoS_config.toml).
Field reference below, straight from the upstream
[XenonRecomp README](https://github.com/hedge-dev/XenonRecomp).

## `[main]`

```toml
[main]
file_path = "../../private/default.xex"
patch_file_path = "../../private/default.xexp"       # optional — omit if no TU
patched_file_path = "../../private/default_patched.xex"
out_directory_path = "../ppc"
switch_table_file_path = "WoS_switch_tables.toml"     # from XenonAnalyse, step 5
```

- `file_path` — your dumped retail XEX.
- `patch_file_path` / `patched_file_path` — only relevant if you have a title
  update XEXP. XenonRecomp merges the patch and writes the merged result to
  `patched_file_path`, then recompiles *that*. If WoS has no TU, omit both and
  point `file_path` straight at the retail XEX.
- `out_directory_path` — where generated `.cpp`/`.h` land.
- `switch_table_file_path` — output of `XenonAnalyse` from the getting-started
  guide, step 5.

## Optimization flags

```toml
skip_lr = false
skip_msr = false
ctr_as_local = false
xer_as_local = false
reserved_as_local = false
cr_as_local = false
non_argument_as_local = false
non_volatile_as_local = false
```

All default `false` (safe/conservative — every register is modeled fully).
Flipping these to `true` trades correctness-in-edge-cases for speed/simpler
generated code, by treating certain PPC registers as local variables instead
of persistent context state. **Leave these `false` until you have a working
build**, then experiment — turning these on prematurely makes miscompiles
much harder to debug.

## Register save/restore functions — **required, WoS-specific**

```toml
restgprlr_14_address = 0x00000000   # TODO: find in WoS default.xex
savegprlr_14_address = 0x00000000
restfpr_14_address   = 0x00000000
savefpr_14_address   = 0x00000000
restvmx_14_address   = 0x00000000
savevmx_14_address   = 0x00000000
restvmx_64_address   = 0x00000000
savevmx_64_address   = 0x00000000
```

Every Xbox 360 binary contains small, near-identical compiler-generated
helper functions (`__savegprlr_14`, `__restgprlr_14`, `__savefpr_14`,
`__restvmx_14`, etc.) that save/restore non-volatile GPRs, FPRs, and VMX
(vector) registers around function prologues/epilogues — they act like
switch-case fallthroughs, saving/restoring registers 14 through 31. Every
function using non-volatile registers either inlines or calls into these.
XenonRecomp needs each one's starting address to recompile calls to them
correctly; per the upstream README, there's currently no auto-detection for
this, so it's a manual lookup per game.

**How to find them** — search `default.xex`'s disassembly for these leading
instruction bytes (from the upstream README):

| Field | Function | Starts with | Byte pattern |
|---|---|---|---|
| `restgprlr_14_address` | `__restgprlr_14` | `ld r14, -0x98(r1)` | `E9 C1 FF 68` |
| `savegprlr_14_address` | `__savegprlr_14` | `std r14, -0x98(r1)` | `F9 C1 FF 68` |
| `restfpr_14_address` | `__restfpr_14` | `lfd f14, -0x90(r12)` | `C9 CC FF 70` |
| `savefpr_14_address` | `__savefpr_14` | `stfd r14, -0x90(r12)` | `D9 CC FF 70` |
| `restvmx_14_address` | `__restvmx_14` | `li r11,-0x120` / `lvx v14,r11,r12` | `39 60 FE E0 7D CB 60 CE` |
| `savevmx_14_address` | `__savevmx_14` | `li r11,-0x120` / `stvx v14,r11,r12` | `39 60 FE E0 7D CB 61 CE` |
| `restvmx_64_address` | `__restvmx_64` | `li r11,-0x400` / `lvx128 v64,r11,r12` | `39 60 FC 00 10 0B 60 CB` |
| `savevmx_64_address` | `__savevmx_64` | `li r11,-0x400` / `stvx128 v64,r11,r12` | `39 60 FC 00 10 0B 61 CB` |

These byte patterns are consistent across Xbox 360 binaries in general (not
just one game), so a hex search for these sequences in `default.xex` should
find all eight quickly, regardless of the disassembler you use. Unleashed
Recompiled's real config
([`UnleashedRecompLib/config/SWA.toml`](https://github.com/hedge-dev/UnleashedRecomp/blob/main/UnleashedRecompLib/config/SWA.toml))
is the canonical worked example — note its addresses are Sonic Unleashed-specific
and won't match WoS, but the byte patterns above are what you're hunting for.

## `longjmp` / `setjmp` — optional but recommended

```toml
longjmp_address = 0x00000000   # TODO
setjmp_address  = 0x00000000   # TODO
```

XenonRecomp redirects these directly to native `setjmp`/`longjmp` rather than
statically translating them. Per the upstream README: a good way to find
`longjmp` is to look for calls to `RtlUnwind`; `setjmp` typically appears
immediately after it in the disassembly. **If WoS doesn't use these at all,
just delete both lines from the config** — they're optional.

## Explicit function boundaries

```toml
functions = [
    # { address = 0x82XXXXXX, size = 0xNN },
]
```

The analyser's control-flow heuristics occasionally get a function's start or
length wrong (e.g. tail-call-heavy code, hand-written ASM, or unusual
epilogues). When you hit a "recompiled code doesn't match" or crash-on-boot
bug that traces back to a specific address, you add an explicit override
here. **Expect this list to grow steadily** as you bring up more of the game
— it's normal, iterative work, not a one-time setup step.

## Invalid instruction skips

```toml
invalid_instructions = [
    # { data = 0x00000000, size = 4 },  # e.g. padding between functions
    # { data = 0x831B1C90, size = 8 },  # e.g. C++ frame handler data embedded in .text
]
```

Non-code data sometimes lives inside `.text` (padding, exception frame
handler tables, etc.) and will fail to disassemble as valid PPC. This tells
the recompiler to skip over known-bad regions rather than choke on them.

## Mid-asm hooks — this is where the runtime plugs in

```toml
[[midasm_hook]]
name = "ExampleMidAsmHook"
address = 0x00000000
registers = ["r3"]
```

A mid-asm hook lets you inject a call out to hand-written C++ **in the
middle** of recompiled PPC code, at a specific address, with specific
registers passed through — without overwriting the instruction at that
address. This is the primary mechanism for splicing your own runtime logic
(e.g. replacing a D3D9 draw call, intercepting a save-file write, hooking
audio playback) into otherwise-mechanically-translated game code. You
implement the corresponding C++ function (e.g.
`void ExampleMidAsmHook(PPCRegister& r3) { ... }`) in the `WoSRecomp/`
runtime; the linker resolves it. You'll accumulate many of these as you
bring up graphics, audio, and I/O — see
[`docs/03-runtime-architecture.md`](03-runtime-architecture.md).

Additional optional fields per hook (mutually exclusive in the combinations
noted — the recompiler warns if violated):

| Field | Purpose |
|---|---|
| `return` | Function returns immediately after the hook call. |
| `return_on_true` / `return_on_false` | Conditional return based on the hook's bool return value. |
| `jump_address` | Jump to another address in the *same* function after the hook call. |
| `jump_address_on_true` / `jump_address_on_false` | Conditional jump based on the hook's bool return value. |
| `after_instruction` | Place the hook after the instruction at `address` instead of before it. |

## Workflow for finding these addresses

1. Load `default.xex` in a disassembler that understands the Xenon PPC ABI
   (Xenia has reference symbol databases for many 360 titles; IDA/Ghidra with
   an XEX loader plugin also works).
2. Identify the compiler helper functions (savegprlr/restgprlr/etc.) — these
   are boilerplate and typically easy to pattern-match once you've seen one.
3. Run `XenonAnalyse` early and often — feed its switch-table output back in,
   then attempt a recompile, and use compiler/runtime errors to find the next
   address you need to fix or add.
4. Cross-reference against Unleashed Recompiled's finished config for
   *patterns*, not addresses — the actual addresses are unique to each XEX
   build.

This is inherently a reverse-engineering loop: recompile → hit an error or
crash → identify the offending address/function → add a config entry or
mid-asm hook → recompile again.
