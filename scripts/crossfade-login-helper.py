#!/usr/bin/env python3
"""Optional sign-in helper for crossfade.

--google: open a WebKitGTK sign-in window and write browser.json. If WebKitGTK
          is unavailable, fall back to importing cookies from Firefox.
          On Windows: open YouTube Music in the default browser and a small
          Tkinter window that turns copied request headers into browser.json.
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

if sys.platform == "win32":
    _BASE = os.environ.get("APPDATA") or os.path.expanduser("~")
    CONFIG = os.environ.get("P100_CONFIG_DIR") or os.path.join(_BASE, "crossfade")
    UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) "
          "Gecko/20100101 Firefox/140.0")
else:
    CONFIG = os.environ.get("P100_CONFIG_DIR") or os.path.join(
        os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "crossfade")
    UA = "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0"


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
    if sys.platform == "win32":
        root = os.path.join(os.environ.get("APPDATA", home), "Mozilla", "Firefox")
    else:
        root = os.path.join(home, ".mozilla", "firefox")
    ini = os.path.join(root, "profiles.ini")
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
    candidates += glob.glob(os.path.join(root, "*"))
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


def _find_make_browser_json():
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = []
    share = os.environ.get("P100_SHARE_DIR")
    if share:
        candidates.append(os.path.join(share, "make_browser_json.py"))
    candidates += [
        os.path.join(here, "..", "make_browser_json.py"),
        os.path.join(here, "..", "packaging", "windows", "make_browser_json.py"),
        os.path.join(here, "make_browser_json.py"),
    ]
    for path in candidates:
        path = os.path.abspath(path)
        if os.path.isfile(path):
            return path
    return ""


def _clipboard_text():
    import subprocess
    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command", "Get-Clipboard"],
            capture_output=True, timeout=15,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return result.stdout.decode("utf-8", errors="replace")
    except Exception:
        return ""


def _finish_windows_login(text, make_json):
    """Write browser.json from a copied header block. Returns (ok, output)."""
    import subprocess
    tmp = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
    try:
        tmp.write(text)
        tmp.close()
        result = subprocess.run(
            [sys.executable, make_json, tmp.name],
            capture_output=True, text=True, timeout=180,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return result.returncode == 0, (result.stdout or "") + (result.stderr or "")
    except Exception as e:
        return False, str(e)
    finally:
        try:
            os.unlink(tmp.name)
        except OSError:
            pass


def _windows_login():
    """Browser + Tkinter clipboard flow for Windows (no WebKitGTK/gi needed).

    The sign-in itself happens in the user's normal browser, which already has
    their Google session. DevTools' "Copy request headers" gives us the cookie
    block; make_browser_json.py turns it into browser.json and verifies it.
    """
    import webbrowser

    make_json = _find_make_browser_json()
    if not make_json:
        raise RuntimeError("make_browser_json.py was not found; reinstall crossfade")

    site = "https://music.youtube.com/"
    webbrowser.open(site)

    try:
        import tkinter as tk
        from tkinter import messagebox
    except Exception:
        # No Tk (rare): wait for the headers on the clipboard in the console.
        print("Waiting for request headers on the clipboard ...", file=sys.stderr)
        deadline = time.time() + 600
        while time.time() < deadline:
            text = _clipboard_text()
            if "cookie" in text.lower():
                ok, output = _finish_windows_login(text, make_json)
                print(output)
                if ok:
                    return True
                raise RuntimeError(output.strip()[-500:] or "sign-in failed")
            time.sleep(1)
        raise RuntimeError("timed out waiting for the clipboard")

    root = tk.Tk()
    root.title("Sign in to crossfade")
    root.geometry("600x400")
    root.minsize(520, 340)
    root.attributes("-topmost", True)
    root.after(1000, lambda: root.attributes("-topmost", False))
    completed = {"ok": False}

    frame = tk.Frame(root, padx=18, pady=16)
    frame.pack(fill="both", expand=True)

    tk.Label(frame, text="Sign in to YouTube Music", font=("Segoe UI", 14, "bold"),
             anchor="w").pack(fill="x")
    instructions = (
        "1. Your browser opened YouTube Music. Make sure you are signed in\n"
        "   (your profile picture is visible in the top-right corner).\n\n"
        "2. Press F12, open the Network tab, and click any POST request to\n"
        "   music.youtube.com. Right-click it and choose\n"
        "   Copy -> Copy request headers.\n\n"
        "3. Come back to this window and click \"I copied the headers\"."
    )
    tk.Label(frame, text=instructions, justify="left", anchor="w",
             font=("Segoe UI", 10), wraplength=545).pack(fill="x", pady=(10, 8))

    status = tk.StringVar(value="")
    tk.Label(frame, textvariable=status, fg="#555", anchor="w",
             font=("Segoe UI", 9)).pack(fill="x")

    buttons = tk.Frame(frame)
    buttons.pack(side="bottom", fill="x", pady=(12, 0))

    def open_browser():
        webbrowser.open(site)

    def finish():
        try:
            text = root.clipboard_get()
        except tk.TclError:
            text = ""
        if "cookie" not in text.lower():
            messagebox.showwarning(
                "No request headers",
                "The clipboard does not contain request headers.\n\n"
                "In the browser: F12 -> Network -> click a POST request to "
                "music.youtube.com -> right-click -> Copy -> Copy request headers.",
                parent=root,
            )
            return
        status.set("Signing in ...")
        root.update_idletasks()
        ok, output = _finish_windows_login(text, make_json)
        if ok:
            completed["ok"] = True
            messagebox.showinfo(
                "Signed in",
                "crossfade is signed in to YouTube Music.\n\n"
                "Restart crossfade if it is already running.",
                parent=root,
            )
            root.destroy()
            return
        status.set("")
        tail = "\n".join(output.strip().splitlines()[-14:])
        messagebox.showerror("Sign-in failed", tail or "Unknown error", parent=root)

    tk.Button(buttons, text="Open YouTube Music again", command=open_browser,
              width=24).pack(side="left")
    tk.Button(buttons, text="I copied the headers", command=finish,
              width=22, default="active").pack(side="right")
    tk.Button(buttons, text="Cancel", command=root.destroy,
              width=10).pack(side="right", padx=(0, 8))

    root.mainloop()
    return completed["ok"]


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
        elif sys.platform == "win32":
            if not _windows_login():
                raise RuntimeError("sign-in was cancelled or did not finish")
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
