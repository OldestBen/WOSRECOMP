# Resuming after a reboot

Everything needed to get from a freshly-booted Windows machine back to a
running build, in order. Nothing here needs to be remembered — it is all in
this file precisely so it does not.

## 1. Open a shell that has the MSVC environment

**This is the step that catches people out.** Plain Git Bash cannot build this
project: `clang-cl` needs `INCLUDE`, `LIB` and the Windows SDK on `PATH`, and
only the Visual Studio developer environment sets those.

1. Start menu → type `x64 Native Tools` → open **"x64 Native Tools Command
   Prompt for VS"**. The banner should say `Environment initialized for: 'x64'`.
2. From that prompt, launch bash **without** `--login`, so it inherits the
   environment instead of rebuilding `PATH`:

   ```
   "C:\Program Files\Git\bin\bash.exe"
   ```

   The prompt will look bare (`bash-5.2$`) rather than the usual coloured
   MINGW64 one. That is correct — it means `/etc/profile` did not run and did
   not clobber the compiler paths.

To keep the coloured prompt instead, tell Git Bash to inherit `PATH`:

```
set MSYS2_PATH_TYPE=inherit
"C:\Program Files\Git\bin\bash.exe" --login -i
```

**Verify before building** — both of these must print something:

```bash
cd /c/Users/benro/WOSRECOMP
which clang-cl && echo "INCLUDE is ${INCLUDE:0:60}..."
```

If `INCLUDE` is empty the environment did not carry over; go back to step 1.

## 2. Set the game root

The extracted disc contents. Not in the repo, and never will be.

```bash
export WOS_GAME_ROOT=/c/Users/benro/Downloads/wos
```

This is per-shell, so it has to be set again in every new terminal. Nothing
will crash without it — the game just fails to open every file, which shows up
as `[file] open FAILED`.

## 3. Build

```bash
git pull
./tools/build_host.sh
```

`build_host.sh` is the normal one and takes seconds. The other two are only
needed sometimes:

| script | when |
|---|---|
| `./tools/build_host.sh` | every time — builds `WoSRecomp.exe` |
| `./tools/build_tools.sh` | only when `xex_info` or the recompiler changed |
| `./tools/build_ppc.sh` | only when the recompiler itself changed — slow, regenerates all 198 translated sources |

Before pushing a change to `WoSRecomp/kernel/`, run:

```bash
tools/check_syntax.sh
```

It compiles every hand-written kernel source with `-fsyntax-only` against a
stand-in for the generated recompiler header, so it needs neither MSVC nor the
XEX and takes about a second. It proves the code parses and type-checks; it
proves nothing about behaviour and is not a substitute for `build_host.sh`.

If the link fails with an **undefined symbol in namespace `wos`**, that is our
own code, not a missing guest import: a new `.cpp` under `kernel/` was not
added to `WOS_SOURCES` in `WoSRecomp/CMakeLists.txt`. That list is explicit,
not a `GLOB`. The build script says so when it detects it.

## 4. Run

```bash
tools/run_game.sh
```

Runs for 45 seconds, stops itself, and copies the interesting lines to the
clipboard. No Ctrl-C, no second `grep`, nothing to time by eye.

```bash
tools/run_game.sh 90                  # longer
tools/run_game.sh 45 '^\[state\]'     # only the state dumps
tools/run_game.sh 45 all              # everything (large)
```

The full log always lands in `logs/` regardless of the filter, so filtering
narrowly never loses anything.

## 5. Ask the disassembler things

```bash
tools/ask.sh --func 0x82A25CE8 --func 0x829F4C80
```

Runs every query and copies **all** the output at once. This matters with one
terminal: `clip` overwrites, so four separate `| clip` commands leave only the
fourth.

Query forms:

| flag | answers |
|---|---|
| `--func <addr>` | disassemble the whole function *containing* an address |
| `--disasm <addr> [n]` | disassemble from exactly that address |
| `--xrefs <addr>` | who calls or references it, with the containing function named |
| `--field <disp> [--stores] [--context N]` | who touches a struct field |

Use `--func` for anything that came out of `--xrefs` or a runtime return
address — those are interior addresses, and `--disasm` starting there hides the
prologue.

## What is *not* in the repo

Deliberately, and permanently:

- `private/` — the XEX and any disc contents
- `WoSRecompLib/ppc/` — the generated translation of the game's code
- `WoSRecomp/kernel/imports_generated.cpp` — generated stubs
- `logs/*.log` — raw run logs

If `private/default.xex` is missing after a reinstall, re-ingest the disc dump
with `tools/import_dump.sh`; the recompile then regenerates `WoSRecompLib/ppc/`
via `tools/build_ppc.sh`.

## Where the state of play lives

- `PROGRESS.md` — where the project is, what is next, and a dated log
- `docs/05-findings-log.md` — every finding with its evidence, **including the
  wrong turns**. The wrong turns are the point: several of them are the same
  mistake in different clothes, and having them written down is what stopped
  the fourth repeat.
