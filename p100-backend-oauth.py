#!/usr/bin/env python3
"""Crossfade backend using OAuth.

Set P100_OAUTH_CLIENT_ID / P100_OAUTH_CLIENT_SECRET (or edit CLIENT_ID /
CLIENT_SECRET below), or pass --login once to create the token. All command
handling lives in cf_backend.core so both auth modes stay identical.
"""

import json
import os
import shutil
import sys

import ytmusicapi
from ytmusicapi import OAuthCredentials, YTMusic

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "py"))
from cf_backend.core import (  # noqa: E402
    _classify_error,
    _err,
    check_logged_in,
    ensure_config_dir,
    run_command_loop,
    secure_auth_file,
)

_XDG_CONFIG = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
CONFIG_DIR = os.environ.get("P100_CONFIG_DIR") or os.path.join(_XDG_CONFIG, "p100")

# Environment overrides keep the repo free of bundled credentials.
CLIENT_ID = os.environ.get("P100_OAUTH_CLIENT_ID", "")
CLIENT_SECRET = os.environ.get("P100_OAUTH_CLIENT_SECRET", "")


def _resolve_oauth_file():
    preferred = os.path.join(CONFIG_DIR, "browseroauth.json")
    legacy = os.path.join(CONFIG_DIR, "oauth.json")
    if not os.path.exists(preferred) and os.path.exists(legacy):
        try:
            _ensure_config_dir()
            shutil.copy2(legacy, preferred)
            print(f"Migrated {legacy} -> {preferred}", file=sys.stderr)
        except Exception as e:
            print(f"Could not migrate {legacy}: {e}", file=sys.stderr)
    env = os.environ.get("P100_AUTH_FILE")
    if env:
        return env
    return preferred


OAUTH_FILE = _resolve_oauth_file()


def _ensure_config_dir():
    ensure_config_dir(CONFIG_DIR)


def _credentials():
    return OAuthCredentials(client_id=CLIENT_ID, client_secret=CLIENT_SECRET)


def load_or_create_session():
    creds = _credentials()
    if os.path.exists(OAUTH_FILE):
        secure_auth_file(OAUTH_FILE)
        try:
            return YTMusic(auth=OAUTH_FILE, oauth_credentials=creds)
        except Exception as e:
            print(f"Stored login invalid ({e}). Re-authenticating...", file=sys.stderr)
            os.remove(OAUTH_FILE)

    print(
        "No valid saved login found. Opening browser for Google login...",
        file=sys.stderr,
    )
    ytmusicapi.setup_oauth(
        client_id=CLIENT_ID,
        client_secret=CLIENT_SECRET,
        filepath=OAUTH_FILE,
        open_browser=True,
    )
    secure_auth_file(OAUTH_FILE)
    yt = YTMusic(auth=OAUTH_FILE, oauth_credentials=creds)
    print("Login successful. Token saved.", file=sys.stderr)
    return yt


def main():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass

    _ensure_config_dir()
    if not CLIENT_ID or not CLIENT_SECRET:
        print(
            json.dumps(
                {
                    "error": "Set P100_OAUTH_CLIENT_ID and P100_OAUTH_CLIENT_SECRET "
                    "or pass --login after configuring the OAuth client.",
                    "kind": "auth",
                }
            )
        )
        sys.stdout.flush()
        return

    try:
        yt = load_or_create_session()
    except Exception as e:
        msg = f"Login failed: {e}"
        print(json.dumps(_err(msg, kind=_classify_error(msg))))
        sys.stdout.flush()
        return

    logged_in, err = check_logged_in(yt)
    if not logged_in:
        print(
            f"Note: logged_in check was inconclusive ({err}). "
            "Continuing - will confirm via actual data instead.",
            file=sys.stderr,
        )

    print(json.dumps({"status": "ready"}))
    sys.stdout.flush()
    run_command_loop(yt)


def login_only():
    _ensure_config_dir()
    if not CLIENT_ID or not CLIENT_SECRET:
        print(
            "Set P100_OAUTH_CLIENT_ID and P100_OAUTH_CLIENT_SECRET first.",
            file=sys.stderr,
        )
        sys.exit(1)
    load_or_create_session()
    print(f"OAuth login complete. Token saved to {OAUTH_FILE}")
    print("Restart crossfade to use the new login.")


if __name__ == "__main__":
    if "--login" in sys.argv[1:]:
        login_only()
    else:
        main()
