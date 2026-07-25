#!/usr/bin/env bash
# Builds XenonAnalyse, XenonRecomp, and XenosRecomp from the submodules in
# tools/. Requires CMake 3.20+ and Clang 18+ (see docs/01-getting-started.md).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JOBS="${JOBS:-$(command -v nproc >/dev/null && nproc || echo 4)}"

CC_BIN="${CC:-clang-18}"
CXX_BIN="${CXX:-clang++-18}"

if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    echo "warning: '$CC_BIN' not found on PATH; falling back to 'clang'/'clang++'." >&2
    echo "         XenonRecomp requires Clang 18+. Set CC/CXX env vars to override." >&2
    CC_BIN="clang"
    CXX_BIN="clang++"
fi

build_one() {
    local name="$1"
    local src_dir="$SCRIPT_DIR/$name"
    local build_dir="$src_dir/build"

    if [ ! -f "$src_dir/CMakeLists.txt" ]; then
        echo "error: $src_dir is missing/empty — did you run 'git submodule update --init --recursive'?" >&2
        exit 1
    fi

    echo "==> Configuring $name"
    cmake -S "$src_dir" -B "$build_dir" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER="$CC_BIN" \
        -DCMAKE_CXX_COMPILER="$CXX_BIN" \
        2>/dev/null || \
    cmake -S "$src_dir" -B "$build_dir" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER="$CC_BIN" \
        -DCMAKE_CXX_COMPILER="$CXX_BIN"

    echo "==> Building $name"
    cmake --build "$build_dir" --config RelWithDebInfo -j "$JOBS"
}

build_one "XenonRecomp"
build_one "XenosRecomp"

cat <<EOF

Build complete.

  XenonAnalyse : tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse
  XenonRecomp  : tools/XenonRecomp/build/XenonRecomp/XenonRecomp
  XenosRecomp  : tools/XenosRecomp/build/XenosRecomp/XenosRecomp

Note: XenosRecomp (shader recompiler) isn't needed until Phase 4
(docs/04-roadmap.md) — XenonAnalyse/XenonRecomp (the CPU code path) are what
you need first. It builds fine cross-platform via the bundled dxc-bin
submodule (verified on Linux), but if it ever fails to build for you, that's
not a blocker for early phases.

Next: see docs/01-getting-started.md step 4 onward.
EOF
