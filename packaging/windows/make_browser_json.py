"""Create <config>/browser.json from pasted browser request headers.

More forgiving than `ytmusicapi browser`: cleans up HTTP/2 pseudo-header
views (:authority/:method/:path), cURL-style "-H '...'" lines, request/
status lines, hop-by-hop headers, and duplicate header names, so any of
the common "copy request headers" outputs work.

Usage:
  python make_browser_json.py            # paste, finish with Ctrl+Z+Enter
  python make_browser_json.py <file>     # read headers from a file

After writing, the login is verified against the YouTube Music library.
"""
import json
import os
import re
import sys

from ytmusicapi import YTMusic
from ytmusicapi.auth.browser import setup_browser

PSEUDO = re.compile(r"^:[a-z-]+")
REQ_LINE = re.compile(r"^(?:GET|POST|PUT|HEAD|OPTIONS|DELETE) \S+ HTTP/")
CURL_H = re.compile(r"^-H\s+['\"](.*)['\"],?\s*$")
DROP = {
    "host", "content-length", "content-encoding", "connection",
    "accept-encoding", "priority", "authority", "method", "path", "scheme",
}


def clean(text):
    lines = []
    for line in text.splitlines():
        line = line.strip().rstrip("\\").strip()
        if (not line or PSEUDO.match(line) or REQ_LINE.match(line)
                or line.lower().startswith(("curl ", "compressed"))):
            continue
        if ":" not in line and lines:
            # Console pastes wrap ultra-long headers (the Cookie line);
            # re-attach the fragment to the previous header line.
            lines[-1] += line
            continue
        lines.append(line)
    # cURL format: append every -H 'name: value' (and -b/--cookie) whose
    # header name wasn't already provided by the raw block above.
    for pair in re.findall(r"-H\s*['\"]([^'\"]+)['\"]", text):
        lines.append(pair)
    m = re.search(r"(?:-b|--cookie)\s+['\"]([^'\"]+)['\"]", text)
    if m:
        lines.append("cookie: " + m.group(1))

    out = []
    seen = set()
    for line in lines:
        name = line.split(":", 1)[0].strip().lower()
        if not name or name in DROP:
            continue
        if ":" not in line:
            continue
        if name in seen:
            # Multiple Cookie: lines are legal and Edge emits them; merge
            # instead of dropping, or sign-in cookies get lost.
            if name == "cookie":
                for i, prev in enumerate(out):
                    if prev.split(":", 1)[0].strip().lower() == "cookie":
                        prev_val = prev.split(":", 1)[1].strip().rstrip(";")
                        add_val = line.split(":", 1)[1].strip().rstrip(";")
                        out[i] = "cookie: " + prev_val + "; " + add_val
                        break
            continue
        seen.add(name)
        out.append(line)
    return "\n".join(out)


def clipboard_text():
    import subprocess
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command", "Get-Clipboard"],
            capture_output=True, timeout=15,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return r.stdout.decode("utf-8", errors="replace")
    except Exception as e:
        print("Clipboard read failed:", e)
        return ""


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "clipboard":
        text = clipboard_text()
        if "cookie" not in text.lower():
            print("Clipboard has no request headers to use.")
            sys.exit(4)
        print("Using headers from the clipboard ...")
    elif len(sys.argv) > 1:
        with open(sys.argv[1], encoding="utf-8", errors="replace") as f:
            text = f.read()
        print("Read headers from", sys.argv[1])
    else:
        print("Paste the request headers now (whole block), then press")
        print("Ctrl+Z and Enter to finish. cURL format also works.\n")
        text = sys.stdin.buffer.read().decode("utf-8", errors="replace")

    cleaned = clean(text)
    if "cookie" not in cleaned.lower():
        print("\nWARNING: no Cookie header found in the paste - it cannot work.")
        print("Copy the request headers of a POST request to music.youtube.com")
        print("while signed in, and try again.")
        sys.exit(2)

    config_dir = os.environ.get("P100_CONFIG_DIR")
    if not config_dir:
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        config_dir = os.path.join(base, "crossfade")
    os.makedirs(config_dir, exist_ok=True)
    dest = os.path.join(config_dir, "browser.json")
    try:
        setup_browser(filepath=dest, headers_raw=cleaned)
    except Exception as e:
        print("Could not parse the headers:", e)
        sys.exit(2)
    print("Wrote", dest)

    print("Verifying login ...", flush=True)
    try:
        yt = YTMusic(dest)
        pl = yt.get_library_playlists(limit=1)
        print("LOGIN OK - YouTube Music library responded.")
        print("Restart crossfade and you are signed in.")
    except Exception as e:
        msg = str(e).strip().splitlines()[0][:300] if str(e) else "unknown"
        print(f"\nHeaders parsed, but YouTube Music rejected them: {msg}")
        print("Most common causes:")
        print("  - music.youtube.com was not signed in when headers were copied")
        print("    (check for a 'Sign in' button on the page)")
        print("  - the browser profile differs from the one you are signed into")
        sys.exit(3)


if __name__ == "__main__":
    main()
