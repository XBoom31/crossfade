#!/usr/bin/env bash
# Print a small, repeatable resource table for the current build.
#
# Usage: scripts/bench.sh [path-to-crossfade]
#
# Runtime numbers use a throwaway config directory and a short idle window, so
# they don't touch your real library. On a headless machine the script still
# prints binary/source/test/install sizes and skips the GUI rows.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/crossfade}"
BUILD="${BIN%/*}"

echo "crossfade bench"
echo "---------------"

if [ -x "$BIN" ]; then
    echo "binary:            $(du -h "$BIN" | cut -f1)"
else
    echo "binary:            missing ($BIN)"
fi

SRC_LINES="$(find "$ROOT/src" -name '*.cpp' -o -name '*.h' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')"
echo "source lines:      ${SRC_LINES:-?}"
INSTALL_BYTES=$(( $(stat -c%s "$BIN" 2>/dev/null || echo 0) + $(du -sb "$ROOT/assets" 2>/dev/null | cut -f1 || echo 0) ))
echo "install estimate:  $(numfmt --to=iec --suffix=B "$INSTALL_BYTES" 2>/dev/null || echo "$INSTALL_BYTES bytes")"

if [ -x "$BUILD/test_library" ] || [ -x "$BUILD/test_party_link" ]; then
    if ctest --test-dir "$BUILD" --output-on-failure >/tmp/crossfade-bench-tests.log 2>&1; then
        COUNT="$(grep -c 'Passed' /tmp/crossfade-bench-tests.log || true)"
        echo "unit tests:        ${COUNT:-?} passed"
    else
        echo "unit tests:        FAILED (see /tmp/crossfade-bench-tests.log)"
    fi
else
    echo "unit tests:        not built (configure with -DCROSSFADE_BUILD_TESTS=ON)"
fi

if [ -z "${DISPLAY:-}" ] || [ ! -x "$BIN" ]; then
    echo "runtime:           skipped (no DISPLAY)"
    exit 0
fi

CONFIG="$(mktemp -d -t crossfade-bench.XXXXXX)"
LOG="$(mktemp -t crossfade-bench-log.XXXXXX)"
cleanup() {
    kill -TERM "$PID" 2>/dev/null || true
    sleep 0.2
    kill -KILL "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    rm -rf "$CONFIG" "$LOG"
}
trap cleanup EXIT

START_MS="$(date +%s%3N)"
P100_CONFIG_DIR="$CONFIG" "$BIN" >"$LOG" 2>&1 &
PID=$!

READY_MS=""
for _ in $(seq 1 100); do
    if [ -f "$CONFIG/library.db" ] || grep -qE "Session restored|SQLite library ready|Backend not ready|Backend error" "$LOG"; then
        READY_MS="$(date +%s%3N)"
        break
    fi
    sleep 0.05
done

sleep 5
RSS="$(awk '/VmRSS/{print $2 " " $3}' "/proc/$PID/status" 2>/dev/null || echo "?")"
CPU="$(ps -p "$PID" -o %cpu= 2>/dev/null | tr -d ' ' || echo "?")"

if [ -n "$READY_MS" ]; then
    echo "startup->ready:    $((READY_MS - START_MS)) ms"
else
    echo "startup->ready:    ?"
fi
echo "idle RSS:          ${RSS:-?}"
echo "idle CPU (5 s):    ${CPU:-?}%"
