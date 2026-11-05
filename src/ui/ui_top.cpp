#include "ui.h"
#include "ui_backdrop.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ytdlp.h"
#include "library.h"
#include "tasks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

static std::vector<Track> filter_tracks_locally(const std::vector<Track> &tracks, const std::string &query)
{
    std::vector<Track> results;
    if (tracks.empty() || query.empty())
        return results;

    std::string q_lower = query;
    std::transform(q_lower.begin(), q_lower.end(), q_lower.begin(), ::tolower);
    for (const auto &t : tracks)
    {
        std::string title_lower = t.title;
        std::string artist_lower = t.artist;
        std::transform(title_lower.begin(), title_lower.end(), title_lower.begin(), ::tolower);
        std::transform(artist_lower.begin(), artist_lower.end(), artist_lower.begin(), ::tolower);
        if (title_lower.find(q_lower) != std::string::npos ||
            artist_lower.find(q_lower) != std::string::npos)
            results.push_back(t);
    }
    return results;
}

static void on_search_edit(AppState &s);

static void reset_search_context(AppState &s)
{
    s.search_buffer[0] = '\0';
    s.local_search_active = false;
    s.local_search_results.clear();
    s.search_dirty = false;
    s.queued_search.clear();
    s.artist_search_active = false;
    s.artist_search_dirty = false;
    s.artist_search_loading = false;
    s.queued_artist_search.clear();
    std::atomic_store(&s.artist_search_results,
                      std::make_shared<const std::vector<ArtistSummary>>());
    ++s.search_id;
}

void ui_reset_search(AppState &s)
{
    reset_search_context(s);
    ui_bump_page_epoch();
    s.search_loading = false;
    std::atomic_store(&s.search_album_results,
                      std::make_shared<const std::vector<AlbumSummary>>());
    std::atomic_store(&s.search_artist_results,
                      std::make_shared<const std::vector<ArtistSummary>>());
}

void ui_set_search(AppState &s, const std::string &query)
{
    std::snprintf(s.search_buffer, sizeof(s.search_buffer), "%s", query.c_str());
    on_search_edit(s);
}

static void activate_tab(AppState &s, Tab tab)
{
    if (tab == Tab::Queue && s.current_tab != Tab::Queue)
        s.pre_queue_tab = s.current_tab;
    ui_invalidate_forward(s);
    reset_search_context(s);
    s.selected_track_ids.clear();
    s.selection_context.clear();
    s.selection_anchor.clear();
    s.current_tab = tab;

    switch (tab)
    {
    case Tab::Home:
        s.current_playlist_source = PlaylistSource::HOME;
        break;
    case Tab::Downloads:
        s.current_playlist_source = PlaylistSource::DOWNLOADS;
        if (!s.downloaded_loaded)
            load_downloaded_tracks(s);
        break;
    case Tab::Radio:
        s.current_playlist_source = PlaylistSource::RADIO;
        break;
    case Tab::Podcasts:
        s.current_playlist_source = PlaylistSource::PODCAST;
        load_podcast_library(s);
        break;
    case Tab::Liked:
        s.current_playlist_source = PlaylistSource::LIKED;
        break;
    case Tab::Playlists:
        s.playlists.clear();
        for (const auto &name : get_playlist_names())
        {
            Playlist pl;
            pl.name = name;
            pl.tracks = load_playlist_tracks(name);
            s.playlists.push_back(pl);
        }
        if (s.selected_playlist_index >= (int)s.playlists.size())
            s.selected_playlist_index = -1;
        if (s.selected_playlist_index >= 0)
            s.current_playlist_source = PlaylistSource::PLAYLIST;
        break;
    case Tab::History:
        s.current_playlist_source = PlaylistSource::HISTORY;
        // Freeze what the tab shows for this visit: playing a row moves it
        // to the front of the live history, which would otherwise reshuffle
        // the list under the user's cursor.
        s.history_view = std::make_shared<const std::vector<Track>>(s.playback_history);
        break;
    case Tab::Stats:
        s.current_playlist_source = PlaylistSource::STATS;
        s.stats_dirty = true;
        break;
    case Tab::Smart:
        s.current_playlist_source = PlaylistSource::SMART;
        build_smart_view(s);
        break;
    case Tab::Queue:
        break;
    default:
        break;
    }
}

