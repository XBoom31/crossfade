#pragma once

#include <memory>
#include <string>
#include "ui.h"
#include "mpvplayer.h"

class MprisPlayer
{
public:
    MprisPlayer(AppState &state, MpvPlayer *&player);
    ~MprisPlayer();

    void update_metadata(const Track &track, double duration_sec);
    void update_playback_status(bool playing);
    void update_position(double position_sec);
    void update_volume(double volume);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};