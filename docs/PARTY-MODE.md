# Party mode — design notes

This document records the recon that shaped party mode, the architecture it
landed on, and the security and testing decisions that keep it contained.

## Goals

- The host starts a party from the **Party** tab, gets a QR code, and guests
  add YouTube songs from a phone on the same LAN.
- No account, no cloud service, no telemetry, no outbound connection beyond the
  existing `ytmusicapi` / `yt-dlp` backend calls the app already makes.
- Party off means **zero** extra threads, sockets, timers or render work.
- All queue mutation stays on the main/UI thread; HTTP threads only read
  immutable snapshots and push bounded requests into an inbox.
- The guest page is one self-contained HTML document: inline CSS/JS, no CDN,
  no external fonts, no third-party requests, mobile-first and accessible.

## Recon: how the app is put together

- `main.cpp` owns the SDL/OpenGL/ImGui loop. It is **demand driven**:
  `SDL_WaitEventTimeout()` parks between frames and only input, animation,
  playback, a requested repaint or a ~1 s safety tick wakes it.
- `AppState` is a large shared struct. Most cross-thread fields are guarded by
  `g_data_mutex` or are atomics (for example `repaint_requested`,
  `queue_dirty`, `playing`).
- `ytdlp.cpp` owns three line-based JSON backend channels:
  - the **main channel** for search, metadata, lyrics and library reads;
  - a dedicated **play channel** so a host click never queues behind search;
  - a dedicated **prefetch channel** for speculative stream warming, so a long
    background resolve cannot starve party search or metadata lookups.
  `send_command_on()` serialises each channel with its own mutex.
- `texture.cpp` already keeps downloaded cover art in an on-disk cache and
  exposes `get_cached_thumbnail_path()` / `thumbnail_cache_path()`.
- `ui_settings.cpp` persists settings by writing flat JSON keys and reading them
  back with small substring helpers. It never persists transient state.
- `p100-backend.py` and `p100-backend-oauth.py` share the same line protocol and
  must be changed together.
- The UI is immediate-mode ImGui. There is no texture upload path for arbitrary
  images, but `ImDrawList` can draw the QR modules directly, so no new GPU
  resource or file is needed.

## Architecture

```
phone ──HTTP──> party_server.cpp (httplib worker threads)
                       │  immutable reads + bounded inbox
                       ▼
                party.cpp (state machine, token, guests, snapshots)
                       │  SDL_USEREVENT wake
                       ▼
                main.cpp loop ──> party_tick() on the main thread
                       │
                       ├─ queue insert / approval / undo
                       ├─ metadata lookup via backend `meta <id>`
                       ├─ SDL / ImGui host UI (ui_party.cpp)
                       └─ prefetch_urls_for() for guest songs
```

### Threading rules

- HTTP worker threads **never** mutate `AppState`. They:
  - validate the token/cookie/rate limit,
  - take an immutable `shared_ptr<Snapshot>` for `/api/state`,
  - enqueue an `InboxAdd` / `InboxUndo` and wait on a condition variable.
- `party_tick()` runs on the main thread once per frame. It drains the inbox,
  applies duplicate/quota/approval rules, mutates `state.play_queue`, and
  publishes a fresh snapshot.
- The HTTP worker wakes the main loop with `SDL_PushEvent(SDL_USEREVENT)`. SDL
  event posting is thread-safe; ImGui is never touched from a worker.
- Metadata lookup happens on the HTTP worker (bounded at 8 s) and only the
  resulting immutable `Track` enters the inbox. The main thread never blocks on
  the network to add a guest song.

### Queue fairness

`party_queue::insert_fair()` assigns each guest a round equal to the number of
songs they already have in the queue. New songs are inserted:

1. after leading host-pinned items (so "Play next" keeps its priority),
2. before the host's playlist/source rows (global-queue tracks are marked
   `from_queue_source` and are not treated as pins),
3. interleaved round-robin with other guest items, FIFO within a round.

This gives round-robin interleaving between guests without starving anyone and
without ever overriding the host's explicit ordering. The guest snapshot is
capped at 40 upcoming rows so a long host playlist does not bloat every poll;
the guest page shows an "and N more" line for the remainder.

