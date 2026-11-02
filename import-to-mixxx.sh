#!/usr/bin/env bash
set -euo pipefail

CONFIG_DIR="${P100_CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/crossfade}"
SRC="$CONFIG_DIR/audio_cache"
DST="$HOME/Music/p100-import"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$DST"

milli() {
    date +%s%N | awk '{printf "%d", $1/1000000}'
}

work() {
    meta="$1"
    SRC="${P100_CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/crossfade}/audio_cache"
    DST="$HOME/Music/p100-import"

    base=$(basename "$meta" .meta.json)
    audio="$SRC/$base"
    [ -f "$audio" ] || { echo "SKIP: no audio for $base"; exit 0; }

    info=$(python3 -c "
import json, sys
try:
    d = json.load(open(sys.argv[1]))
    print(d.get('title','Unknown').strip().replace('\n',' '))
    print(d.get('artist','Unknown').strip().replace('\n',' '))
except:
    print('Unknown')
    print('Unknown')
" "$meta" 2>/dev/null)

    title=$(echo "$info" | head -1)
    artist=$(echo "$info" | tail -1)
    [ -n "$title" ] || title="Unknown"
    [ -n "$artist" ] || artist="Unknown"

    safe=$(echo "$artist - $title" | sed 's/[\/\\:*?"<>|]/_/g' | sed 's/  */_/g' | cut -c1-200)
    out="$DST/$safe.mp3"

    [ -f "$out" ] && { echo "SKIP: $safe.mp3 (already exists)"; exit 0; }

    start=$(milli)
    echo "CONVERT: $base -> $safe.mp3"

    if [[ "$audio" == *.mp3 ]]; then
        cp "$audio" "$out"
    else
        if ! ffmpeg -y -i "$audio" \
            -threads 1 \
            -codec:a libmp3lame -q:a 2 \
            -metadata title="$title" \
            -metadata artist="$artist" \
            "$out" 2>/dev/null; then
            echo "FAIL: $audio"
            rm -f "$out"
            exit 0
        fi
    fi

    end=$(milli)
    elapsed=$(( end - start ))
    mins=$(( elapsed / 60000 ))
    secs=$(( (elapsed % 60000) / 1000 ))
    ms=$(( elapsed % 1000 ))
    echo "DONE: $safe.mp3 (${mins}m${secs}s${ms}ms)"
}

export -f work

total=$(find "$SRC" -maxdepth 1 -name '*.meta.json' | wc -l)
tally=$(mktemp)
script_start=$(milli)

echo "Converting $total tracks with $JOBS parallel jobs..."
echo ""

find "$SRC" -maxdepth 1 -name '*.meta.json' -print0 | \
    xargs -0 -P "$JOBS" -I{} bash -c 'work "$1"' _ {} 2>/dev/null | \
    tee "$tally"

script_end=$(milli)
script_elapsed=$(( script_end - script_start ))
script_mins=$(( script_elapsed / 60000 ))
script_secs=$(( (script_elapsed % 60000) / 1000 ))
script_ms=$(( script_elapsed % 1000 ))

done=$(grep -c '^DONE:' "$tally" || true)
fail=$(grep -c '^FAIL:' "$tally" || true)
skipped=$(grep -c '^SKIP:' "$tally" || true)

rm -f "$tally"

echo ""
echo "========== SUMMARY =========="
echo "  Converted : $done"
echo "  Failed    : $fail"
echo "  Skipped   : $skipped"
echo "  Total     : $total"
echo "  Time      : ${script_mins}m ${script_secs}s ${script_ms}ms"
[ "$done" -gt 0 ] && echo "  Speed     : $(echo "scale=1; $done / ($script_elapsed / 1000)" | bc) tracks/sec"
echo "  Output    : $DST"
echo "============================="
