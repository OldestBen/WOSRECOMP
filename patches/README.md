# patches/

Changes we make to the vendored submodules under `tools/`.

## Why these exist

`tools/XenonRecomp` and `tools/XenosRecomp` are submodules pointing at
upstream repositories we cannot push to. Any edit made directly inside them
lives only in the local checkout: it is invisible to git in this repo, and a
fresh clone silently loses it. **These patch files are the version-controlled
source of truth for those edits.**

Layout mirrors the submodule name:

```
patches/
└── XenonRecomp/
    └── 0001-implement-missing-instructions.patch
```

## How they're applied

`tools/build_tools.sh` applies every patch under `patches/<Submodule>/` at
the start of each build. It's idempotent:

| State | Detected by | Action |
|---|---|---|
| Already applied | `git apply --reverse --check` succeeds | skip |
| Not yet applied | `git apply --check` succeeds | apply |
| Neither | both checks fail | **hard error, build stops** |

The third case means the submodule commit has moved and the patch no longer
matches. That's deliberately fatal rather than a warning — quietly building
without our instruction implementations would emit subtly wrong game code
that fails much later and much less obviously.

## Drift detection

`apply_patches` only proves our patches *are* applied — it cannot tell
whether someone also hand-edited the submodule afterwards. Such an edit is
invisible to git in this repo and vanishes on a fresh clone, which is the
exact failure this directory exists to prevent.

So `build_tools.sh` also compares the submodule's live `git diff HEAD`
against the concatenated patch files. They're generated the same way, so a
tree that is precisely HEAD+patches reproduces them byte for byte. Any
mismatch prints a warning naming the submodule and the command to fold the
edit back in. It warns rather than fails, because a legitimate
work-in-progress edit shouldn't block a build.

**If you edit a submodule, refresh its patch before committing.**

## Consequence: the submodule always looks "dirty"

After a build, `git status` will show:

```
     m tools/XenonRecomp
```

(`.gitmodules` sets `ignore = untracked` on each submodule so build output
inside them stays out of `git status`; **tracked-content changes are still
shown deliberately**, since that's where patch drift would surface.)

That is expected and correct. **Do not commit the submodule pointer** — the
tracked commit is still upstream's, and the patch is what carries our
changes. Do not `git checkout` inside the submodule to "clean" it either;
the next build simply re-applies.

## Refreshing a patch after a submodule update

```bash
cd tools/XenonRecomp
git diff HEAD -- <changed files> > ../../patches/XenonRecomp/0001-*.patch
```

Then verify it applies to a clean tree:

```bash
git stash push -- <changed files>
git apply --check ../../patches/XenonRecomp/0001-*.patch   # must succeed
git stash pop
```

## Current patches

### `XenonRecomp/0001-implement-missing-instructions.patch`

Implements 15 PPC opcodes the recompiler didn't handle, which appeared 265
times across *Web of Shadows*'s `.text` and would otherwise have been
silently skipped. See the instruction table in
[`docs/05-findings-log.md`](../docs/05-findings-log.md) for the per-opcode
approach and how it was verified.
