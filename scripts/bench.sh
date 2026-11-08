#!/usr/bin/env bash
# Print a small, repeatable resource table for the current build.
#
# Usage: scripts/bench.sh [path-to-crossfade]
#
# By default the runtime rows use a throwaway config directory, so they never
# touch your real library. If a real config exists, two cached-playback runs
# (normal and --low-power) measure cold start -> first frame, cold start ->
# first audio, playing RSS and playing CPU. Set P100_BENCH_NO_PLAYBACK=1 to
# skip them, or P100_BENCH_CONFIG=/path to choose the config used.
#
# On a headless machine the script still prints binary/source/test/install
# sizes and skips the GUI rows.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/crossfade}"
BUILD="${BIN%/*}"
CLK_TCK="$(getconf CLK_TCK 2>/dev/null || echo 100)"
CORES="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

echo "crossfade bench"
echo "---------------"

if [ -x "$BIN" ]; then
    echo "binary:            $(du -h "$BIN" | cut -f1)"
else
    echo "binary:            missing ($BIN)"
fi

SRC_LINES="$(find "$ROOT/src" -name '*.cpp' -o -name '*.h' | xargs wc -l 2>/dev/null | tail -1 | awk '{print $1}')"
echo "source lines:      ${SRC_LINES:-?}"
echo "CPU threads:       ${CORES}"
INSTALL_BYTES=$(( $(stat -c%s "$BIN" 2>/dev/null || echo 0) + $(du -sb "$ROOT/assets" 2>/dev/null | cut -f1 || echo 0) + $(du -sb "$ROOT/py/cf_backend" 2>/dev/null | cut -f1 || echo 0) ))
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

now_ms() { date +%s%3N; }
cpu_ticks() { awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || echo 0; }
rss_kb() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2>/dev/null || echo "?"; }

# cpu_percent PID SETTLE_WINDOW WINDOW_SECONDS
cpu_percent() {
    local pid="$1" settle="$2" window="$3" t0 t1
    sleep "$settle"
    t0="$(cpu_ticks "$pid")"
    sleep "$window"
    t1="$(cpu_ticks "$pid")"
    # Report % of total machine CPU, not % of one core.
    awk -v a="$t0" -v b="$t1" -v hz="$CLK_TCK" -v w="$window" -v cores="$CORES" \
        'BEGIN { printf "%.2f", (b - a) * 100.0 / hz / w / cores }'
}

PID=""
LOG=""
CONFIG=""
CONFIG_IS_TEMP=0
cleanup_run() {
    if [ -n "$PID" ]; then
        kill -TERM "$PID" 2>/dev/null || true
        sleep 0.2
        kill -KILL "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
        PID=""
    fi
    # Safety: only ever delete a throwaway config under /tmp or /var/tmp.
    # Never remove a real user config used for the playback run.
    if [ "$CONFIG_IS_TEMP" = "1" ] && [ -n "$CONFIG" ]; then
        case "$CONFIG" in
            /tmp/*|/var/tmp/*) rm -rf "$CONFIG" ;;
        esac
    fi
    [ -n "$LOG" ] && rm -f "$LOG"
    CONFIG=""; LOG=""; CONFIG_IS_TEMP=0
}

# start_app CONFIG_DIR [extra args...]
start_app() {
    local config="$1"; shift
    LOG="$(mktemp -t crossfade-bench-log.XXXXXX)"
    CONFIG="$config"
    START_MS="$(now_ms)"
    P100_CONFIG_DIR="$CONFIG" "$BIN" "$@" >"$LOG" 2>&1 &
    PID=$!
}

trap cleanup_run EXIT

# ---- Idle / window startup on a throwaway config ----
CONFIG_IS_TEMP=1
start_app "$(mktemp -d -t crossfade-bench.XXXXXX)"

FRAME_MS=""
READY_MS=""
for _ in $(seq 1 600); do
    T="$(now_ms)"
    if [ -z "$FRAME_MS" ] && grep -q "boot: first frame" "$LOG" 2>/dev/null; then
        FRAME_MS="$((T - START_MS))"
    fi
    if [ -z "$READY_MS" ] && { [ -f "$CONFIG/library.db" ] || grep -qE "Session restored|SQLite library ready|Backend not ready|Backend error" "$LOG" 2>/dev/null; }; then
        READY_MS="$((T - START_MS))"
    fi
    if [ -n "$FRAME_MS" ] && [ -n "$READY_MS" ]; then break; fi
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.02
done

IDLE_CPU="$(cpu_percent "$PID" 5 3)"
IDLE_RSS="$(rss_kb "$PID")"
cleanup_run

echo "warm start:        0 s (resident; show/raise IPC only)"
[ -n "$FRAME_MS" ] && echo "startup->window:   ${FRAME_MS} ms"
[ -n "$READY_MS" ] && echo "startup->ready:    ${READY_MS} ms"
echo "idle RSS:          ${IDLE_RSS:-?} kB"
echo "idle CPU (all):    ${IDLE_CPU:-?}%"

# ---- Cached playback startup (opt-in by the presence of a real config) ----
PLAY_CONFIG="${P100_BENCH_CONFIG:-$HOME/.config/crossfade}"
if [ "${P100_BENCH_NO_PLAYBACK:-0}" = "1" ] || [ ! -f "$PLAY_CONFIG/session.json" ]; then
    exit 0
fi

# playback_pass LABEL [extra args...]
playback_pass() {
    local label="$1"; shift
    local frame_ms="" audio_ms="" cpu rss t
    CONFIG_IS_TEMP=0
    start_app "$PLAY_CONFIG" "$@"
    for _ in $(seq 1 600); do
        t="$(now_ms)"
        if [ -z "$frame_ms" ] && grep -q "boot: first frame" "$LOG" 2>/dev/null; then
            frame_ms="$((t - START_MS))"
        fi
        if [ -z "$audio_ms" ] && grep -q "boot: first audio" "$LOG" 2>/dev/null; then
            audio_ms="$((t - START_MS))"
        fi
        if [ -n "$frame_ms" ] && [ -n "$audio_ms" ]; then break; fi
        kill -0 "$PID" 2>/dev/null || break
        sleep 0.02
    done
    cpu="$(cpu_percent "$PID" 8 4)"
    rss="$(rss_kb "$PID")"
    cleanup_run
    [ -n "$frame_ms" ] && echo "${label} startup->window: ${frame_ms} ms"
    [ -n "$audio_ms" ] && echo "${label} startup->audio:  ${audio_ms} ms"
    echo "${label} RSS:       ${rss:-?} kB"
    echo "${label} CPU (all): ${cpu:-?}%"
}

playback_pass "play" --autoplay-last
playback_pass "low-power" --autoplay-last --low-power
