# Windows port status

The Linux build is the reference. Windows support is **in progress** — the
platform layer exists (see the `P100_WINDOWS` blocks). CI
(`.github/workflows/build.yml`) builds the Windows binary on every push, which
is the source of truth. The desktop-integration layer (media controls,
notifications) has been exercised end-to-end on Windows 10; the rest of the
port is still awaiting broader real-machine testing.

## What is done

| Area | Approach |
|---|---|
| Backend processes (`ytdlp.cpp`) | `CreateProcess` + two anonymous pipes; a reader thread per channel replaces `poll()`/`read()` with the same 45 s deadline and error payloads; `TerminateProcess` on stop |
| Shell-outs | `popen`/`system` kept (cmd.exe), `2>/dev/null` → `2>NUL`, `shell_quote` → double quotes, `yt-dlp.exe` resolution via the venv's `Scripts\` dir |
| Directory scans | `std::filesystem::directory_iterator` instead of `dirent.h` |
| Paths (`paths.cpp`) | exe path via `GetModuleFileNameA`, cwd via `GetCurrentDirectoryA`, data in `%APPDATA%\crossfade`, home via `APPDATA`/`USERPROFILE` |
| Notifications | WinRT toasts from a dedicated worker thread; the AUMID display name/icon is registered under `HKCU\Software\Classes\AppUserModelId`, so no Start-menu shortcut is required |
| MPRIS | `win32/mpris_windows.cpp` maps the same surface onto System Media Transport Controls (`win32/media_session.cpp`): media keys, media flyout, lock screen, timeline seek |
| Warm start | Per-user, per-session named pipe (`\\.\pipe\crossfade-warm-<user>-<session>`): `show`/`hide`/`toggle`/`quit` from a second launch, no console |
| Console window | The app links as a GUI subsystem binary; helper processes (Python backend, yt-dlp, curl, login) are launched with `CREATE_NO_WINDOW` (`src/core/win_process.cpp`) |
| Discord RPC | `win32/discord_rpc_stub.cpp` — silent no-op (named-pipe transport `\\.\pipe\discord-ipc-N` = future work) |
| Accent colour | XDG portal compiled out; daily-accent fallback and `P100_ACCENT` still work |
| Misc | `localtime_r` → `localtime_s`, `getenv("HOME")` → `USERPROFILE`, venv layout `Scripts\python.exe` |

Playback (libmpv), the UI (SDL2 + ImGui + OpenGL), the Python backend and the
progressive audio cache are platform-portable and shared.

## Feature parity on Windows (current)

Desktop integration is implemented natively:

- **Media controls** — playback is published to System Media Transport
  Controls (SMTC), so the Windows media flyout, hardware media keys, lock
  screen and the "now playing" overlay show title/artist/album/cover art and
  can play, pause, stop, skip and seek. `win32/media_session.cpp` owns the
  WinRT session on a dedicated MTA worker thread; `win32/mpris_windows.cpp`
  adapts it to the same `MprisPlayer` surface the Linux build uses.
- **Notifications** — new-track toasts use the Windows notification platform
  (`win32/notifications.cpp`), including cached cover art and the app name
  from the registered AppUserModelID. Delivery happens on a worker thread so
  the mpv event thread never blocks on COM. The Settings → Notifications
  toggle still applies.

Lost until the replacement lands: Discord Rich Presence. Everything else
works the same, including warm start (`--warm`, `--toggle`, `--quit`) and the
installer's opt-in "start hidden at sign-in" Run entry.

`tests/test_win_platform.cpp` is a standalone smoke executable for both
integrations. Run `scripts/windows-platform-smoke.ps1` from PowerShell (after
a CMake build) for the end-to-end check: it verifies the SMTC session and
metadata, sends media commands and confirms the app received them, and checks
that Windows accepted the toast.

## Building on Windows (MSYS2)

```bash
pacman -S mingw-w64-ucrt-x86_64-toolchain cmake mingw-w64-ucrt-x86_64-SDL2 \
          mingw-w64-ucrt-x86_64-mpv python curl

# Python side
python -m venv venv
venv/Scripts/pip install -U pip
venv/Scripts/pip install -r requirements.txt

# Auth (the bash login helper is Linux-only; call ytmusicapi directly)
venv/Scripts/ytmusicapi browser --file "%APPDATA%/crossfade/browser.json"
# ...paste request headers copied from music.youtube.com DevTools

# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMPV_INCLUDE_DIR=... -DMPV_LIBRARY=...
cmake --build build -j
./build/crossfade.exe
```

Unit tests run with `ctest --test-dir build`. The desktop integration has its
own end-to-end check (interactive session required):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/windows-platform-smoke.ps1
```

It drives `build/test_win_platform.exe`, verifies the SMTC session/metadata
through `GlobalSystemMediaTransportControlsSessionManager`, sends media
commands, and confirms the toast platform accepted a notification.

`MPV_INCLUDE_DIR` / `MPV_LIBRARY` point at the libmpv development files
(`mpv.h` + `libmpv.dll.a`/`mpv.lib` from the [mpv dev
package](https://mpv.io/)); MSYS2's `mingw-w64-ucrt-x86_64-mpv` provides
them via pkg-config, which CMake uses automatically on MSYS2.

## Updating

`updater.cpp` runs a Windows-only startup check against
`releases/latest/download/version.txt` (a small asset published by the release
workflow) and compares it with the compile-time `CROSSFADE_VERSION`. When a
newer version exists, the normal toast stack offers **Update now**. That action
launches `install-crossfade.cmd` from the install directory, falling back to
downloading the current one from the release page for older installs. The
installer runs in its own console, replaces the app and asks whether to restart
it. Linux builds compile the updater as a no-op because distro packages own
their files.

## Known-unknowns (first Windows run will tell)

- `imgui_impl_sdl2` + OpenGL 3.2 core on Windows drivers
- The mpv `stream-record` / `ytdl` hooks with Windows paths
- `curl.exe` availability (ships with Windows 10+) and cmd.exe quoting of
  googlevideo URLs (they are double-quoted, but `&` in URLs deserves a test)
- `subprocess` calls in the backend now set `CREATE_NO_WINDOW`; the GUI build
  itself no longer has a console to inherit