static void nav_item(AppState &state, Tab tab, UiIcon icon, const char *label, bool rail,
                     bool active_override = false, bool active_value = false)
{
    const UiMetrics &m = ui_m();
    const bool active = active_override ? active_value : state.current_tab == tab;
    const float h = rail ? std::max(26.0f * m.layout_scale, m.control_h * 0.64f)
                         : std::max(m.control_h, m.row_art * 0.82f);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    if (clicked)
        activate_tab(state, tab);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_accent_u32(0.13f), m.radius_sm);
    else if (hovered)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_overlay_u32(0.045f), m.radius_sm);

    if (active)
        dl->AddRectFilled(ImVec2(pos.x, pos.y + m.space_1), ImVec2(pos.x + 3.0f * m.layout_scale, pos.y + h - m.space_1),
                          ui_accent_u32(1.0f), 1.5f);

    const float icon_size = rail ? std::min(22.0f * m.layout_scale, h * 0.76f)
                                 : std::min(22.0f * m.layout_scale, h * 0.54f);
    const float icon_x = rail ? pos.x + w * 0.5f : pos.x + m.space_3 + icon_size * 0.5f;
    const ImU32 col = active ? ui_accent_text_u32() : (hovered ? ui_text_u32() : ui_text_dim_u32());
    ui_draw_icon(dl, icon, ImVec2(icon_x, pos.y + h * 0.5f), icon_size, col, 1.65f);

    if (!rail)
    {
        const float tx = pos.x + m.space_3 + icon_size + m.space_3;
        const float tw = pos.x + w - tx - m.space_2;
        const float fs = 20.0f * m.text_scale;
        std::string txt = ui_ellipsize(g_ui_fonts.medium, 20.0f, label, tw);
        ImVec2 ts = g_ui_fonts.medium->CalcTextSizeA(fs, FLT_MAX, 0.0f, txt.c_str());
        dl->AddText(g_ui_fonts.medium, fs, ImVec2(tx, pos.y + (h - ts.y) * 0.5f), col, txt.c_str());
    }
    else if (hovered)
    {
        ImGui::SetTooltip("%s", label);
    }
}

static void section_label(const char *label, bool rail)
{
    if (rail)
        return;
    ui_push_micro();
    ImGui::TextColored(ui_c().text_muted, "%s", label);
    ImGui::PopFont();
}

static void sidebar_playlist(AppState &state, Playlist &pl, int index, bool rail)
{
    const UiMetrics &m = ui_m();
    const float h = std::max(m.control_h * 0.88f, 30.0f * m.layout_scale);
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    std::string id = "##pl_" + std::to_string(index);
    ImGui::InvisibleButton(id.c_str(), ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    const bool active = state.current_tab == Tab::Playlists && state.selected_playlist_index == index;
    if (clicked)
    {
        ui_invalidate_forward(state);
        state.current_tab = Tab::Playlists;
        state.selected_playlist_index = index;
        state.current_playlist_source = PlaylistSource::PLAYLIST;
        state.selected_track_ids.clear();
        state.selection_context.clear();
        state.selection_anchor.clear();
        reset_search_context(state);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_accent_u32(0.11f), m.radius_sm);
    else if (hovered)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_overlay_u32(0.04f), m.radius_sm);
    if (rail)
    {
        ui_draw_icon(dl, UiIcon::Playlist, ImVec2(pos.x + w * 0.5f, pos.y + h * 0.5f),
                     18.0f * m.layout_scale, active ? ui_accent_text_u32() : ui_text_muted_u32(), 1.5f);
        if (hovered)
            ImGui::SetTooltip("%s", pl.name.c_str());
        return;
    }
    const float fs = 18.0f * m.text_scale;
    std::string txt = ui_ellipsize(g_ui_fonts.body, 18.0f, pl.name, w - m.space_4 * 2.0f);
    ImVec2 ts = g_ui_fonts.body->CalcTextSizeA(fs, FLT_MAX, 0.0f, txt.c_str());
    dl->AddText(g_ui_fonts.body, fs, ImVec2(pos.x + m.space_4, pos.y + (h - ts.y) * 0.5f),
                active ? ui_accent_text_u32() : ui_text_dim_u32(), txt.c_str());
}


static bool sidebar_theme_toggle(AppState &state, bool rail)
{
    const UiMetrics &m = ui_m();
    const double now = ImGui::GetTime();
    static double s_click_time = -10.0;
    static UiIcon s_old_icon = UiIcon::Sun;

    const bool target_light = state.settings.day_mode;
    const UiIcon target_icon = target_light ? UiIcon::Moon : UiIcon::Sun;
    const char *label = target_light ? "Night mode" : "Day mode";

    const float spin_t = (float)(now - s_click_time);
    const bool animating = (spin_t >= 0.0f && spin_t < 0.5f);
    const float progress = animating ? (spin_t / 0.5f) : 1.0f;

    float width, height;
    if (rail)
    {
        width = m.control_h;
        height = m.control_h;
    }
    else
    {
        width = ImGui::GetContentRegionAvail().x;
        height = m.control_h;
    }

    ImVec2 pos;
    if (rail)
        pos = ImVec2(ImGui::GetWindowPos().x + (ImGui::GetWindowSize().x - m.control_h) * 0.5f,
                     ImGui::GetCursorScreenPos().y);
    else
        pos = ImGui::GetCursorScreenPos();

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##theme_toggle", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float rounding = rail ? m.control_h * 0.5f : m.radius_sm;
    const ImU32 bg = held ? ui_accent_u32(0.18f)
                          : (hovered ? ui_accent_u32(0.10f)
                                     : ImGui::GetColorU32(ImVec4(0, 0, 0, 0)));
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bg, rounding);
    if (hovered)
        dl->AddRect(pos, ImVec2(pos.x + width, pos.y + height), ui_accent_u32(0.5f), rounding, 0, 1.0f);

    const float icon_sz = 20.0f * m.layout_scale;
    float icon_x = rail ? pos.x + (width - icon_sz) * 0.5f : pos.x + m.space_3 + icon_sz * 0.5f;
    const float cy = pos.y + height * 0.5f;
    const ImVec2 center(icon_x + icon_sz * 0.5f, cy);
    const ImU32 icol = hovered ? ui_accent_text_u32() : ui_text_dim_u32();

    if (animating)
    {
        // Old icon spins out (0->pi) while fading out.
        const float old_alpha = 1.0f - progress;
        const float old_rot = progress * 3.14159265f;
        ui_draw_icon(dl, s_old_icon, center, icon_sz, ui_with_alpha(icol, old_alpha), 1.6f, old_rot);
        // New icon fades in at rotation 0.
        const float new_alpha = progress;
        ui_draw_icon(dl, target_icon, center, icon_sz, ui_with_alpha(icol, new_alpha), 1.6f, 0.0f);
    }
    else
    {
        ui_draw_icon(dl, target_icon, center, icon_sz, icol, 1.6f, 0.0f);
    }

    if (!rail)
    {
        const float tx = icon_x + icon_sz + m.space_3;
        const float fs = 20.0f * m.text_scale;
        const ImVec2 ts = g_ui_fonts.medium->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
        dl->AddText(g_ui_fonts.medium, fs, ImVec2(tx, cy - ts.y * 0.5f),
                    hovered ? ui_text_u32() : ui_text_dim_u32(), label);
    }

    if (clicked)
    {
        s_old_icon = target_icon;
        state.settings.day_mode = !state.settings.day_mode;
        save_settings(state.settings);
        s_click_time = now;
    }

    if (animating)
        ui_anim_request();

    return clicked;
}


