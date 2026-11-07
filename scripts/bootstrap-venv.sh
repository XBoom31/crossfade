#!/usr/bin/env bash
# Create/refresh the backend Python venv.
#
#   bootstrap-venv.sh <venv-dir> <requirements.txt> [wheels-dir]
#
# A wheels dir enables a fully offline install (used by ./install.sh when a
# local wheels/ directory exists). Returns non-zero on failure so best-effort
# callers can decide whether to ignore it.
set -euo pipefail

VENV="${1:?usage: bootstrap-venv.sh <venv> <requirements> [wheels]}"
REQ="${2:?usage: bootstrap-venv.sh <venv> <requirements> [wheels]}"
WHEELS="${3:-}"

if [ ! -x "$VENV/bin/python" ]; then
    python3 -m venv "$VENV"
fi

if [ ! -x "$VENV/bin/python" ]; then
    echo "Could not create Python venv at $VENV" >&2
    exit 1
fi

if [ -n "$WHEELS" ] && [ -d "$WHEELS" ]; then
    "$VENV/bin/python" -m pip install --no-index --find-links "$WHEELS" -r "$REQ"
else
    "$VENV/bin/python" -m pip install --upgrade pip
    "$VENV/bin/python" -m pip install -r "$REQ"
fi