## Backend protocol

A new command was added to **both** Python backends:

```
meta <video_id>   → {"ok":true,"id":...,"title":...,"artist":...,
                     "durationSec":...,"thumbUrl":...}
```

It performs a metadata-only lookup (`get_song` / search fallback) and never
resolves a playable stream. `ytdlp.cpp` exposes it as
`fetch_track_meta()`. It uses the main channel so guest lookups cannot delay a
host play, which uses the play channel.

## HTTP surface

| Method | Path | Purpose |
|---|---|---|
| GET | `/healthz` | Plain `ok` for diagnostics |
| GET | `/j/<token>` | Validate token, serve the guest page with a guest cookie |
| GET | `/` | Self-contained guest page |
| GET | `/api/state?v=N` | Full snapshot or `{"unchanged":true}` |
| POST | `/api/add` | Add a link or bare 11-char id |
| DELETE | `/api/add/<id>` | Withdraw your own queued/pending song |
| GET | `/api/search?q=` | Up to 10 tracks via the main backend channel |
| POST | `/api/name` | Set an optional display name (≤ 24 codepoints) |
| GET | `/api/art/<id>` | Cached cover art for a validated 11-char id |

All responses are `no-store` except successfully served validated art
(`public, max-age=86400`); art misses stay uncached so the page can retry once
thumbnails finish downloading.
The guest page is served with a restrictive CSP, `X-Content-Type-Options`,
`Referrer-Policy: no-referrer`, `X-Frame-Options: DENY` and
`Cross-Origin-Resource-Policy: same-origin`. Error responses keep a short HTML
or JSON explanation rather than an empty body, so a phone never shows a blank
white page when a cookie is missing or a link is stale.

## Security model

- **LAN only.** The server binds to one selected IPv4 interface (or `0.0.0.0`
  only when the selected address cannot be bound). It is not a public service
  and does not use HTTPS.
- **Address selection.** Automatic mode asks the kernel which source address it
  would use for outbound traffic and prefers it. This avoids stale secondary
  DHCP aliases that still answer on the laptop itself but are unreachable from
  phones. The network picker labels the recommended address.
- **Token.** 128 bits from the OS CSPRNG, base64url-encoded, compared in
  constant time. The token appears only in the QR URL; it is never written to
  disk, logged, or returned by `/api/state`.
- **Session cookie.** `cf_guest` is a 128-bit random id, `HttpOnly`,
  `SameSite=Strict`, `Path=/`, 12 h `Max-Age`. Guest ids and names live in
  memory only and are dropped when the party stops.
- **Host allowlist.** Every request must carry the bound IP (or
  `localhost`/`127.0.0.1`) and port as its `Host`; anything else gets `421`.
- **CSRF.** State-changing JSON requests require `Content-Type:
  application/json` and `X-CF: 1`. Cookies alone are not accepted. Combined
  with `SameSite=Strict`, another website cannot queue songs.
- **Input bounds.** 4 KiB request bodies, 2048-character link input, strict
  `^[A-Za-z0-9_-]{11}$` video ids, at most 64 live guests (evicted after 30 min
  idle), at most 64 queued add/undo requests per inbox, bounded
  queue/max-per-guest/cooldown/video length, and token buckets for adds and
  searches.
- **No filesystem serving.** Art requests are validated ids mapped through the
  in-memory art map to paths that came from `texture.cpp`'s cache. Unknown ids
  are always `404`.
- **No auth files.** Party mode never reads, logs or serves `browser.json`,
  `browseroauth.json` or any other authentication material. It only uses the
  already-running backend channel.

## Persistence

Only settings are saved, nested under a `party` key in `settings.json`:

```json
"party": {
  "name": "...", "port": 8484, "interface_ip": "",
  "require_approval": false, "max_per_guest": 3, "cooldown_sec": 5,
  "max_video_minutes": 10, "max_queue": 100,
  "allow_search": true, "allow_duplicates": false, "show_names": true
}
```

The token, guest ids, guest names, pending requests and live queue provenance
are **never** persisted. A party never auto-starts on launch.

## Rendering