void draw_sidebar(AppState &state)
{
    const AppLayout l = ui_compute_layout(state, ImGui::GetIO());
    const UiMetrics &m = ui_m();
    const bool rail = l.sidebar_rail;

    ScopedNoMinSize no_min_size;
    ImGui::SetNextWindowPos(l.sidebar_pos);
    ImGui::SetNextWindowSize(l.sidebar_size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.space_3, m.space_3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, m.space_1 * 0.55f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_backdrop_window_bg());
    ImGui::Begin("Sidebar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    dl->AddLine(ImVec2(wp.x + ws.x - 1.0f, wp.y), ImVec2(wp.x + ws.x - 1.0f, wp.y + ws.y), ui_hairline_u32(), 1.0f);

    const float logo_h = std::max(50.0f * m.layout_scale, m.row_art * 1.2f);
    const ImVec2 logo_pos = ImGui::GetCursorScreenPos();
    const float avail_w = ImGui::GetContentRegionAvail().x;
    // In the expanded sidebar the collapse button lives at the right end of
    // this row. Stop the brand hit-box before it: an earlier full-width
    // InvisibleButton would claim ActiveId first and swallow that click.
    const float brand_w = rail ? avail_w
                               : std::max(0.0f, avail_w - m.control_h - m.space_1 * 2.0f);
    ImGui::InvisibleButton("##brand", ImVec2(brand_w, logo_h));
    const bool brand_hover = ImGui::IsItemHovered();
    const bool brand_click = ImGui::IsItemClicked();
    if (rail && brand_click)
        state.sidebar_collapsed = false;

    const float mark = 34.0f * m.layout_scale;
    const float mark_x = rail ? logo_pos.x + (avail_w - mark) * 0.5f : logo_pos.x + m.space_1;
    const ImVec2 m0(mark_x, logo_pos.y + (logo_h - mark) * 0.5f);
    const ImVec2 m1(m0.x + mark, m0.y + mark);
    dl->AddRectFilled(m0, m1, ui_accent_u32(1.0f), m.radius_sm);
    ui_draw_icon(dl, UiIcon::Equalizer, ImVec2((m0.x + m1.x) * 0.5f, (m0.y + m1.y) * 0.5f),
                 mark * 0.48f, ui_col(ui_c().on_accent), 1.6f);
    if (!rail)
    {
        const float tx = m1.x + m.space_3;
        dl->AddText(g_ui_fonts.semibold, 27.0f * m.text_scale, ImVec2(tx, logo_pos.y + logo_h * 0.5f - 21.0f * m.text_scale),
                    ui_text_u32(), "crossfade");
        dl->AddText(g_ui_fonts.body, 15.0f * m.text_scale, ImVec2(tx, logo_pos.y + logo_h * 0.5f + 7.0f * m.text_scale),
                    ui_text_muted_u32(), "music");
    }
    if (!rail)
    {
        ImGui::SetCursorScreenPos(ImVec2(logo_pos.x + avail_w - m.control_h - m.space_1, logo_pos.y + (logo_h - m.control_h) * 0.5f));
        if (ui_icon_button("##collapse", UiIcon::ChevronLeft, m.control_h, false, 0, "Collapse sidebar"))
            state.sidebar_collapsed = true;
    }
    else if (brand_hover)
    {
        ImGui::SetTooltip("Expand sidebar");
    }

    ImGui::SetCursorScreenPos(ImVec2(logo_pos.x, logo_pos.y + logo_h + m.space_2));
    ImGui::Dummy(ImVec2(0, 0));

    const float bottom_reserve = m.control_h * 2.0f + m.space_5 + m.space_2;
    const float body_h = std::max(0.0f, ImGui::GetContentRegionAvail().y - bottom_reserve);
    ImGui::BeginChild("##sidebar_scroll", ImVec2(0, body_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);

    section_label(rail ? "" : "BROWSE", rail);
    nav_item(state, Tab::Home, UiIcon::Home, "Home", rail);
    nav_item(state, Tab::Downloads, UiIcon::Download, "Downloads", rail);
    nav_item(state, Tab::Radio, UiIcon::Radio, "Radio", rail);
    nav_item(state, Tab::Podcasts, UiIcon::Podcast, "Podcasts", rail);
    if (state.settings.show_visualizer)
        nav_item(state, Tab::Visualizer, UiIcon::Visualizer, "Visualizer", rail);
    else if (state.current_tab == Tab::Visualizer)
    {
        state.current_tab = Tab::Home;
        state.current_playlist_source = PlaylistSource::HOME;
    }
    nav_item(state, Tab::Liked, UiIcon::Heart, "Liked", rail);
    nav_item(state, Tab::History, UiIcon::History, "History", rail);
    nav_item(state, Tab::Stats, UiIcon::Stats, "Stats", rail);
    nav_item(state, Tab::Smart, UiIcon::Repeat, "Smart", rail);
    nav_item(state, Tab::Party, UiIcon::Party, "Party", rail);

    if (!rail)
        ImGui::Dummy(ImVec2(0, m.space_3));
    section_label(rail ? "" : "LIBRARY", rail);
    nav_item(state, Tab::Albums, UiIcon::Album, "Albums", rail, state.current_tab == Tab::Album, state.current_tab == Tab::Album || state.current_tab == Tab::Albums);
    nav_item(state, Tab::Artists, UiIcon::Artist, "Artists", rail, state.current_tab == Tab::Artist, state.current_tab == Tab::Artist || state.current_tab == Tab::Artists);
    nav_item(state, Tab::Playlists, UiIcon::Playlist, "Playlists", rail);
    nav_item(state, Tab::YouTube, UiIcon::Playlist, "YouTube", rail);

    if (!rail && !state.playlists.empty())
    {
        ImGui::Dummy(ImVec2(0, m.space_1));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + m.space_4);
        ui_push_micro();
        ImGui::TextColored(ui_c().text_muted, "MY PLAYLISTS");
        ImGui::PopFont();
        for (int i = 0; i < (int)state.playlists.size(); ++i)
        {
            if (i >= 8)
                break;
            sidebar_playlist(state, state.playlists[i], i, rail);
        }
    }

    ImGui::EndChild();

    const float bottom_y = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - m.space_3;
    if (rail)
    {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + (ImGui::GetWindowSize().x - m.control_h) * 0.5f,
                                         bottom_y - m.control_h * 2.0f - m.space_2));
        sidebar_theme_toggle(state, true);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + (ImGui::GetWindowSize().x - m.control_h) * 0.5f,
                                         bottom_y - m.control_h));
        if (ui_icon_button("##settings", UiIcon::Settings, m.control_h, state.current_tab == Tab::Settings, 0, "Settings"))
            activate_tab(state, Tab::Settings);
    }
    else
    {
        // Theme toggle centered above the settings button.
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + (ImGui::GetWindowSize().x - m.control_h) * 0.5f,
                                         bottom_y - m.control_h * 2.0f - m.space_2));
        sidebar_theme_toggle(state, true);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + m.space_3,
                                         bottom_y - m.control_h));
        const float settings_w = ImGui::GetContentRegionAvail().x;
        if (ui_ghost_button("Settings", ImVec2(settings_w, m.control_h)))
            activate_tab(state, Tab::Settings);
    }

    ImGui::End();
}

