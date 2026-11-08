#pragma once

// Windows desktop notifications (toasts) for the unpackaged desktop build.
//
// The Linux build uses libnotify; on Windows the equivalent is the toast
// platform. Toasts are delivered from a worker thread so showing a track
// notification never blocks the UI or the mpv event thread, and the toast
// XML/COM plumbing stays out of the app code.

#include <string>

namespace win_notify
{

struct Toast
{
    std::string title;
    std::string body;
    // Local image file or http(s) URL. Empty (or a freedesktop icon name such
    // as "audio-x-generic") means no image.
    std::string image;
    // Song notifications stay silent: the user is already listening to music.
    bool silent = true;
};

// Register the AppUserModelID (display name/icon) and start the delivery
// worker. `aumid` must match the one used by the Start-menu shortcut /
// process explicit AUMID, e.g. "Crossfade.Music".
bool init(const std::string &aumid, const std::string &display_name,
          const std::string &icon_path);

// Flush the queue, stop the worker and unregister the COM objects.
void shutdown();

// Queue a toast. Safe to call from any thread; no-op before init().
void show(const Toast &toast);

// True once the WinRT notifier is usable.
bool ready();

// Number of toasts Windows accepted (diagnostics/tests).
unsigned int shown_count();

// Last error reported by the toast platform ("" when healthy).
std::string last_error();

} // namespace win_notify