- `ui_settings.cpp` owns the **Settings → Party** configuration form (name,
  network address, port, approval, limits and guest options). The Party tab's
  **Party settings** shortcut switches tabs and scrolls that section into view.
- `ui_party.cpp` draws the Party tab as a Radio-style hero. Before a party it
  shows a single **Start party** action; after starting, the live QR appears
  in-tab with **Copy link**, **Present QR** and **End party**, followed by
  pending approvals and guest requests. The bottom-bar party chip remains.
- **Present QR** opens a true ImGui modal popup, so it is top-most, dims the
  app, and blocks clicks to tabs and lists behind it. `Esc` or the close button
  dismisses it; starting a party does **not** auto-open fullscreen.
- QR modules are drawn with `ImDrawList::AddRectFilled`; no texture upload and
  no temporary image file.
- Queue artwork is registered from the host's thumbnail cache and, when
  missing, downloaded in the background for the first 40 upcoming rows (the
  same cap as the guest snapshot). The phone retries the local `/api/art`
  endpoint before showing the music-note fallback.
- The demand-driven loop is unchanged. While a party is running, a small
  animation request keeps the live dot, QR rings and scan highlight moving;
  the existing wake event, playback cadence and 1 s safety tick keep guest
  state fresh. Animations respect **Settings → Appearance → Animations** and
  stop when that setting is Off.

## Tests

- `tests/test_party_link.cpp` — URL forms, bare ids, playlist/channel rejection,
  bounds.
- `tests/test_party_queue.cpp` — host pin preservation, round-robin fairness,
  duplicates, per-guest counts.
- `tests/test_party_util.cpp` — CSPRNG tokens, constant-time compare, UTF-8
  cleaning, JSON/HTML escaping, accent validation, token bucket.
- `tests/party_smoke_server.cpp` + `scripts/party-smoke-test.sh` — real
  `party_server.cpp` with stubbed state, exercised with `curl`: health, cookie
  flags, Host allowlist, CSRF/content-type guards, add/undo/search, art id
  validation and unknown routes. No GUI, backend or auth files involved.

Build and run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCROSSFADE_BUILD_TESTS=ON
cmake --build build -j"$(nproc)"
(cd build && ctest --output-on-failure)
scripts/party-smoke-test.sh
```

`./mk` builds into `build/` and runs the app; unit tests are enabled by default but not executed automatically.

## Known limitations / deliberate deviations

- Re-scanning the QR code creates a **new guest id** rather than reusing the
  existing cookie. This makes a second phone or a cleared cookie behave
  predictably; an existing session is simply left to expire in memory.
- A guest's own items get a "you" badge client-side from the `you.ids` list in
  each snapshot; the queue DOM is only rebuilt when its identity/status/badge
  signature changes, so the 1 s snapshot refresh does not restart animations.
- Playback progress is extrapolated from the moment the phone *receives* a
  snapshot using a monotonic local clock, not by comparing the phone's wall
  clock with the host's. Phones on an internet-less hotspot can have a badly
  skewed clock; using `serverTime` made the position sit at 0:00 for tens of
  seconds and then run permanently late.
- IPv4 only (the QR URL and Host check are IPv4-based). IPv6 LANs are out of
  scope for this pass.
- HTTP, not HTTPS. That is appropriate for a trusted home LAN; do not port
  forward the party server.
- Guest search and metadata still depend on the local Python backend and the
  network; failed lookups return a friendly retryable error.
- Pending approvals are not persisted, so restarting the app ends the party
  and discards them.
- Hotspots and public Wi-Fi commonly enable client isolation. The hotspot
  owner's phone can then reach the host while another phone cannot. Party mode
  cannot work around that at the application layer; use a normal router, make
  the laptop the hotspot, or disable client isolation where the network
  exposes the option.
- Android 16 and newer can gate local-network access behind a runtime
  permission. Chrome may report `ERR_TOO_MANY_RETRIES` (or
  `ERR_LOCAL_NETWORK_PERMISSION_MISSING`) until **Nearby devices** is allowed
  for the browser; Firefox or another browser can be used to confirm the host
  is reachable. The host cannot detect or grant this permission remotely.
