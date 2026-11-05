# Changelog

All notable changes to crossfade are documented here.

## Unreleased

## 0.3.0 - 2026-11-05

### Added

- **Podcasts.** A first-class Podcasts tab backed by `ytmusicapi`: browse your
  saved shows, search the YouTube Music podcast catalogue, open a show with its
  recent episodes, play episodes through the normal queue, save shows locally,
  and download a single episode or a whole show for offline listening.
- **Audio-reactive visualizer.** A new full-canvas Visualizer page with Bars,
  Waves and Orbits modes. It captures the default PipeWire/PulseAudio monitor
  with `parec` and runs a real FFT on the audio, so the spectrum, waveform,
  radial spokes and bass pulses follow the music instead of a canned animation.
  Falls back gracefully when capture is unavailable.
- **Spotify playlist import.** Paste a public Spotify playlist link in
  Playlists; crossfade reads its track list and matches each song on YouTube
  Music, then creates a local playlist. Optional Spotify sign-in stores an
  `sp_dc` cookie for private playlists.
- **Seamless Google sign-in.** Settings can open a real WebKitGTK sign-in popup
  (WebKit 6/GTK4 or WebKit2GTK 4.1/GTK3) and write `browser.json` directly once
  the session cookie appears. If WebKitGTK is missing it imports the existing
  Firefox session instead. No OAuth client keys or manual header copying.
- **Visualizer visibility toggle** in Settings, since the page is still
  experimental.

### Changed

- **Settings redesigned.** A two-pane layout with a vertical category rail,
  rounded section cards, a live scale preview, custom sliders, switches and
  segmented controls. The centred column is gone; content stays left-aligned
  and reflows cleanly at 75%, 100% and 200% scale.
- **Fine-tune works for every element.** Top navigation, search bar, bottom bar,
  track thumbnails, row spacing, bottom cover, transport controls, seek/volume
  bars and the lyrics cover all honour their individual multiplier.
- **AppImage** now ships `libjack.so.0` and puts the bundled library directory
  on the loader path, fixing the startup failure on systems without JACK.


## 0.2.0 - 2026-11-05

### Added

- **Local SQLite library.** Likes, playlists, the queue, history, play counts and log,
  saved albums/artists, and downloaded-track metadata now live in `library.db`, backed
  by the vendored SQLite amalgamation with FTS5. Audio files stay in `audio_cache/`.
  `settings.json` and `session.json` remain JSON. Existing JSON data is imported once,
  backed up to `library-backup-*`, and left in place as a fallback if the database can't
  be opened.
- **Local library in every search.** Normal search now merges library matches ahead of
  YouTube results, so downloaded and liked tracks are one query away. Search animates
  its first batch once and only new rows after that.
- **Diagnostics panel** in Settings: copy a diagnostics bundle, run an SQLite integrity
  check, back up the library, open the config folder, view recent log lines, and toggle
  **Safe mode on next launch**. Crashes now write local `crash-*.log` (Linux) or
  `crash-*.dmp` (Windows) files.
- **Undo** for clearing Up Next, deleting a playlist, and the Danger Zone reset buttons.
- **Onboarding copy** in the empty Home state.
- **Tests and CI.** CTest now covers the library, party queue/link helpers, crossfade
  decisions, cache eviction, JSON/lyrics parsing, sessions, playlists and settings.
  CI runs GCC and Clang builds, ASan/UBSan, TSan, the Python backend protocol test, and
  a `scripts/bench.sh` resource report.

### Changed

- Empty Home states now point at search, party mode, and the shortcut help instead of
  only suggesting a refresh.
- Search and Refresh stay pinned to the right edge and adapt to narrow windows.
- The source tree is split under `src/` (app, core, playback, data, net, ui, party).
- Cookie and OAuth backends share one command loop in `py/cf_backend/core.py`.
- Short-lived workers are tracked and drained on shutdown; SIGTERM exits cleanly and
  closes SQLite without leaving a WAL behind.

## 0.1.10 - 2026-11-04

### Added

- Artist pages now keep the original five top songs and offer a **Load 5 more
  songs** button, paging through the artist's full songs shelf. The backend
  caches the full list per artist, and expanded lists persist in the artist
  cache.

### Changed