static double s_last_edit_time = 0.0;
static bool s_pending_search = false;

static void on_search_edit(AppState &state)
{
    ++state.search_id;
    if (state.current_tab == Tab::Artists)
    {
        state.artist_search_active = state.search_buffer[0] != '\0';
        state.artist_search_loading = state.artist_search_active;
        std::atomic_store(&state.artist_search_results,
                          std::make_shared<const std::vector<ArtistSummary>>());
    }
    s_last_edit_time = ImGui::GetTime();
    s_pending_search = true;
}

static void dispatch_search(AppState &state)
{
    if (!s_pending_search || (ImGui::GetTime() - s_last_edit_time) < 0.35)
        return;
    s_pending_search = false;
    const int current_id = state.search_id;
    const std::string query(state.search_buffer);

    if (state.current_tab == Tab::Home)
    {
        if (query.empty())
        {
            if (state.search_id == current_id)
            {
                std::atomic_store(&state.search_results,
                                  std::make_shared<const std::vector<Track>>());
                std::atomic_store(&state.search_album_results,
                                  std::make_shared<const std::vector<AlbumSummary>>());
                std::atomic_store(&state.search_artist_results,
                                  std::make_shared<const std::vector<ArtistSummary>>());
                state.search_loading = false;
                state.current_playlist_source = PlaylistSource::HOME;
            }
        }
        else
        {
            auto it = std::find(state.search_recent.begin(), state.search_recent.end(), query);
            if (it != state.search_recent.end())
                state.search_recent.erase(it);
            state.search_recent.insert(state.search_recent.begin(), query);
            if (state.search_recent.size() > 10)
                state.search_recent.resize(10);
            save_search_recent(state.search_recent);

            // Publish local-library matches immediately, then let the backend
            // fill in the online results. The worker merges and dedupes them.
            auto local = library::search_tracks(query, 60);
            std::atomic_store(
                &state.search_results,
                std::make_shared<const std::vector<Track>>(std::move(local)));
            std::atomic_store(&state.search_album_results,
                              std::make_shared<const std::vector<AlbumSummary>>());
            std::atomic_store(&state.search_artist_results,
                              std::make_shared<const std::vector<ArtistSummary>>());
            state.queued_search = query;
            state.search_dirty = true;
            state.search_loading = true;
        }
    }
    else if (state.current_tab == Tab::Podcasts)
    {
        if (query.empty())
        {
            state.podcast_search_loading = false;
            state.podcast_search_dirty = false;
            std::atomic_store(&state.podcast_search_results,
                              std::make_shared<const std::vector<PodcastShow>>());
        }
        else
        {
            state.queued_podcast_search = query;
            state.podcast_search_dirty = true;
            state.podcast_search_loading = true;
            ++state.podcast_search_id;
        }
    }
    else if (state.current_tab == Tab::Artists)
    {
        if (query.empty())
        {
            state.artist_search_active = false;
            state.artist_search_loading = false;
            std::atomic_store(&state.artist_search_results,
                              std::make_shared<const std::vector<ArtistSummary>>());
        }
        else
        {
            state.queued_artist_search = query;
            state.artist_search_dirty = true;
            state.artist_search_active = true;
            state.artist_search_loading = true;
        }
    }
    else if (state.current_tab == Tab::Downloads ||
             state.current_tab == Tab::Liked ||
             state.current_tab == Tab::Playlists)
    {
        if (query.empty())
        {
            state.local_search_active = false;
            state.local_search_results.clear();
        }
        else
        {
            const std::vector<Track> *source = nullptr;
            if (state.current_tab == Tab::Downloads)
                source = &state.downloaded_tracks;
            else if (state.current_tab == Tab::Liked)
                source = &state.liked_tracks;
            else if (state.selected_playlist_index >= 0 &&
                     state.selected_playlist_index < (int)state.playlists.size())
                source = &state.playlists[state.selected_playlist_index].tracks;

            if (source)
            {
                state.local_search_results = filter_tracks_locally(*source, query);
                state.local_search_active = true;
                state.local_search_tab = state.current_tab;
            }
        }
    }
}

