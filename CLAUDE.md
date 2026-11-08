# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
# Development loop: build into build/ and run from there (no root, no install)
./mk

# Per-user install into ~/.local (builds, installs, creates the backend venv)
./install.sh
p100
```

Manual build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/p100
```

**Dependencies:**
- SDL2, OpenGL, CMake, mpv, glib-2.0/gio-2.0, libnotify
- Python 3 with `ytmusicapi` and `yt-dlp` in an isolated venv
- For browser auth: run `p100-login` (or `./scripts/p100-login`) to create
  `<config>/browser.json`

`./install.sh` installs to `~/.local` by default:

```text
~/.local/bin/p100
~/.local/bin/p100-login
~/.local/share/p100/p100-backend.py
~/.local/share/p100/p100-backend-oauth.py
~/.local/share/p100/assets/fonts/
~/.local/share/p100/venv/
~/.local/share/applications/p100.desktop
```

Override the prefix with `PREFIX=/some/prefix ./install.sh`. For an offline
install, put wheels in `wheels/` at the project root; the installer then uses
`pip --no-index --find-links wheels`.

**Python backend authentication:** The app communicates with YouTube Music via a Python backend process. Two variants exist:
- `p100-backend.py` — uses browser cookies (`browser.json`, generated via `ytmusicapi browser`)
- `p100-backend-oauth.py` — uses OAuth (hardcoded client ID/secret; first run opens browser for login)

The backend is selected from the auth mode stored in `settings.json` (Settings →
Authentication), defaulting to browser cookies.

**Build from scratch after checkout** only requires the native dependencies and
`python3`; `./install.sh` creates the venv and installs the Python packages.

## Architecture

Desktop YouTube Music client. C++ frontend (SDL2 + OpenGL + Dear ImGui) drives the UI; mpv handles playback; a Python backend process bridges to YouTube Music's API.

### Process structure

```
p100 binary (C++)
  ├── mpv (libmpv)          — audio playback, gapless/crossfade
  ├── Python backend (fork) — stdin/stdout JSON protocol, communicates with ytmusicapi
  └── yt-dlp subprocess     — audio stream URL extraction, audio file caching
```

The C++ process forks the Python backend at startup, communicating via pipes. Each command (`recommendations`, `search <q>`, `radio <id>`, `album <browse_id>`, `artist <channel_id>`, `artist_search <q>`, `lyrics <id>`) sends a line to stdin, reads a JSON line from stdout. The backend is long-lived, processing one command per line. The `lyrics <id>` command is metadata-only (no stream resolution/audio), so it works for tracks served from the local audio cache.

`lyrics <id>` runs a multi-provider cascade (first timestamped result wins, else the first plain one): YouTube Music native timed lyrics (unauthenticated mobile browse) → LRCLIB → NetEase Cloud Music → Kugou → lyrics.ovh → YouTube Music native plain (LyricFind). Search hits are matched on normalised title (+artist/duration) so lyrics for the wrong song are never returned.

### Module overview

