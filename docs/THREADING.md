# Threading and lifetime

This document describes which threads exist, what they may touch, and how
shutdown drains them. It is intentionally conservative: when in doubt, a
worker publishes through an atomic `shared_ptr` and never mutates UI-owned
vectors.

## Threads

| Thread | Lifetime | Writes | Synchronisation / shutdown |
|---|---|---|---|
| UI / render | whole app | ImGui, GL, `AppState` main-thread fields, library calls | Owns `g_state`. `library::*` is guarded by a recursive mutex. |
| mpv event callbacks | mpv-owned | `g_play_queue`, `g_state` flags, crossfade state, notifications | Push work into `g_play_queue`/atomics; the UI thread drains it. |
| `tasks::spawn` workers | short-lived | fetch/search/lyrics/radio/album/artist/cache/download results, `g_downloads_dirty`, `g_state` atomics | Tracked in `src/core/tasks.cpp`; `tasks::shutdown()` waits up to 8 s and joins finished workers. |
| Backend reader | backend lifetime | backend reply lines, `g_backend_error` | `stop_backend()` closes pipes, sends `quit`, then waits for the process. |
| Writeback worker | app lifetime | playlist server ids, rate-song calls | Queue + condition variable; `stop_writeback_worker()` sets the quit flag and joins after the backend pipes close. |
| Discord RPC worker | app lifetime | Discord IPC socket | `g_running` + condition variable; process exit closes the socket. |
| Party HTTP workers | party lifetime | party inbox, art cache map | `party_stop()` stops the server; request handlers use the party mutexes. |
| GLib/GDBus (MPRIS) | process lifetime | MPRIS properties | Owned by GLib; `MprisPlayer` is reset during shutdown. |

## AppState discipline

`AppState` is split into groups by convention:

- **Network-fed lists** (`recommendations`, `search_results`,
  `radio_results`, `artist_search_results`, `lyrics`, `smart_view`, …) are
  published with `std::atomic_store` on `std::shared_ptr<const vector<...>>`.
  Readers take a snapshot with `std::atomic_load` and never hold a raw pointer.
- **Playback/UI scalars** (`playing`, `loading`, `search_id`, `queue_dirty`,
  pending flags) are atomics or are only touched on the UI thread.
- **Vectors and maps** (`liked_tracks`, `playlists`, `play_queue`,
  `play_counts`, `downloaded_tracks`, album/artist caches) are main-thread
  owned. Workers publish new snapshots instead of mutating them.
- **Library DB** calls go through `src/data/library.cpp`, which serialises
  every open/prepare/step behind a recursive mutex. WAL + `busy_timeout`
  handle a second process or a slow checkpoint.

## Shutdown order

`app_run()` exits in this order:

1. Save session/settings/history/counts/log/queue.
2. `discard_recording()`.
3. `party_stop()`.
4. `stop_backend()` (closes the backend pipes and reaps the child).
5. `stop_writeback_worker()`.
6. `library::shutdown()` (checkpoint/close SQLite).
7. `tasks::shutdown()` (drain tracked workers).
8. Clear textures, reset both mpv players, reset MPRIS.
9. Shut down ImGui/SDL and exit with `_Exit(0)` so GLib/GDBus static teardown
   cannot leave the process hanging on helper threads.

## Ownership notes

- `g_player` and `g_fade_player` are owning `unique_ptr<MpvPlayer>`.
- `g_player_raw` / `g_fade_player_raw` are non-owning aliases for MPRIS and
  the mpv callbacks; they are updated after every role swap.
- `g_mpris` is non-owning; the owning `unique_ptr` lives in `app_run()`.
- `party_server` owns its `httplib::Server` with `unique_ptr`.
- No owning raw `new`/`delete` remains outside vendored code.
