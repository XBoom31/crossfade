#include "ui_layout.h"
#include "ui.h"
#include "ui_theme.h"

#include <algorithm>

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

AppLayout ui_compute_layout(const AppState &state, const ImGuiIO &io)
{
    const UiMetrics &m = ui_m();
    AppLayout l;
    l.window_w = io.DisplaySize.x;
    l.window_h = io.DisplaySize.y;
    l.narrow = l.window_w < 1040.0f;
    const bool want_rail = state.sidebar_collapsed || l.window_w < 1120.0f;
    l.sidebar_rail = want_rail;

    const float title_line = 34.0f * m.text_scale;
    const float body_line = 26.0f * m.text_scale;
    const float nav_label_w = ui_measure_width(g_ui_fonts.medium, 20.0f, "Downloads");
    float rail_w = std::max(m.sidebar_rail_w, m.space_4 * 2.0f + m.row_art * 0.75f);
    float expanded_w = std::max(m.sidebar_w, nav_label_w + m.row_art + m.space_4 * 2.5f);
    const float max_sidebar_w = l.window_w * 0.38f;
    rail_w = std::min(rail_w, max_sidebar_w);
    expanded_w = std::min(expanded_w, max_sidebar_w);
    const float anim_w = ui_anim_sidebar_width(want_rail ? rail_w : expanded_w);
    l.sidebar_w = clampf(anim_w, std::min(rail_w, expanded_w), std::max(rail_w, expanded_w));

    l.header_h = std::max(m.header_h, title_line + m.space_3 * 2.2f);
    l.bottom_h = std::max(m.bottom_h, std::max(m.row_art + m.space_4 * 1.6f, body_line * 2.0f + m.space_3 * 2.0f));
    l.bottom_h = std::min(l.bottom_h, l.window_h * 0.28f);

    l.sidebar_pos = ImVec2(0.0f, 0.0f);
    l.sidebar_size = ImVec2(l.sidebar_w, std::max(0.0f, l.window_h - l.bottom_h));
    l.content_pos = ImVec2(l.sidebar_w, 0.0f);
    l.content_size = ImVec2(std::max(0.0f, l.window_w - l.sidebar_w), std::max(0.0f, l.window_h - l.bottom_h));
    l.header_pos = l.content_pos;
    l.header_size = ImVec2(l.content_size.x, std::min(l.header_h, l.content_size.y));
    l.body_pos = ImVec2(l.content_pos.x, l.content_pos.y + l.header_size.y);
    l.body_size = ImVec2(l.content_size.x, std::max(0.0f, l.content_size.y - l.header_size.y));
    l.bottom_pos = ImVec2(0.0f, l.window_h - l.bottom_h);
    l.bottom_size = ImVec2(l.window_w, l.bottom_h);
    l.gap = m.space_3;

    // The lyrics drawer is available on every tab now (settings, album and
    // artist views included), so the panel can stay visible everywhere.
    const bool lyrics_allowed = true;
    if (lyrics_allowed)
    {
        const float list_min = 260.0f * m.layout_scale;
        const float want = std::max(0.0f, state.settings.lyrics_panel_width * m.layout_scale);
        l.lyrics_min_w = m.lyrics_min;
        l.lyrics_max_w = std::min(m.lyrics_max,
                                  std::max(m.lyrics_min,
                                           l.body_size.x - l.gap - list_min - 1.0f));

        float target_w = 0.0f;
        bool target_docked = true;
        if (state.settings.lyrics_panel_open)
        {
            target_w = clampf(want, l.lyrics_min_w, l.lyrics_max_w);
            target_docked = l.body_size.x - target_w - l.gap >= list_min;
            if (!target_docked)
                target_w = std::min(std::max(want, l.lyrics_min_w), l.body_size.x * 0.74f);
        }

        static bool s_last_docked = true;
        if (state.settings.lyrics_panel_open)
            s_last_docked = target_docked;
        const bool docked_mode = state.settings.lyrics_panel_open ? target_docked
                                                                  : s_last_docked;

        if (docked_mode)
        {
            const float anim_lyrics_w = ui_anim_lyrics_width(target_w);
            l.lyrics_open = anim_lyrics_w > 0.5f;
            l.lyrics_w = anim_lyrics_w;
            l.lyrics_docked = true;
        }
        else if (state.settings.lyrics_panel_open)
        {
            // Not enough room to dock: the panel takes over the content area.
            l.lyrics_open = true;
            l.lyrics_docked = false;
            l.lyrics_w = target_w;
        }
    }
    else
    {
        // Keep the clock parked so reopening on a lyrics tab starts from 0.
        ui_anim_lyrics_width(0.0f);
    }
    return l;
}