- Crossfade visuals now switch the bottom bar metadata, progress, lyrics and
  backdrop around the visual midpoint. The volume slider stays at the user
  volume instead of showing the temporary fade gain.
- Added the optional **Smooth backdrop shader**: one full-screen pass with
  in-shader dithering, replacing the layered ImGui ellipses. It is toggleable
  in Settings and syncs its colour wash with the crossfade midpoint.
- Removed the dark panel-wide tab-switch fade; scroll edges now fade the
  content itself (track rows, media cards and lyric lines) so the backdrop is
  never covered by a sampled colour band.
- Radio seed cards keep their entrance pop but no longer fade their cover
  images, and the Station Tracks list waits for the seed cards to settle.
- README Party-mode docs refreshed with new host and Present QR screenshots.

## 0.1.9 - 2026-11-04

### Added

- **Windows auto-update.** At startup the app checks the latest GitHub release
  and offers **Update now** in a toast. The updater launches the one-click
  installer in its own console, which installs the new build and asks whether
  to restart crossfade. Linux remains on package-manager/manual updates.

## 0.1.8 - 2026-11-04

### Fixed

- Party guest page: cover art in Up Next now resolves for host playlist tracks
  too, not only the current song and guest requests. The host registers cached
  artwork for the next 40 queue rows and downloads missing thumbnails in the
  background; the phone retries until they are ready.

## 0.1.7 - 2026-11-04

### Added

- `Q` opens the Up Next queue, matching the bottom-bar queue button.

### Fixed

- **One global Up Next queue.** Starting a playlist/album/artist/radio now
  replaces the queue with that list, and both manual Next and natural
  end-of-track pop the same queue. Previously manual Next skipped the queue,
  so guest party requests (and other queued songs) only played when a track
  ended by itself.
- Guest requests still outrank the host playlist but stay below explicit
  **Play next** pins; the party queue cap now counts guest requests instead of
  the whole host playlist.

## 0.1.6 - 2026-11-04

### Changed

- Guest party page: ambient accent glow and glassy cards that follow the app's
  palette, clearer "Search YouTube or paste a link" copy, and an Add/Search
  button that switches automatically.
- Search result thumbnails retry the local art endpoint before falling back to
  a music-note placeholder, so results no longer show broken images while the
  host finishes caching artwork.

## 0.1.5 - 2026-11-04

### Added

- **Party mode** — start a local-network listening party from the new **Party**
  view (or press `P`). The host shows a QR code and link; guests on the same
  Wi-Fi open a self-contained mobile page to add YouTube / YouTube Music songs.
- Optional host approval for guest requests, fair round-robin guest queueing,
  per-guest limits (songs, cooldown, video length) and a queue cap.
- Fullscreen QR code for a TV or projector, with `Esc` to return.
- Guest page search with add, undo, now-playing progress and Up Next.
- `meta <video_id>` command in both Python backends for lightweight track
  metadata lookups without resolving a stream.
- Party settings persist across restarts; the party token, guest ids and guest
  names never touch disk, and the party never auto-starts.
- Android 16+ connection help: Chrome may require the **Nearby devices**
  permission before it can open a LAN address such as the party link.

### Changed

- **Party tab overhaul.** The tab now uses the same animated hero style as
  Radio. **Start party** puts a working QR code on the page immediately; setup
  fields moved to **Settings → Party**, with a direct shortcut from the hero.
- **Present QR** is now a true top-most modal: it dims the app, blocks clicks
  to tabs and lists, and closes with `Esc` or the close button. Starting a
  party no longer auto-opens fullscreen.
- The floating party indicator moved out of the now-playing panel and toast
  stack, and the connection help includes a `/healthz` link for phone-side
  diagnostics.

### Security

- Party HTTP server binds to one LAN IPv4 address, validates `Host`, requires a
  per-session token plus an HttpOnly `SameSite=Strict` cookie, and rejects
  state-changing requests without the `X-CF: 1` header.
- Inputs, guest count, queue length and the request inbox are bounded; only
  validated artwork ids are served from disk.

### Testing

- Added unit tests for link parsing, queue interleaving and input sanitising,
  plus a headless HTTP smoke test. Tests are opt-in with
  `-DCROSSFADE_BUILD_TESTS=ON`; `./mk` is unchanged.
