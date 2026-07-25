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

*(Not yet filled in — paste `import_dump.sh` output here once run.)*

- **Region/edition:** TBD (NTSC-U retail / Platinum Hits / PAL / JP)
- **`default.xex` size:** TBD
- **`default.xexp` present:** TBD
- **Directory tree of source dump:**
  ```
  (paste here)
  ```
- **`xex_info` output** (base address, entry point, section layout):
  ```
  (paste here)
  ```

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
