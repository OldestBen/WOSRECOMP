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

Keep summaries even for successful runs: a known-good baseline (how long a
clean build takes, how many warnings are normal) is what makes a later
regression obvious.
