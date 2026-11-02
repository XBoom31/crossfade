# Windows port status

The Linux build is the reference. Windows support is **in progress** — the
platform layer exists (see the `P100_WINDOWS` blocks), but it has not been
verified on a real Windows machine yet. CI (`.github/workflows/build.yml`)
builds the Windows binary on every push, which is the source of truth.

## What is done

| Area | Approach |
|---|---|
| Backend processes (`ytdlp.cpp`) | `CreateProcess` + two anonymous pipes; a reader thread per channel replaces `poll()`/`read()` with the same 45 s deadline and error payloads; `TerminateProcess` on stop |
| Shell-outs | `popen`/`system` kept (cmd.exe), `2>/dev/null` → `2>NUL`, `shell_quote` → double quotes, `yt-dlp.exe` resolution via the venv's `Scripts\` dir |
| Directory scans | `std::filesystem::directory_iterator` instead of `dirent.h` |
| Paths (`paths.cpp`) | exe path via `GetModuleFileNameA`, cwd via `GetCurrentDirectoryA`, data in `%APPDATA%\crossfade`, home via `APPDATA`/`USERPROFILE` |
| Notifications | libnotify compiled out; `show_notification` is a no-op (Toast notifications = future work) |
| MPRIS | `win32/mpris_stub.cpp` — no-op class (Media Session API = future work) |
| Discord RPC | `win32/discord_rpc_stub.cpp` — silent no-op (named-pipe transport `\\.\pipe\discord-ipc-N` = future work) |
| Accent colour | XDG portal compiled out; daily-accent fallback and `P100_ACCENT` still work |
| Misc | `localtime_r` → `localtime_s`, `getenv("HOME")` → `USERPROFILE`, venv layout `Scripts\python.exe` |

Playback (libmpv), the UI (SDL2 + ImGui + OpenGL), the Python backend and the
progressive audio cache are platform-portable and shared.

## Feature parity on Windows (current)

Lost until the replacements land: MPRIS (media keys, `playerctl`, KDE
Connect), desktop notifications, Discord Rich Presence. Everything else
works the same.

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

`MPV_INCLUDE_DIR` / `MPV_LIBRARY` point at the libmpv development files
(`mpv.h` + `libmpv.dll.a`/`mpv.lib` from the [mpv dev
package](https://mpv.io/)); MSYS2's `mingw-w64-ucrt-x86_64-mpv` provides
them via pkg-config, which CMake uses automatically on MSYS2.

## Known-unknowns (first Windows run will tell)

- `imgui_impl_sdl2` + OpenGL 3.2 core on Windows drivers
- The mpv `stream-record` / `ytdl` hooks with Windows paths
- `curl.exe` availability (ships with Windows 10+) and cmd.exe quoting of
  googlevideo URLs (they are double-quoted, but `&` in URLs deserves a test)
- Whether the backend's `subprocess.check_output(yt-dlp …)` inherits the
  console cleanly when the C++ parent is a GUI app
