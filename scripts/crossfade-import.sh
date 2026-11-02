#!/usr/bin/env bash
# Import an archive made by scripts/crossfade-export.sh into ~/.config/crossfade.
set -euo pipefail

ARCHIVE="${1:-}"
if [ -z "$ARCHIVE" ] || [ ! -f "$ARCHIVE" ]; then
    echo "Usage: $0 <crossfade-data.tar.zst|crossfade-data.tar.gz>" >&2
    exit 1
fi

CONFIG_DIR="${P100_CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/crossfade}"
CONFIG_PARENT="$(dirname "$CONFIG_DIR")"

if pgrep -x crossfade >/dev/null 2>&1; then
    echo "crossfade is running. Close it before importing data." >&2
    exit 1
fi

# Extract beside the destination, not in /tmp. On Fedora /tmp is often a
# size-limited tmpfs; a full 4.5 GB library can fill it even when the real
# disk is nearly empty. P100_IMPORT_TMP can point at another large filesystem.
TMP_ROOT="${P100_IMPORT_TMP:-$CONFIG_PARENT}"
mkdir -p "$TMP_ROOT"
TMP="$(mktemp -d "$TMP_ROOT/.crossfade-import.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

case "$ARCHIVE" in
    *.tar.zst|*.tzst)
        tar --zstd -xf "$ARCHIVE" -C "$TMP"
        ;;
    *.tar.gz|*.tgz)
        tar -xzf "$ARCHIVE" -C "$TMP"
        ;;
    *)
        tar -xf "$ARCHIVE" -C "$TMP"
        ;;
esac

SRC="$(find "$TMP" -mindepth 1 -maxdepth 1 -type d -print -quit)"
if [ -z "$SRC" ]; then
    echo "Archive does not contain a crossfade data directory." >&2
    exit 1
fi

mkdir -p "$CONFIG_DIR"
cp -a "$SRC/." "$CONFIG_DIR/"

echo "Imported crossfade data into $CONFIG_DIR"
echo "Start crossfade and check Settings / auth if cookies have expired."
