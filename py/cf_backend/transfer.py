#!/usr/bin/env python3
"""Cross-platform data transfer for crossfade (Linux/Windows/macOS).

Uses only the Python standard library so it works with the bundled venv.

Export:
    transfer.py export --zip BACKUP.zip --config CONFIG_DIR \
        --sections settings,auth,library,album_cache,artist_cache,lyrics,thumbs,audio,logs \
        [--library SNAPSHOT_DB] [--version 0.3.1]

Import (normally started by the app; it waits for the app to exit):
    transfer.py import --zip BACKUP.zip --config CONFIG_DIR \
        [--wait-ready READY_FILE] [--result RESULT.json]
"""

import argparse
import json
import os
import shutil
import sys
import time
import zipfile
from pathlib import Path, PurePosixPath

SECTIONS = {
    "settings": {
        "files": ["settings.json", "session.json", "imgui.ini"],
    },
    "auth": {
        "files": ["browser.json", "browseroauth.json", "spotify.json"],
    },
    "library": {
        "files": [
            "library.db", "liked.json", "history.json", "queue.json",
            "play_counts.json", "play_log.json", "saved_albums.json",
            "saved_artists.json", "saved_podcasts.json",
            "recommendations_cache.json", "search_recent.txt",
            "radio_last.cache", "youtube_playlists_cache.json",
        ],
        "globs": ["youtube_playlist_*.json"],
        "dirs": ["playlists"],
    },
    "album_cache": {"dirs": ["album_cache"]},
    "artist_cache": {"dirs": ["artist_cache"]},
    "lyrics": {"dirs": ["lyrics"]},
    "thumbs": {"dirs": ["thumb_cache"]},
    "audio": {"dirs": ["audio_cache"]},
    "logs": {
        "files": ["backend.log"],
        "globs": ["crash-*.log", "hang-*.log", "diagnostics-*.txt"],
    },
}

SECTION_NAMES = list(SECTIONS.keys())
DEFAULT_SECTIONS = ["settings", "auth", "library", "album_cache", "artist_cache", "lyrics", "thumbs"]

# Never put transient state or previous backups into a new archive.
EXCLUDED_NAMES = {
    "library.db-wal", "library.db-shm",
    "transfer-result.json", ".transfer-ready", "imgui.ini.bak",
}
EXCLUDED_PREFIXES = (".crossfade-import-", ".crossfade-pre-import-")
EXCLUDED_SUFFIXES = (".tmp", ".zip", ".part")
STORED_SUFFIXES = (".webm", ".opus", ".m4a", ".mp3", ".flac", ".aac", ".ogg",
                   ".jpg", ".jpeg", ".png", ".webp", ".gif")


def parse_sections(raw):
    if not raw:
        return list(DEFAULT_SECTIONS)
    out = []
    for name in raw.split(","):
        name = name.strip()
        if name in SECTIONS and name not in out:
            out.append(name)
    return out


def excluded(rel, include_logs):
    parts = rel.parts
    if any(p in (".tmp", "__pycache__") for p in parts):
        return True
    name = rel.name
    if name in EXCLUDED_NAMES:
        return True
    if name.startswith(EXCLUDED_PREFIXES):
        return True
    if name.endswith(EXCLUDED_SUFFIXES):
        return True
    if not include_logs and (name.startswith("crash-") or name.startswith("hang-")
                             or name.startswith("diagnostics-") or name == "backend.log"):
        return True
    return False


def collect(config, sections, library_snapshot, skipped):
    files, dirs = [], []

    def add_file(path, rel=None):
        rel = Path(rel) if rel is not None else path.relative_to(config)
        if excluded(rel, "logs" in sections):
            return
        if path.is_file() and rel.as_posix() not in files:
            files.append(rel.as_posix())

    for section in sections:
        spec = SECTIONS[section]
        for name in spec.get("files", []):
            path = config / name
            if name == "library.db" and library_snapshot:
                path = Path(library_snapshot)
            if path.is_file():
                add_file(path, Path(name) if name == "library.db" else None)
            elif not excluded(Path(name), "logs" in sections):
                skipped.append(name)
        for pattern in spec.get("globs", []):
            for path in sorted(config.glob(pattern)):
                if path.is_file():
                    add_file(path)
        for name in spec.get("dirs", []):
            root = config / name
            if not root.is_dir():
                skipped.append(name + "/")
                continue
            dirs.append(name)
            for path in root.rglob("*"):
                if path.is_file():
                    add_file(path)

    # Stable order makes the archive reproducible and easier to inspect.
    files = sorted(set(files))
    dirs = sorted(set(dirs))
    return files, dirs


def write_archive(args):
    config = Path(args.config).expanduser().resolve()
    zip_path = Path(args.zip).expanduser().resolve()
    sections = parse_sections(args.sections)
    if not sections:
        raise SystemExit("No valid sections selected")
    zip_path.parent.mkdir(parents=True, exist_ok=True)

    skipped = []
    files, dirs = collect(config, sections, args.library, skipped)

    manifest = {
        "app": "crossfade",
        "format": 1,
        "version": args.version or "",
        "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "platform": sys.platform,
        "sections": sections,
        "files": files,
        "dirs": dirs,
    }

    stored_roots = {"audio": "audio_cache/", "thumbs": "thumb_cache/"}
    stored_prefixes = tuple(prefix for section, prefix in stored_roots.items() if section in sections)

    tmp_zip = zip_path.with_name(zip_path.name + ".part")
    with zipfile.ZipFile(tmp_zip, "w", allowZip64=True, compression=zipfile.ZIP_DEFLATED,
                         compresslevel=6) as zf:
        zf.writestr("manifest.json", json.dumps(manifest, indent=2))
        for rel in files:
            source = (Path(args.library) if rel == "library.db" and args.library
                      else config / rel)
            if not source.is_file():
                continue
            stored = rel.startswith(stored_prefixes) or rel.lower().endswith(STORED_SUFFIXES)
            zf.write(source, rel, compress_type=(zipfile.ZIP_STORED if stored
                                                 else zipfile.ZIP_DEFLATED))
    os.replace(tmp_zip, zip_path)
    print(json.dumps({"ok": True, "zip": str(zip_path), "files": len(files),
                      "dirs": dirs, "skipped": skipped}))


