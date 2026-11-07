#include "ui.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

int ui_tracks_duration(const std::vector<Track> &tracks)
{
    int total = 0;
    for (const auto &track : tracks)
        total += std::max(0, track.duration_sec);
    return total;
}

std::string ui_format_duration(int seconds)
{
    if (seconds <= 0)
        return "";
    const int h = seconds / 3600;
    const int m = (seconds / 60) % 60;
    const int s = seconds % 60;
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}
