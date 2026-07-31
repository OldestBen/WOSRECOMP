#!/usr/bin/env bash
# Compile every hand-written kernel source for SYNTAX ERRORS ONLY.
#
# The real build needs clang-cl, the MSVC environment, and 198 generated
# translation units derived from the game's XEX — none of which exist on a
# machine that only has the repo. So an obvious mistake in kernel/*.cpp could
# only be found by pushing, pulling on the Windows box, and building. Two
# consecutive builds were broken that way by edits that any compiler would have
# rejected instantly.
#
# This compiles each file with -fsyntax-only against tools/syntax_shim, which
# stands in for the generated recompiler header. It proves the code PARSES and
# TYPE-CHECKS. It proves nothing about behaviour, and it is not a substitute
# for ./tools/build_host.sh.
#
# Usage:  tools/check_syntax.sh [file.cpp ...]      (default: all of kernel/)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KERNEL="$REPO_ROOT/WoSRecomp/kernel"

CXX="${CXX:-g++}"
if ! command -v "$CXX" >/dev/null 2>&1; then
    CXX=clang++
fi
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "error: no g++ or clang++ found. Set CXX to a C++20 compiler." >&2
    exit 1
fi

if [ $# -gt 0 ]; then
    FILES=("$@")
else
    # imports_generated.cpp is generated and gitignored; skip it if absent.
    FILES=()
    for f in "$KERNEL"/*.cpp "$REPO_ROOT/WoSRecomp/gpu"/*.cpp; do
        [ -e "$f" ] || continue
        FILES+=("$f")
    done
fi

fail=0
checked=0
for f in "${FILES[@]}"; do
    name="$(basename "$f")"
    out="$("$CXX" -fsyntax-only -std=c++20 -Wall -Wno-unused-function \
        -I"$SCRIPT_DIR/syntax_shim" -I"$KERNEL" \
        -I"$REPO_ROOT/WoSRecomp/gpu" "$f" 2>&1)"
    if [ $? -eq 0 ]; then
        printf '  ok    %s\n' "$name"
    else
        printf '  FAIL  %s\n' "$name"
        printf '%s\n' "$out" | sed 's/^/        /' | head -30
        fail=$((fail + 1))
    fi
    checked=$((checked + 1))
done

echo
if [ "$fail" -eq 0 ]; then
    echo "$checked file(s) checked, all parse."
else
    echo "$checked file(s) checked, $fail FAILED."
fi
echo "(Syntax only — this says nothing about behaviour. Still run build_host.sh.)"
exit "$fail"