| File | Role |
|---|---|
| `main.cpp` | Thin entry point that calls `app_run()` |
| `app/app.cpp` | SDL/mpv/ImGui init, main loop, session restore, shutdown |
| `app/app_input.cpp` | Touch/trackpad/wheel gesture translation and momentum scrolling |
| `app/app_playback.cpp` | Queue building, stream resolution, recording, crossfade and player events |
| `app/app_workers.cpp` | Background search, recommendations, lyrics and cover/download workers |
| `app/app_internal.h` | Shared app-module globals, types and function declarations |
| `app/diagnostics.cpp` | Crash logs, watchdog, boot marks and the full diagnostics bundle |
| `app/warm_start.cpp` | Single-instance UNIX socket, warm start and show/hide/quit IPC |
| `app/data_transfer.cpp` | In-app ZIP backup/restore orchestration + detached import helper |
| `py/cf_backend/transfer.py` | Cross-platform stdlib ZIP export/import with section selection |
| `ui.h` | `AppState` struct (global state), `Track`/`Playlist`/`Settings`/`SessionState` types, UI function declarations, layout constants |
| `ui_helpers.cpp` | Shared UI helpers, playlist source resolution, scroll/content fades |
| `data/store.cpp` | JSON persistence fallbacks (`liked.json`, `history.json`, counts, log, queue) when SQLite is unavailable |
| `data/cache.cpp` | Album/artist cache files and lyrics cache |
| `ui_hero.cpp` | Shared hero band used by Radio, list pages and Podcasts |
| `core/json_util.h` | Shared JSON escaping/parsing and track serialization |
| `ui_top.cpp` | Top navigation bar (Home/Downloads/Radio/Liked/Albums/Artists/Playlists/Settings) + search bar with debounce |
| `ui_main.cpp` | Main content area — track list with thumbnails, right-click context menu (like/radio/view album/view artist/save album/save artist/add to playlist), double-click to play |
| `ui_bottom.cpp` | Bottom bar — album art, track info, prev/play-pause/next buttons, custom seek bar, volume slider |
| `ui_playlists.cpp` | Left-right split panel — playlist list on left, track list on right |
| `ui_album.cpp` | Album detail view (drill-down with save/unsave, album download) + saved albums grid |
| `ui_artists.cpp` | Artist detail view (popular songs + albums/singles, save/unsave) + saved artists grid + artist search results |
| `ui_settings.cpp` | Settings panel (crossfade, volume normalization, notifications, lyrics timing offset, browser choice) + "danger zone" reset buttons |
| `ui_podcasts.cpp` | Podcast library, search, show pages and episode lists |
| `ui_lyrics.cpp` | Lyrics side panel on list tabs — Spotify-style treatment (enlarged active line, distance dimming, glow-weighted auto-center scroll, per-line morph on verse change, edge fades), load/retry states; toggled from the bottom bar |
| `ytdlp.h` / `ytdlp.cpp` | Python backend process management (fork/exec, pipe I/O), JSON parsing (custom, no dependency), audio caching via yt-dlp, `Track` struct |
| `mpvplayer.h` / `mpvplayer.cpp` | mpv wrapper — play/stop/pause/seek/volume, event callback for start/end/error |
| `texture.h` / `texture.cpp` | Async thumbnail loading pipeline — curl download → STB decode → GPU upload (thread-safe queue) with failed-URL backoff |
| `mpris.h` / `mpris.cpp` | MPRIS D-Bus integration (org.mpris.MediaPlayer2.p100) via raw GDBus — Play/Pause/Next/Prev/Seek/Stop, metadata updates |
| `paths.h` / `paths.cpp` | Config/share/asset path resolution (`P100_CONFIG_DIR`, XDG, installed vs dev tree) |
| `accent.h` / `accent.cpp` | Accent colour: `P100_ACCENT`, XDG portal, daily date-seeded fallback |
| `daily_accent.h` | In-tree deterministic date-seeded RGB helper |
| `p100-backend.py` | Python backend (browser cookie auth via `browser.json`) |
| `p100-backend-oauth.py` | Python backend (OAuth variant) |
| `scripts/p100-login` | Browser-cookie auth helper (writes `<config>/browser.json`) |
| `scripts/p100-export.sh` / `scripts/p100-import.sh` | Full/slim data migration |
| `install.sh` / `dist.sh` | Per-user install and source tarball |

### AppState — global state hub

`AppState` (in `ui.h`) is the central shared state struct. It's passed by reference to every UI function and mutated directly (with `std::atomic` for cross-thread flags). Key groups:

- **Playlists/tracks:** `recommendations`, `search_results`, `downloaded_tracks`, `radio_results`, `liked_tracks`, `playlists[]` — all `std::vector<Track>`
- **Current playback:** `current_title`, `current_artist`, `current_track_id`, `current_track_url`, `current_track_index`
- **Pending play requests:** `pending_*` fields consumed by main loop to kick off audio resolution
- **User controls:** `next_requested`, `prev_requested`, `user_pause_requested`, `user_play_requested`, `search_dirty`, `refresh_requested` — set by UI, consumed by main loop
- **Playlist source tracking:** `current_playlist_source` (Home/Search/Downloads/Radio/Liked/Playlist/Album/Artist) determines which vector drives the current view

