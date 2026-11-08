#pragma once

#include <memory>
#include <string>
#include "ui.h"
#include "mpvplayer.h"

class MprisPlayer
{
public:
    // native_window is the app's top-level HWND on Windows (SMTC is bound to
    // the window); ignored by the Linux MPRIS implementation.
    MprisPlayer(AppState &state, MpvPlayer *&player, void *native_window = nullptr);
    ~MprisPlayer();

    void update_metadata(const Track &track, double duration_sec);
    void update_playback_status(bool playing);
    void update_position(double position_sec);
    void update_volume(double volume);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};