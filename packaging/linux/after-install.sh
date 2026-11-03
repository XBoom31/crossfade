#!/bin/bash
# Shared post-install scriptlet for the deb/rpm packages: build the backend
# venv in place so the app works right after install (needs network once).
set -e
SHARE=/usr/share/crossfade
if command -v python3 >/dev/null 2>&1; then
    if [ ! -x "$SHARE/venv/bin/python" ]; then
        python3 -m venv "$SHARE/venv" || true
    fi
    if [ -x "$SHARE/venv/bin/pip" ]; then
        "$SHARE/venv/bin/pip" install --quiet --upgrade pip >/dev/null 2>&1 || true
        "$SHARE/venv/bin/pip" install --quiet -r "$SHARE/requirements.txt" >/dev/null 2>&1 || true
    fi
fi
# Best-effort: if this failed (offline install, missing venv module), the app
# shows a backend error on first launch and `pip install -r requirements.txt`
# into the venv fixes it.
exit 0
