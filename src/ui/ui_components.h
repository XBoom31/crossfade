#pragma once

struct UiMetrics;

#include "imgui.h"
#include "ui_icons.h"
#include <string>
#include <vector>

struct AppState;
struct Track;

struct TrackRowOptions
{
    bool show_rank = false;
    int rank = 0;
    bool show_album = false;
    bool show_duration = true;
    bool show_plays = false;
    bool show_delete = false;
    bool show_download = false;
    bool show_art_play = true;
    bool compact = false;
    float art_size = 0.0f;
    float progress = -1.0f;
    // Staggered list reveal: the caller passes the list's reveal timestamp
    // (from ui_anim_track_list_reveal) and this row's position.
    double reveal_start = -1.0;
    int reveal_index = 0;
    // Keep the reveal_index stagger even when the scroll-reveal path takes
    // over (used by Home for rows appended by endless scroll, which enter the
    // viewport after the append itself).
    bool stagger_scroll_reveal = false;
    std::string secondary;
    std::string right_text;
    bool selected = false;
    bool auto_play_double_click = true;
    bool draggable = false;
    const char *drag_type = nullptr;
};

struct TrackRowResult
{
    bool hovered = false;
    bool double_clicked = false;
    bool deleted = false;
    bool menu_open = false;
    bool download_requested = false;
    bool download_remove = false;
    bool clicked = false;
    bool ctrl_clicked = false;
    bool shift_clicked = false;
    bool drop_received = false;
    int drag_from = -1;
    int drop_to = -1;
};

TrackRowResult ui_track_row(AppState &state, const Track &track, int index, const TrackRowOptions &options = {});

// True shortly after main.cpp synthesized a click from a touch tap. Rows use
// it to make a single tap act like the double-click "play" gesture.
extern double g_touch_click_time;
bool ui_touch_tap_recent();
// Matching row pitch for ImGuiListClipper users of ui_track_row().
float ui_track_row_pitch(float art_size, bool compact = false);

// Hero pill button (Radio / list-page heroes): outlined or accent-filled
// rounded button with icon+label, hover ease and click pulse. Returns true
// when clicked.
bool ui_pill_button(const char *id, const char *label, UiIcon icon, ImVec2 pos,
                    ImVec2 size, bool primary, float alpha, bool enabled);
float ui_pill_hover_value(ImGuiID id, bool hovered);
float ui_pill_pulse_value(ImGuiID id, double now, bool clicked);

// Cover-art tile: draws the surface, a fallback icon, then the cached
// thumbnail (fetched async if needed) cover-fitted on top. `scope` tags the
// caller (e.g. "radio", "liked"): while the requested URL has no ready
// texture yet, the last texture that actually rendered for this scope stays
// on screen, so a background list re-scan can never flash
// cover -> placeholder -> cover.
void ui_draw_cover(ImDrawList *dl, const std::string &url, ImVec2 pmin, ImVec2 pmax,
                   float rounding, float alpha, UiIcon fallback, const char *scope = "",
                   bool instant = false, bool skip_texture_fade = false);

// Small outlined pill for hero metadata; returns its width.
float ui_meta_chip(ImDrawList *dl, const UiMetrics &m, const char *text, ImVec2 pos,
                   bool accent, float alpha);

// Hover/pulse maps are keyed by row ImGuiIDs, which embed the row index --
// scrolling a long list grows them without bound. Entries whose eased value
// has settled back to rest are visually identical to a fresh entry, so they
// can be dropped. Call from the owning site when the map gets large.
#include <unordered_map>
#include <cmath>
template <typename MapT>
inline void ui_prune_hover_map(MapT &m)
{
    if (m.size() < 512)
        return;
    for (auto it = m.begin(); it != m.end();)
    {
        if (std::fabs((double)it->second) < 0.003)
            it = m.erase(it);
        else
            ++it;
    }
}
void ui_track_context_menu(AppState &state, const Track &track, bool allow_delete,
                           bool show_download = false, bool *download_requested = nullptr,
                           bool *download_remove = nullptr);
void ui_play_track(AppState &state, const Track &track, int index);
void ui_selection_bar(AppState &state, const std::vector<Track> &visible, const char *context,
                      bool allow_like, bool allow_delete, int playlist_index = -1);

bool ui_section_header(const char *title, const char *meta = nullptr, const char *action = nullptr);
// Glowing rule for section headers: a soft album-coloured light streak that
// holds near its left end and tapers to nothing at the right. Reserves its own
// vertical space whether or not the effect is enabled, so layout never jumps.
void ui_glow_separator(float width = 0.0f);
bool ui_primary_button(const char *label, const ImVec2 &size = ImVec2(0, 0));
bool ui_secondary_button(const char *label, const ImVec2 &size = ImVec2(0, 0));
bool ui_ghost_button(const char *label, const ImVec2 &size = ImVec2(0, 0));
void ui_empty_state(UiIcon icon, const char *title, const char *subtitle = nullptr);
void ui_searching_state(UiIcon icon, const char *title, const char *subtitle = nullptr);
// Inline error state for a failed fetch: error icon, reason, Retry button.
// Returns true in the frame Retry is pressed.
bool ui_error_state(const char *message, const char *retry_label = "Retry");

struct MediaCardResult
{
    bool hovered = false;
    bool clicked = false;
    bool double_clicked = false;
    bool right_clicked = false;
};

MediaCardResult ui_media_card(const char *id, const std::string &title, const std::string &meta,
                              const std::string &thumb_url, float width, float art_size,
                              bool circular, bool current = false);
