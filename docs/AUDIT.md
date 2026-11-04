# Correctness audit

Bounded review of the places most likely to hide real bugs. Each finding is
either fixed or has a concrete follow-up.

## Crash handler (`src/app/diagnostics.cpp`)

- **Fixed:** the POSIX handler uses a precomputed crash directory buffer, a
  generated version macro, and an epoch filename. It no longer builds a
  `std::string`, calls `localtime_r`/`strftime`, or takes the diagnostics
  mutex from a signal context.
- **Remaining risk:** `backtrace()`/`backtrace_symbols_fd()` are not strictly
  async-signal-safe on every libc. They are used only after the dump file is
  opened, and the handler re-raises the signal. If the process is already
  corrupt, the dump may be incomplete.
- Windows uses `SetUnhandledExceptionFilter` + `MiniDumpWriteDump`; no signal
  path is involved.

## SQLite (`src/data/library.cpp`)

- WAL mode, `PRAGMA foreign_keys=ON`, `sqlite3_busy_timeout(3000)`, prepared
  statements, and one transaction per batch write.
- Migration is idempotent: `meta.legacy_imported` is written only after the
  imported counts verify. A failed import closes the DB and leaves the legacy
  JSON path active for the next launch.
- Backups use `VACUUM INTO`, which produces a consistent copy while WAL is
  active.
- DB writes happen on the UI thread on user actions/track changes, never per
  frame. FTS search is a single prepared query.
- **Follow-up:** a second process using the same config can still hit a
  transient busy timeout; the 3 s timeout makes that unlikely, and the app
  falls back to JSON rather than losing data.

## Party server (`src/party/party_server.cpp`, `party.cpp`)

- Host header validation rejects non-configured hosts.
- Request bodies are capped at 4096 bytes (`set_payload_max_length`).
- Per-guest token checks use `party_util::constant_time_equal`.
- Artwork is served only from a map populated from validated 11-character
  video ids; the URL never becomes an arbitrary filesystem path.
- Rate limiting/cooldowns and queue caps are enforced in the party API.
- **Follow-up:** fuzz `party_link`/`party_util` with arbitrary bytes in CI if a
  fuzzing harness is added later.

## Backend JSON parser (`src/net/ytdlp.cpp`)

- **Fixed:** input size is capped at 16 MiB and object nesting at 256 for
  `parse_track_array`/`parse_lyrics_json`; the quote-aware scanners stop
  rather than walking a pathological payload.
- Tests cover escapes, unicode, nested objects, truncated input, empty
  id/title, synced/plain lyrics, and oversized/deep input.

## Texture/cache lifetime

- The texture upload queue is drained on the GL thread; loaders publish
  completed textures instead of calling GL from a worker.
- Cache eviction is covered by `tests/test_cache.cpp`: least-listened first,
  `cap=0` = unlimited, current track never evicted, `audio_cache_clear(id)`
  keeps the current file and its sidecar.
- **Follow-up:** a full TSan run needs a display/audio session; the CI ASan/
  UBSan suite is the current automated guard.

## Notes

These are review findings, not a claim that the app is race-free. The tracked
worker registry in `src/core/tasks.cpp` is the main lifetime improvement; the
remaining long-lived workers are documented in `docs/THREADING.md`.
