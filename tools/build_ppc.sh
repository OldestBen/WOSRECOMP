#!/usr/bin/env bash
# Compiles the recompiled PowerPC code (WoSRecompLib/ppc/) into a static
# library.
#
# Usage:
#   tools/build_ppc.sh            # auto-pick a memory-safe job count
#   JOBS=8 tools/build_ppc.sh     # override
#   CLEAN=1 tools/build_ppc.sh    # wipe the build tree first
#
# Why this exists rather than just `cmake --build`: the generated code is
# roughly 2.4 million PPC instructions spread over many large translation
# units, and each parallel Clang process holds its whole TU in memory. On a
# 32-thread / 32 GB machine, a naive -j32 can exceed RAM and start swapping,
# which is dramatically SLOWER than using fewer jobs — and can OOM outright.
# So parallelism is capped by available memory, not core count.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/WoSRecompLib"
BUILD_DIR="$SRC_DIR/build"

if [ ! -d "$SRC_DIR/ppc" ] || [ -z "$(ls -A "$SRC_DIR/ppc"/*.cpp 2>/dev/null)" ]; then
    echo "error: no generated sources in WoSRecompLib/ppc/." >&2
    echo "       Run the recompile first:" >&2
    echo "         tools/XenonRecomp/build/XenonRecomp/XenonRecomp \\" >&2
    echo "             WoSRecompLib/config/WoS_config.toml \\" >&2
    echo "             tools/XenonRecomp/XenonUtils/ppc_context.h" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Job count: min(cores, memory budget), because memory is the real limit here.
# ---------------------------------------------------------------------------
detect_cores() {
    if command -v nproc >/dev/null 2>&1; then nproc
    elif [ -n "${NUMBER_OF_PROCESSORS:-}" ]; then echo "$NUMBER_OF_PROCESSORS"
    elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.ncpu >/dev/null 2>&1; then sysctl -n hw.ncpu
    else echo 4
    fi
}

# Total RAM in GiB, best effort. Returns empty if it can't be determined.
detect_ram_gib() {
    if [ -r /proc/meminfo ]; then
        awk '/^MemTotal:/ { printf "%d", $2 / 1048576 }' /proc/meminfo
    elif command -v sysctl >/dev/null 2>&1 && sysctl -n hw.memsize >/dev/null 2>&1; then
        echo $(( $(sysctl -n hw.memsize) / 1073741824 ))
    elif command -v wmic >/dev/null 2>&1; then
        # Git Bash on Windows. TotalVisibleMemorySize is in KiB.
        wmic OS get TotalVisibleMemorySize 2>/dev/null \
            | tr -d '\r' | awk 'NR==2 { printf "%d", $1 / 1048576 }'
    else
        echo ""
    fi
}

CORES="$(detect_cores)"
RAM_GIB="$(detect_ram_gib)"

if [ -n "${JOBS:-}" ]; then
    echo "==> Jobs: $JOBS (from JOBS env var)"
elif [ -n "$RAM_GIB" ] && [ "$RAM_GIB" -gt 0 ] 2>/dev/null; then
    # ~2 GiB per concurrent Clang process on these translation units, and
    # leave a couple of GiB for the OS. Never below 2 jobs.
    MEM_JOBS=$(( (RAM_GIB - 2) / 2 ))
    [ "$MEM_JOBS" -lt 2 ] && MEM_JOBS=2
    JOBS=$(( CORES < MEM_JOBS ? CORES : MEM_JOBS ))
    echo "==> Jobs: $JOBS  (cores: $CORES, RAM: ${RAM_GIB} GiB -> memory cap ${MEM_JOBS})"
    if [ "$JOBS" -lt "$CORES" ]; then
        echo "    Capped below core count on purpose: swapping is slower than"
        echo "    fewer jobs. Override with JOBS=<n> if you have headroom."
    fi
else
    JOBS=$(( CORES / 2 )); [ "$JOBS" -lt 2 ] && JOBS=2
    echo "==> Jobs: $JOBS  (cores: $CORES, RAM unknown — using half the cores)"
    echo "    Override with JOBS=<n> once you know how much headroom you have."
fi

if [ "${CLEAN:-0}" != "0" ] && [ -d "$BUILD_DIR" ]; then
    echo "==> CLEAN set; wiping $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Reuse the compiler selection logic already proven by build_tools.sh.
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

GENERATOR_ARGS=()
command -v ninja >/dev/null 2>&1 && GENERATOR_ARGS=(-G Ninja)

echo "==> Configuring WoSRecompLib ($CXX_BIN)"
# CXX only — the generated code is all C++, and passing CMAKE_C_COMPILER to a
# CXX-only project just produces an "unused variable" warning.
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    ${GENERATOR_ARGS[@]+"${GENERATOR_ARGS[@]}"} \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER="$CXX_BIN"

echo "==> Building (this is the long one — expect minutes, not seconds)"
START="$(date +%s)"
cmake --build "$BUILD_DIR" --config RelWithDebInfo -j "$JOBS"
END="$(date +%s)"

echo
echo "Built in $(( END - START ))s with $JOBS job(s)."
find "$BUILD_DIR" -name 'libWoSRecompLib.*' -o -name 'WoSRecompLib.lib' 2>/dev/null \
    | while read -r lib; do
        echo "  $(du -h "$lib" 2>/dev/null | cut -f1)  ${lib#$REPO_ROOT/}"
      done
