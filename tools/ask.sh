#!/usr/bin/env bash
# Run several xex_info queries and put ALL of their output on the clipboard at
# once.
#
# Why this exists: `clip` overwrites. Running four queries each piped to `clip`
# leaves only the fourth, so answering four questions meant four round trips
# through a chat window — and with one terminal open, four chances to lose a
# result by pasting in the wrong order. This runs them all, concatenates with
# headers, and copies once.
#
# Usage:
#   tools/ask.sh --func 0x82A25CE8 --func 0x829F4C80
#   tools/ask.sh --xrefs 0x82965D58 --xrefs 0x82966C58 --func 0x82965D58
#   tools/ask.sh --field 0x2ABE --stores
#
# Each flag and its argument becomes one query. Flags that take a value
# (--func, --disasm, --xrefs, --field) consume the next token; trailing
# modifiers (--stores, --context N) attach to the query in front of them.
#
# Output also goes to a file under logs/ so it survives the clipboard.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
XEX_INFO="$REPO_ROOT/tools/xex_info/build/xex_info"
XEX="${WOS_XEX:-$REPO_ROOT/private/default.xex}"

if [ ! -x "$XEX_INFO" ] && [ ! -x "$XEX_INFO.exe" ]; then
    echo "error: xex_info not built. Run ./tools/build_tools.sh first." >&2
    exit 1
fi
if [ ! -f "$XEX" ]; then
    echo "error: no XEX at $XEX" >&2
    echo "       Set WOS_XEX to point at yours, or put it at private/default.xex." >&2
    exit 1
fi
if [ $# -eq 0 ]; then
    echo "Usage: $0 --func 0xADDR [--xrefs 0xADDR] [--disasm 0xADDR [N]] ..." >&2
    echo "Runs each query and copies all the output to the clipboard at once." >&2
    exit 1
fi

# Split the argument list into queries. A new query starts at any flag that
# takes an address; everything after it up to the next such flag belongs to it.
declare -a QUERIES=()
current=""
for arg in "$@"; do
    case "$arg" in
        --func|--xrefs|--disasm|--field|--imports|--helpers)
            if [ -n "$current" ]; then QUERIES+=("$current"); fi
            current="$arg"
            ;;
        *)
            if [ -z "$current" ]; then
                echo "error: \"$arg\" does not follow a query flag." >&2
                exit 1
            fi
            current="$current $arg"
            ;;
    esac
done
[ -n "$current" ] && QUERIES+=("$current")

mkdir -p "$REPO_ROOT/logs"
OUT="$REPO_ROOT/logs/$(date +%Y%m%d-%H%M%S)-ask.txt"

{
    for q in "${QUERIES[@]}"; do
        echo "=============================================================="
        echo "\$ xex_info default.xex $q"
        echo "=============================================================="
        # Word splitting is intended here: $q holds a flag and its arguments.
        # shellcheck disable=SC2086
        "$XEX_INFO" "$XEX" $q 2>&1
        echo
    done
} > "$OUT"

lines="$(wc -l < "$OUT" | tr -d ' ')"

if command -v clip >/dev/null 2>&1; then
    clip < "$OUT"
    copied="copied to clipboard"
else
    copied="clip not found — paste from the file"
fi

echo "==> ${#QUERIES[@]} quer$( [ "${#QUERIES[@]}" -eq 1 ] && echo y || echo ies ), $lines lines, $copied"
echo "==> Saved: ${OUT#$REPO_ROOT/}"
# The clipboard holds one thing. Running anything else that copies will
# replace this, so say up front how to get it back.
echo "==> Re-copy later with:  clip < ${OUT#$REPO_ROOT/}"

# A very large paste is worth flagging before it lands in a chat window.
if [ "$lines" -gt 1200 ]; then
    echo
    echo "NOTE: that is a big paste. If it is unwieldy, the file above has the"
    echo "      same content and you can send a slice of it instead."
fi
