# Changelog

All notable changes to crossfade are documented here.

## Unreleased

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
