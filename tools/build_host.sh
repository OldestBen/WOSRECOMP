#!/usr/bin/env bash
# Builds the host harness (WoSRecomp/) and links it against the recompiled
# game code.
#
#   tools/build_host.sh          # memory-aware job count
#   JOBS=8 tools/build_host.sh
#   CLEAN=1 tools/build_host.sh
#
# THIS IS EXPECTED TO FAIL TO LINK, at least initially. Every undefined symbol
# is a kernel/OS import Web of Shadows calls that we haven't implemented. The
# script collects those symbols and prints them as a to-do list rather than
# leaving you to read raw linker output — that list, derived from the game
# itself, is what the runtime has to provide.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/WoSRecomp"
BUILD_DIR="$SRC_DIR/build"

if ! ls "$REPO_ROOT/WoSRecompLib/ppc"/*.cpp >/dev/null 2>&1; then
    echo "error: no generated sources in WoSRecompLib/ppc/." >&2
    echo "       Run the recompile first (see tools/build_ppc.sh)." >&2
    exit 1
fi

detect_cores() {
    if command -v nproc >/dev/null 2>&1; then nproc
    elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then echo "$NUMBER_OF_PROCESSORS"
    elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then sysctl -n hw.ncpu
    else echo 4
    fi
}
detect_ram_gib() {
    if [ -r /proc/meminfo ]; then awk '/^MemTotal:/ { printf "%d", $2 / 1048576 }' /proc/meminfo
    elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.memsize >/dev/null 2>&1; then echo $(( $(sysctl -n hw.memsize) / 1073741824 ))
    elif command -v wmic >/dev/null 2>&1; then wmic OS get TotalVisibleMemorySize 2>/dev/null | tr -d '\r' | awk 'NR==2 { printf "%d", $1 / 1048576 }'
    else echo ""
    fi
}

CORES="$(detect_cores)"; RAM_GIB="$(detect_ram_gib)"
if [ -z "${JOBS:-}" ]; then
    if [ -n "$RAM_GIB" ] && [ "$RAM_GIB" -gt 0 ] 2>/dev/null; then
        MEM_JOBS=$(( (RAM_GIB - 2) / 2 )); [ "$MEM_JOBS" -lt 2 ] && MEM_JOBS=2
        JOBS=$(( CORES < MEM_JOBS ? CORES : MEM_JOBS ))
    else
        JOBS=$(( CORES / 2 )); [ "$JOBS" -lt 2 ] && JOBS=2
    fi
fi
echo "==> Jobs: $JOBS"

if [ "${CLEAN:-0}" != "0" ] && [ -d "$BUILD_DIR" ]; then
    echo "==> CLEAN set; wiping $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;;
    *)                    IS_WINDOWS=0 ;;
esac

if [ -n "${CC:-}" ] || [ -n "${CXX:-}" ]; then
    CC_BIN="${CC:-${CXX:-clang}}"; CXX_BIN="${CXX:-${CC:-clang++}}"
elif [ "$IS_WINDOWS" -eq 1 ] && command -v clang-cl >/dev/null 2>&1; then
    CC_BIN="clang-cl"; CXX_BIN="clang-cl"
elif command -v clang-18 >/dev/null 2>&1; then
    CC_BIN="clang-18"; CXX_BIN="clang++-18"
else
    CC_BIN="clang"; CXX_BIN="clang++"
fi

TOOLCHAIN_ARGS=()
if [ "$IS_WINDOWS" -eq 1 ] && [ "$(basename "$CC_BIN")" = "clang-cl" ]; then
    _cl="$(command -v clang-cl 2>/dev/null || true)"
    if [ -n "$_cl" ]; then
        _ar="$(dirname "$_cl")/llvm-lib.exe"
        if [ -x "$_ar" ]; then
            command -v cygpath >/dev/null 2>&1 && _ar="$(cygpath -m "$_ar")"
            TOOLCHAIN_ARGS+=(-DCMAKE_AR="$_ar")
        fi
    fi
fi

GENERATOR_ARGS=()
command -v ninja >/dev/null 2>&1 && GENERATOR_ARGS=(-G Ninja)

echo "==> Configuring WoSRecomp ($CXX_BIN)"
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$CC_BIN" \
    -DCMAKE_CXX_COMPILER="$CXX_BIN" \
    ${TOOLCHAIN_ARGS[@]+"${TOOLCHAIN_ARGS[@]}"} || exit 1

echo "==> Building"
# Delete the previous executable first.
#
# Otherwise a failed build leaves the old binary sitting there, and anyone
# running the build and the game as one chained command silently gets the
# *previous* build's behaviour. That happened: a compile error scrolled past
# and the run that followed looked like a normal result from new code.
find "$BUILD_DIR" -name 'WoSRecomp' -o -name 'WoSRecomp.exe' 2>/dev/null \
    | while read -r stale; do rm -f "$stale"; done

BUILD_LOG="$(mktemp)"
cmake --build "$BUILD_DIR" --config RelWithDebInfo -j "$JOBS" 2>&1 | tee "$BUILD_LOG"
RC="${PIPESTATUS[0]}"

if [ "$RC" -eq 0 ]; then
    echo
    echo "Linked successfully."
    find "$BUILD_DIR" -name 'WoSRecomp' -o -name 'WoSRecomp.exe' 2>/dev/null \
        | while read -r exe; do echo "  ${exe#$REPO_ROOT/}"; done
    rm -f "$BUILD_LOG"
    exit 0
fi

# ---------------------------------------------------------------------------
# Link failed — extract the unresolved guest imports.
#
# Both linkers name the symbol in their own way:
#   lld-link : error: undefined symbol: __imp__XamFoo
#   ld       : undefined reference to `__imp__XamFoo'
# ---------------------------------------------------------------------------
echo
echo "=========================================================="
echo " Build failed. Extracting undefined symbols..."
echo "=========================================================="

# The name pattern used to be [A-Za-z_][A-Za-z0-9_]*, which is right for a C
# import (__imp__XamFoo) and wrong for everything else. lld-link demangles C++,
# so a missing kernel function comes out as
#
#   undefined symbol: void __cdecl wos::ReportLoaderState(unsigned char *)
#
# and the old pattern reported that as the symbol "void" — which named nothing,
# pointed nowhere, and made a one-line CMakeLists omission look like a mystery.
# Take the rest of the line instead; it is always exactly the symbol.
grep -oE "undefined symbol: .*|undefined reference to \`[^']*'" "$BUILD_LOG" \
    | sed -E "s/^undefined symbol: //; s/^undefined reference to \`//; s/'$//" \
    | sed -E 's/[[:space:]]+$//' \
    | sort -u > "$BUILD_LOG.syms"

COUNT="$(wc -l < "$BUILD_LOG.syms" | tr -d ' ')"

if [ "$COUNT" -eq 0 ]; then
    echo "No undefined symbols found — the failure is something else."
    echo "Last 30 lines:"
    tail -30 "$BUILD_LOG"
else
    echo
    echo "$COUNT distinct undefined symbol(s) — this is the runtime to-do list:"
    echo
    sed 's/^/    /' "$BUILD_LOG.syms"
    echo
    echo "Saved to: ${BUILD_LOG}.syms"
    echo
    if grep -q "wos::" "$BUILD_LOG.syms"; then
        echo "NOTE: at least one of these is in namespace wos, so it is OUR code,"
        echo "not a guest import. The usual cause is a new .cpp under kernel/ that"
        echo "was never added to WOS_SOURCES in WoSRecomp/CMakeLists.txt — that"
        echo "list is explicit, not a GLOB."
    else
        echo "Each is a guest import the game calls. Implementing them is what"
        echo "WoSRecomp/kernel/ and WoSRecomp/os/ are for."
    fi
fi

exit "$RC"
