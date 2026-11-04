#pragma once

#include "imgui.h"

enum class UiIcon
{
    Home,
    Radio,
    Heart,
    Album,
    Artist,
    Playlist,
    Download,
    History,
    Stats,
    Settings,
    Search,
    Close,
    ChevronLeft,
    ChevronRight,
    Play,
    Pause,
    Prev,
    Next,
    More,
    Volume,
    Lyrics,
    Equalizer,
    Shuffle,
    Repeat,
    RepeatOne,
    Queue,
    Check,
    Refresh,
    Sun,
    Moon,
    Party
};

void ui_draw_icon(ImDrawList *dl, UiIcon icon, ImVec2 center, float size, ImU32 color,
                  float thickness = 1.6f, float rotation = 0.0f);
bool ui_icon_button(const char *id, UiIcon icon, float hit_size, bool active = false,
                    ImU32 color = 0, const char *tooltip = nullptr);
