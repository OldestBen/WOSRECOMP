#!/usr/bin/env bash
# Ingests a raw Xbox 360 disc dump into private/ and reports back exactly
# the metadata needed to start docs/02-config-guide.md — without printing
# or committing any copyrighted game content.
#
# Usage:
#   tools/import_dump.sh <path-to-extracted-folder-or-.iso>
#
# What it does:
#   1. If given a .iso/.god file, runs extract-xiso to unpack it first.
#   2. Recursively finds default.xex / default.xexp (any case) under the
#      given path, handling both flat (XISO-style) and nested (GOD-style)
#      dump layouts.
#   3. Copies them into private/ (never moves — your original dump is left
#      untouched).
#   4. Prints a directory tree of the source dump (filenames + sizes only).
#   5. Runs xex_info against the copied default.xex (and .xexp, if present)
#      to print base address / entry point / section layout.
#
# Nothing this script prints or writes to private/ is ever committed —
# private/ is gitignored. The directory-tree and xex_info output are safe
# to paste back into chat: filenames, sizes, and structural addresses, no
# code or asset content.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PRIVATE_DIR="$REPO_ROOT/private"

XEX_INFO_BIN="$SCRIPT_DIR/xex_info/build/xex_info"
EXTRACT_XISO_BIN="$SCRIPT_DIR/extract-xiso/build/extract-xiso"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <path-to-extracted-folder-or-.iso>" >&2
    exit 1
fi

SRC="$1"
if [ ! -e "$SRC" ]; then
    echo "error: '$SRC' does not exist." >&2
    exit 1
fi

# --- Step 1: if given a disc image, extract it first -----------------------
if [ -f "$SRC" ]; then
    case "$SRC" in
        *.iso|*.ISO|*.god|*.GOD)
            if [ ! -x "$EXTRACT_XISO_BIN" ]; then
                echo "error: '$SRC' looks like a disc image but extract-xiso isn't built." >&2
                echo "       Run ./tools/build_tools.sh first." >&2
                exit 1
            fi
            EXTRACT_DEST="$REPO_ROOT/private/_extracted"
            mkdir -p "$EXTRACT_DEST"
            echo "==> Extracting disc image with extract-xiso..."
            "$EXTRACT_XISO_BIN" -x "$SRC" -d "$EXTRACT_DEST"
            SRC="$EXTRACT_DEST"
            ;;
        *)
            echo "error: '$SRC' is a file but not a .iso/.god — point this at either" >&2
            echo "       a disc image or an already-extracted folder." >&2
            exit 1
            ;;
    esac
fi

# --- Step 2: locate default.xex / default.xexp -----------------------------
echo "==> Scanning '$SRC' for default.xex / default.xexp..."

mapfile -t XEX_CANDIDATES < <(find "$SRC" -iname 'default.xex' -type f | sort)
mapfile -t XEXP_CANDIDATES < <(find "$SRC" -iname 'default.xexp' -type f | sort)

if [ "${#XEX_CANDIDATES[@]}" -eq 0 ]; then
    echo "error: no default.xex found under '$SRC'." >&2
    echo "       Directory listing for reference:" >&2
    find "$SRC" -maxdepth 4 | sed 's/^/    /' >&2
    exit 1
fi

if [ "${#XEX_CANDIDATES[@]}" -gt 1 ]; then
    echo "warning: multiple default.xex files found — using the first, but check the others:" >&2
    printf '    %s\n' "${XEX_CANDIDATES[@]}" >&2
fi

FOUND_XEX="${XEX_CANDIDATES[0]}"
FOUND_XEXP=""
if [ "${#XEXP_CANDIDATES[@]}" -gt 0 ]; then
    FOUND_XEXP="${XEXP_CANDIDATES[0]}"
    if [ "${#XEXP_CANDIDATES[@]}" -gt 1 ]; then
        echo "warning: multiple default.xexp files found — using the first, but check the others:" >&2
        printf '    %s\n' "${XEXP_CANDIDATES[@]}" >&2
    fi
fi

# --- Step 3: copy into private/ ---------------------------------------------
mkdir -p "$PRIVATE_DIR"
cp -v "$FOUND_XEX" "$PRIVATE_DIR/default.xex"
if [ -n "$FOUND_XEXP" ]; then
    cp -v "$FOUND_XEXP" "$PRIVATE_DIR/default.xexp"
fi

# --- Step 4: print a safe directory tree of the source dump ----------------
echo
echo "==> Directory tree of '$SRC' (filenames + sizes only — safe to share):"
find "$SRC" -not -path '*/.*' -printf '%y %10s  %p\n' 2>/dev/null | sort -k3 || \
    find "$SRC" -not -path '*/.*' | sort

# --- Step 5: run xex_info if available --------------------------------------
echo
if [ -x "$XEX_INFO_BIN" ]; then
    echo "==> xex_info on private/default.xex (safe to share — structural metadata only):"
    "$XEX_INFO_BIN" "$PRIVATE_DIR/default.xex" || true
    if [ -n "$FOUND_XEXP" ]; then
        echo
        echo "==> xex_info on private/default.xexp:"
        "$XEX_INFO_BIN" "$PRIVATE_DIR/default.xexp" || true
    fi
else
    echo "note: xex_info isn't built yet — run ./tools/build_tools.sh, then re-run this" \
         "script (it will re-copy over the same files) or just run:"
    echo "      $XEX_INFO_BIN $PRIVATE_DIR/default.xex"
fi

echo
echo "Done. private/default.xex is in place$( [ -n "$FOUND_XEXP" ] && echo " (with default.xexp)" )."
echo "Next: docs/02-config-guide.md — paste the tree + xex_info output above back" \
     "into chat, and note the game's region/edition if you know it."
