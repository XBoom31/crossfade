#!/bin/bash
# AppImage launcher: the squashfs is read-only, so the Python venv lives in
# the user's data dir and is bootstrapped on first run.
APPDIR="${APPDIR:-$(dirname "$(readlink -f "$0")")/..}"
VENV="${HOME}/.local/share/crossfade/venv"
REQ="$APPDIR/usr/share/crossfade/requirements.txt"

if [ ! -x "$VENV/bin/python" ]; then
    if command -v python3 >/dev/null 2>&1; then
        mkdir -p "$(dirname "$VENV")"
        python3 -m venv "$VENV" >/dev/null 2>&1 || true
        [ -x "$VENV/bin/pip" ] && "$VENV/bin/pip" install --quiet --upgrade pip >/dev/null 2>&1 || true
        [ -x "$VENV/bin/pip" ] && "$VENV/bin/pip" install --quiet -r "$REQ" >/dev/null 2>&1 || true
    fi
fi
if [ -x "$VENV/bin/python" ]; then
    export P100_PYTHON="$VENV/bin/python"
    export P100_YTDLP="$VENV/bin/yt-dlp"
fi
exec "$APPDIR/usr/bin/crossfade.bin" "$@"
