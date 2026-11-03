<div align="center">

# crossfade

**The YouTube Music player your desktop deserves.**

Instant starts. Offline without thinking about it. Synced lyrics, your real library,
media keys, and yes — real crossfade. Native C++, not a browser tab.

C++17 · SDL2 + OpenGL + Dear ImGui · playback by `mpv` · music data by a Python backend (`ytmusicapi`)

[![Platform](https://img.shields.io/badge/platform-Linux-blue)](#platform-support)
[![Build](https://img.shields.io/badge/build-CMake%20%2B%20Make-green)](#install-linux)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue)](https://isocpp.org/)
[![License](https://img.shields.io/badge/license-MIT-yellow)](LICENSE)

</div>

---

<div align="center">
<img src="docs/screenshots/home.png" alt="crossfade home" width="900"/>
</div>

> Native instead of a browser tab. Instant instead of buffering. Offline without even
> thinking about it. Beautiful, light on your machine, and yours to customize — built for
> people who live in YouTube Music and want better than the web app can give them.

---

## Contents

- [Why crossfade](#why-crossfade)
- [Screens](#screens)
- [Install (Linux)](#install-linux)
- [Sign in](#sign-in)
- [A quick tour](#a-quick-tour)
- [Everyday use](#everyday-use)
- [Controls](#controls)
- [Configuration](#configuration)
- [How it works](#how-it-works)
- [Development](#development)
- [Platform support](#platform-support)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)
- [Acknowledgments](#acknowledgments)

## Why crossfade

- **It feels instant.** Click a song and it plays — search results are URL-prefetched before
  you click, a dedicated play worker guarantees your click never queues behind background
  work, and an optional in-process extractor removes the per-song startup cost entirely.
  Repeat searches come back in milliseconds. No spinner purgatory.
- **Offline, automatically.** Every song you listen to is cached as it streams, so the
  second play starts from your disk — no network, no wait. Want it explicit? Download whole
  albums with one click. A configurable cache cap with least-listened-first eviction keeps
  your disk honest.
- **Beautiful.** Album artwork colours the entire interface with a soft, drifting glow.
  Four animation levels, a light and a dark theme, an accent colour that follows your
  desktop (or the calendar), and text/widget/layout scaling down to per-element fine-tuning.
  It looks the way music should feel.
- **Lightweight. Native. Quiet.** Plain C++17 and mpv — no Electron, no bundled Chromium,
  no 500 MB of JavaScript. The render loop is demand-driven, so the app idles at
  **near-zero CPU** and gets out of the way of your music.
- **Everything the web player can't give you.** Karaoke-style synced lyrics you can click
  to seek, media keys and MPRIS, a sleep timer, a 5-band EQ, volume normalization, playback
  speed, smart **On Repeat** lists, listening stats, an endless radio, M3U/XSPF export, and
  a keyboard-first interface.
- **Your library, actually yours.** Likes, saved albums/artists, local playlists, history,
  and your YouTube account playlists — mirrored locally, with optional write-back to
  YouTube Music.
- **And real crossfade.** Two libmpv players blend tracks into each other — 0–20 s on
  natural ends, a shorter fade on manual skips, instant switches for uncached targets. The
  name is the feature; it's just not the only one.

Press play once: session restore puts you back exactly where you were, the queue keeps
flowing into endless radio, and every track lands in the cache on its way by. It just
keeps going.

## Screens

| Home | Search | Album |
|:---:|:---:|:---:|
| ![Home](docs/screenshots/home.png) | ![Search](docs/screenshots/search.png) | ![Album](docs/screenshots/album.png) |

| Lyrics | Radio | Stats |
|:---:|:---:|:---:|
| ![Lyrics](docs/screenshots/lyrics.png) | ![Radio](docs/screenshots/radio.png) | ![Stats](docs/screenshots/stats.png) |

| Smart lists | Playlists | Liked |
|:---:|:---:|:---:|
| ![Smart](docs/screenshots/smart.png) | ![Playlists](docs/screenshots/playlists.png) | ![Liked](docs/screenshots/liked.png) |

| History | Artists | Up Next |
|:---:|:---:|:---:|
| ![History](docs/screenshots/history.png) | ![Artists](docs/screenshots/artists.png) | ![Queue](docs/screenshots/queue.png) |

| Saved albums | Track actions | Settings |
|:---:|:---:|:---:|
| ![Albums](docs/screenshots/albums.png) | ![Track actions](docs/screenshots/track-actions.png) | ![Settings](docs/screenshots/settings.png) |

## Install (Linux)

### Prebuilt packages (recommended)

Grab the latest file for your distro from [Releases](../../releases):

| File | For |
|---|---|
| `crossfade-*-x86_64.AppImage` | **Anything** — one file, run it. No install. |
| `crossfade-*-amd64.deb` | Debian, Ubuntu, Mint, Pop!_OS (`sudo apt install ./crossfade-*amd64.deb`) |
| `crossfade-*-x86_64.rpm` | Fedora / openSUSE (`sudo dnf install ./crossfade-*x86_64.rpm`) — `mpv-libs` comes from RPM Fusion |
| `crossfade-*-linux-x64.tar.gz` | Manual installs to any prefix |

Arch users: `packaging/arch/PKGBUILD` in this repo builds via `makepkg -si`.

The packages set up the Python backend for you during install (one-time
network fetch of `ytmusicapi` + `yt-dlp`). The AppImage bootstraps it into
`~/.local/share/crossfade/venv` on first launch.

### Build from source

crossfade installs per-user (no root) and manages its own Python environment.

### 1. Native dependencies

A handful of library headers, available in every mainstream distro:

```bash
# Debian / Ubuntu / Mint
sudo apt install build-essential cmake pkg-config libsdl2-dev libgl1-mesa-dev \
                 libmpv-dev libglib2.0-dev libnotify-dev python3 python3-venv curl

# Arch / Manjaro
sudo pacman -S --needed base-devel cmake sdl2 mesa mpv glib2 libnotify python curl

# Fedora
sudo dnf install @development-tools cmake SDL2-devel mesa-libGL-devel \
                 mpv-devel glib2-devel libnotify-devel python3 python3-pip curl
```

You need **mpv ≥ 0.35** (libmpv is what the app links against).

### 2. Build and install

```bash
git clone <this-repo> crossfade
cd crossfade
./install.sh
```

`install.sh`:

- configures and builds a **Release** binary into `build/`;
- installs `~/.local/bin/crossfade` and `~/.local/bin/crossfade-login`;
- installs the backend, requirements, and fonts under `~/.local/share/crossfade/`;
- creates an isolated venv at `~/.local/share/crossfade/venv` with `ytmusicapi` + `yt-dlp`;
- installs a desktop entry (`~/.local/share/applications/crossfade.desktop`) with an
  absolute `Exec=` path, plus the app icon.

Nothing else needs to be installed by hand.

> If `~/.local/bin` is not on your `PATH`, either add it to your shell profile or call the
> binaries by full path (`~/.local/bin/crossfade`, `~/.local/bin/crossfade-login`).

### 3. Sign in

One time — see [Sign in](#sign-in).

### 4. Run

Launch **crossfade** from your application menu, or run `crossfade` in a terminal.

<details>
<summary><b>Other ways to build and run</b></summary>

```bash
# Custom install prefix
PREFIX=/opt/crossfade ./install.sh

# Reinstall data only (skip compiling)
SKIP_BUILD=1 ./install.sh

# Offline install: pre-download wheels into ./wheels first
mkdir wheels && pip download -d wheels -r requirements.txt
./install.sh

# Development loop: build into build/ and run from there (no install)
./mk

# Manual build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/crossfade
```

The installer honours `PREFIX`, `BUILD_DIR`, and `SKIP_BUILD`; the app itself honours
`P100_CONFIG_DIR` (see [Configuration](#configuration)). `./dist.sh` produces a portable
source tarball; `./scripts/crossfade-export.sh` and `./scripts/crossfade-import.sh` move
your data between machines.

</details>

## Install on Windows (one click)

No admin rights, no build tools:

1. Grab **`install-crossfade.cmd`** from the [latest release](https://github.com/XBoom31/crossfade/releases/latest)
   and double-click it.
2. Launch **crossfade** from the Start Menu.
3. Sign in once: Start Menu → **crossfade-login**, and follow the prompts
   (it takes the request headers from your clipboard — see [Sign in](#sign-in)).

The installer sets up everything per-user under `%LOCALAPPDATA%\crossfade`:
the app with every runtime DLL bundled (SDL2, libmpv + codecs, and Mesa
llvmpipe software OpenGL, so it starts even without a GL 3.2 driver — remove
`opengl32.dll`, `libgallium_wgl.dll` and `dxil.dll` from `bin\` to use your
GPU driver instead), a private Python 3.12, the backend venv with
`ytmusicapi` + `yt-dlp`, and Start Menu shortcuts.

## Build on Windows

The same source tree builds on Windows with the MSYS2 UCRT64 toolchain (MinGW-w64) —
the Linux-only desktop integration (MPRIS, libnotify, GLib) is compiled out and
replaced by no-op stubs.

```bash
# In a UCRT64 MSYS2 shell (C:\msys64\ucrt64.exe)
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \
                   mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-SDL2 \
                   mingw-w64-ucrt-x86_64-mpv
git clone <this-repo> crossfade
cd crossfade
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To run the exe outside the MSYS2 shell, copy `SDL2.dll`, `libmpv-2.dll`,
`libstdc++-6.dll`, `libgcc_s_seh-1.dll` and `libwinpthread-1.dll` from
`/ucrt64/bin` next to `crossfade.exe`. The Python backend needs a Windows Python 3
with `ytmusicapi` + `yt-dlp`; without it the UI runs but cannot reach YouTube Music.

## Sign in

crossfade talks to YouTube Music through `ytmusicapi`, which needs your logged-in session
once.

### The easy way: copy request headers

1. Open [music.youtube.com](https://music.youtube.com) in your browser and log in.
2. Open DevTools (**F12**) → **Network** tab.
3. Make a POST request happen: refresh the page or click any song.
4. Find a request to `music.youtube.com` (for example `youtubei/v1/browse`), right-click →
   **Copy → Copy request headers**.
5. In a terminal, run:

   ```bash
   crossfade-login
   ```

6. Paste the headers when prompted. That writes `browser.json` into
   `~/.config/crossfade/`.

Launch crossfade and everything — home feed, likes, playlists, lyrics — is yours.

> **Cookies expire** (weeks to months, Google's choice). When the home feed or search
> suddenly comes back empty, run `crossfade-login` again. It takes about 30 seconds.

> **Privacy tip:** the copied headers contain your full Google session cookies. Consider a
> dedicated browser profile for the header copy so your daily profile's cookies never leave
> it:
>
> ```bash
> firefox -P "crossfade" -no-remote                       # create once, log in there
> chromium --user-data-dir=~/.config/crossfade-browser    # Chromium-family equivalent
> ```

### Alternative: OAuth

An OAuth token is long-lived and refreshes automatically instead of expiring like cookies.

1. Create an OAuth client in the [Google Cloud Console](https://console.cloud.google.com/)
   → **APIs & Services → Credentials**, type **“TVs and Limited Input devices”**.
2. Put the client ID and secret into `CLIENT_ID` / `CLIENT_SECRET` at the top of
   `p100-backend-oauth.py` (replace the bundled credentials).
3. Switch **Settings → YouTube Music → Authentication → OAuth** and restart crossfade.
4. On first start the backend opens a browser login once and saves `browseroauth.json`.

You can also trigger the login-only flow directly:

```bash
~/.local/share/crossfade/venv/bin/python \
    ~/.local/share/crossfade/p100-backend-oauth.py --login
```

> The two backends share the same line-based JSON protocol. If you change one, change the
> other too — see [Development](#development).

## A quick tour

The window has four persistent regions:

| Region | What lives there |
|---|---|
| **Sidebar** | **Browse:** Home, Downloads, Radio, Liked, History, Stats, Smart. **Library:** Albums, Artists, Playlists, YouTube. Settings is pinned at the bottom. |
| **Top bar** | Back / forward, the current view title and subtitle, search, and Refresh where it applies. |
| **Main area** | Track lists, album/artist detail pages, grids, the queue, stats, or the settings panel. |
| **Bottom bar** | Cover art, track + artist, like, previous / play-pause / next, sleep timer, Up Next, lyrics, volume, and the speed preset. |

### Views at a glance

- **Home** — recently searched chips and a “Made for You” list that mirrors the YouTube
  Music home feed (Quick picks, Mixed for you, and similar sections).
- **Downloads** — every cached track, with sort by title/artist/duration/most played and
  a per-track **Delete** action.
- **Radio** — an endless station seeded from your most-played tracks, plus **Start from**
  shortcuts and a station track list.
- **Liked** — liked songs with the same sorting and shuffle controls.
- **History** — a newest-first timeline with play counts, **Play all**, **Shuffle**, and
  **Clear history**.
- **Stats** — total plays, unique songs/artists, top artist, peak hour/day, streak, repeat
  rate, top-artist bars, and listening-by-hour charts for **All Time / This Month / This
  Week**.
- **Smart** — **On Repeat** (your most-played tracks) and **Recently Played**.
- **Albums / Artists** — saved grids; open any card for a detail page with **Play all**,
  save/unsave, and (for albums) download/delete.
- **Playlists** — local playlists on the left, tracks on the right, with create, rename,
  add/remove, M3U/XSPF export, and optional YouTube Music sync.
- **YouTube** — your YouTube Music account playlists, including the auto-generated
  “Liked Music”.
- **Up Next** — the playback queue, with a **Nothing queued** empty state and instructions
  for adding tracks.
- **Settings** — appearance, playback, lyrics, YouTube Music, cache, shortcuts, and a
  danger zone.

## Everyday use

### Search

Click the search box (or press `/` / `Ctrl+F`). Typing debounces briefly, then queries
YouTube Music; **Enter** searches immediately. Results have **Songs / Albums / Artists /
Playlists** tabs. While a search is open, the local list filters as you type. Clear the
query or press **Esc** to go back.

### Playing and queueing

- **Double-click** a row to play it (single tap on touchscreens).
- **Right-click** any track for **Like, Play next, Add to queue, Start Radio, View album,
  View artist, Add to playlist**.
- The bottom-bar **Up Next** button opens the queue; **Play next** inserts at the front,
  **Add to queue** appends.
- **Shuffle** is available on the list views; **Play all** starts from the top.
- **Endless music** (Settings) keeps a radio going when the list and queue run out.

### Downloads and the cache

- Tracks are cached **as they stream**, so the second play is instant.
- An album page has **Download** / **Download (n/m)** / **Delete download**.
- **Settings → Download Cache** shows disk usage, a 0–20 GB cap (0 = unlimited), and
  **Clear cache**.
- When the cap is reached, the **least-listened** tracks are evicted first; the currently
  playing track is never evicted.
- **Repair cached songs** re-fetches missing covers and lyrics for downloaded tracks.

### Radio and discovery

- **Start Radio** on any track seeds an endless station.
- The **Radio** tab also offers **Start from** your most-played tracks and a **New seed**
  button.
- **Endless music** continues with a radio seeded by the last song when everything else
  runs out.

### Lyrics

Open the lyrics panel with the bottom-bar lyrics button or **L**. Synced lines highlight as
the track plays, the active line is enlarged and auto-centered, and **clicking a line seeks
to it**. The panel shows the provider (for example `Synced · LRCLIB`). Adjust timing in
**Settings → Lyrics → Timing offset** (−5 to +5 s). Lyrics are cached per track.

### Library write-back

**Settings → Playback → Sync to YouTube Music** is **off by default**. When enabled, likes
and playlist edits also apply to your signed-in account; each playlist gets a private
server-side mirror that follows renames, additions, removals, reordering, and deletion.
Local-only use is fully supported.

### Desktop integration

- **MPRIS** — media keys, `playerctl`, KDE Connect, and desktop widgets work out of the box.
- **Notifications** — optional desktop notifications on track changes.
- **Discord Rich Presence** — paste an application client ID in Settings; empty means off.
- **Sleep timer** — end of track or 15/30/45/60/90 minutes, with a gentle volume fade.
- **Accent colour** — follows the XDG desktop portal (Plasma, recent GNOME); otherwise a
  date-seeded colour that rolls at local midnight. Override with `P100_ACCENT`.

## Controls

### Keyboard

| Action | Shortcut |
|---|---|
| Play / pause | `Space` |
| Seek backward / forward 5 s | `←` / `→` |
| Seek backward / forward 20 s | `Shift+←` / `Shift+→` |
| Previous / next track | `Ctrl+←` / `Ctrl+→` |
| Volume down / up 5% | `↓` / `↑` |
| Volume down / up 10% | `Shift+↓` / `Shift+↑` |
| Focus search | `/` or `Ctrl+F` |
| Toggle lyrics panel | `L` |
| Toggle sidebar | `S` |
| Back / clear search | `Esc` |
| Shortcut help | `?` |
| ImGui demo (development) | `Ctrl+Shift+D` |

### Mouse and touch

| Action | How |
|---|---|
| Play a track | Double-click a row (single tap on touchscreens) |
| Track actions | Right-click: Like, Play next, Add to queue, Start Radio, View album/artist, Add to playlist |
| Seek | Drag the bottom-bar edge fill, click the seek bar, or click a synced lyric line |
| Volume | Drag the bottom-bar slider or scroll over it |
| Scroll lists | Mouse wheel / touchpad, with inertia |
| Collapse the sidebar | The chevron at the top of the sidebar |
| Queue / sleep / lyrics | The three icons at the right of the bottom bar |

## Configuration

Everything crossfade owns lives in one directory. Resolution order:

1. `$P100_CONFIG_DIR`
2. `$XDG_CONFIG_HOME/crossfade`
3. `~/.config/crossfade`

On first launch, an existing `~/.config/p100` directory is renamed to
`~/.config/crossfade` automatically, so old installs keep their auth, settings, caches, and
history.

| File / directory | What it is |
|---|---|
| `settings.json` | Every Settings-panel option (crossfade, normalization, animations, UI scale, …) |
| `session.json` | Resume state: last track, position, volume, view |
| `browser.json` | **Your cookie auth — treat it like a password** (git-ignored, never shipped) |
| `browseroauth.json` | OAuth token when using OAuth mode |
| `liked.json` | Liked tracks |
| `playlists/` | One JSON file per local playlist |
| `albums.json`, `artists.json` | Saved album/artist metadata |
| `album_cache/`, `artist_cache/` | Cached album/artist headers and track lists (instant reopen, offline) |
| `history.json` | Recently played timeline |
| `play_counts.json`, `play_log.json` | Play counts and per-play log used by Stats |
| `radio_last.cache` | Last radio station, so it reopens instantly |
| `recommendations_cache.json` | Cached Home feed |
| `youtube_playlist_*.json` | Cached mirrors of your YouTube account playlists |
| `audio_cache/` | Streamed/downloaded audio plus `.meta.json` sidecars (size cap configurable) |
| `thumb_cache/` | Cached artwork |
| `lyrics/` | Per-track lyrics cache (`<videoId>.json`) |
| `imgui.ini` | Window layout |
| `backend.log` | Backend stderr — the first place to look when auth or `yt-dlp` misbehaves |
| `config.ini` | Legacy browser setting; superseded by `settings.json` |

> Some internal filenames and environment variables still use the historical `p100`
> prefix (`p100-backend.py`, `P100_CONFIG_DIR`, …). They are kept deliberately so existing
> installs, scripts, and data keep working. The product name is **crossfade**.

### Moving to a new machine

A copy is enough:

```bash
./scripts/crossfade-export.sh                 # everything (~GB, audio dominates)
./scripts/crossfade-export.sh --slim          # settings/auth/library only, re-pull media
# on the new machine:
./install.sh && ./scripts/crossfade-import.sh crossfade-data-*.tar.zst
```

`crossfade-export.sh` writes a `.tar.zst` (or `.tar.gz` when `zstd` is missing) and excludes
`backend.log` and temp files. `--slim` also excludes `audio_cache/` and `thumb_cache/`.

### Environment overrides

| Variable | Purpose |
|---|---|
| `P100_CONFIG_DIR` | Move the whole data root |
| `P100_ACCENT` | Hex accent colour override, e.g. `#6db2d0` |
| `P100_YTDLP` / `P100_PYTHON` / `P100_BACKEND` | Pin `yt-dlp` / Python / backend script |
| `P100_AUTH_FILE` | Custom auth file path |
| `P100_SHARE_DIR` / `P100_ASSETS` | Override installed data/asset directories |
| `P100_DISCORD_CLIENT_ID` | Discord Rich Presence app ID (same as the Settings field) |
| `P100_IMPORT_TMP` | Scratch directory for `crossfade-import.sh` on a large filesystem |

### Useful scripts

| Script | Purpose |
|---|---|
| `scripts/crossfade-login` | Create/refresh `browser.json` |
| `scripts/crossfade-export.sh` | Export data for another machine |
| `scripts/crossfade-import.sh` | Import an exported archive |
| `import-to-mixxx.sh` | Convert the audio cache to tagged MP3s in `~/Music/p100-import` |
| `dist.sh` | Build a portable source tarball |

## How it works

```text
crossfade (C++17, SDL2 + OpenGL + ImGui)
 ├── libmpv player A ─┐
 ├── libmpv player B ─┴── crossfade handoff (end-of-track + manual skips)
 ├── Python backend: main channel  — search, library, lyrics, radio
 ├── Python backend: play channel  — stream-URL resolves (clicks never wait)
 └── yt-dlp                        — in-process extractor or subprocess fallback
```

### Playback and crossfade

Two `libmpv` instances share the work. For a natural track end, the next track is appended
to the current player shortly before the end and mpv's `audio-crossfade` blends them. For a
manual skip, the incoming track starts on the second player, the outgoing player fades out,
and the two swap roles when the fade completes. If the target track is not cached, the skip
is instant instead of waiting for a resolve.

### Backends and data flow

The C++ process keeps **two long-lived Python backends** over stdin/stdout pipes, one JSON
command per line. Splitting them means speculative work (search prefetch, library refresh)
can never delay a user click's stream resolve, and vice versa. The play channel is the only
one that resolves `play` / `play_fresh`; speculative prefetches go to the main channel and
self-throttle.

A typical click:

1. The UI sets a flag in `AppState` (`pending_*`, `next_requested`, …).
2. The main loop picks it up and spawns a detached thread for network work.
3. The thread calls the backend, parses the JSON, and publishes the result under
   `g_data_mutex` / atomics.
4. The next frame renders it; a repaint is requested so the UI does not wait for the idle
   tick.

### Cache

`mpv`'s `stream-record` writes bytes to `audio_cache/` while you listen. When playback ends,
the recording is validated, moved into place, and a `.meta.json` sidecar is written. The
cache has a configurable cap; eviction removes the least-listened tracks first and never
touches the currently playing one. Stream URLs themselves are cached for 2 hours
(`_STREAM_TTL`) and probed before reuse.

### Search, lyrics, and thumbnails

- **Search** runs its sub-queries in parallel (4 workers), caches results for 15 minutes,
  and prefetches stream URLs for the first 10 songs.
- **Lyrics** cascade: YouTube Music native timed → LRCLIB → NetEase → Kugou → lyrics.ovh →
  YouTube Music plain. The first timestamped result wins, with normalized title/artist/
  duration matching so the wrong song's lyrics are never shown.
- **Thumbnails** load asynchronously: curl download → stb_image decode → GPU upload on the
  main thread, with a 60-second backoff for failed URLs.

### Render loop

The loop uses `SDL_WaitEventTimeout()` and a per-activity frame budget: 60 FPS for visible
animation and a short grace window after input, about 10 FPS during steady playback, and
event-driven repaints when fully idle (with a ~1 s safety tick for MPRIS). A full ImGui
layout and GL redraw does not run every millisecond just because audio is playing.

### Module map

| Area | Files |
|---|---|
| Entry point, main loop, crossfade engine, session restore | `main.cpp` |
| Global state, types, layout constants | `ui.h` |
| Backend process management, JSON parsing, audio cache | `ytdlp.h`, `ytdlp.cpp` |
| mpv wrapper | `mpvplayer.h`, `mpvplayer.cpp` |
| Top bar / navigation / search | `ui_top.cpp` |
| Track lists and context menu | `ui_main.cpp` |
| Album and artist pages | `ui_album.cpp`, `ui_artists.cpp` |
| Playlists, queue, radio, YouTube library | `ui_playlists.cpp`, `ui_queue.cpp`, `ui_radio.cpp`, `ui_youtube.cpp` |
| Bottom bar, lyrics, notifications | `ui_bottom.cpp`, `ui_lyrics.cpp`, `ui_notify.cpp` |
| Stats, settings, theme, backdrop, components, icons | `ui_stats.cpp`, `ui_settings.cpp`, `ui_theme.cpp`, `ui_backdrop.cpp`, `ui_components.cpp`, `ui_icons.cpp` |
| Persistence and shared helpers | `ui_helpers.cpp` |
| Async artwork | `texture.h`, `texture.cpp` |
| MPRIS, Discord RPC | `mpris.cpp`, `discord_rpc.cpp` |
| Paths, config, accent | `paths.cpp`, `config.cpp`, `accent.cpp` |
| Python backends | `p100-backend.py`, `p100-backend-oauth.py` |

## Development

```bash
./mk            # build into build/ and run from there
```

Manual build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/crossfade
```

### Ground rules

- **C++17**, vendored ImGui and stb. No new C++ dependencies without discussion.
- **Thread safety:** cross-thread writes go through `AppState`'s mutex/atomics. Network work
  runs on detached threads; the UI thread owns GL and ImGui.
- **Backend protocol:** one command per line, one JSON reply per command. Keep
  `p100-backend.py` and `p100-backend-oauth.py` in sync.
- **Persistence:** user data belongs in the config directory, never next to the binary.
- **Style:** match the surrounding file; prefer small helpers and existing UI components.

### Backend by hand

Useful when debugging auth or a specific command:

```bash
~/.local/share/crossfade/venv/bin/python \
    ~/.local/share/crossfade/p100-backend.py
# Type commands, one per line:
#   recommendations
#   search test query
#   search_all test query
#   album <browse_id>
#   artist <channel_id>
#   radio <video_id>
#   play <video_id>
#   lyrics <video_id>
#   quit
```

A JSON reply means auth and the API work; `{"error": ..., "kind": "auth"}` means re-login.

### Continuous integration

`.github/workflows/build.yml` builds the Linux binary on every push and pull request and
also compiles the Windows port under MSYS2. The CI build is the source of truth for
platform health.

## Platform support

| Platform | Status |
|---|---|
| **Linux** (X11; Wayland via XWayland) | ✅ Supported — the target platform |
| **Windows** | 🚧 In progress — see [`docs/PORTING-WINDOWS.md`](docs/PORTING-WINDOWS.md) |
| **macOS** | ❌ Not started |

What is Linux-only by design: **MPRIS** media control and **desktop notifications**. The
Windows port has stubs for both (plus Discord RPC) until the platform replacements land.
Everything else — playback, crossfade, caching, lyrics, library — is portable C++/Python.

## Troubleshooting

| Problem | Fix |
|---|---|
| Home feed / search suddenly empty | Stale cookies — run `crossfade-login` again |
| “Auth file not found” at startup | Run `crossfade-login`, or point `P100_AUTH_FILE` at your `browser.json` |
| Streams fail to resolve / 403s | Keep `yt-dlp` fresh: `~/.local/share/crossfade/venv/bin/pip install -U yt-dlp`. Also make sure the cookie browser matches your actual browser |
| Wrong browser for cookies | **Settings → YouTube Music → Cookie browser** |
| No sound / mpv errors | Check `mpv --version` (≥ 0.35) and that `mpv <some-url>` plays |
| Backend weirdness | Read `~/.config/crossfade/backend.log` |
| Media keys / `playerctl` don't work | Needs a running D-Bus session; `playerctl -l` should list crossfade |
| Songs start slowly | Enable **Settings → YouTube Music → Fast stream resolve** and update `yt-dlp` |
| Crossfade doesn't happen | Both tracks must be playable; uncached skips switch instantly by design. Check the crossfade duration and **Crossfade manual skips** |
| Cache is huge | **Settings → Download Cache**: set a cap or **Clear cache** |
| Lyrics are out of sync | **Settings → Lyrics → Timing offset** |
| OAuth token rejected | Delete `browseroauth.json`, re-run the OAuth login, restart |
| App won't start on Wayland | Run under XWayland (`SDL_VIDEODRIVER=x11 crossfade`) |

<details>
<summary><b>Reset the auth state from scratch</b></summary>

```bash
# Cookie auth
rm ~/.config/crossfade/browser.json
crossfade-login

# OAuth
rm ~/.config/crossfade/browseroauth.json
~/.local/share/crossfade/venv/bin/python \
    ~/.local/share/crossfade/p100-backend-oauth.py --login
```

</details>

<details>
<summary><b>Inspect the audio cache</b></summary>

```bash
# What is cached, and how big is it?
du -sh ~/.config/crossfade/audio_cache
ls ~/.config/crossfade/audio_cache | head

# Each audio file has a metadata sidecar:
cat ~/.config/crossfade/audio_cache/<videoId>.meta.json
```

</details>

## Contributing

Issues and pull requests are welcome.

```bash
./mk            # build + run from build/
```

- Keep the backend protocol line-based and update both backend scripts together.
- Keep user data in the config directory and out of git.
- Match the existing C++/ImGui patterns; no new dependencies without discussion.
- Test auth, playback, and at least one cached and one uncached track before sending a PR.

## License

[MIT](LICENSE) — free to use, modify, and share.

**Not affiliated with or endorsed by Google/YouTube.** Use your own account and respect
YouTube's terms of service.

## Acknowledgments

- [ytmusicapi](https://github.com/sigma67/ytmusicapi) — the reverse-engineered YouTube Music
  API this whole thing runs on
- [yt-dlp](https://github.com/yt-dlp/yt-dlp) — stream extraction
- [mpv](https://mpv.io/) / libmpv — playback, crossfade, `stream-record`
- [Dear ImGui](https://github.com/ocornut/imgui) · [SDL2](https://www.libsdl.org/) ·
  [stb_image](https://github.com/nothings/stb) · [GLib](https://docs.gtk.org/glib/)

---

<div align="center">
<i>Built for personal use, shared in case it's useful.</i>
</div>
