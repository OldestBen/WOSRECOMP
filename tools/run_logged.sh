#!/usr/bin/env bash
# Runs a command, captures full output to a log, and writes a compact
# summary alongside it.
#
# Usage:
#   tools/run_logged.sh <label> -- <command> [args...]
#
# Examples:
#   tools/run_logged.sh build -- ./tools/build_tools.sh
#   tools/run_logged.sh analyse -- tools/XenonRecomp/build/XenonAnalyse/XenonAnalyse \
#       private/default.xex WoSRecompLib/config/WoS_switch_tables.toml
#
# Produces, in logs/:
#   YYYYMMDD-HHMMSS-<label>.log          full raw output (gitignored — can be huge)
#   YYYYMMDD-HHMMSS-<label>.summary.md   compact summary (committed — share this)
#
# Why the summary exists: a failing recompile can emit tens of thousands of
# error lines that are really only a handful of *distinct* problems repeated.
# The summary deduplicates them, so "4213 errors, 6 distinct kinds" replaces
# 4213 lines. Commit the summary; only dig into the raw log when the summary
# isn't enough.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_DIR="$REPO_ROOT/logs"

if [ $# -lt 1 ]; then
    echo "Usage: $0 <label> -- <command> [args...]" >&2
    exit 1
fi

LABEL="$1"
shift
if [ "${1:-}" = "--" ]; then
    shift
fi

if [ $# -lt 1 ]; then
    echo "error: no command given. Usage: $0 <label> -- <command> [args...]" >&2
    exit 1
fi

# Sanitise label for use in a filename.
LABEL="$(printf '%s' "$LABEL" | tr -c 'A-Za-z0-9._-' '-')"

mkdir -p "$LOG_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="$LOG_DIR/$STAMP-$LABEL.log"
SUMMARY_FILE="$LOG_DIR/$STAMP-$LABEL.summary.md"

echo "==> Running: $*"
echo "==> Log: ${LOG_FILE#$REPO_ROOT/}"
echo

START_EPOCH="$(date +%s)"
# Capture combined stdout+stderr while still showing it live.
# PIPESTATUS[0] preserves the command's real exit code past the tee pipe.
"$@" 2>&1 | tee "$LOG_FILE"
EXIT_CODE="${PIPESTATUS[0]}"
END_EPOCH="$(date +%s)"
DURATION=$(( END_EPOCH - START_EPOCH ))

# ---------------------------------------------------------------------------
# Summary generation
# ---------------------------------------------------------------------------

total_lines="$(wc -l < "$LOG_FILE" | tr -d ' ')"

count_matches() {
    # $1 = grep -E pattern. Always succeeds, even with zero matches.
    grep -Ec "$1" "$LOG_FILE" 2>/dev/null || true
}

err_count="$(count_matches '(^|[^a-zA-Z])error:')"
warn_count="$(count_matches '(^|[^a-zA-Z])warning:')"
ninja_failed="$(count_matches '^FAILED:')"
cmake_err="$(count_matches '^CMake Error')"

# Normalise an error line so repeats collapse into one signature:
#   - drop leading "path/file.cpp:123:45: " location prefixes
#   - replace hex literals and long decimals with placeholders
normalise() {
    sed -E \
        -e 's#^[^ ]*:[0-9]+:[0-9]+: ##' \
        -e 's#^[^ ]*:[0-9]+: ##' \
        -e 's#0x[0-9a-fA-F]+#0xADDR#g' \
        -e 's#\b[0-9]{4,}\b#NNN#g'
}

{
    echo "# Run summary — \`$LABEL\`"
    echo
    echo "| | |"
    echo "|---|---|"
    echo "| **Command** | \`$(printf '%s ' "$@" | sed 's/ $//' | sed 's/|/\\|/g')\` |"
    echo "| **Exit code** | \`$EXIT_CODE\` $( [ "$EXIT_CODE" -eq 0 ] && echo '✅' || echo '❌' ) |"
    echo "| **Duration** | ${DURATION}s |"
    echo "| **Date** | $(date -u '+%Y-%m-%d %H:%M:%S UTC') |"
    echo "| **Host** | $(uname -srm) |"
    echo "| **Log lines** | $total_lines |"
    echo "| **Raw log** | \`${LOG_FILE#$REPO_ROOT/}\` (gitignored) |"
    echo
    echo "## Counts"
    echo
    echo "- \`error:\` lines: **$err_count**"
    echo "- \`warning:\` lines: **$warn_count**"
    echo "- \`FAILED:\` (ninja targets): **$ninja_failed**"
    echo "- \`CMake Error\`: **$cmake_err**"
    echo

    if [ "$err_count" -gt 0 ]; then
        echo "## Distinct errors (deduplicated, most frequent first)"
        echo
        echo '```'
        grep -E '(^|[^a-zA-Z])error:' "$LOG_FILE" \
            | normalise \
            | sort | uniq -c | sort -rn | head -25
        echo '```'
        echo
    fi

    if [ "$ninja_failed" -gt 0 ]; then
        echo "## Failed targets"
        echo
        echo '```'
        grep -E '^FAILED:' "$LOG_FILE" | head -25
        echo '```'
        echo
    fi

    echo "## Last 30 lines"
    echo
    echo '```'
    tail -n 30 "$LOG_FILE"
    echo '```'
} > "$SUMMARY_FILE"

echo
echo "==> Exit code: $EXIT_CODE (${DURATION}s)"
echo "==> Summary:  ${SUMMARY_FILE#$REPO_ROOT/}"
echo
echo "Share the .summary.md (it's committed; the raw .log is gitignored)."
echo "If the summary isn't enough, say so and we'll dig into the raw log."

exit "$EXIT_CODE"
