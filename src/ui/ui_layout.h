#pragma once

#include "imgui.h"

struct AppState;

struct AppLayout
{
    float window_w = 0.0f;
    float window_h = 0.0f;
    bool narrow = false;
    bool sidebar_rail = false;
    float sidebar_w = 0.0f;
    float header_h = 0.0f;
    float bottom_h = 0.0f;
    ImVec2 sidebar_pos = ImVec2(0, 0);
    ImVec2 sidebar_size = ImVec2(0, 0);
    ImVec2 content_pos = ImVec2(0, 0);
    ImVec2 content_size = ImVec2(0, 0);
    ImVec2 header_pos = ImVec2(0, 0);
    ImVec2 header_size = ImVec2(0, 0);
    ImVec2 body_pos = ImVec2(0, 0);
    ImVec2 body_size = ImVec2(0, 0);
    ImVec2 bottom_pos = ImVec2(0, 0);
    ImVec2 bottom_size = ImVec2(0, 0);
    bool lyrics_open = false;
    bool lyrics_docked = false;
    float lyrics_w = 0.0f;
    float lyrics_min_w = 0.0f;
    float lyrics_max_w = 0.0f;
    float gap = 0.0f;
};

AppLayout ui_compute_layout(const AppState &state, const ImGuiIO &io);
