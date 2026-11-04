#include "ui.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <string>

std::string truncate_text(const std::string &text, float max_width)
{
    if (max_width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= max_width)
        return text;

    std::string result = text;
    while (!result.empty() && ImGui::CalcTextSize((result + "...").c_str()).x > max_width)
        result.pop_back();

    if (result.empty())
        return "...";
    return result + "...";
}

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
