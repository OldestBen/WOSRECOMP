# Session logs

Per-session working records. One file per session, named `YYYY-MM-DD.md`
(add `-2`, `-3` if multiple sessions land on one day).

## Why this exists (and why it isn't just the PR history)

Git records **what changed and succeeded**. Committed code is, by
definition, the stuff that worked. It cannot record:

- what we tried that **didn't** work, and why
- approaches **ruled out**, and the reasoning that ruled them out
- things we **half-did** and abandoned mid-thought
- context for *why* a decision went the way it did

Those are the expensive things to rediscover. A dead end you already walked
costs the same to walk again if nobody wrote it down. **Negative results are
the highest-value content here**, because they're the only ones git
structurally cannot capture — abandoned work never gets committed.

## The three layers

Read top-down; stop as soon as you know enough.

| Layer | File | Content | Rewritten or appended? |
|---|---|---|---|
| 1. Orientation | [`PROGRESS.md`](../../PROGRESS.md) § Current State | What is true **right now**. Short. | **Rewritten in place** |
| 2. Durable record | [`PROGRESS.md`](../../PROGRESS.md) § Log, [`docs/05-findings-log.md`](../05-findings-log.md) | Dated project changes; WoS technical data with provenance. | **Appended** |
| 3. Detail | `docs/sessions/*.md` (here) | Blow-by-blow of one session, incl. dead ends. | **Appended** (new file) |

**You should never have to read layer 3 to get oriented.** It's for digging
into a specific past episode ("didn't we already try something with the VMX
addresses?"), not for figuring out where the project stands.

## The rules that keep this from rotting

1. **Exactly one file is authoritative for "now"** — `PROGRESS.md` §
   Current State. If anything here contradicts it, that file wins and this
   one is stale history.
2. **Session logs are append-only and never edited after the fact.** They
   are a record of what we believed *at the time*, including things we later
   found out were wrong. Corrections go in a *newer* entry, never by
   rewriting an old one. An edited history you can't trust is worse than no
   history.
3. **Date everything.** A claim with no date can't be aged out.
4. **Keep it light enough that it actually gets done.** A half-maintained
   log system is worse than a small one that's genuinely current, because
   you'll trust it and it'll be stale. If a section has nothing real in it,
   write "nothing" and move on — don't pad it.
5. **No copyrighted content**, same as everywhere else in this repo:
   addresses, names, sizes, and structural metadata only. See
   [`private/README.md`](../../private/README.md).

## Template

```markdown
# Session — YYYY-MM-DD

**Goal:** one line — what this session was trying to achieve.

## Outcome
One-paragraph summary. Did the goal get met?

## What worked
- Brief. The repo/diff has the details; just enough to find them.

## What didn't work / dead ends
- The valuable section. What was tried, what happened, why it was abandoned.
- Include things that *looked* right and weren't.

## Ruled out (with reasoning)
- Approaches deliberately rejected, so nobody re-proposes them cold.

## Open threads
- Anything left mid-air, and what the next step would be.

## Unverified claims
- Anything asserted but not actually tested, flagged so it doesn't
  silently harden into "known good".
```
