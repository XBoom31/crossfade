#!/usr/bin/env python3
"""Crossfade backend using browser-cookie authentication.

Create the cookie file once with `ytmusicapi browser`. Everything else is in
cf_backend.core so the cookie and OAuth entry points stay behaviorally
identical.
"""

import json
import os
import sys

from ytmusicapi import YTMusic

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "py"))
from cf_backend.core import _err, ensure_config_dir, run_command_loop, secure_auth_file  # noqa: E402


def _config_dir():
    xdg = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.environ.get("P100_CONFIG_DIR") or os.path.join(xdg, "crossfade")


CONFIG_DIR = _config_dir()


def _resolve_auth_file():
    env = os.environ.get("P100_AUTH_FILE")
    if env:
        return env
    candidates = [
        os.path.join(CONFIG_DIR, "browser.json"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "browser.json"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return candidates[0]


AUTH_FILE = _resolve_auth_file()


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    ensure_config_dir(CONFIG_DIR)
    if os.path.exists(AUTH_FILE):
        secure_auth_file(AUTH_FILE)

    if not os.path.exists(AUTH_FILE):
        print(
            json.dumps(
                _err(
                    f"Auth file '{AUTH_FILE}' not found. Run `ytmusicapi browser` "
                    "to generate it, or set P100_AUTH_FILE to its path.",
                    kind="auth",
                )
            )
        )
        sys.stdout.flush()
        return

    try:
        yt = YTMusic(AUTH_FILE)
    except Exception as e:
        print(json.dumps(_err(f"Failed to initialize YTMusic: {str(e)}")))
        sys.stdout.flush()
        return

    print(json.dumps({"status": "ready"}))
    sys.stdout.flush()
    run_command_loop(yt)


if __name__ == "__main__":
    main()
