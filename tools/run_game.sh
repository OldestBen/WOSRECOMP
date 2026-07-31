#!/usr/bin/env bash
# Run the game for a fixed time, stop it, and put the interesting lines on the
# clipboard — one command, one terminal, no Ctrl-C.
#
# The old sequence was three commands: run, Ctrl-C when it looked done, then a
# separate grep piped to clip. With a single terminal that is fragile — the
# stopping point is a judgement call, and any second `| clip` overwrites the
# first. This does the whole thing.
#
# Usage:
#   tools/run_game.sh                 # 45 s, default filter
#   tools/run_game.sh 90              # 90 s
#   tools/run_game.sh 45 '^\[state\]' # 45 s, only [state] lines
#   tools/run_game.sh 45 all          # everything (big)
#
# The full log is always written to logs/ regardless of the filter, so nothing
# is lost by filtering narrowly.
#
# On losing output: WoSRecomp.exe sets stdout unbuffered (see the setvbuf call
# at the top of main.cpp), so stopping it mid-run does NOT drop buffered lines
# the way it would for a normally-buffered program. The reason to use a timer
# rather than Ctrl-C is repeatability, not data loss.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
EXE="$REPO_ROOT/WoSRecomp/build/WoSRecomp.exe"
[ -x "$EXE" ] || EXE="$REPO_ROOT/WoSRecomp/build/WoSRecomp"

SECONDS_TO_RUN="${1:-45}"
FILTER="${2:-}"

# The default filter is everything that has ever answered a question here:
# our own diagnostics, plus the file/thread/import lines that show progress.
# Deliberately NOT the per-heartbeat event census — that repeats every 5 s and
# drowns the rest.
DEFAULT_FILTER='^\[state\]|^\[probe\]|^\[trace\]|^\[file\]|^\[thread\]|^\[import|^\[sync\]|^\[watchdog\]|^\[heartbeat\]'

if [ ! -x "$EXE" ]; then
    echo "error: no built binary at WoSRecomp/build/. Run ./tools/build_host.sh first." >&2
    exit 1
fi
if [ -z "${WOS_GAME_ROOT:-}" ]; then
    echo "warning: WOS_GAME_ROOT is not set — the game will not find its files." >&2
    echo "         export WOS_GAME_ROOT=/c/path/to/extracted/game" >&2
    echo
fi
case "$SECONDS_TO_RUN" in
    ''|*[!0-9]*)
        echo "error: duration must be a whole number of seconds, got \"$SECONDS_TO_RUN\"" >&2
        exit 1
        ;;
esac

mkdir -p "$REPO_ROOT/logs"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$REPO_ROOT/logs/$STAMP-run.log"

echo "==> Running for ${SECONDS_TO_RUN}s: ${EXE#$REPO_ROOT/}"
echo "==> Log: ${LOG#$REPO_ROOT/}"
echo

"$EXE" > "$LOG" 2>&1 &
GAME_PID=$!

# Report progress while waiting, so a long run does not look like a hang, and
# stop early if the game exits on its own (a crash should not cost the full
# wait).
for (( elapsed = 0; elapsed < SECONDS_TO_RUN; elapsed += 5 )); do
    sleep 5
    if ! kill -0 "$GAME_PID" 2>/dev/null; then
        echo "==> Game exited on its own after ~${elapsed}s."
        break
    fi
    printf '    %ds... (%s lines)\n' "$((elapsed + 5))" "$(wc -l < "$LOG" | tr -d ' ')"
done

if kill -0 "$GAME_PID" 2>/dev/null; then
    kill "$GAME_PID" 2>/dev/null
    sleep 1
    kill -9 "$GAME_PID" 2>/dev/null
    wait "$GAME_PID" 2>/dev/null
    echo "==> Stopped after ${SECONDS_TO_RUN}s."
fi

total="$(wc -l < "$LOG" | tr -d ' ')"

DIGEST="$REPO_ROOT/logs/$STAMP-run.digest.txt"
if [ "$FILTER" = "all" ]; then
    cp "$LOG" "$DIGEST"
else
    grep -E "${FILTER:-$DEFAULT_FILTER}" "$LOG" > "$DIGEST" 2>/dev/null || true
fi

kept="$(wc -l < "$DIGEST" | tr -d ' ')"

if command -v clip >/dev/null 2>&1; then
    clip < "$DIGEST"
    copied="copied to clipboard"
else
    copied="clip not found — paste from the digest file"
fi

echo
echo "==> $total lines logged, $kept kept, $copied"
echo "==> Full log: ${LOG#$REPO_ROOT/}"
echo "==> Digest:   ${DIGEST#$REPO_ROOT/}"

if [ "$kept" -eq 0 ]; then
    echo
    echo "NOTE: the filter matched nothing. Last 20 lines of the full log:"
    tail -20 "$LOG" | sed 's/^/    /'
fi
