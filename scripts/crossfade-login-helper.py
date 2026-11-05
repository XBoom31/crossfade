#!/usr/bin/env python3
"""Optional sign-in helper for crossfade.

--google: open a WebKitGTK sign-in window and write browser.json. If WebKitGTK
          is unavailable, fall back to importing cookies from Firefox.
--import-firefox: import an existing Firefox YouTube Music session.
--spotify: open a WebKitGTK Spotify sign-in and save the sp_dc cookie.
"""
import argparse
import configparser
import glob
import hashlib
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import time
import urllib.request
import uuid

CONFIG = os.environ.get("P100_CONFIG_DIR") or os.path.join(
    os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "crossfade")
UA = ("Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0")


def _write_private(path, text):
    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
    try:
        os.chmod(os.path.dirname(path), 0o700)
    except OSError:
        pass
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass


def _sapisid_hash(cookies):
    sapisid = cookies.get("SAPISID") or cookies.get("__Secure-3PAPISID") or ""
    if not sapisid:
        raise RuntimeError("Google did not return a SAPISID cookie")
    ts = int(time.time())
    h = hashlib.sha1(f"{ts} {sapisid} https://music.youtube.com".encode()).hexdigest()
    parts = [f"SAPISIDHASH {ts}_{h}"]
    for name, scheme in (("__Secure-1PAPISID", "SAPISID1PHASH"),
                         ("__Secure-3PAPISID", "SAPISID3PHASH")):
        if name in cookies:
            hh = hashlib.sha1(f"{ts} {cookies[name]} https://music.youtube.com".encode()).hexdigest()
            parts.append(f"{scheme} {ts}_{hh}")
    return " ".join(parts)


def _write_browser_json(cookie_header, cookies):
    headers = {
        "cookie": cookie_header,
        "authorization": _sapisid_hash(cookies),
        "x-goog-authuser": "0",
        "origin": "https://music.youtube.com",
        "x-origin": "https://music.youtube.com",
        "referer": "https://music.youtube.com/",
        "user-agent": UA,
        "accept": "*/*",
        "accept-language": "en-US,en;q=0.9",
    }
    _write_private(os.path.join(CONFIG, "browser.json"),
                   json.dumps(headers, indent=2, sort_keys=True))
    return True


def _firefox_cookie_db():
    home = os.path.expanduser("~")
    ini = os.path.join(home, ".mozilla", "firefox", "profiles.ini")
    candidates = []
    if os.path.exists(ini):
        cp = configparser.ConfigParser()
        cp.read(ini)
        for section in cp.sections():
            if cp.get(section, "IsRelative", fallback="1") == "1":
                path = os.path.join(os.path.dirname(ini), cp.get(section, "Path", fallback=""))
            else:
                path = cp.get(section, "Path", fallback="")
            if os.path.isdir(path):
                candidates.append(path)
    candidates += glob.glob(os.path.join(home, ".mozilla", "firefox", "*"))
    for path in candidates:
        db = os.path.join(path, "cookies.sqlite")
        if os.path.exists(db):
            return db
    return ""


def import_firefox():
    db = _firefox_cookie_db()
    if not db:
        raise RuntimeError("No Firefox profile with cookies.sqlite was found")
    tmp = tempfile.mktemp(prefix="crossfade-cookies-", suffix=".sqlite")
    # Firefox may hold the DB; a copy is enough and avoids locks.
    shutil.copy2(db, tmp)
    try:
        con = sqlite3.connect(tmp)
        rows = con.execute(
            "select name,value,host from moz_cookies "
            "where host like '%youtube.com' or host like '%google.com'").fetchall()
        con.close()
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
    cookies = {}
    for name, value, host in rows:
        if name in ("SAPISID", "__Secure-1PAPISID", "__Secure-3PAPISID",
                    "SID", "HSID", "SSID", "APISID", "__Secure-1PSID",
                    "__Secure-3PSID", "SIDCC", "__Secure-1PSIDTS", "__Secure-3PSIDTS"):
            cookies[name] = value
    if "SAPISID" not in cookies:
        raise RuntimeError("Firefox has no signed-in YouTube Music session")
    header = "; ".join(f"{k}={v}" for k, v in cookies.items())
    return _write_browser_json(header, cookies)


