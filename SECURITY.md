# Security

## Reporting

Please report suspected vulnerabilities privately to the maintainers rather
than opening a public issue. Include the affected version, platform, and a
minimal reproduction if possible.

## Local data

- `browser.json` and `browseroauth.json` contain live YouTube Music
  credentials. Treat them like passwords. They are git-ignored and should be
  mode `0600`; the config directory is `0700`.
- `library.db`, playlists, history and caches can reveal listening habits.
  Export/import archives contain them.
- The app sends no telemetry. Party mode stays on the local network.

## Party mode

The guest server binds to the LAN and validates the `Host` header, caps
request bodies, rate-limits per guest, and uses constant-time token checks.
Do not expose the party port to the public internet; it is designed for a
trusted local network.
