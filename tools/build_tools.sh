#!/usr/bin/env bash
# Builds XenonAnalyse, XenonRecomp, and XenosRecomp from the submodules in
# tools/. Requires CMake 3.20+ and Clang 18+ (see docs/01-getting-started.md).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Core count: nproc on Linux, NUMBER_OF_PROCESSORS on Windows/Git Bash
# (which has no nproc), sysctl on macOS.
detect_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then
        echo "$NUMBER_OF_PROCESSORS"
    elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then
        sysctl -n hw.ncpu
    else
        echo 4
    fi
}
JOBS="${JOBS:-$(detect_jobs)}"

# Upstream names the binaries clang-18/clang++-18 on Debian/Ubuntu, but they
# are plain clang/clang++ on Windows and macOS.
CC_BIN="${CC:-clang-18}"
CXX_BIN="${CXX:-clang++-18}"

if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    CC_BIN="clang"
    CXX_BIN="clang++"
fi

if ! command -v "$CXX_BIN" >/dev/null 2>&1; then
    echo "error: no Clang found on PATH (tried clang++-18 and clang++)." >&2
    echo "       XenonRecomp requires Clang 18+. Install it, or set CC/CXX." >&2
    exit 1
fi

# Verify the version, rather than silently building with whatever was found.
# An old Clang produces confusing failures much later, so fail loudly here.
CLANG_MAJOR="$("$CXX_BIN" --version 2>/dev/null \
    | grep -oiE 'clang version [0-9]+' | grep -oE '[0-9]+' | head -1 || true)"

if [ -z "$CLANG_MAJOR" ]; then
    echo "warning: could not parse a version from '$CXX_BIN --version'." >&2
    echo "         Proceeding, but XenonRecomp requires Clang 18+." >&2
elif [ "$CLANG_MAJOR" -lt 18 ]; then
    echo "error: '$CXX_BIN' is Clang $CLANG_MAJOR, but Clang 18+ is required." >&2
    echo "       XenonRecomp relies on Clang-specific behaviour; older versions" >&2
    echo "       fail in confusing ways. Install Clang 18+, or set CC/CXX to it." >&2
    exit 1
fi

echo "==> Using $CXX_BIN (Clang ${CLANG_MAJOR:-unknown}), $JOBS parallel jobs"

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
build_one "xex_info"
build_one "extract-xiso"

cat <<EOF

Build complete.

  XenonAnalyse  : tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse
  XenonRecomp   : tools/XenonRecomp/build/XenonRecomp/XenonRecomp
  XenosRecomp   : tools/XenosRecomp/build/XenosRecomp/XenosRecomp
  xex_info      : tools/xex_info/build/xex_info
  extract-xiso  : tools/extract-xiso/build/extract-xiso

Note: XenosRecomp (shader recompiler) isn't needed until Phase 4
(docs/04-roadmap.md) — XenonAnalyse/XenonRecomp (the CPU code path) are what
you need first. It builds fine cross-platform via the bundled dxc-bin
submodule (verified on Linux), but if it ever fails to build for you, that's
not a blocker for early phases.

xex_info and extract-xiso are our own additions (not upstream XenonRecomp) —
see tools/import_dump.sh, which uses both to ingest a raw disc dump into
private/.

Next: see docs/01-getting-started.md step 4 onward.
EOF