static const char *view_title(const AppState &s)
{
    switch (s.current_tab)
    {
    case Tab::Home: return s.search_buffer[0] ? "Search" : "Home";
    case Tab::Downloads: return "Downloads";
    case Tab::Radio: return "Radio";
    case Tab::Liked: return "Liked";
    case Tab::Playlists: return "Playlists";
    case Tab::YouTube: return "YouTube Library";
    case Tab::Albums: return "Albums";
    case Tab::Artists: return s.artist_search_active ? "Artist Search" : "Artists";
    case Tab::History: return "History";
    case Tab::Stats: return "Stats";
    case Tab::Queue: return "Up Next";
    case Tab::Party: return "Party";
    case Tab::Album: return "Album";
    case Tab::Artist: return "Artist";
    case Tab::Podcasts: return "Podcasts";
    case Tab::Visualizer: return "Visualizer";
    case Tab::Settings: return "Settings";
    case Tab::Smart: return "Smart";
    }
    return "crossfade";
}

static std::string view_subtitle(const AppState &s)
{
    if (s.current_tab == Tab::Album)
        return s.album_artist;
    if (s.current_tab == Tab::Artist)
        return s.artist_subscribers;
    if (s.current_tab == Tab::Home && s.search_buffer[0])
        return "Your library + YouTube Music";
    if (s.current_tab == Tab::Artists && s.artist_search_active)
        return "Results from YouTube Music";
    if (s.current_playlist_source == PlaylistSource::RADIO)
        return "Endless radio";

    switch (s.current_tab)
    {
    case Tab::Home: return "Made for you";
    case Tab::Downloads: return "Your offline library";
    case Tab::Radio: return "Endless stations";
    case Tab::Liked: return "Songs you love";
    case Tab::History: return "Recently played";
    case Tab::Playlists: return "Your playlists";
    case Tab::YouTube: return "Your YouTube account";
    case Tab::Albums: return "Saved albums";
    case Tab::Artists: return "Artists you follow";
    case Tab::Stats: return "Your listening, at a glance";
    case Tab::Queue: return "Playback queue";
    case Tab::Party: return "Let guests add songs from their phones";
    case Tab::Podcasts: return "Shows and episodes";
    case Tab::Visualizer: return "A little light for your music";
    case Tab::Settings: return "Make crossfade yours";
    default: return s.status_text;
    }
}