def _webview(site):
    """Open a real sign-in window and harvest the session cookies.

    Works with both WebKitGTK 6.x (GTK4, `WebKit` namespace) and the older
    WebKit2GTK 4.1 (GTK3, `WebKit2` namespace). The window closes itself as
    soon as the required cookie appears, so the user never has to find a
    "done" button after the final Google/Spotify redirect.
    """
    import gi

    webkit_version = None
    try:
        gi.require_version("WebKit", "6.0")
        from gi.repository import WebKit
        webkit_version = "6.0"
    except Exception:
        gi.require_version("WebKit2", "4.1")
        from gi.repository import WebKit2 as WebKit
        webkit_version = "4.1"

    if webkit_version == "6.0":
        gi.require_version("Gtk", "4.0")
        from gi.repository import Gtk
        gtk4 = True
    else:
        gi.require_version("Gtk", "3.0")
        from gi.repository import Gtk
        gtk4 = False
    from gi.repository import GLib

    start = ("https://accounts.google.com/ServiceLogin?service=youtube&continue="
             "https%3A%2F%2Fmusic.youtube.com%2F" if site == "google" else
             "https://accounts.spotify.com/en/login?continue=https%3A%2F%2Fopen.spotify.com%2F")
    home = "https://music.youtube.com/" if site == "google" else "https://open.spotify.com/"
    required = "SAPISID" if site == "google" else "sp_dc"

    state = {"cookies": None, "error": None, "finished": False}

    def cookie_manager(view):
        try:
            return view.get_network_session().get_cookie_manager()
        except Exception:
            return view.get_cookie_manager()

    def persist(cookies):
        if required not in cookies:
            return False
        if site == "spotify":
            _write_private(os.path.join(CONFIG, "spotify.json"),
                           json.dumps({"sp_dc": cookies["sp_dc"]}, indent=2))
        else:
            header = "; ".join(f"{k}={v}" for k, v in cookies.items())
            _write_browser_json(header, cookies)
        return True

    if gtk4:
        app = Gtk.Application(application_id="dev.crossfade.login")

        def on_activate(application):
            win = Gtk.ApplicationWindow(application=application)
            win.set_title("Sign in to crossfade")
            win.set_default_size(540, 820)
            view = WebKit.WebView()
            win.set_child(view)
            manager = cookie_manager(view)
            manager.set_accept_policy(WebKit.CookieAcceptPolicy.ALWAYS)

            def on_cookies(mgr, task, *_):
                if state["finished"]:
                    return
                try:
                    cookies = mgr.get_cookies_finish(task)
                    mapping = {c.get_name(): c.get_value() for c in cookies}
                    state["cookies"] = mapping
                    if persist(mapping):
                        state["finished"] = True
                        application.quit()
                except Exception as e:
                    state["error"] = str(e)

            def poll():
                if state["finished"]:
                    return False
                manager.get_cookies(home, None, on_cookies)
                return True

            def on_close(*_):
                state["finished"] = True
                application.quit()
                return False

            view.connect("load-changed", lambda v, event: None)
            win.connect("close-request", on_close)
            view.load_uri(start)
            win.present()
            GLib.timeout_add(650, poll)

        app.connect("activate", on_activate)
        app.run([])
    else:
        win = Gtk.Window(title="Sign in to crossfade")
        win.set_default_size(540, 820)
        view = WebKit.WebView()
        win.add(view)
        manager = cookie_manager(view)
        manager.set_accept_policy(WebKit.CookieAcceptPolicy.ALWAYS)
        loop = GLib.MainLoop()

        def on_cookies(mgr, task, *_):
            if state["finished"]:
                return
            try:
                cookies = mgr.get_cookies_finish(task)
                mapping = {c.get_name(): c.get_value() for c in cookies}
                state["cookies"] = mapping
                if persist(mapping):
                    state["finished"] = True
                    loop.quit()
            except Exception as e:
                state["error"] = str(e)

        def poll():
            if state["finished"]:
                return False
            manager.get_cookies(home, None, on_cookies)
            return True

        def on_close(*_):
            state["finished"] = True
            loop.quit()

        win.connect("destroy", on_close)
        view.load_uri(start)
        win.show_all()
        GLib.timeout_add(650, poll)
        loop.run()

    cookies = state["cookies"] or {}
    if state["error"] and required not in cookies:
        raise RuntimeError(state["error"])
    if required not in cookies:
        raise RuntimeError("Sign-in was cancelled or no signed-in cookie was found")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--google", action="store_true")
    ap.add_argument("--import-firefox", action="store_true")
    ap.add_argument("--spotify", action="store_true")
    args = ap.parse_args()
    if not (args.google or args.import_firefox or args.spotify):
        ap.error("choose --google, --import-firefox or --spotify")
    try:
        if args.import_firefox:
            import_firefox()
        elif args.spotify:
            _webview("spotify")
        else:
            try:
                _webview("google")
            except RuntimeError as e:
                print(f"Popup unavailable ({e}); importing Firefox cookies...", file=sys.stderr)
                import_firefox()
        print("ok")
        return 0
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
