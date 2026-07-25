# Docker dev environment

An alternative to installing Homebrew/Clang 18 natively — builds and runs
the toolchain inside a container instead. Useful if you don't have (or
don't want) Homebrew on macOS, or just want the environment fully
disposable/isolated.

This mirrors the **exact** combination already verified to build this
project cleanly: Ubuntu 24.04 + Clang 18.1.3 + CMake + Ninja. The
containerized build itself hasn't been run end-to-end (this was written
from a sandbox that has the Docker CLI but no running daemon) — but since
it's the identical package set already proven to work natively, confidence
is high. Report back if anything about the container build itself goes
wrong.

## 1. Build the image

From the repo root:

```bash
docker build -t wosrecomp-dev -f tools/docker/Dockerfile tools/docker
```

## 2. Pick a workspace folder on your Mac

This is where the cloned repo will live — outside the container, so it
persists across container restarts and you can inspect it normally in
Finder/Terminal. Doesn't need to exist yet:

```bash
mkdir -p ~/wosrecomp-work
```

## 3. Run the container

Mount your workspace folder and your extracted `wos` dump (read-only —
this container never needs to modify your original dump):

```bash
docker run -it --rm \
    -v ~/wosrecomp-work:/workspace \
    -v /path/to/your/wos:/wos:ro \
    wosrecomp-dev
```

You're now in a bash shell inside the container, in `/workspace` (mapped to
`~/wosrecomp-work` on your Mac). Your `wos` dump is visible read-only at
`/wos`.

## 4. First time only: clone and build

```bash
git clone https://github.com/OldestBen/WOSRECOMP.git .
git checkout claude/spiderman-web-shadows-recompile-ad7kn5
git submodule update --init --recursive
./tools/build_tools.sh
```

## 5. Ingest your dump

```bash
tools/import_dump.sh /wos
```

Same behavior as documented in
[`docs/01-getting-started.md`](../../docs/01-getting-started.md) — finds
`default.xex`/`default.xexp` regardless of layout, copies them into
`private/` (which lives in `~/wosrecomp-work/private/` on your Mac, not
just inside the ephemeral container), and prints a directory tree +
`xex_info` output safe to paste back into chat.

## Resuming later

Since `/workspace` is a bind mount, everything you cloned/built survives
after the container exits. Next time, skip step 4's `git clone` — just
re-run step 3 (`docker run ...`) and you'll land back in the same
`/workspace` with the repo, submodules, and build output already there.
Only re-run `./tools/build_tools.sh` if you've pulled new commits that
touch the toolchain.

## Notes

- Docker Desktop picks the right image architecture automatically (arm64 on
  Apple Silicon, x86_64 on Intel) — no `--platform` flag needed for the
  official `ubuntu:24.04` base image.
- Files created inside the container will typically show up owned by your
  normal Mac user account thanks to Docker Desktop's filesystem bridging;
  if you hit permission errors editing files from outside the container,
  that's the thing to look into.
- To remove everything and start over: delete `~/wosrecomp-work` and
  `docker rmi wosrecomp-dev`. Nothing about this setup touches anything
  outside those two locations.