### Main loop power management

Data transfer (Settings → System → Data transfer) exports selected sections
(settings/auth/library/caches/lyrics/thumbs/audio/logs) to one portable ZIP and
imports it on another OS. Import is a detached helper that waits for the app to
exit, replaces only the sections present, and keeps a `crossfade-pre-import-*`
safety copy. Playback rebases old absolute audio-cache paths by track id.

Diagnostics (Settings → System → Diagnostics) captures boot marks, backend
channel counters, named background-task timings, system/GL/tool versions, cache
stats, app state, recent logs and the newest crash log. The detailed history is
toggleable (`diagnostics_history`); boot marks and live counters stay on.

Warm start (Settings → System → Warm start, `--warm`) keeps one background
instance and routes later launches over a UNIX socket (`$XDG_RUNTIME_DIR/crossfade.sock`):
`crossfade` raises it, `--toggle` hides/shows, `--quit` stops it. The window can
start hidden; `warm_start_mode` controls whether all backends, only the main one,
or none stay resident while parked. `--new-instance` bypasses the socket.

The main loop uses `SDL_WaitEventTimeout()` with a per-activity frame budget: 16 ms for animation/input, 100 ms during steady playback, 1 s when fully idle, and a 5 s safety frame. ImGui only renders when a frame is actually due. Low power mode (Settings → System → Power, `--low-power`, or `P100_LOW_POWER=1`) uses 33 ms for interaction, 250 ms during playback and 2 s idle, disables motion, and skips speculative prefetch/downloads.

### Data flow

1. User action (click, search) → sets flag in `AppState`
2. Main loop detects flag → spawns detached `std::thread` for network work
3. Background thread calls Python backend → parses JSON → writes result into `AppState` (under `g_data_mutex`)
4. Next frame renders updated data

### Persistence locations

All user data lives in one directory, resolved as `$P100_CONFIG_DIR`, then
`$XDG_CONFIG_HOME/crossfade`, then `~/.config/crossfade` (legacy `p100` config
directories are migrated once on first launch).

- `session.json` — last-played track, position, volume, playlist source (saved every 10s + on quit)
- `settings.json` — crossfade duration, volume normalization, notifications, browser choice, lyrics timing offset (±5 s)
- `liked.json` — liked tracks
- `albums.json` — saved albums (metadata for the permanent Albums tab)
- `artists.json` — saved artists (metadata for the permanent Artists tab)
- `album_cache/<browse_id>.json` — cached album header + track list (instant reopen/offline)
- `artist_cache/<channel_id>.json` — cached artist header + top songs + albums/singles (instant reopen/offline)
- `playlists/*.json` — one file per playlist
- `audio_cache/` — downloaded audio files (opus/m4a/mp3) with `.meta.json` sidecars
- `thumb_cache/` — cached thumbnail images
- `lyrics/` — per-track lyrics cache (`<videoId>.json`; synced lines + time offsets)
- `imgui.ini` — window layout (used to live next to the binary)
- `backend.log` — Python backend stderr (yt-dlp probe noise)

### Notable patterns

- **Custom JSON parser:** No nlohmann/json or similar — `ytdlp.cpp` has hand-rolled `json_get_string()`, `json_get_int()`, `parse_track_array()` with Unicode escape support
- **Async texture loading:** `get_texture()` fires a detached thread that downloads → decodes (stb_image) → pushes to a queue; `process_texture_queue()` consumes it on the main GL thread each frame
- **Failed URL backoff:** 60-second window before retrying a failed thumbnail download, preventing continuous subprocess spawning for dead URLs
- **Search debounce:** 1-second quiet period before dispatching a search (tracked in `ui_top.cpp` statics with `search_id` to discard stale in-flight results)
- **MPRIS via raw GDBus:** Interface built from XML strings at runtime with `g_dbus_connection_register_object` — no auto-generated bindings
- **No external C++ dependencies** beyond SDL2, OpenGL, mpv, glib, libnotify, and vendored ImGui/stb
