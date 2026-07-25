# logs/

Output from [`tools/run_logged.sh`](../tools/run_logged.sh).

```bash
tools/run_logged.sh <label> -- <command...>
```

Each run produces two files:

| File | Committed? | Purpose |
|---|---|---|
| `YYYYMMDD-HHMMSS-<label>.log` | **No** (gitignored) | Full raw output. Can be enormous. |
| `YYYYMMDD-HHMMSS-<label>.summary.md` | **Yes** | Compact summary — this is the one to share. |

## Why summaries

A failing recompile can emit tens of thousands of error lines that are
really a handful of *distinct* problems repeated across hundreds of
generated translation units. The summary deduplicates them (normalising
away file/line prefixes and hex addresses), so:

```
    200 error: use of undeclared identifier 'PPCFuncMapping'
     50 error: no member named 'vmx128' in 'PPCContext' at 0xADDR
```

replaces 250 lines — and tells you there are two bugs, not 250.

Summaries also record exit code, duration, host, and the last 30 lines
(where the fatal message usually is).

## Workflow

1. Run anything meaningful through `run_logged.sh` rather than bare.
2. Commit the `.summary.md`.
3. Raw `.log` stays on your machine. If a summary isn't enough to diagnose
   something, say so — we can pull specific sections out of the raw log
   deliberately rather than dumping the whole thing.

## Successful runs count too

`run_logged.sh` summarises **every** run, pass or fail — it doesn't only
trigger on errors. That's deliberate. Successful runs are what give you:

- **Baselines.** How long a clean build takes, how many warnings are
  normal. Without a recorded "good" run, you can't tell later whether
  something regressed or was always like that.
- **Provenance.** A successful `XenonAnalyse` summary records exactly which
  binary, which command, on what host, at what time — that's the audit
  trail behind whatever addresses end up in
  [`WoS_config.toml`](../WoSRecompLib/config/WoS_config.toml).
- **Reproducibility.** Six months on, "how did we invoke this?" is
  answerable.
- **Drift.** Slowly climbing warning counts are an early signal.

## Which summaries to commit

Generating a summary is automatic and free. **Committing** it is a
judgement call — otherwise the repo fills with noise from routine rebuilds.

Commit a summary when the run **means something**:

- a first success at anything (new baseline)
- a failure you're actively diagnosing
- a state change — config edited, new addresses, different input
- anything you're about to ask about or reference later

Skip committing the dozen near-identical rebuilds in between. If in doubt,
commit it — they're a few KB each, and a missing baseline costs more than
a redundant file.
