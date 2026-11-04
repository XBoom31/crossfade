# Architecture

```text
src/
  main.cpp            thin entry point -> app_run()
  app/                SDL/GL/ImGui lifecycle, event loop, diagnostics
  core/               public types, AppState, tracked workers, generated version
  playback/           crossfade decisions/state, mpv wrapper, session JSON
  data/               SQLite library, store/cache persistence, paths/config
  net/                yt-dlp backend, textures, updater, Discord RPC
  party/              party API, server, queue/link helpers, guest page
  ui/                 ImGui panels, components, theme, formatting, playlist I/O
external/             vendored ImGui, SQLite, httplib, qrcodegen
py/cf_backend/        shared Python command loop for cookie/OAuth entry points
```

## Data flow

1. `ui_*` panels mutate `AppState` on the UI thread and enqueue work through
   `ytdlp::fetch_*` or `tasks::spawn`.
2. Workers publish immutable snapshots with `std::atomic_store`; the UI takes
   snapshots with `std::atomic_load`.
3. `src/data/library.cpp` is the source of truth for likes, playlists, queue,
   history, play counts/log, saved albums/artists and download metadata.
   Legacy JSON is imported once and kept as the fallback.
4. The Python backend is one line-based command loop. The C++ side spawns
   `p100-backend.py` (cookies) or `p100-backend-oauth.py` (OAuth); both call
   `py/cf_backend/core.py`.

## Persistence

- `settings.json` and `session.json` remain small JSON files.
- Everything user-facing else lives in `library.db` (SQLite + FTS5).
- Caches (album/artist/lyrics/thumb/audio) are files under the config dir.
- A failed DB open leaves the legacy JSON path active; a failed migration
  backs up the JSON first and retries on the next launch.

## Testing

`tests/` contains hand-rolled C++ unit tests for party, queue fairness,
crossfade decisions, library migration/CRUD/FTS, cache eviction, JSON/lyrics
parsing, session, playlists and settings. `tests/test_backend_protocol.py`
drives the shared Python command loop with mocked helpers. CI also builds the
same tests under ASan/UBSan and TSan.
