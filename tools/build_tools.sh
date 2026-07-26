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

case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
    *)                    IS_WINDOWS=0 ;;
esac

# Compiler selection:
#   - explicit CC/CXX always wins
#   - on Windows prefer clang-cl (the MSVC-style driver). XenonRecomp's
#     CMakeLists sets CMAKE_MSVC_RUNTIME_LIBRARY and, under CMP0141,
#     CMAKE_MSVC_DEBUG_INFORMATION_FORMAT. Those are MSVC-ABI abstractions
#     that the GNU-style clang.exe driver does not implement, so configuring
#     with plain clang.exe fails with:
#       MSVC_DEBUG_INFORMATION_FORMAT value 'ProgramDatabase' not known
#     clang-cl is the same compiler with an MSVC-compatible driver, and is
#     what this codebase expects on Windows.
#   - clang-18/clang++-18 is the Debian/Ubuntu naming convention
#   - plain clang/clang++ otherwise (macOS, other distros)
if [ -n "${CC:-}" ] || [ -n "${CXX:-}" ]; then
    CC_BIN="${CC:-${CXX:-clang}}"
    CXX_BIN="${CXX:-${CC:-clang++}}"
elif [ "$IS_WINDOWS" -eq 1 ] && command -v clang-cl >/dev/null 2>&1; then
    # clang-cl drives both C and C++; CMake distinguishes by source language.
    CC_BIN="clang-cl"
    CXX_BIN="clang-cl"
elif command -v clang-18 >/dev/null 2>&1; then
    CC_BIN="clang-18"
    CXX_BIN="clang++-18"
else
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

# When compiling with clang-cl, CMake emits the MSVC lib.exe-style archive
# rule:
#     <CMAKE_AR> /nologo /out:<TARGET> <OBJECTS>
# but its CMAKE_AR autodetection can land on llvm-ar.exe, the GNU-style
# archiver, which only understands '-'-prefixed flags and dies with:
#     llvm-ar.exe: error: unknown option /
# llvm-lib.exe is the lib.exe-compatible archiver and ships in the same
# LLVM bin directory, so pin CMAKE_AR to it explicitly.
TOOLCHAIN_ARGS=()
if [ "$IS_WINDOWS" -eq 1 ] && [ "$(basename "$CC_BIN")" = "clang-cl" ]; then
    _clang_cl="$(command -v clang-cl 2>/dev/null || true)"
    if [ -n "$_clang_cl" ]; then
        _llvm_bin="$(dirname "$_clang_cl")"
        for _ar in "$_llvm_bin/llvm-lib.exe" "$_llvm_bin/llvm-lib"; do
            [ -x "$_ar" ] || continue
            # CMake wants a native path; cygpath -m yields C:/... form.
            if command -v cygpath >/dev/null 2>&1; then
                _ar="$(cygpath -m "$_ar")"
            fi
            TOOLCHAIN_ARGS+=(-DCMAKE_AR="$_ar")
            echo "==> Archiver: $_ar"
            break
        done
        if [ "${#TOOLCHAIN_ARGS[@]}" -eq 0 ]; then
            echo "warning: clang-cl in use but llvm-lib not found next to it." >&2
            echo "         If archiving fails with \"unknown option /\", pass" >&2
            echo "         CMAKE_ARGS=\"-DCMAKE_AR=<path to llvm-lib.exe or lib.exe>\"." >&2
        fi
    fi
fi

# Choose the generator once, up front. This previously attempted Ninja with
# stderr suppressed and retried with the default generator on failure —
# which hid the real CMake error and made every configure appear to run
# twice. Checking for ninja explicitly keeps the output honest.
if command -v ninja >/dev/null 2>&1; then
    GENERATOR_ARGS=(-G Ninja)
else
    GENERATOR_ARGS=()
    echo "note: ninja not found; using CMake's default generator." >&2
fi

# Extra flags for every configure, e.g.
#   CMAKE_ARGS="-DCMAKE_POLICY_DEFAULT_CMP0141=OLD" ./tools/build_tools.sh
read -ra CMAKE_EXTRA_ARGS <<< "${CMAKE_ARGS:-}"

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
        ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_C_COMPILER="$CC_BIN" \
        -DCMAKE_CXX_COMPILER="$CXX_BIN" \
        ${TOOLCHAIN_ARGS[@]+"${TOOLCHAIN_ARGS[@]}"} \
        ${CMAKE_EXTRA_ARGS[@]+"${CMAKE_EXTRA_ARGS[@]}"}

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
