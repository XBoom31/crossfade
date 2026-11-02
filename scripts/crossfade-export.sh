#!/usr/bin/env bash
# Export crossfade's config + caches so they can be copied to another machine.
#
#   ./scripts/crossfade-export.sh                 # everything (settings, auth, audio, art)
#   ./scripts/crossfade-export.sh --slim          # no audio_cache/ or thumb_cache/
#   ./scripts/crossfade-export.sh --slim /tmp/crossfade-data.tar.zst
set -euo pipefail

MODE=full
if [ "${1:-}" = "--slim" ]; then
    MODE=slim
    shift
fi

CONFIG_DIR="${P100_CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/crossfade}"
PARENT="$(dirname "$CONFIG_DIR")"
BASE="$(basename "$CONFIG_DIR")"
OUT="${1:-crossfade-data-$(date +%Y%m%d-%H%M%S).tar.zst}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ ! -d "$CONFIG_DIR" ]; then
    echo "No crossfade data directory at $CONFIG_DIR" >&2
    exit 1
fi

# Older builds wrote imgui.ini next to the binary / repo root. Preserve that
# layout on first migration instead of starting a fresh one.
if [ -f "$ROOT/imgui.ini" ] && [ ! -f "$CONFIG_DIR/imgui.ini" ]; then
    cp "$ROOT/imgui.ini" "$CONFIG_DIR/imgui.ini"
fi

EXCLUDES=(--exclude="$BASE/backend.log" --exclude="$BASE/.tmp" --exclude="$BASE/*.tmp"
          --exclude="$BASE/audio_cache/.tmp")
if [ "$MODE" = slim ]; then
    EXCLUDES+=(--exclude="$BASE/audio_cache" --exclude="$BASE/thumb_cache")
fi

echo "Exporting $MODE data from $CONFIG_DIR"
if command -v zstd >/dev/null 2>&1 && [[ "$OUT" == *.zst ]]; then
    tar --zstd -cf "$OUT" -C "$PARENT" "${EXCLUDES[@]}" "$BASE"
else
    if [[ "$OUT" == *.zst ]]; then
        OUT="${OUT%.zst}.tar.gz"
    fi
    tar -czf "$OUT" -C "$PARENT" "${EXCLUDES[@]}" "$BASE"
fi

ls -lh "$OUT"
echo
echo "Copy it to the other machine, then run:"
echo "  ./scripts/crossfade-import.sh $(basename "$OUT")"
