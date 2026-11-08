#!/usr/bin/env python3
"""Re-download the audio cache for tracks recorded in library.db.

After a config/cache loss, library.db still knows which tracks had local
audio files (their URL points at audio_cache/<id>.<ext>). This script asks
yt-dlp for the same bestaudio stream, stores it under the same id, and writes
the .meta.json sidecar the Downloads tab expects.

Usage:
    scripts/redownload-from-library.py --dry-run
    scripts/redownload-from-library.py
    scripts/redownload-from-library.py --all          # every track in library.db
    scripts/redownload-from-library.py --no-cookies   # public tracks only

Existing <id>.<ext> files are skipped, so it is safe to re-run.
"""

import argparse
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path

EXTS = ("opus", "m4a", "mp3", "webm", "flac", "aac", "ogg")
ID_RE = re.compile(r"^[A-Za-z0-9_-]{6,32}$")


def default_config() -> Path:
    return Path(os.environ.get("P100_CONFIG_DIR", "~/.config/crossfade")).expanduser()


def ytdlp_bin() -> str:
    env = os.environ.get("P100_YTDLP")
    if env:
        return env
    venv = Path.home() / ".local/share/crossfade/venv/bin/yt-dlp"
    if venv.is_file() and os.access(venv, os.X_OK):
        return str(venv)
    found = shutil.which("yt-dlp")
    return found or "yt-dlp"


def find_audio(cache: Path, video_id: str) -> Path | None:
    for ext in EXTS:
        p = cache / f"{video_id}.{ext}"
        if p.is_file():
            return p
    return None


def write_sidecar(audio: Path, row) -> None:
    (video_id, title, artist, artist_id, album, album_id, track_number,
     thumbnail_url, duration_sec) = row
    meta = {
        "id": video_id,
        "title": title or "",
        "artist": artist or "",
        "artist_id": artist_id or "",
        "album": album or "",
        "album_id": album_id or "",
        "track_number": int(track_number or 0),
        "thumbnail_url": thumbnail_url or "",
        "duration_sec": int(duration_sec or 0),
    }
    audio.with_name(audio.name + ".meta.json").write_text(
        json.dumps(meta, ensure_ascii=False, separators=(",", ":")), encoding="utf-8")


def run(cmd: list[str]) -> int:
    try:
        return subprocess.run(cmd, check=False).returncode
    except FileNotFoundError:
        print(f"error: {cmd[0]} not found", file=sys.stderr)
        return 127


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=str(default_config()))
    ap.add_argument("--db", default=None)
    ap.add_argument("--all", action="store_true",
                    help="include every track in library.db, not just cached ones")
    ap.add_argument("--no-cookies", action="store_true")
    ap.add_argument("--cookies-from-browser", default="firefox")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    config = Path(args.config).expanduser()
    db = Path(args.db) if args.db else config / "library.db"
    cache = config / "audio_cache"
    if not db.is_file():
        print(f"error: library not found: {db}", file=sys.stderr)
        return 1
    cache.mkdir(parents=True, exist_ok=True)

    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    where = "" if args.all else "where url like '%audio_cache%'"
    rows = con.execute(
        f"select video_id,title,artist,artist_id,album,album_id,track_number,"
        f"thumbnail_url,duration_sec from tracks {where} order by rowid").fetchall()
    con.close()

    todo = []
    seen = set()
    for row in rows:
        vid = row[0]
        if vid in seen or not ID_RE.match(vid or ""):
            continue
        seen.add(vid)
        if find_audio(cache, vid) is None:
            todo.append(row)
    if args.limit:
        todo = todo[:args.limit]

    print(f"library tracks: {len(rows)}  unique ids: {len(seen)}  to download: {len(todo)}")
    if args.dry_run:
        for row in todo[:20]:
            print(f"  {row[0]}  {row[1]}")
        if len(todo) > 20:
            print(f"  ... and {len(todo) - 20} more")
        return 0
    if not todo:
        return 0

    ytdlp = ytdlp_bin()
    cookie = [] if args.no_cookies else ["--cookies-from-browser", args.cookies_from_browser]
    ok = failed = 0
    for i, row in enumerate(todo, 1):
        vid, title = row[0], row[1] or row[0]
        print(f"[{i}/{len(todo)}] {vid}  {title}")
        out = str(cache / f"{vid}.%(ext)s")
        base = [ytdlp, "-f", "bestaudio", "--no-playlist", "--newline",
                "-o", out, f"https://www.youtube.com/watch?v={vid}"]
        rc = run(base[:1] + cookie + base[1:])
        if rc != 0 and cookie:
            print("  cookie attempt failed; retrying without cookies")
            rc = run(base)
        audio = find_audio(cache, vid)
        if rc == 0 and audio:
            write_sidecar(audio, row)
            ok += 1
        else:
            failed += 1
            print("  download failed")
    print(f"done: {ok} downloaded, {failed} failed, {len(todo)} attempted")
    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
