#pragma once

// Windows System Media Transport Controls (SMTC) wrapper.
//
// On Windows the shell's media flyout, hardware media keys, the lock screen
// and the "now playing" overlay are all driven by SMTC. This class owns one
// SMTC session bound to the app window: it publishes track metadata, playback
// status, artwork and the timeline, and forwards button presses (play, pause,
// next, previous, stop, timeline seeks) back to the caller.
//
// All WinRT calls happen on a dedicated MTA worker thread, so the update
// methods are safe to call from any thread (the mpv event thread, UI thread
// and background workers all report playback state).

#include <functional>
#include <memory>
#include <string>

enum class MediaCommand
{
    Play,
    Pause,
    TogglePlayPause,
    Stop,
    Next,
    Previous,
    SeekTo,   // argument: absolute position in seconds
    SeekBy,   // argument: relative offset in seconds
};

struct MediaSessionState
{
    std::string track_id;
    std::string title;
    std::string artist;
    std::string album;
    // A local file path or an http(s) URL. Converted to a URI internally.
    std::string art;
    double duration_sec = 0.0;
    double position_sec = 0.0;
    bool playing = false;
    bool can_seek = true;
    bool can_next = true;
    bool can_prev = true;
    bool shuffle = false;
    // 0 = off, 1 = repeat all, 2 = repeat one.
    int repeat = 0;
};

class WinMediaSession
{
public:
    using CommandHandler = std::function<void(MediaCommand, double)>;

    // native_window is the HWND of the app's top-level window. The SMTC
    // session is bound to it and disappears when the window is destroyed.
    explicit WinMediaSession(void *native_window);
    ~WinMediaSession();

    WinMediaSession(const WinMediaSession &) = delete;
    WinMediaSession &operator=(const WinMediaSession &) = delete;

    // True once the SMTC session has been created and enabled.
    bool valid() const;
    // Human-readable reason when valid() is false (shown in diagnostics).
    std::string last_error() const;

    void set_command_handler(CommandHandler handler);

    // Publish the latest state. Updates are coalesced: metadata/status changes
    // are applied immediately, position-only changes are throttled (the shell
    // extrapolates the time while playing).
    void update(const MediaSessionState &state);

    // Tear the session down (media keys stop targeting the app).
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
