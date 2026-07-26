#!/usr/bin/env bash
# Runs the XenonAnalyse -> XenonRecomp pipeline against the WoS XEX in
# private/, using WoSRecompLib/config/WoS_config.toml.
#
# Prerequisites:
#   - ./tools/build_tools.sh has been run successfully
#   - private/default.xex exists (your own legally dumped copy — see
#     docs/01-getting-started.md)
#   - WoSRecompLib/config/WoS_config.toml has real addresses filled in
#     (see docs/02-config-guide.md) — the shipped template is placeholders
#     only and will not produce a correct recompile.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

XENON_ANALYSE="$REPO_ROOT/tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse"
XENON_RECOMP="$REPO_ROOT/tools/XenonRecomp/build/XenonRecomp/XenonRecomp"
PPC_CONTEXT="$REPO_ROOT/tools/XenonRecomp/XenonUtils/ppc_context.h"

CONFIG_DIR="$REPO_ROOT/WoSRecompLib/config"
CONFIG_TOML="$CONFIG_DIR/WoS_config.toml"
SWITCH_TABLES="$CONFIG_DIR/WoS_switch_tables.toml"
XEX_PATH="$REPO_ROOT/private/default.xex"

for f in "$XENON_ANALYSE" "$XENON_RECOMP" "$PPC_CONTEXT"; do
    if [ ! -f "$f" ]; then
        echo "error: missing $f — did you run tools/build_tools.sh?" >&2
        exit 1
    fi
done

if [ ! -f "$XEX_PATH" ]; then
    echo "error: $XEX_PATH not found." >&2
    echo "       Dump your own legally owned copy of Web of Shadows and place" >&2
    echo "       default.xex there. See docs/01-getting-started.md." >&2
    exit 1
fi

echo "==> Running XenonAnalyse (jump table detection)"
"$XENON_ANALYSE" "$XEX_PATH" "$SWITCH_TABLES"

echo "==> Running XenonRecomp (PPC -> C++)"
RECOMP_OUT="$(mktemp)"
trap 'rm -f "$RECOMP_OUT"' EXIT
"$XENON_RECOMP" "$CONFIG_TOML" "$PPC_CONTEXT" 2>&1 | tee "$RECOMP_OUT"

# Verify WoSRecompLib/ppc/ holds exactly what this run wrote.
#
# XenonRecomp names its output ppc_recomp.0.cpp .. ppc_recomp.N-1.cpp and,
# before our patch, never deleted anything — so a run producing fewer chunks
# than the one before it left the previous run's tail behind, holding the
# same functions under stale boundaries. Those files compile fine and only
# fail at link time as "duplicate symbol".
#
# Note leftovers are *contiguous*: both runs number from zero, so the strays
# are always the top of an unbroken 0..M sequence. Counting files or looking
# for gaps therefore cannot detect them — an earlier version of this check
# did exactly that and passed while two stale files sat on disk. The only
# reliable comparison is against the count the recompiler itself reports.
PPC_DIR="$REPO_ROOT/WoSRecompLib/ppc"
WROTE="$(sed -n 's/^Wrote \([0-9]\+\) chunk file(s)\.$/\1/p' "$RECOMP_OUT" | tail -1)"
ON_DISK=0
compgen -G "$PPC_DIR/ppc_recomp.*.cpp" >/dev/null && \
    ON_DISK=$(ls "$PPC_DIR"/ppc_recomp.*.cpp | wc -l)

if [ -z "$WROTE" ]; then
    echo >&2
    echo "warning: XenonRecomp did not report a chunk count, so stale output" >&2
    echo "         files cannot be detected. Your XenonRecomp build predates" >&2
    echo "         patches/XenonRecomp/0001-wos-recompiler-fixes.patch — re-run" >&2
    echo "         ./tools/build_tools.sh, then this script again." >&2
    echo >&2
elif [ "$WROTE" -ne "$ON_DISK" ]; then
    echo >&2
    echo "error: this run wrote $WROTE chunk file(s) but $ON_DISK are on disk." >&2
    echo "       WoSRecompLib/ppc/ holds output from more than one recompiler" >&2
    echo "       run; linking would fail with duplicate symbols. Clear it and" >&2
    echo "       re-run this script:" >&2
    echo "         rm -f WoSRecompLib/ppc/ppc_recomp.*.cpp" >&2
    exit 1
else
    echo "==> $ON_DISK chunk file(s) in WoSRecompLib/ppc/ — matches this run"
fi

echo
echo "Done. Generated C++ should be in WoSRecompLib/ppc/ (per out_directory_path"
echo "in $CONFIG_TOML)."
echo
echo "If this is your first run and WoS_config.toml still has 0x00000000"
echo "placeholder addresses, expect this to fail or produce broken output —"
echo "see docs/02-config-guide.md to fill in real addresses first."
