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
"$XENON_RECOMP" "$CONFIG_TOML" "$PPC_CONTEXT"

echo
echo "Done. Generated C++ should be in WoSRecompLib/ppc/ (per out_directory_path"
echo "in $CONFIG_TOML)."
echo
echo "If this is your first run and WoS_config.toml still has 0x00000000"
echo "placeholder addresses, expect this to fail or produce broken output —"
echo "see docs/02-config-guide.md to fill in real addresses first."