def safe_rel(name):
    rel = PurePosixPath(name)
    if not rel.parts:
        return None
    if rel.is_absolute() or ".." in rel.parts:
        return None
    first = rel.parts[0]
    if ":" in first or first.startswith("\\"):
        return None
    return Path(*rel.parts)


def wait_for_ready(path, timeout=180):
    if not path:
        return True
    start = time.time()
    while time.time() - start < timeout:
        if Path(path).exists():
            try:
                Path(path).unlink()
            except OSError:
                pass
            return True
        time.sleep(0.25)
    return False


def write_result(path, ok, message, backup=None):
    payload = {"ok": bool(ok), "message": message, "backup": backup or "",
               "time": time.strftime("%Y-%m-%dT%H:%M:%S%z")}
    if path:
        try:
            Path(path).write_text(json.dumps(payload), encoding="utf-8")
        except OSError:
            pass
    print(json.dumps(payload))
    return 0 if ok else 1


def import_archive(args):
    config = Path(args.config).expanduser().resolve()
    zip_path = Path(args.zip).expanduser().resolve()
    result = args.result

    if not wait_for_ready(args.wait_ready):
        return write_result(result, False, "crossfade did not close in time; nothing was imported.")

    if not zip_path.is_file():
        return write_result(result, False, f"Backup not found: {zip_path}")

    temp = config.parent / (".crossfade-import-" + str(os.getpid()))
    backup = config.parent / ("crossfade-pre-import-" + time.strftime("%Y%m%d-%H%M%S")
                              + "-" + str(os.getpid()))
    try:
        if temp.exists():
            shutil.rmtree(temp)
        temp.mkdir(parents=True)
        with zipfile.ZipFile(zip_path) as zf:
            if "manifest.json" not in zf.namelist():
                raise RuntimeError("Not a crossfade backup (manifest.json missing)")
            manifest = json.loads(zf.read("manifest.json").decode("utf-8"))
            if manifest.get("app") != "crossfade":
                raise RuntimeError("Not a crossfade backup")
            for info in zf.infolist():
                if info.is_dir() or info.filename == "manifest.json":
                    continue
                rel = safe_rel(info.filename)
                if rel is None:
                    raise RuntimeError(f"Unsafe path in backup: {info.filename}")
                target = temp / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(info) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst)

        files = [f for f in manifest.get("files", []) if safe_rel(f) is not None]
        dirs = [d for d in manifest.get("dirs", []) if safe_rel(d) is not None]
        if not files and not dirs:
            raise RuntimeError("Backup contains no data sections")

        backup.mkdir(parents=True, exist_ok=True)
        applied = []
        try:
            # Directories are renamed (fast, even for multi-GB audio caches).
            for rel in dirs:
                src = temp / rel
                dst = config / rel
                if not src.is_dir():
                    continue
                saved = backup / rel
                if dst.exists():
                    saved.parent.mkdir(parents=True, exist_ok=True)
                    shutil.move(str(dst), str(saved))
                applied.append(("dir", rel, saved if saved.exists() else None))
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(src), str(dst))
            # Files are small; move the old one aside then the new one in.
            for rel in files:
                src = temp / rel
                dst = config / rel
                if not src.is_file():
                    continue
                saved = backup / rel
                if dst.exists():
                    saved.parent.mkdir(parents=True, exist_ok=True)
                    shutil.move(str(dst), str(saved))
                applied.append(("file", rel, saved if saved.exists() else None))
                dst.parent.mkdir(parents=True, exist_ok=True)
                os.replace(src, dst)
        except Exception:
            # Roll back what we already applied.
            for kind, rel, saved in reversed(applied):
                dst = config / rel
                try:
                    if dst.exists():
                        if dst.is_dir():
                            shutil.rmtree(dst)
                        else:
                            dst.unlink()
                    if saved and saved.exists():
                        if kind == "dir":
                            shutil.move(str(saved), str(dst))
                        else:
                            shutil.move(str(saved), str(dst))
                except OSError:
                    pass
            raise
        return write_result(result, True,
                            f"Imported {len(applied)} item(s) from {zip_path.name}.",
                            str(backup))
    except Exception as e:
        return write_result(result, False, f"Import failed: {e}")
    finally:
        try:
            shutil.rmtree(temp)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="command", required=True)

    exp = sub.add_parser("export")
    exp.add_argument("--zip", required=True)
    exp.add_argument("--config", required=True)
    exp.add_argument("--sections", default="")
    exp.add_argument("--library", default=None)
    exp.add_argument("--version", default="")

    imp = sub.add_parser("import")
    imp.add_argument("--zip", required=True)
    imp.add_argument("--config", required=True)
    imp.add_argument("--wait-ready", default=None)
    imp.add_argument("--result", default=None)

    args = ap.parse_args()
    if args.command == "export":
        write_archive(args)
        return 0
    return import_archive(args)


if __name__ == "__main__":
    sys.exit(main())