// An icon + label pill that slides/fades in only when the Home feed is showing
// (it is meaningless while searching or on another tab) and becomes a spinner
// while a refresh is in flight.
static bool header_refresh_button(AppState &state, const ImVec2 &pos, float width,
                                  float height, float appear, bool compact)
{
    const UiMetrics &m = ui_m();
    const float alpha = std::min(std::max(appear, 0.0f), 1.0f);
    const float slide = (1.0f - alpha) * ui_anim_offset(14.0f);
    const ImVec2 p0(pos.x + slide, pos.y);
    const ImVec2 p1(p0.x + width, p0.y + height);

    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("##header_refresh", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();
    const bool busy = state.home_refreshing.load();
    if (hovered)
        ImGui::SetMouseCursor(busy ? ImGuiMouseCursor_Arrow : ImGuiMouseCursor_Hand);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float rounding = height * 0.5f;
    const float pulse = busy ? 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 5.0f) : 0.0f;
    const ImU32 bg = busy ? ui_accent_u32((0.06f + 0.035f * pulse) * alpha)
                          : (held ? ui_accent_u32(0.18f * alpha)
                                  : (hovered ? ui_accent_u32(0.10f * alpha)
                                             : ui_overlay_u32(0.03f * alpha)));
    dl->AddRectFilled(p0, p1, bg, rounding);
    const ImU32 border = busy ? ui_accent_u32((0.34f + 0.22f * pulse) * alpha)
                              : (hovered ? ui_accent_u32(0.55f * alpha)
                                         : ui_with_alpha(ui_border_u32(), 0.9f * alpha));
    dl->AddRect(p0, p1, border, rounding, 0, 1.0f);

    const float icon = 17.0f * m.layout_scale;
    const float gap = m.space_2;
    const float fs = 18.0f * m.text_scale;
    const char *label = "Refresh";
    const ImVec2 ts = compact
                          ? ImVec2(0.0f, 0.0f)
                          : g_ui_fonts.medium->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
    const float content_w = icon + (compact ? 0.0f : gap + ts.x);
    const float x = p0.x + (width - content_w) * 0.5f;
    const float cy = p0.y + height * 0.5f;

    const float spin = busy ? (float)ImGui::GetTime() * 4.5f : 0.0f;
    if (busy)
        ui_anim_request();
    const ImU32 icon_col = busy ? ui_accent_text_u32()
                                : (hovered ? ui_accent_text_u32() : ui_text_dim_u32());
    ui_draw_icon(dl, UiIcon::Refresh, ImVec2(x + icon * 0.5f, cy), icon,
                 ui_with_alpha(icon_col, alpha), 1.7f, spin);
    if (!compact)
    {
        const ImU32 label_col =
            busy ? ui_text_u32() : (hovered ? ui_text_u32() : ui_text_dim_u32());
        dl->AddText(g_ui_fonts.medium, fs, ImVec2(x + icon + gap, cy - ts.y * 0.5f),
                    ui_with_alpha(label_col, alpha), label);
    }
    return clicked && !busy;
}

