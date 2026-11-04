# Changelog

All notable changes to crossfade are documented here.

## Unreleased

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
