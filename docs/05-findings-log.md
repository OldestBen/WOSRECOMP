# Findings Log — Web of Shadows binary data

This is the persistent record of everything discovered about *Web of
Shadows*'s actual XEX — paste `import_dump.sh`/`xex_info` output and any
addresses found here so they survive context resets/new sessions, instead of
living only in chat history.

**What goes here vs. elsewhere:**
- **This file** — human-readable findings *with provenance* (what we found,
  how we found it, why). The working record.
- [`WoSRecompLib/config/WoS_config.toml`](../WoSRecompLib/config/WoS_config.toml) —
  the machine-readable result. XenonRecomp reads this one; it doesn't explain
  itself.
- [`PROGRESS.md`](../PROGRESS.md) — project/tooling changelog (what code
  changed and why), not game data.

Nothing here should ever be copyrighted content (no disassembly dumps, no
code, no asset data) — only addresses, sizes, names, and structural metadata,
same rule as everywhere else in this repo. See [`private/README.md`](../private/README.md).

---

## Dump info

Ingested 2026-07-26 via `tools/import_dump.sh` on the Windows workstation.
First real run of that script — previously only exercised against synthetic
fixtures.

- **Region/edition:** TBD (NTSC-U retail / Platinum Hits / PAL / JP)
- **`default.xex` size:** **14,528,512 bytes** (0xDDB000)
- **`default.xexp` present:** **No.** No title update in this dump, so the
  config's `[main]` section should omit `patch_file_path` /
  `patched_file_path` and point `file_path` straight at the retail XEX.
  Worth revisiting later — if a TU exists for WoS it may fix retail bugs.
- **Layout:** flat XISO-style — `default.xex` at the dump root, no nested
  GOD-style content folders.
- **`xex_info` output** (base address, entry point, section layout):
  ```
  (pending — not yet captured)
  ```

### Notable non-executable files

Not needed for recompilation, but they map the engine's shape and will
matter for the runtime's filesystem/asset layer later:

| File | Size | Notes |
|---|---|---|
| `amalga.toc` | 54,316 | Almost certainly the engine's master archive/table-of-contents. "Amalga" is likely the internal engine name. Asset loading starts here — first thing to reverse when building the I/O layer. |
| `game_shared.ini` | 45 | Tiny config; trivial to inspect. |
| `movies/*.bik` | ~1.3 GB total | **Bink Video** (RAD Game Tools). Runtime will need Bink playback or a substitute. `credits.bik` alone is 652 MB. |
| `$SystemUpdate/` | 7,262,208 | Standard Xbox 360 disc system-update payload. Irrelevant to us. |

`shaba.bik` and `treyarch.bik` confirm Shaba Games (developer, now defunct)
and Treyarch involvement — consistent with the "no public RE work on this
engine" assessment in the odds discussion.

## Register save/restore function addresses

Byte-pattern search targets are documented in
[`docs/02-config-guide.md`](02-config-guide.md#register-saverestore-functions--required-wos-specific).
Fill in as found; each row should also get written into
`WoS_config.toml`.

| Field | Address | How found | Notes |
|---|---|---|---|
| `restgprlr_14_address` | TBD | | |
| `savegprlr_14_address` | TBD | | |
| `restfpr_14_address` | TBD | | |
| `savefpr_14_address` | TBD | | |
| `restvmx_14_address` | TBD | | |
| `savevmx_14_address` | TBD | | |
| `restvmx_64_address` | TBD | | |
| `savevmx_64_address` | TBD | | |

## setjmp / longjmp

| Field | Address | How found |
|---|---|---|
| `longjmp_address` | TBD | |
| `setjmp_address` | TBD | |

## Explicit function boundary overrides

Running log of `functions = [...]` entries added to the config and *why*
(what error/crash led to adding each one). Append, don't rewrite — this is
a history, and the reasoning matters more than the raw TOML (which already
lives in the config file itself).

*(none yet)*

## Invalid instruction skips

Running log of `invalid_instructions = [...]` entries and what was found at
each address (padding, exception handler data, etc.).

*(none yet)*

## Mid-asm hooks

Running log of hooks added, what they're for, and their runtime
implementation status (stubbed / implemented / working).

*(none yet)*

## Open questions / blockers

*(none yet)*
