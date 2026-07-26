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
    └── 0001-wos-recompiler-fixes.patch
```

## How they're applied

`tools/build_tools.sh` applies every patch under `patches/<Submodule>/` at
the start of each build. The target state is exact: the submodule working
tree must equal **`HEAD` + the patches, nothing more and nothing less**.
Because patch files are generated with `git diff HEAD`, that state is
directly verifiable — the tree's own `git diff HEAD` reproduces them byte
for byte.

| State | Action |
|---|---|
| Tree matches `patches/` exactly | skip |
| Tree is clean | apply all patches |
| Tree is modified but doesn't match | save the diff to `logs/`, reset the affected files to `HEAD`, apply |
| A patch fails on a *clean* tree | **hard error, build stops** |

The last case means the submodule commit has moved and the patch no longer
matches. That's deliberately fatal rather than a warning — quietly building
without our instruction implementations would emit subtly wrong game code
that fails much later and much less obviously.

### Why it isn't decided per patch file

It used to be: reverses cleanly meant applied, applies cleanly meant apply,
neither was fatal. That breaks the moment a patch file is **amended** while
an older version of it is applied — the already-applied hunks block a
forward apply, the new hunks block a reverse apply, and every build dies
with `does not apply and is not already applied` until someone resets the
submodule by hand. This happened for real when the stale-output fix was
folded into `0001`, and it would have recurred on every future amendment.

The reset path saves what it discards to
`logs/<timestamp>-<Submodule>-discarded.diff` (gitignored) before touching
anything. Usually that's just a superseded copy of our own patch, but the
other way a tree reaches this state is a genuine hand edit inside the
submodule — which is real work and must not vanish silently. Only the files
the patches actually touch are reset, so nested submodule pointers under
`thirdparty/` are left alone.

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

## The submodules are permanently modified, and git is told to ignore it

Every build applies these patches, so the submodule working trees are
*always* modified. `.gitmodules` therefore sets `ignore = dirty` on each one,
and `git status` stays quiet.

That is deliberate. Reporting a permanent, intended modification as a change
needing attention is a false alarm, and a status output that always shows
something is one you stop reading.

**It does not weaken the safety net.** `ignore = dirty` only affects how the
*parent* repo reports the submodule. The drift check above runs
`git -C tools/<name> diff HEAD` *inside* the submodule, which the setting
does not touch — verified by confirming a stray edit is still caught with
`ignore = dirty` in place.

**Never commit the submodule pointer.** The tracked commit is upstream's;
`patches/` carries our changes. Equally, don't `git checkout` inside a
submodule to "clean" it — the next build just re-applies.

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

### `XenonRecomp/0001-wos-recompiler-fixes.patch`

Two unrelated changes, kept in one file because the drift check compares the
submodule's whole `git diff HEAD` against the concatenation of these patches
— two patches touching the same file could not reproduce that byte for byte.

**1. Missing instructions.** Implements 15 PPC opcodes the recompiler didn't
handle, which appeared 265 times across *Web of Shadows*'s `.text` and would
otherwise have been silently skipped. See the instruction table in
[`docs/05-findings-log.md`](../docs/05-findings-log.md) for the per-opcode
approach and how it was verified.

**2. Stale output files.** `Recompiler::Recompile` writes its output as
`ppc_recomp.0.cpp … ppc_recomp.N.cpp` and skips rewriting any file whose
contents didn't change, so incremental builds stay cheap. Nothing ever
*deletes* a file, though — so when a run produces fewer chunks than the run
before it (which happens on any config change that reduces the function
count), the surplus files survive holding code for the old boundaries.

That builds fine: a static library is never checked for duplicate symbols.
It only surfaces when an executable is finally linked against it, as
`duplicate symbol: sub_XXXXXXXX` defined in two different chunk files. The
patch removes `ppc_recomp.N.cpp` upward from the current file count once
generation finishes.