void draw_content_header(AppState &state)
{
    const AppLayout l = ui_compute_layout(state, ImGui::GetIO());
    const UiMetrics &m = ui_m();
    const double now = ImGui::GetTime();
    const bool detail = state.current_tab == Tab::Album || state.current_tab == Tab::Artist ||
                        state.current_tab == Tab::Queue || state.current_tab == Tab::Settings;

    ScopedNoMinSize no_min_size;
    ImGui::SetNextWindowPos(l.header_pos);
    ImGui::SetNextWindowSize(l.header_size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.content_pad, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_backdrop_window_bg());
    ImGui::Begin("ContentHeader", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();

    // Very subtle top sheen + hairline bottom border. Keeps the bar feeling
    // like a surface instead of a flat black strip.
    dl->AddRectFilledMultiColor(
        wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
        ui_overlay_u32(0.028f),
        ui_overlay_u32(0.028f),
        ui_overlay_u32(0.0f),
        ui_overlay_u32(0.0f));
    dl->AddLine(ImVec2(wp.x, wp.y + ws.y - 1.0f),
                ImVec2(wp.x + ws.x, wp.y + ws.y - 1.0f), ui_hairline_u32(), 1.0f);

    float left = m.content_pad;
    if (detail)
    {
        ImGui::SetCursorPos(ImVec2(m.content_pad, (l.header_h - m.control_h) * 0.5f));
        if (ui_icon_button("##back", UiIcon::ChevronLeft, m.control_h, false, 0, "Back"))
            ui_go_back(state);
        left += m.control_h + m.space_3;
        ImGui::SetCursorPos(ImVec2(left, (l.header_h - m.control_h) * 0.5f));
        if (ui_icon_button("##forward", UiIcon::ChevronRight, m.control_h, false,
                           state.nav_forward_valid ? 0 : ui_text_muted_u32(),
                           "Forward"))
            ui_go_forward(state);
        left += m.control_h + m.space_3;
    }

    // Title/subtitle crossfade + slide whenever the view changes.
    std::string title = view_title(state);
    const std::string subtitle = view_subtitle(state);
    static std::string s_last_title;
    static std::string s_last_subtitle;
    static double s_text_start = -1.0;
    if (s_text_start < 0.0 || title != s_last_title || subtitle != s_last_subtitle)
    {
        s_last_title = title;
        s_last_subtitle = subtitle;
        s_text_start = now;
    }
    auto text_t = [&](float delay) {
        if (!ui_animations_enabled())
            return 1.0f;
        const float dur = ui_anim_duration(0.30f);
        if (dur <= 0.0f)
            return 1.0f;
        const float t = (float)((now - s_text_start - ui_anim_delay(delay)) / dur);
        return ui_anim_ease_out(std::min(std::max(t, 0.0f), 1.0f));
    };
    const float title_t = text_t(0.0f);
    const float sub_t = text_t(0.045f);
    if (title_t < 1.0f || sub_t < 1.0f)
        ui_anim_request();

    // Use the real font metrics so the title + subtitle block is optically
    // centred (the old 1.22x line-height guess pushed it visibly high).
    const float title_base = 30.0f;
    const float sub_base = 16.0f;
    const float title_fs = title_base * m.text_scale;
    const float sub_fs = sub_base * m.text_scale;
    const ImVec2 title_ts =
        g_ui_fonts.semibold->CalcTextSizeA(title_fs, FLT_MAX, 0.0f, title.c_str());
    const ImVec2 sub_ts =
        subtitle.empty()
            ? ImVec2(0.0f, 0.0f)
            : g_ui_fonts.body->CalcTextSizeA(sub_fs, FLT_MAX, 0.0f, subtitle.c_str());
    const float sub_gap = m.space_1 + 2.0f * m.layout_scale;
    const float block_h = title_ts.y + (subtitle.empty() ? 0.0f : sub_gap + sub_ts.y);
    const float top = (l.header_h - block_h) * 0.5f;

    const bool can_search = state.current_tab == Tab::Home ||
                            state.current_tab == Tab::Downloads ||
                            state.current_tab == Tab::Liked ||
                            state.current_tab == Tab::Artists ||
                            state.current_tab == Tab::Playlists ||
                            state.current_tab == Tab::Podcasts;
    const bool home_feed =
        state.current_tab == Tab::Home && state.search_buffer[0] == '\0';

    // Animate the refresh pill's footprint so the search field glides left and
    // right instead of jumping when the button appears/disappears.
    const float refresh_fs = 18.0f;
    const float refresh_text_w = ui_measure_width(g_ui_fonts.medium, refresh_fs, "Refresh");
    const float refresh_icon = 17.0f * m.layout_scale;
    const float refresh_pad = 15.0f * m.layout_scale;
    const float refresh_full_w =
        refresh_pad * 2.0f + refresh_icon + m.space_2 + refresh_text_w;
    const float refresh_compact_w = refresh_pad * 2.0f + refresh_icon;
    const float refresh_gap = m.space_3;
    static float s_refresh_vis = -1.0f;
    if (s_refresh_vis < 0.0f)
        s_refresh_vis = home_feed ? 1.0f : 0.0f;
    const float refresh_target = home_feed ? 1.0f : 0.0f;
    s_refresh_vis = ui_anim_approach(s_refresh_vis, refresh_target, 16.0f);
    if (std::fabs(s_refresh_vis - refresh_target) < 0.002f)
        s_refresh_vis = refresh_target;
    else
        ui_anim_request();

    // Keep the controls pinned to the right edge and give the title room as
    // the window (or UI scale) shrinks. The refresh pill becomes icon-only
    // and the search field narrows before anything can overlap the title.
    const float right_edge = ws.x - m.content_pad;
    const float controls_room = right_edge - left - m.space_4;
    const float title_reserve = std::min(title_ts.x, 220.0f * m.layout_scale);
    float search_w = std::min(420.0f * m.layout_scale, ws.x * 0.38f);
    search_w = std::max(search_w, 180.0f * m.layout_scale);
    bool compact_refresh = false;
    float refresh_w = refresh_full_w;
    if (controls_room < title_reserve + search_w + refresh_gap + refresh_w)
    {
        compact_refresh = true;
        refresh_w = refresh_compact_w;
    }
    if (controls_room < title_reserve + search_w + refresh_gap + refresh_w)
    {
        const float max_search =
            controls_room - title_reserve - refresh_gap - refresh_w;
        search_w = std::max(110.0f * m.layout_scale, max_search);
    }
    if (controls_room < title_reserve + search_w + refresh_gap + refresh_w)
    {
        const float max_search =
            controls_room - 100.0f * m.layout_scale - refresh_gap - refresh_w;
        search_w = std::max(90.0f * m.layout_scale, max_search);
    }
    const float refresh_block = s_refresh_vis * (refresh_gap + refresh_w);
    const float search_x = right_edge - search_w - refresh_block;
    const float title_max =
        std::max(60.0f, (can_search ? search_x : right_edge) - left - m.space_4);

    // Accent marker beside the title (skipped in detail views where the
    // back/forward buttons already anchor the block).
    if (!detail)
    {
        const float marker_w = 3.0f * m.layout_scale;
        const float marker_x = wp.x + left - m.space_2 - marker_w;
        const float marker_h = title_ts.y * 0.82f * title_t;
        const float marker_y = wp.y + top + (title_ts.y - marker_h) * 0.5f;
        if (marker_h > 0.5f)
        {
            dl->AddRectFilled(ImVec2(marker_x - 1.0f, marker_y - 1.0f),
                              ImVec2(marker_x + marker_w + 1.0f, marker_y + marker_h + 1.0f),
                              ui_accent_u32(0.10f * title_t), marker_w * 0.5f + 1.0f);
            dl->AddRectFilled(ImVec2(marker_x, marker_y),
                              ImVec2(marker_x + marker_w, marker_y + marker_h),
                              ui_with_alpha(ui_accent_u32(0.95f), title_t),
                              marker_w * 0.5f);
        }
    }

    const float title_dx = (1.0f - title_t) * ui_anim_offset(10.0f);
    const float sub_dx = (1.0f - sub_t) * ui_anim_offset(7.0f);
    std::string title_draw = ui_ellipsize(g_ui_fonts.semibold, title_base, title, title_max);
    dl->AddText(g_ui_fonts.semibold, title_fs,
                ImVec2(wp.x + left + title_dx, wp.y + top),
                ui_with_alpha(ui_text_u32(), title_t), title_draw.c_str());
    if (!subtitle.empty())
    {
        std::string sub_draw = ui_ellipsize(g_ui_fonts.body, sub_base, subtitle, title_max);
        dl->AddText(g_ui_fonts.body, sub_fs,
                    ImVec2(wp.x + left + sub_dx, wp.y + top + title_ts.y + sub_gap),
                    ui_with_alpha(ui_text_dim_u32(), 0.78f * sub_t), sub_draw.c_str());
    }

    if (can_search)
    {
        const float field_h = m.control_h;
        const float fy = (l.header_h - field_h) * 0.5f;
        ImGui::SetCursorPos(ImVec2(search_x, fy));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(34.0f * m.layout_scale, (field_h - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, m.radius_sm);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ui_c().frame_bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ui_c().frame_bg_hovered);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ui_c().frame_bg_active);
        // Border is drawn manually below so it can animate to the accent on focus.
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, ui_c().text_muted);
        ImGui::SetNextItemWidth(search_w);
        // Let the clear button on the right receive clicks while the field is
        // focused; without this the active InputText swallows the first click.
        ImGui::SetNextItemAllowOverlap();
        if (state.focus_search_requested)
        {
            ImGui::SetKeyboardFocusHere();
            state.focus_search_requested = false;
        }
        if (ImGui::InputTextWithHint("##global_search", "Search", state.search_buffer,
                                     sizeof(state.search_buffer)))
            on_search_edit(state);
        // Enter dispatches immediately instead of waiting out the debounce.
        if (ImGui::IsItemActive() &&
            (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        {
            s_last_edit_time = 0.0;
        }
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(2);

        const ImVec2 field_pos = ImGui::GetItemRectMin();
        const ImVec2 field_max = ImGui::GetItemRectMax();
        const bool search_active = ImGui::IsItemActive();
        const bool search_hovered = ImGui::IsItemHovered();
        static float s_search_glow = 0.0f;
        const float glow_target = search_active ? 1.0f : (search_hovered ? 0.35f : 0.0f);
        s_search_glow = ui_anim_approach(s_search_glow, glow_target, 18.0f);
        if (std::fabs(s_search_glow - glow_target) < 0.002f)
            s_search_glow = glow_target;
        else
            ui_anim_request();

        dl = ImGui::GetWindowDrawList();
        if (s_search_glow > 0.01f)
        {
            dl->AddRect(ImVec2(field_pos.x - 1.5f, field_pos.y - 1.5f),
                        ImVec2(field_max.x + 1.5f, field_max.y + 1.5f),
                        ui_accent_u32(0.10f * s_search_glow), m.radius_sm + 1.5f, 0, 3.0f);
        }
        const ImU32 border = s_search_glow > 0.01f
                                 ? ui_accent_u32(0.30f + 0.35f * s_search_glow)
                                 : ui_border_u32();
        dl->AddRect(field_pos, field_max, border, m.radius_sm, 0, 1.0f);
        ui_draw_icon(dl, UiIcon::Search,
                     ImVec2(field_pos.x + 15.0f * m.layout_scale,
                            (field_pos.y + field_max.y) * 0.5f),
                     16.0f * m.layout_scale,
                     s_search_glow > 0.01f ? ui_accent_text_u32() : ui_text_muted_u32(), 1.5f);
        if (state.search_buffer[0])
        {
            const float cb_size = m.control_h * 0.82f;
            ImGui::SetCursorScreenPos(ImVec2(field_max.x - cb_size - 4.0f * m.layout_scale,
                                             field_pos.y + (field_h - cb_size) * 0.5f));
            if (ui_icon_button("##clear_search", UiIcon::Close, cb_size, false, 0, "Clear"))
            {
                // Drop every bit of search state so Home falls straight back
                // to its feed. The input releases the active id on this click
                // thanks to SetNextItemAllowOverlap above.
                ui_reset_search(state);
                std::atomic_store(&state.search_results,
                                  std::make_shared<const std::vector<Track>>());
                s_pending_search = false;
                s_last_edit_time = 0.0;
                state.current_playlist_source = PlaylistSource::HOME;
            }
        }

        if (s_refresh_vis > 0.01f)
        {
            const ImVec2 rp(wp.x + search_x + search_w + refresh_gap, wp.y + fy);
            if (header_refresh_button(state, rp, refresh_w, field_h, s_refresh_vis,
                                      compact_refresh))
                state.refresh_requested = true;
        }
    }

    dispatch_search(state);
    ImGui::End();
}
