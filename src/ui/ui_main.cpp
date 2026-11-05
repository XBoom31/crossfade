#include "tasks.h"
#include "ui.h"
#include "ui_backdrop.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "mpvplayer.h"
#include "ytdlp.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <thread>
#include <unordered_set>

static TrackListPtr source_for_tab(AppState &state)
{
    switch (state.current_tab)
    {
    case Tab::Home:
        return strlen(state.search_buffer) > 0 ? std::atomic_load(&state.search_results)
                                               : std::atomic_load(&state.recommendations);
    case Tab::Downloads:
        return alias_of(state.downloaded_tracks);
    case Tab::Radio:
        return std::atomic_load(&state.radio_results);
    case Tab::Liked:
        return alias_of(state.liked_tracks);
    case Tab::History:
        // Display uses the snapshot taken on tab entry (see activate_tab);
        // the live vector keeps feeding playback order.
        if (state.history_view)
            return state.history_view;
        return alias_of(state.playback_history);
    case Tab::Smart:
        if (state.smart_view)
            return state.smart_view;
        return alias_of(state.playback_history);
    default:
        return nullptr;
    }
}

static bool is_local_search_list(const AppState &state)
{
    return state.local_search_active &&
           state.current_tab == state.local_search_tab &&
           (state.current_tab == Tab::Downloads || state.current_tab == Tab::Liked);
}

static std::string lower_copy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

static void sort_tracks_for_view(std::vector<Track> &tracks, int mode, const AppState &state)
{
    if (mode == 1)
    {
        std::stable_sort(tracks.begin(), tracks.end(), [](const Track &a, const Track &b) {
            return lower_copy(a.title) < lower_copy(b.title);
        });
    }
    else if (mode == 2)
    {
        std::stable_sort(tracks.begin(), tracks.end(), [](const Track &a, const Track &b) {
            const std::string aa = lower_copy(a.artist) + "\x1f" + lower_copy(a.title);
            const std::string bb = lower_copy(b.artist) + "\x1f" + lower_copy(b.title);
            return aa < bb;
        });
    }
    else if (mode == 3)
    {
        std::stable_sort(tracks.begin(), tracks.end(), [](const Track &a, const Track &b) {
            return a.duration_sec < b.duration_sec;
        });
    }
    else if (mode == 4)
    {
        std::stable_sort(tracks.begin(), tracks.end(), [&](const Track &a, const Track &b) {
            auto plays = [&](const Track &t) {
                auto it = state.play_counts.find(t.id);
                return it == state.play_counts.end() ? 0 : it->second;
            };
            return plays(a) > plays(b);
        });
    }
}

static bool search_chip(const char *label, bool active, float width = 0.0f)
{
    const UiMetrics &m = ui_m();
    const float h = m.control_h * 0.86f;
    if (width <= 0.0f)
        width = ImGui::CalcTextSize(label).x + m.space_4 * 1.6f;

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##search_chip", ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 pmax(pos.x + width, pos.y + h);
    if (active)
        dl->AddRectFilled(pos, pmax, ui_accent_u32(0.94f), m.radius_sm);
    else
    {
        dl->AddRectFilled(pos, pmax, ui_overlay_u32(hovered ? 0.065f : 0.022f), m.radius_sm);
        dl->AddRect(pos, pmax, ui_border_u32(), m.radius_sm, 0, 1.0f);
    }

    ImFont *font = g_ui_fonts.body;
    const float fs = 18.0f * m.text_scale;
    std::string text = ui_ellipsize(font, 18.0f, label, std::max(1.0f, width - m.space_3 * 2.0f));
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
    const ImU32 col = active ? ui_col(ui_c().on_accent)
                             : (hovered ? ui_text_u32() : ui_text_dim_u32());
    dl->AddText(font, fs, ImVec2(pos.x + (width - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col, text.c_str());
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

// Recent searches are a compact, wrapping row of pills rather than a list of
// full-width buttons: clock icon, query text, and a subtle accent tint on
// hover. Width is shared with the layout pass so wrapping is measured, not
// guessed from the previous item's rect.
static float recent_search_chip_width(const std::string &label, float max_width)
{
    const UiMetrics &m = ui_m();
    const float pad_x = m.space_3;
    const float icon = 15.0f * m.layout_scale;
    const float gap = m.space_2;
    const float text = ui_measure_width(g_ui_fonts.body, 16.0f, label);
    // +1px slack: text_room is recomputed from rounded positions below, and a
    // hair under the measured width would needlessly ellipsize the last glyph.
    return std::min(pad_x * 2.0f + icon + gap + text + 1.0f, std::max(1.0f, max_width));
}

static bool recent_search_chip(const char *label, float max_width, float appear)
{
    const UiMetrics &m = ui_m();
    const float base = 16.0f;
    const float h = 32.0f * m.layout_scale;
    const float pad_x = m.space_3;
    const float icon = 15.0f * m.layout_scale;
    const float gap = m.space_2;
    const float width = recent_search_chip_width(label, max_width);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##recent_search", ImVec2(width, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 pmax(pos.x + width, pos.y + h);
    const float rounding = h * 0.5f;
    dl->AddRectFilled(pos, pmax,
                      hovered ? ui_accent_u32(0.10f * appear)
                              : ui_overlay_u32(0.035f * appear),
                      rounding);
    dl->AddRect(pos, pmax,
                hovered ? ui_accent_u32(0.45f * appear)
                        : ui_with_alpha(ui_border_u32(), 0.85f * appear),
                rounding, 0, 1.0f);

    const float cy = pos.y + h * 0.5f;
    const float icon_cx = pos.x + pad_x + icon * 0.5f;
    ui_draw_icon(dl, UiIcon::History, ImVec2(icon_cx, cy), icon,
                 ui_with_alpha(hovered ? ui_accent_text_u32() : ui_text_muted_u32(), appear),
                 1.4f);

    const float text_x = icon_cx + icon * 0.5f + gap;
    const float text_room = std::max(1.0f, pmax.x - pad_x - text_x);
    const std::string draw = ui_ellipsize(g_ui_fonts.body, base, label, text_room);
    const ImVec2 ts =
        g_ui_fonts.body->CalcTextSizeA(base * m.text_scale, FLT_MAX, 0.0f, draw.c_str());
    dl->AddText(g_ui_fonts.body, base * m.text_scale, ImVec2(text_x, cy - ts.y * 0.5f),
                ui_with_alpha(hovered ? ui_text_u32() : ui_text_dim_u32(), appear),
                draw.c_str());
    return clicked;
}

static void draw_recent_searches(AppState &state)
{
    if (state.search_recent.empty())
        return;
    const UiMetrics &m = ui_m();

    // Replay a short stagger whenever Home reappears (or the user leaves and
    // returns to the feed). A gap in frame numbers means the section was not
    // drawn, so this resets naturally on tab changes and search exit.
    static uint64_t s_last_epoch = (uint64_t)-1;
    static double s_start = 0.0;
    const double now = ImGui::GetTime();
    if (s_last_epoch != ui_page_epoch() || s_start <= 0.0)
        s_start = now;
    s_last_epoch = ui_page_epoch();

    const bool animate = ui_animations_enabled();
    const double elapsed = now - s_start;
    if (animate && elapsed < 0.62)
        ui_anim_request();
    auto appear_at = [&](float delay) {
        if (!animate)
            return 1.0f;
        const float t = ((float)elapsed - delay) / 0.30f;
        return ui_anim_ease_out(std::min(std::max(t, 0.0f), 1.0f));
    };

    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const float header_h = 30.0f * m.layout_scale;
    const float header_t = appear_at(0.0f);
    const float header_off = (1.0f - header_t) * ui_anim_offset(6.0f);
    ImGui::Dummy(ImVec2(avail, header_h));

    const float header_cy = origin.y + header_h * 0.5f + header_off;
    const float title_fs = 17.0f * m.text_scale;
    const char *title = "Recent searches";
    const ImVec2 title_ts =
        g_ui_fonts.semibold->CalcTextSizeA(title_fs, FLT_MAX, 0.0f, title);
    dl->AddText(g_ui_fonts.semibold, title_fs, ImVec2(origin.x, header_cy - title_ts.y * 0.5f),
                ui_with_alpha(ui_text_u32(), header_t), title);

    // Right-aligned "Clear" text link.
    const float link_fs = 15.0f * m.text_scale;
    const char *clear_label = "Clear";
    const ImVec2 clear_ts =
        g_ui_fonts.body->CalcTextSizeA(link_fs, FLT_MAX, 0.0f, clear_label);
    const float link_pad = m.space_2;
    const float link_x = origin.x + avail - clear_ts.x - link_pad;
    ImGui::SetCursorScreenPos(ImVec2(link_x - link_pad * 0.5f, origin.y + header_off));
    ImGui::InvisibleButton("##clear_recent", ImVec2(clear_ts.x + link_pad, header_h));
    const bool clear_hovered = ImGui::IsItemHovered();
    const bool clear_clicked = ImGui::IsItemClicked();
    if (clear_hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    dl->AddText(g_ui_fonts.body, link_fs, ImVec2(link_x, header_cy - clear_ts.y * 0.5f),
                ui_with_alpha(clear_hovered ? ui_text_u32() : ui_text_muted_u32(), header_t),
                clear_label);

    const float chip_h = 32.0f * m.layout_scale;
    const float gap_x = m.space_2;
    const float row_top = origin.y + header_h + m.space_1;

    // Single line: keep the chips that fit the current width and collapse the
    // rest into a compact "+N" chip, so narrow windows stay on one row.
    const size_t shown = std::min<size_t>(state.search_recent.size(), 10);
    std::vector<float> widths(shown);
    size_t fit = 0;
    float x = 0.0f;
    for (size_t i = 0; i < shown; ++i)
    {
        widths[i] = recent_search_chip_width(state.search_recent[i], avail);
        const float need = (fit > 0 ? gap_x : 0.0f) + widths[i];
        if (x + need > avail)
            break;
        x += need;
        fit = i + 1;
    }
    const size_t hidden = shown - fit;

    x = 0.0f;
    for (size_t i = 0; i < fit; ++i)
    {
        const float t = appear_at(0.06f + (float)i * 0.035f);
        const float off = (1.0f - t) * ui_anim_offset(10.0f);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + x, row_top + off));
        ImGui::PushID((int)i);
        if (recent_search_chip(state.search_recent[i].c_str(), avail, t))
            ui_set_search(state, state.search_recent[i]);
        ImGui::PopID();
        x += widths[i] + gap_x;
    }

    if (hidden > 0 && x + gap_x <= avail)
    {
        // "+N" overflow chip: lists the hidden queries in a tooltip.
        char ovfl[32];
        std::snprintf(ovfl, sizeof(ovfl), "+%zu", hidden);
        const float fs = 16.0f * m.text_scale;
        const ImVec2 ts = g_ui_fonts.body->CalcTextSizeA(fs, FLT_MAX, 0.0f, ovfl);
        const float w = ts.x + m.space_3 * 2.0f;
        const float t = appear_at(0.06f + (float)fit * 0.035f);
        const float off = (1.0f - t) * ui_anim_offset(10.0f);
        const ImVec2 pos(origin.x + x + gap_x, row_top + off);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(9900);
        ImGui::InvisibleButton("##recent_overflow", ImVec2(w, chip_h));
        const bool hov = ImGui::IsItemHovered();
        if (hov)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        const ImVec2 pmax(pos.x + w, pos.y + chip_h);
        dl->AddRectFilled(pos, pmax, ui_overlay_u32((hov ? 0.065f : 0.035f) * t), chip_h * 0.5f);
        dl->AddRect(pos, pmax, ui_with_alpha(ui_border_u32(), 0.85f * t), chip_h * 0.5f, 0, 1.0f);
        dl->AddText(g_ui_fonts.body, fs, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (chip_h - ts.y) * 0.5f),
                    ui_with_alpha(ui_text_dim_u32(), t), ovfl);
        if (hov && ImGui::BeginTooltip())
        {
            for (size_t i = fit; i < shown; ++i)
                ImGui::BulletText("%s", state.search_recent[i].c_str());
            ImGui::EndTooltip();
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, row_top + chip_h + m.space_4));

    if (clear_clicked)
    {
        state.search_recent.clear();
        save_search_recent(state.search_recent);
    }
}

static void draw_search_chips(AppState &state)
{
    const UiMetrics &m = ui_m();
    static const char *labels[] = {"Songs", "Albums", "Artists", "Playlists"};
    const float gap = m.space_1;
    const float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f);
    for (int i = 0; i < 4; ++i)
    {
        if (i > 0)
            ImGui::SameLine(0.0f, gap);
        ImGui::PushID(i);
        if (search_chip(labels[i], state.search_filter == i, width))
            state.search_filter = i;
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0.0f, m.space_3));
}

static void draw_search_album_grid(AppState &state)
{
    const UiMetrics &m = ui_m();
    auto results = std::atomic_load(&state.search_album_results);
    if (!results || results->empty())
    {
        if (state.search_loading)
            ui_searching_state(UiIcon::Search, "Searching albums", "Looking across YouTube Music");
        else
            ui_empty_state(UiIcon::Album, "No albums found", "Try a different search");
        return;
    }

    const float gap = m.card_gap;
    const float available = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((available + gap) / (180.0f * m.layout_scale + gap)));
    cols = std::min(cols, 6);
    const float col_w = (available - gap * (cols - 1)) / cols;
    const float art = std::min(col_w, 210.0f * m.layout_scale);

    for (int i = 0; i < (int)results->size(); ++i)
    {
        const AlbumSummary &album = (*results)[i];
        const int col = i % cols;
        if (col == 0 && i > 0)
            ImGui::Dummy(ImVec2(0.0f, m.space_1));
        if (col > 0)
            ImGui::SameLine(0.0f, gap);

        ImGui::PushID(i);
        std::string meta = album.artist;
        if (!album.year.empty())
            meta += (meta.empty() ? "" : "  ·  ") + album.year;
        MediaCardResult r = ui_media_card("##search_album", album.title, meta,
                                          album.thumbnail_url, col_w, art, false, false);
        if (r.clicked && !r.double_clicked)
            open_album_view(state, album.browse_id);
        if (r.right_clicked)
            ImGui::OpenPopup("##search_album_ctx");
        if (ImGui::BeginPopup("##search_album_ctx"))
        {
            if (ImGui::MenuItem("Open album"))
                open_album_view(state, album.browse_id);
            if (!is_album_saved(state, album.browse_id) && ImGui::MenuItem("Save album"))
            {
                SavedAlbum a;
                a.browse_id = album.browse_id;
                a.title = album.title;
                a.artist = album.artist;
                a.year = album.year;
                a.thumbnail_url = album.thumbnail_url;
                save_album(state, a);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

static void draw_search_artist_grid(AppState &state)
{
    const UiMetrics &m = ui_m();
    auto results = std::atomic_load(&state.search_artist_results);
    if (!results || results->empty())
    {
        if (state.search_loading)
            ui_searching_state(UiIcon::Search, "Searching artists", "Looking across YouTube Music");
        else
            ui_empty_state(UiIcon::Artist, "No artists found", "Try a different name");
        return;
    }

    const float gap = m.card_gap;
    const float available = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((available + gap) / (150.0f * m.layout_scale + gap)));
    cols = std::min(cols, 6);
    const float col_w = (available - gap * (cols - 1)) / cols;
    const float art = std::min(col_w, 190.0f * m.layout_scale);

    for (int i = 0; i < (int)results->size(); ++i)
    {
        const ArtistSummary &artist = (*results)[i];
        const int col = i % cols;
        if (col == 0 && i > 0)
            ImGui::Dummy(ImVec2(0.0f, m.space_1));
        if (col > 0)
            ImGui::SameLine(0.0f, gap);

        ImGui::PushID(i);
        MediaCardResult r = ui_media_card("##search_artist", artist.name,
                                          artist.subscribers.empty() ? "Artist" : artist.subscribers,
                                          artist.thumbnail_url, col_w, art, true, false);
        if (r.clicked && !r.double_clicked)
            open_artist_view(state, artist.browse_id, artist.name);
        if (r.right_clicked)
            ImGui::OpenPopup("##search_artist_ctx");
        if (ImGui::BeginPopup("##search_artist_ctx"))
        {
            if (ImGui::MenuItem("Open artist"))
                open_artist_view(state, artist.browse_id, artist.name);
            if (!is_artist_saved(state, artist.browse_id) && ImGui::MenuItem("Save artist"))
            {
                SavedArtist a;
                a.browse_id = artist.browse_id;
                a.name = artist.name;
                a.thumbnail_url = artist.thumbnail_url;
                a.subscribers = artist.subscribers;
                save_artist(state, a);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
}

static void draw_search_playlist_grid(AppState &state)
{
    const UiMetrics &m = ui_m();
    const std::string query = lower_copy(std::string(state.search_buffer));
    std::vector<int> matches;
    for (int i = 0; i < (int)state.playlists.size(); ++i)
        if (lower_copy(state.playlists[i].name).find(query) != std::string::npos)
            matches.push_back(i);

    if (matches.empty())
    {
        ui_empty_state(UiIcon::Playlist, "No playlists found", "Try a different name");
        return;
    }

    const float gap = m.card_gap;
    const float available = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((available + gap) / (180.0f * m.layout_scale + gap)));
    cols = std::min(cols, 6);
    const float col_w = (available - gap * (cols - 1)) / cols;
    const float art = std::min(col_w, 210.0f * m.layout_scale);

    for (int n = 0; n < (int)matches.size(); ++n)
    {
        const int pi = matches[n];
        const Playlist &pl = state.playlists[pi];
        const int col = n % cols;
        if (col == 0 && n > 0)
            ImGui::Dummy(ImVec2(0.0f, m.space_1));
        if (col > 0)
            ImGui::SameLine(0.0f, gap);

        ImGui::PushID(pi);
        std::string meta = std::to_string(pl.tracks.size()) + (pl.tracks.size() == 1 ? " track" : " tracks");
        const std::string total = ui_format_duration(ui_tracks_duration(pl.tracks));
        if (!total.empty())
            meta += "  \xC2\xB7  " + total;
        MediaCardResult r = ui_media_card("##search_playlist", pl.name, meta, "", col_w, art, false, false);
        if (r.clicked && !r.double_clicked)
        {
            ui_invalidate_forward(state);
            state.current_tab = Tab::Playlists;
            state.current_playlist_source = PlaylistSource::PLAYLIST;
            state.selected_playlist_index = pi;
            ui_reset_search(state);
        }
        ImGui::PopID();
    }
}

// ----------------------------------------------------------------------
// Library list pages (Liked / Downloads / History)
// ----------------------------------------------------------------------
// These three tabs share the Radio/Stats motion design: one start stamp
// drives a hero-card pop, staggered chips and section fades, and in_view()
// keeps anything below the fold from animating until it scrolls in.
static float ll_clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float ll_ease_out_back(float t)
{
    t = ll_clamp01f(t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

struct ListPageAnim
{
    double now = 0.0;
    double start = 0.0;

    float stagger(float delay, float dur) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float d = ui_anim_delay(delay);
        const float du = ui_anim_duration(dur);
        if (du <= 0.0001f)
            return 1.0f;
        return ui_anim_ease_out(ll_clamp01f((float)((now - start - d) / du)));
    }

    float pop(float delay, float dur) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float d = ui_anim_delay(delay);
        const float du = ui_anim_duration(dur);
        if (du <= 0.0001f)
            return 1.0f;
        return ll_ease_out_back(ll_clamp01f((float)((now - start - d) / du)));
    }

    float in_view(float screen_y, float span = 120.0f) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
        return ui_anim_ease_out(ll_clamp01f((bottom - screen_y) / ui_px(span)));
    }
};

// A small outlined pill for hero metadata ("N tracks", total duration).

// Radio-style hero band for the Liked / Downloads / History tabs: accent art
// tile with a soft glow, micro label, big title, meta chips and actions.
// Returns true when the user asked to clear history.
static bool draw_list_hero(AppState &state, const std::vector<Track> &tracks,
                           bool liked, bool downloads, bool history, const ListPageAnim &anim)
{
    const UiMetrics &m = ui_m();
    const float avail = ImGui::GetContentRegionAvail().x;
    const double now = anim.now;

    const char *eyebrow_text = liked ? "YOUR LIBRARY" : downloads ? "OFFLINE" : "TIMELINE";
    const char *title = liked ? "Liked Songs" : downloads ? "Downloaded Songs" : "Recently Played";
    const char *sub = liked ? "Songs you love, one place"
                           : downloads ? "Stored on this device"
                                       : "Everything you\'ve played, newest first";
    const UiIcon fallback_icon = liked ? UiIcon::Heart : downloads ? UiIcon::Download : UiIcon::History;
    const std::string cover_url = tracks.empty() ? std::string() : tracks.front().thumbnail_url;

    // ---- Radio hero geometry, verbatim ----
    const bool compact_hero = avail < 620.0f * m.layout_scale;
    const bool narrow_hero = avail < 460.0f * m.layout_scale;
    const float hero_h = (narrow_hero ? 272.0f : 210.0f) * m.layout_scale;
    const float pad = m.space_4;
    float art;
    if (narrow_hero)
    {
        art = std::min(avail - pad * 2.0f, avail * 0.30f);
        art = std::max(art, 44.0f * m.layout_scale);
    }
    else
    {
        art = std::min(hero_h - pad * 2.0f, avail * (compact_hero ? 0.18f : 0.30f));
        art = std::max(art, 64.0f * m.layout_scale);
    }
    art = std::max(art, 44.0f * m.layout_scale);
    const float title_base = narrow_hero ? 21.0f : (compact_hero ? 24.5f : 32.0f);
    const float sub_base = narrow_hero ? 13.5f : (compact_hero ? 14.5f : 17.0f);
    const float text_gap = compact_hero ? m.space_4 : m.space_5;
    const float chip_h = 24.0f * m.layout_scale;
    const float btn_h = m.button_h;

    const ImVec2 hero_origin = ImGui::GetCursorScreenPos();
    const float art_x = narrow_hero ? hero_origin.x + (avail - art) * 0.5f
                                    : hero_origin.x + pad;
    const float art_y = hero_origin.y + pad;
    ImGui::Dummy(ImVec2(avail, hero_h));

    const float hero_t = anim.pop(0.0f, 0.62f);
    const float lift = (1.0f - hero_t) * ui_anim_offset(10.0f);
    const float card_scale = 0.965f + 0.035f * hero_t;
    const ImVec2 card_centre(hero_origin.x + avail * 0.5f, hero_origin.y + hero_h * 0.5f);
    auto C = [&](ImVec2 p) {
        return ImVec2(card_centre.x + (p.x - card_centre.x) * card_scale,
                      card_centre.y + (p.y - card_centre.y) * card_scale);
    };

    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Soft accent glow behind the artwork.
    const ImVec2 art_min(art_x, art_y + lift);
    const ImVec2 art_max(art_x + art, art_y + art + lift);
    const ImVec2 art_centre((art_min.x + art_max.x) * 0.5f, (art_min.y + art_max.y) * 0.5f);
    dl->AddCircleFilled(art_centre, art * 0.78f, ui_accent_u32(0.045f * hero_t), 48);
    dl->AddCircleFilled(art_centre, art * 0.62f, ui_accent_u32(0.035f * hero_t), 40);

    // Card shell.
    const ImVec2 card_min = C(hero_origin);
    const ImVec2 card_max = C(ImVec2(hero_origin.x + avail, hero_origin.y + hero_h));
    dl->AddRectFilled(card_min, card_max,
                      ui_with_alpha(ui_col(ui_c().surface_hover), hero_t),
                      m.radius_lg);
    dl->AddRectFilled(card_min, card_max, ui_accent_u32(0.040f * hero_t), m.radius_lg);
    dl->AddRect(card_min, card_max,
                ui_with_alpha(ui_border_u32(), (0.45f + 0.55f * hero_t) * hero_t),
                m.radius_lg, 0, 1.0f);

    // Pulsing rings around the art for the first few seconds (Radio verbatim;
    // they self-terminate and re-fire on page re-entry via the epoch clock).
    const float elapsed = (float)(now - anim.start);
    if (ui_animations_enabled() && elapsed >= 0.0f && elapsed < 4.8f)
    {
        for (int k = 0; k < 2; ++k)
        {
            const float phase = std::fmod(elapsed * 0.55f + (float)k * 0.5f, 1.0f);
            const float grow = (6.0f + 30.0f * phase) * m.layout_scale;
            const float a = 0.16f * (1.0f - phase) * hero_t;
            if (a > 0.005f)
                dl->AddRect(ImVec2(art_min.x - grow, art_min.y - grow),
                            ImVec2(art_max.x + grow, art_max.y + grow),
                            ui_accent_u32(a), m.radius_lg + grow, 0, 1.5f);
        }
        ui_anim_request();
    }

    // Artwork: the list's first track cover (Radio's draw_cover, shared).
    dl->AddRectFilled(ImVec2(art_min.x + 3.0f * m.layout_scale, art_min.y + 4.0f * m.layout_scale),
                      ImVec2(art_max.x + 3.0f * m.layout_scale, art_max.y + 4.0f * m.layout_scale),
                      IM_COL32(0, 0, 0, (int)(120.0f * hero_t)), m.radius_lg);
    ui_draw_cover(dl, cover_url, art_min, art_max, m.radius_lg, 1.0f, fallback_icon,
                  liked ? "liked" : downloads ? "downloads" : "history", true);

    const float text_right = hero_origin.x + avail - pad;
    const float text_x = narrow_hero ? hero_origin.x + pad
                                     : std::min(art_max.x + text_gap, text_right - 1.0f);
    const float text_w = std::max(1.0f, text_right - text_x);
    const float centre_x = hero_origin.x + avail * 0.5f;
    const float text_top = narrow_hero ? (art_y + art + m.space_2 + lift)
                                       : (hero_origin.y + pad + lift);

    const float eyebrow_y = text_top;
    const float title_y = text_top + 22.0f * m.layout_scale;
    const float title_line = title_base * m.text_scale * 1.20f;
    const float sub_y = title_y + title_line + 2.0f * m.layout_scale;
    const float sub_line = sub_base * m.text_scale * 1.24f;
    const float chips_y = sub_y + sub_line + m.space_3;
    const float buttons_y = hero_origin.y + hero_h - pad - btn_h + lift;

    dl->PushClipRect(ImVec2(text_x - 2.0f, hero_origin.y),
                     ImVec2(text_right + 2.0f, hero_origin.y + hero_h), true);

    // Eyebrow with accent dot (fixed alpha -- the lists are not "live").
    {
        const float dot_r = 3.2f * m.layout_scale;
        float eyebrow_x = text_x;
        if (narrow_hero)
        {
            const float group = dot_r * 3.0f +
                                ui_measure_width(g_ui_fonts.medium, 12.0f, eyebrow_text);
            eyebrow_x = centre_x - group * 0.5f;
        }
        dl->AddCircleFilled(ImVec2(eyebrow_x + dot_r, eyebrow_y + 8.0f * m.layout_scale),
                            dot_r, ui_accent_u32(0.85f * hero_t), 16);
        dl->AddText(g_ui_fonts.medium, 12.0f * m.text_scale,
                    ImVec2(eyebrow_x + dot_r * 3.0f, eyebrow_y),
                    ui_with_alpha(ui_accent_text_u32(), 0.82f * hero_t), eyebrow_text);
    }

    const std::string title_draw = ui_ellipsize(g_ui_fonts.semibold, title_base, title, text_w);
    const float title_x =
        narrow_hero ? std::max(text_x,
                               centre_x -
                                   ui_measure_width(g_ui_fonts.semibold, title_base, title_draw) * 0.5f)
                    : text_x;
    dl->AddText(g_ui_fonts.semibold, title_base * m.text_scale, ImVec2(title_x, title_y),
                ui_with_alpha(ui_text_u32(), hero_t), title_draw.c_str());

    const std::string sub_draw = ui_ellipsize(g_ui_fonts.body, sub_base, sub, text_w);
    const float sub_x =
        narrow_hero ? std::max(text_x,
                               centre_x -
                                   ui_measure_width(g_ui_fonts.body, sub_base, sub_draw) * 0.5f)
                    : text_x;
    dl->AddText(g_ui_fonts.body, sub_base * m.text_scale, ImVec2(sub_x, sub_y),
                ui_with_alpha(ui_text_dim_u32(), 0.9f * hero_t), sub_draw.c_str());

    // Meta chips (Radio's rule: drawn only when there is room above the
    // buttons, and only the ones that fit the column).
    if (chips_y + chip_h <= buttons_y - 2.0f * m.layout_scale)
    {
        struct HeroChip
        {
            std::string label;
            bool accent;
        };
        std::vector<HeroChip> chip_list;
        const int n = (int)tracks.size();
        const std::string count_label = std::to_string(n) + (n == 1 ? " TRACK" : " TRACKS");
        chip_list.push_back({count_label, false});
        const int total_sec = ui_tracks_duration(tracks);
        if (total_sec > 0)
            chip_list.push_back({ui_format_duration(total_sec), false});
        if (n > 0 && !tracks[0].artist.empty())
        {
            std::string latest = tracks[0].artist;
            if (history)
                latest = "LATEST \xC2\xB7 " + latest;
            chip_list.push_back({latest, false});
        }

        const float chip_budget = text_right - text_x;
        float chip_total = 0.0f;
        size_t chip_fit = 0;
        std::vector<std::string> fitted(chip_list.size());
        for (size_t i = 0; i < chip_list.size(); ++i)
        {
            const float avail_w = std::max(60.0f, chip_budget - chip_total -
                                                   (chip_fit > 0 ? m.space_2 : 0.0f) -
                                                   20.0f * m.layout_scale);
            fitted[i] = ui_ellipsize(g_ui_fonts.body, 13.0f, chip_list[i].label, avail_w);
            const float cw = ui_measure_width(g_ui_fonts.body, 13.0f, fitted[i]) +
                             20.0f * m.layout_scale;
            const float add = cw + (chip_fit > 0 ? m.space_2 : 0.0f);
            if (chip_total + add > chip_budget)
                break;
            chip_total += add;
            ++chip_fit;
        }

        float cx = narrow_hero ? (centre_x - chip_total * 0.5f) : text_x;
        for (size_t i = 0; i < chip_fit; ++i)
        {
            const float a = anim.stagger(0.16f + 0.08f * (float)i, 0.45f) * hero_t;
            cx += ui_meta_chip(dl, m, fitted[i].c_str(), ImVec2(cx, chips_y),
                               chip_list[i].accent, a) + m.space_2;
        }
    }
    dl->PopClipRect();

    // Actions: Radio's pills, pinned to the card's bottom edge. Degrade to a
    // single compact Play pill when the column is narrow, exactly like the
    // Radio hero.
    const float play_w = 152.0f * m.layout_scale;
    const float seed_w = 126.0f * m.layout_scale;
    const float clear_w = 132.0f * m.layout_scale;
    const bool show_all = text_w >= 288.0f * m.layout_scale;
    const bool can_play = !tracks.empty();
    bool clear = false;

    if (show_all)
    {
        if (ui_pill_button("play_all", "Play all", UiIcon::Play,
                           ImVec2(text_x, buttons_y), ImVec2(play_w, btn_h),
                           true, hero_t, can_play))
            ui_play_track(state, tracks[0], 0);
        bool shuffle_placed = false;
        float next_x = text_x + play_w + m.space_2;
        if ((int)tracks.size() > 1 && next_x + seed_w <= text_right)
        {
            if (ui_pill_button("shuffle", "Shuffle", UiIcon::Shuffle,
                               ImVec2(next_x, buttons_y), ImVec2(seed_w, btn_h),
                               false, hero_t, true))
                ui_play_track(state, tracks[(int)((double)rand() / ((double)RAND_MAX + 1.0) * (int)tracks.size())], -1);
            shuffle_placed = true;
            next_x += seed_w + m.space_2;
        }
        if (history && next_x + clear_w <= text_right)
        {
            if (ui_pill_button("clear", "Clear history", UiIcon::Close,
                               ImVec2(next_x, buttons_y), ImVec2(clear_w, btn_h),
                               false, hero_t, true))
                clear = true;
        }
        (void)shuffle_placed;
    }
    else
    {
        const float w = std::min(text_w, 172.0f * m.layout_scale);
        const float bx = narrow_hero ? centre_x - w * 0.5f : text_x;
        if (ui_pill_button("play_all", w > 128.0f * m.layout_scale ? "Play all" : "Play",
                           UiIcon::Play, ImVec2(bx, buttons_y), ImVec2(w, btn_h),
                           true, hero_t, can_play))
            ui_play_track(state, tracks[0], 0);
        if (history && w + m.space_2 + clear_w <= text_w)
        {
            if (ui_pill_button("clear", "Clear", UiIcon::Close,
                               ImVec2(bx + w + m.space_2, buttons_y), ImVec2(clear_w, btn_h),
                               false, hero_t, true))
                clear = true;
        }
    }

    // Re-anchor the flow in SCREEN space, like the Radio hero -- a
    // content-space SetCursorPosY here would double-apply the child scroll.
    ImGui::SetCursorScreenPos(ImVec2(hero_origin.x, hero_origin.y + hero_h + m.space_4));
    return clear;
}

static void draw_list_page(AppState &state, const AppLayout &layout, float width)
{
    const UiMetrics &m = ui_m();
    std::string page_key = "list_page_" + std::to_string((int)state.current_tab);
    if (state.local_search_active &&
        (state.current_tab == Tab::Downloads || state.current_tab == Tab::Liked))
        page_key += "_search";
    ImGui::PushID(page_key.c_str());
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.content_pad, m.space_3));
    ImGui::BeginChild("ListPage", ImVec2(width, layout.body_size.y),
                      ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child("ListPage");

    TrackListPtr tracks = source_for_tab(state);
    if (is_local_search_list(state))
        tracks = alias_of(state.local_search_results);

    const bool history = state.current_tab == Tab::History;
    const bool downloads = state.current_tab == Tab::Downloads;
    const bool liked = state.current_tab == Tab::Liked;
    const bool home = state.current_tab == Tab::Home;
    const bool hero_tab = (history || downloads || liked) && !is_local_search_list(state);

    // Motion clock: replays the hero entrance whenever the tab reappears.
    const double hero_now = ImGui::GetTime();
    static uint64_t s_list_last_epoch = (uint64_t)-1;
    static double s_list_start = 0.0;
    if (s_list_last_epoch != ui_page_epoch())
        s_list_start = hero_now;
    s_list_last_epoch = ui_page_epoch();
    if (s_list_start <= 0.0)
        s_list_start = hero_now;
    ListPageAnim anim;
    anim.now = hero_now;
    anim.start = s_list_start;
    if (hero_tab && ui_animations_enabled() &&
        (float)(anim.now - anim.start) < ui_anim_delay(1.0f) + ui_anim_duration(0.9f))
        ui_anim_request();

    // Sorted views (Liked/Downloads with a sort mode) are cached: copying
    // and sorting a few thousand rows per frame (with per-comparison string
    // allocations) dominated animation frames. Recompute only when the
    // source list, sort mode, or play counts (used by "Most played") change.
    static std::vector<Track> s_sorted;
    static std::vector<Track> s_sorted_src;
    static int s_sorted_mode = -1;
    static Tab s_sorted_tab = Tab::Home;
    static size_t s_sorted_counts = 0;
    if (tracks && (liked || downloads))
    {
        int &sort_mode = downloads ? state.downloads_sort : state.liked_sort;
        if (sort_mode > 0 && sort_mode <= 4)
        {
            const bool stale = (s_sorted_src.empty() && !tracks->empty()) ||
                               s_sorted_mode != sort_mode || s_sorted_tab != state.current_tab ||
                               s_sorted_counts != state.play_counts.size() ||
                               s_sorted_src.size() != tracks->size() ||
                               (!s_sorted_src.empty() && !tracks->empty() &&
                                (s_sorted_src.front().id != tracks->front().id ||
                                 s_sorted_src.back().id != tracks->back().id));
            if (stale)
            {
                s_sorted = *tracks;
                sort_tracks_for_view(s_sorted, sort_mode, state);
                s_sorted_src = *tracks;
                s_sorted_mode = sort_mode;
                s_sorted_tab = state.current_tab;
                s_sorted_counts = state.play_counts.size();
            }
            tracks = alias_of(s_sorted);
        }
    }
    const char *section = "Tracks";
    std::string meta;

    const bool home_search = home && state.search_buffer[0] != '\0';
    auto album_results = std::atomic_load(&state.search_album_results);
    auto artist_results = std::atomic_load(&state.search_artist_results);
    int playlist_result_count = 0;
    if (home_search && state.search_filter == 3)
    {
        const std::string query = lower_copy(std::string(state.search_buffer));
        for (const auto &pl : state.playlists)
            if (lower_copy(pl.name).find(query) != std::string::npos)
                ++playlist_result_count;
    }

    if (home_search)
    {
        switch (state.search_filter)
        {
        case 1: section = "Album Results"; break;
        case 2: section = "Artist Results"; break;
        case 3: section = "Playlist Results"; break;
        default: section = "Search Results"; break;
        }
    }
    else if (home)
        section = "Made for You";
    else if (downloads)
        section = "Downloaded Songs";
    else if (state.current_tab == Tab::Radio)
        section = "Radio Station";
    else if (state.current_tab == Tab::Liked)
        section = "Liked Songs";
    else if (history)
        section = "Recently Played";
    else if (state.current_tab == Tab::Smart)
        section = state.smart_mode == 0 ? "On Repeat" : "Recently Played";

    int count = tracks ? (int)tracks->size() : 0;
    if (home_search && state.search_filter == 1)
        count = album_results ? (int)album_results->size() : 0;
    else if (home_search && state.search_filter == 2)
        count = artist_results ? (int)artist_results->size() : 0;
    else if (home_search && state.search_filter == 3)
        count = playlist_result_count;

    // NB: emptiness, not nullity -- dispatch_search clears the published
    // vector between searches, leaving a valid shared_ptr to an empty list.
    const bool searching = home_search && state.search_loading &&
                           state.search_filter == 0 && (!tracks || tracks->empty());
    if (is_local_search_list(state))
        section = "Search Results";
    const char *unit = state.search_filter == 1 ? "album" : state.search_filter == 2 ? "artist"
                       : state.search_filter == 3 ? "playlist" : "track";
    meta = searching ? "Searching..." : std::to_string(count) + " " + unit + (count == 1 ? "" : "s");

    // Recent searches sit above the recommendation feed, not between its
    // header and the tracks it describes.
    if (home && !home_search)
        draw_recent_searches(state);

    bool clear_history = false;
    static const std::vector<Track> k_empty_tracks;
    if (hero_tab)
    {
        // The hero band carries title, counts and actions; no plain header.
        clear_history = draw_list_hero(state, tracks ? *tracks : k_empty_tracks,
                                       liked, downloads, history, anim);
    }
    else if (history && count > 0)
        clear_history = ui_section_header(section, meta.c_str(), "Clear history");
    else
        ui_section_header(section, meta.c_str(), nullptr);

    if (clear_history)
    {
        state.playback_history.clear();
        state.history_view = std::make_shared<const std::vector<Track>>();
        save_playback_history(state.playback_history);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopID();
        return;
    }

    if (state.current_tab == Tab::Smart)
    {
        // Segmented view switcher: rebuild the frozen snapshot on switch so
        // the click lands on the list the user asked for.
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f);
        const float seg_w = 150.0f * m.layout_scale;
        if (state.smart_mode == 0 ? ui_primary_button("On Repeat", ImVec2(seg_w, m.control_h))
                                  : ui_ghost_button("On Repeat", ImVec2(seg_w, m.control_h)))
        {
            if (state.smart_mode != 0)
            {
                state.smart_mode = 0;
                build_smart_view(state);
            }
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (state.smart_mode == 1 ? ui_primary_button("Recently Played", ImVec2(seg_w, m.control_h))
                                  : ui_ghost_button("Recently Played", ImVec2(seg_w, m.control_h)))
        {
            if (state.smart_mode != 1)
            {
                state.smart_mode = 1;
                build_smart_view(state);
            }
        }
        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        ImGui::PopStyleVar();
    }

    if (home_search)
    {
        draw_search_chips(state);
        if (state.search_filter != 0)
        {
            if (state.search_filter == 1)
                draw_search_album_grid(state);
            else if (state.search_filter == 2)
                draw_search_artist_grid(state);
            else
                draw_search_playlist_grid(state);
            ImGui::Dummy(ImVec2(0.0f, m.space_4));
            draw_scroll_edge_fades();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopID();
            return;
        }
    }

    // Backend errors surface as toast popups (bottom-right), not inline text.

    if (tracks == nullptr || tracks->empty())
    {
        if (hero_tab)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim.stagger(0.12f, 0.5f));
        if (home && state.home_failed && !searching)
        {
            // The home feed fetch failed with nothing on screen: say why and
            // offer a retry instead of an empty "Nothing here".
            if (ui_error_state(state.home_error_msg.empty()
                                   ? "Couldn't load your recommendations."
                                   : state.home_error_msg.c_str()))
            {
                state.home_failed = false;
                state.home_refreshing = true;
                state.refresh_requested = true;
                state.repaint_requested = true;
            }
        }
        else if (home_search && state.search_failed && !state.search_loading)
        {
            if (ui_error_state(state.search_error_msg.empty()
                                   ? "Couldn't search right now."
                                   : state.search_error_msg.c_str()))
            {
                state.search_failed = false;
                state.search_dirty = true;
                state.queued_search = state.search_buffer;
                state.repaint_requested = true;
            }
        }
        else if (searching)
            ui_searching_state(UiIcon::Search, "Searching YouTube Music", "Looking for tracks");
        else if (home && state.search_buffer[0])
            ui_empty_state(UiIcon::Search, "No results", "Try a different search");
        else if (state.loading && home)
            ui_empty_state(UiIcon::Equalizer, "Loading your music", "Finding something good for you");
        else if (is_local_search_list(state))
            ui_empty_state(UiIcon::Search, "No matches", "Try a different search");
        else if (downloads)
            ui_empty_state(UiIcon::Download, "No downloads yet", "Right-click a track to keep it offline");
        else if (state.current_tab == Tab::Liked)
            ui_empty_state(UiIcon::Heart, "Nothing liked yet", "Tap the heart on a track you love");
        else if (state.current_tab == Tab::History)
            ui_empty_state(UiIcon::History, "No listening history", "Your recently played tracks will appear here");
        else if (state.current_tab == Tab::Radio)
            ui_empty_state(UiIcon::Radio, "Radio is quiet", "Start radio from any track");
        else
            ui_empty_state(UiIcon::Album, "Nothing here",
                           "Press / to search, P to start a party, or ? for shortcuts.");
        if (hero_tab)
            ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopID();
        return;
    }

    if (liked || downloads)
    {
        int &sort_mode = downloads ? state.downloads_sort : state.liked_sort;
        static const char *sort_items[] = {"Default", "Title", "Artist", "Duration", "Most played"};
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, hero_tab ? ll_clamp01f(anim.stagger(0.5f, 0.5f)) : 1.0f);
        // Sort selector as home-style pill chips instead of a combo box.
        const float chip_gap = m.space_1;
        for (int i = 0; i < 5; ++i)
        {
            if (i > 0)
                ImGui::SameLine(0.0f, chip_gap);
            ImGui::PushID(7000 + i);
            if (search_chip(sort_items[i], sort_mode == i))
                sort_mode = i;
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0.0f, m.space_1));
        ImGui::PopStyleVar();
    }

    std::string selection_context;
    if (liked)
        selection_context = "liked";
    else if (downloads)
        selection_context = "downloads";
    if (!selection_context.empty() && state.selection_context != selection_context)
    {
        state.selected_track_ids.clear();
        state.selection_anchor.clear();
        state.selection_context = selection_context;
    }

    if (!selection_context.empty())
        ui_selection_bar(state, *tracks, selection_context.c_str(), true, downloads);

    const float art = ui_elem("list_thumb", 48.0f);
    const float title_line = 24.0f * m.text_scale * 1.22f;
    const float meta_line = 18.0f * m.text_scale * 1.24f;
    const float row_h = std::max(art, title_line + m.space_1 + meta_line) +
                        m.row_pad_y * 2.0f * ui_elem("list_row", 1.0f);
    const float pitch = row_h + m.row_gap;

    std::string reveal_key;
    if (is_local_search_list(state))
    {
        reveal_key = "local_search_";
        reveal_key += std::to_string((int)state.current_tab);
    }
    else if (home)
        reveal_key = state.search_buffer[0] ? "home_search" : "home";
    else
        reveal_key = std::to_string((int)state.current_tab);

    // Home endless scroll: only the newly appended rows should animate; the
    // existing rows keep their (already finished) reveal so the page does
    // not flash/replay on every append.
    static int s_home_prev_count = 0;
    static double s_home_append_start = -1.0;
    static int s_home_append_base = 0;
    static bool s_home_append_active = false;
    static bool s_home_ever_appended = false;
    static int s_home_last_frame = -1;
    const bool is_home_list = home && state.search_buffer[0] == '\0';
    const bool is_search_list = home && state.search_buffer[0] != '\0';
    const int cur_count = tracks ? (int)tracks->size() : 0;
    if (is_home_list)
    {
        // Home was not drawn for a few frames (tab switch, search, lyrics-only):
        // start clean so the full list reveal plays again and the next batch of
        // recommendations is detected from the current count.
        const int frame = ImGui::GetFrameCount();
        if (s_home_last_frame < 0 || frame - s_home_last_frame > 2)
        {
            s_home_prev_count = cur_count;
            s_home_append_start = -1.0;
            s_home_append_base = 0;
            s_home_append_active = false;
            s_home_ever_appended = false;
        }
        s_home_last_frame = frame;

        if (cur_count > s_home_prev_count && s_home_prev_count > 0)
        {
            s_home_append_base = s_home_prev_count;
            s_home_append_start = ImGui::GetTime();
            s_home_append_active = true;
            s_home_ever_appended = true;
        }
        if (s_home_append_active && (ImGui::GetTime() - s_home_append_start > 1.0))
            s_home_append_active = false;
        if (cur_count < s_home_prev_count)
        {
            s_home_ever_appended = false;
            s_home_append_active = false;
        }
        s_home_prev_count = cur_count;
    }

    // Search refines in two stages: local matches appear immediately, then
    // online results are merged in behind them. Only rows that were not
    // already visible should animate; replaying the whole list on every
    // refinement makes the page flash.
    static std::string s_search_query;
    static std::unordered_set<std::string> s_search_seen;
    static std::unordered_set<std::string> s_search_append_ids;
    static double s_search_append_start = -1.0;
    static double s_search_reveal_start = -1.0;
    static uint64_t s_search_epoch = (uint64_t)-1;
    int search_append_first = -1;
    if (is_search_list && tracks)
    {
        const std::string query(state.search_buffer);
        const uint64_t epoch = ui_page_epoch();
        // Reset on a new query or a page change (tab switch / search cleared).
        // Do NOT reset on a frame gap: the app skips idle frames while the
        // backend is still searching, and treating that as a new page replays
        // the whole list when the online results arrive.
        if (s_search_epoch == (uint64_t)-1 || query != s_search_query ||
            epoch != s_search_epoch)
        {
            s_search_seen.clear();
            s_search_append_ids.clear();
            s_search_append_start = -1.0;
            s_search_reveal_start = -1.0;
            s_search_query = query;
            s_search_epoch = epoch;
        }
        if (!tracks->empty() && s_search_reveal_start < 0.0)
            s_search_reveal_start = ImGui::GetTime();

        bool has_new = false;
        for (const auto &t : *tracks)
            if (!s_search_seen.count(t.id))
                has_new = true;

        // Growth within the same query is a refinement; animate only the new
        // ids. The first batch for a query still uses the regular list reveal.
        if (has_new && !s_search_seen.empty())
        {
            s_search_append_ids.clear();
            for (const auto &t : *tracks)
                if (!s_search_seen.count(t.id))
                    s_search_append_ids.insert(t.id);
            s_search_append_start = ImGui::GetTime();
        }
        if (s_search_append_start > 0.0 &&
            ImGui::GetTime() - s_search_append_start > 1.0)
        {
            s_search_append_ids.clear();
            s_search_append_start = -1.0;
        }
        if (!s_search_append_ids.empty())
        {
            for (int i = 0; i < (int)tracks->size(); ++i)
                if (s_search_append_ids.count((*tracks)[i].id))
                {
                    search_append_first = i;
                    break;
                }
        }

        s_search_seen.clear();
        for (const auto &t : *tracks)
            s_search_seen.insert(t.id);
    }

    double base_reveal = -1.0;
    if (tracks)
    {
        const bool home_refining =
            is_home_list && (s_home_append_active || s_home_ever_appended);
        const bool search_refining = is_search_list && !s_search_append_ids.empty();
        if (home_refining || search_refining)
        {
            base_reveal = -1.0; // existing rows: no replay after an append
        }
        else if (is_search_list)
        {
            // Search has its own reveal clock: the initial batch (local or
            // remote) animates once, later refinements only animate new rows.
            base_reveal = s_search_reveal_start;
        }
        else
        {
            base_reveal = ui_anim_track_list_reveal(
                reveal_key.c_str(), *tracks, (uint64_t)(uintptr_t)tracks.get());
        }
    }

    const float start_y = ImGui::GetCursorPosY();
    ImGuiListClipper clipper;
    clipper.Begin(count, pitch);
    while (clipper.Step())
    {
        ImGui::SetCursorPosY(start_y + clipper.DisplayStart * pitch);
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            const Track &track = (*tracks)[i];
            TrackRowOptions opt;
            opt.show_duration = true;
            opt.show_plays = home || liked || history || downloads;
            opt.show_delete = downloads;
            opt.art_size = art;
            double row_reveal = base_reveal;
            const bool appended_row =
                is_home_list && s_home_ever_appended && i >= s_home_append_base;
            const bool search_appended =
                is_search_list && s_search_append_ids.count(track.id) > 0;
            if (appended_row && s_home_append_active &&
                (ImGui::GetTime() - s_home_append_start < 1.0))
                row_reveal = s_home_append_start;
            else if (search_appended)
                row_reveal = s_search_append_start;
            opt.reveal_start = row_reveal;
            // Stagger from the first row currently on screen (or from the start
            // of the appended batch) instead of the absolute list position:
            // a scrolled viewport would otherwise clamp every visible row to
            // the same delay and make the whole page animate as one block.
            const int reveal_base =
                search_appended
                    ? std::max(0, search_append_first)
                    : (appended_row ? s_home_append_base : clipper.DisplayStart);
            opt.reveal_index = std::max(0, i - reveal_base);
            opt.stagger_scroll_reveal = is_home_list || search_appended;
            opt.selected = !selection_context.empty() && state.selected_track_ids.count(track.id) > 0;
            TrackRowResult row = ui_track_row(state, track, i, opt);
            if (row.deleted)
            {
                state.selected_track_ids.erase(track.id);
                if (state.selection_anchor == track.id)
                    state.selection_anchor.clear();
            }
            if (!selection_context.empty() && row.clicked)
            {
                if (row.shift_clicked && !state.selection_anchor.empty())
                {
                    int anchor_idx = -1;
                    for (int j = 0; j < count; ++j)
                    {
                        if ((*tracks)[j].id == state.selection_anchor)
                        {
                            anchor_idx = j;
                            break;
                        }
                    }
                    if (anchor_idx >= 0)
                    {
                        const int lo = std::min(anchor_idx, i);
                        const int hi = std::max(anchor_idx, i);
                        for (int j = lo; j <= hi; ++j)
                            state.selected_track_ids.insert((*tracks)[j].id);
                    }
                    else
                    {
                        state.selected_track_ids.insert(track.id);
                        state.selection_anchor = track.id;
                    }
                }
                else if (row.ctrl_clicked)
                {
                    if (state.selected_track_ids.count(track.id))
                        state.selected_track_ids.erase(track.id);
                    else
                        state.selected_track_ids.insert(track.id);
                    state.selection_anchor = track.id;
                }
                else
                {
                    state.selected_track_ids.clear();
                    state.selection_anchor.clear();
                }
            }
        }
    }
    // Home endless scroll: fetch the next batch when the user nears the
    // bottom of the home list.
    if (state.current_tab == Tab::Home)
    {
        float scroll_y = ImGui::GetScrollY();
        float scroll_max = ImGui::GetScrollMaxY();
        if (state.search_buffer[0] == '\0' && state.home_has_more && !state.home_loading_more.load() &&
            scroll_max > 0.0f && scroll_y >= scroll_max - (pitch * 30.0f))
        {
            state.home_loading_more = true;
            int current_count = tracks ? (int)tracks->size() : 0;
            tasks::spawn([&state, current_count]() {
                auto more = fetch_recommendations(true, current_count + 40);
                prefetch_urls_for(more, 10);
                auto cur = std::atomic_load(&state.recommendations);
                std::vector<Track> merged = cur ? *cur : std::vector<Track>{};
                std::unordered_set<std::string> seen;
                for (auto &t : merged)
                    seen.insert(t.id);
                int added = 0;
                for (auto &t : more)
                {
                    if (seen.count(t.id))
                        continue;
                    seen.insert(t.id);
                    merged.push_back(t);
                    ++added;
                }
                if (added > 0)
                {
                    std::atomic_store(&state.recommendations,
                                     std::make_shared<const std::vector<Track>>(std::move(merged)));
                }
                else
                {
                    state.home_has_more = false;
                }
                state.home_loading_more = false;
                state.repaint_requested = true;
            });
        }
    }

    if (home && state.search_buffer[0] == '\0' && state.home_loading_more.load())
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float scale = m.layout_scale;
        const double now = ImGui::GetTime();
        const float anim = ui_animations_enabled() ? 1.0f : 0.0f;

        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_dim);
        const std::string label = "Loading more tracks";
        float label_w = ImGui::CalcTextSize(label.c_str()).x;
        ImGui::SetCursorPosX((avail - label_w) * 0.5f);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0.0f, m.space_2));

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float row_pad_x = m.row_pad_x;
        const float art = 48.0f * scale;
        const float more_w = 32.0f * scale;
        const float dur_w = 58.0f * scale;
        const float actions_w = more_w + m.space_2;
        const float hit_w = std::max(1.0f, avail - actions_w);
        const float text_x_off = row_pad_x + art + m.space_3;
        const float text_w = std::max(1.0f, hit_w - text_x_off - dur_w - m.space_2);
        const float title_line = 24.0f * m.text_scale * 1.22f;
        const float meta_line = 18.0f * m.text_scale * 1.24f;
        const float row_h = std::max(art, title_line + m.space_1 + meta_line) + m.row_pad_y * 2.0f;
        const float gap = m.row_gap;
        const int rows = 4;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        for (int i = 0; i < rows; ++i)
        {
            const float y = origin.y + (float)i * (row_h + gap);
            const float ax = origin.x + row_pad_x;
            const float ay = y + (row_h - art) * 0.5f;
            const float tx = origin.x + text_x_off;
            const float title_y = y + (row_h - (title_line + m.space_1 + meta_line)) * 0.5f;
            const float th = title_line;
            const float mh = meta_line * 0.85f;
            const float breath = 0.5f + 0.5f * sinf((float)now * 1.8f + (float)i * 0.7f);
            const float base_a = anim > 0.0f ? 0.030f + 0.024f * breath : 0.045f;

            // art square
            dl->AddRectFilled(ImVec2(ax, ay), ImVec2(ax + art, ay + art),
                              ui_overlay_u32(base_a), m.radius_sm);

            // title line
            dl->AddRectFilled(ImVec2(tx, title_y), ImVec2(tx + text_w, title_y + th * 0.72f),
                              ui_overlay_u32(base_a), th * 0.36f);
            // meta line
            dl->AddRectFilled(ImVec2(tx, title_y + title_line + m.space_1),
                              ImVec2(tx + text_w * 0.62f, title_y + title_line + m.space_1 + mh * 0.72f),
                              ui_overlay_u32(base_a * 0.8f), mh * 0.36f);

            // duration block on right
            const float dur_x = origin.x + hit_w - dur_w - m.space_2;
            const float dur_y = title_y + (title_line - 12.0f * m.text_scale) * 0.5f;
            dl->AddRectFilled(ImVec2(dur_x, dur_y),
                              ImVec2(dur_x + dur_w * 0.7f, dur_y + 12.0f * m.text_scale),
                              ui_overlay_u32(base_a * 0.6f), 3.0f * scale);

            // shimmer across art + title
            const float phase = fmodf((float)now * 0.42f + (float)i * 0.17f, 1.55f) - 0.25f;
            if (anim > 0.0f && phase > 0.0f && phase < 1.0f)
            {
                // art shimmer
                {
                    const float band = art * 0.18f;
                    const float centre = ax + art * phase;
                    const float x0 = std::max(ax, centre - band);
                    const float x1 = std::min(ax + art, centre + band);
                    if (x1 > x0)
                    {
                        const float a = 0.10f + 0.12f * (1.0f - fabsf(phase - 0.5f) * 2.0f);
                        dl->AddRectFilled(ImVec2(x0, ay), ImVec2(x1, ay + art),
                                          ui_accent_u32(a), m.radius_sm);
                    }
                }
                // title shimmer
                {
                    const float band = text_w * 0.18f;
                    const float centre = tx + text_w * phase;
                    const float x0 = std::max(tx, centre - band);
                    const float x1 = std::min(tx + text_w, centre + band);
                    if (x1 > x0)
                    {
                        const float a = 0.08f + 0.10f * (1.0f - fabsf(phase - 0.5f) * 2.0f);
                        dl->AddRectFilled(ImVec2(x0, title_y), ImVec2(x1, title_y + th * 0.72f),
                                          ui_accent_u32(a), th * 0.36f);
                    }
                }
            }
        }
        ImGui::Dummy(ImVec2(0.0f, (float)rows * (row_h + gap) + ui_px(6.0f)));
        ui_anim_request();
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_4));
    draw_scroll_edge_fades();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopID();
}

void draw_main_ui(AppState &state, MpvPlayer &player, bool show_recommendations)
{
    (void)show_recommendations;
    resolve_current_track_links(state);
    const AppLayout layout = ui_compute_layout(state, ImGui::GetIO());
    const UiMetrics &m = ui_m();

    // Tab changes replay both the content fade and the list stagger, even
    // when the destination tab still has the same tracks as last time.
    static int s_fade_tab = -1;
    const int tab_index = (int)state.current_tab;
    if (s_fade_tab < 0)
        s_fade_tab = tab_index;
    else if (tab_index != s_fade_tab)
    {
        s_fade_tab = tab_index;
        ui_anim_reset_list_reveals();
        ui_bump_page_epoch();
    }

    // Ending a search by deleting the text (not Esc) also re-reveals the
    // underlying page; the old frame-gap heuristic caught that, so bump the
    // epoch on the search-cleared transition too.
    static bool s_had_search = false;
    const bool has_search = state.search_buffer[0] != '\0';
    if (s_had_search && !has_search)
        ui_bump_page_epoch();
    s_had_search = has_search;

    ScopedNoMinSize no_min_size;
    ImGui::SetNextWindowPos(layout.body_pos);
    ImGui::SetNextWindowSize(layout.body_size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_backdrop_window_bg());
    ImGui::Begin("MainContent", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    const float lyrics_w = layout.lyrics_open ? layout.lyrics_w : 0.0f;
    const bool lyrics_only = layout.lyrics_open && !layout.lyrics_docked;
    const float list_w = (layout.lyrics_open && layout.lyrics_docked)
                             ? std::max(0.0f, layout.body_size.x - lyrics_w - layout.gap)
                             : layout.body_size.x;

    const bool single_panel = state.current_tab == Tab::Settings ||
                              state.current_tab == Tab::Radio ||
                              state.current_tab == Tab::Queue ||
                              state.current_tab == Tab::Party ||
                              state.current_tab == Tab::Playlists ||
                              state.current_tab == Tab::Albums ||
                              state.current_tab == Tab::Artists ||
                              state.current_tab == Tab::Stats ||
                              state.current_tab == Tab::Album ||
                              state.current_tab == Tab::Artist ||
                              state.current_tab == Tab::YouTube;

    // Divider between the list and a docked lyrics panel: drag to resize.
    // The new width is persisted through settings.json on release.
    if (layout.lyrics_open && layout.lyrics_docked && layout.gap > 0.0f)
    {
        const float hx = list_w;
        const float hw = layout.gap;
        ImGui::SetCursorPos(ImVec2(hx, 0.0f));
        ImGui::InvisibleButton("##lyrics_resize", ImVec2(hw, layout.body_size.y));
        const bool hov = ImGui::IsItemHovered();
        const bool act = ImGui::IsItemActive();
        if (hov || act)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (act)
        {
            const float mouse_x = ImGui::GetIO().MousePos.x - ImGui::GetWindowPos().x;
            float new_w = layout.body_size.x - mouse_x;
            new_w = std::max(layout.lyrics_min_w, std::min(layout.lyrics_max_w, new_w));
            const float scale = std::max(0.001f, ui_m().layout_scale);
            state.settings.lyrics_panel_width = new_w / scale;
        }
        if (ImGui::IsItemDeactivated())
            save_settings(state.settings);

        const bool lit = hov || act;
        const ImU32 col = ImGui::GetColorU32(lit ? ImGuiCol_SeparatorActive
                                                 : ImGuiCol_Separator);
        const float half = lit ? ui_px(1.0f) : ui_px(0.5f);
        const float cx = ImGui::GetWindowPos().x + hx + hw * 0.5f;
        const float y0 = ImGui::GetWindowPos().y + ui_px(4.0f);
        const float y1 = ImGui::GetWindowPos().y + layout.body_size.y - ui_px(4.0f);
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(cx - half, y0),
                                                  ImVec2(cx + half, y1), col, half);
    }

    if (!lyrics_only && single_panel)
    {
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(m.content_pad, m.space_3));
        std::string panel_key = "page_" + std::to_string((int)state.current_tab);
        if (state.current_tab == Tab::Album)
            panel_key += "_" + state.album_browse_id;
        else if (state.current_tab == Tab::Artist)
            panel_key += "_" + state.artist_browse_id;
        ImGui::PushID(panel_key.c_str());
        const bool scroll_self = state.current_tab == Tab::Queue ||
                                 state.current_tab == Tab::Playlists ||
                                 state.current_tab == Tab::Albums ||
                                 state.current_tab == Tab::Artists ||
                                 state.current_tab == Tab::Album ||
                                 state.current_tab == Tab::Artist ||
                                 state.current_tab == Tab::YouTube;
        ImGui::BeginChild("PageContent", ImVec2(list_w, layout.body_size.y),
                          ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
        if (!scroll_self)
            smooth_scroll_child("PageContent");

        if (state.current_tab == Tab::Settings)
            draw_settings_panel(state);
        else if (state.current_tab == Tab::Radio)
            draw_radio_panel(state);
        else if (state.current_tab == Tab::Queue)
            draw_queue_panel(state);
        else if (state.current_tab == Tab::Party)
            draw_party_panel(state);
        else if (state.current_tab == Tab::Playlists)
            draw_playlists_panel(state);
        else if (state.current_tab == Tab::Albums)
            draw_albums_panel(state);
        else if (state.current_tab == Tab::Album)
            draw_album_panel(state);
        else if (state.current_tab == Tab::Artists)
            draw_artists_panel(state);
        else if (state.current_tab == Tab::Artist)
            draw_artist_panel(state);
        else if (state.current_tab == Tab::Stats)
            draw_stats_panel(state);
        else if (state.current_tab == Tab::YouTube)
            draw_youtube_panel(state);

        ImGui::EndChild();
        ImGui::PopID();
        ImGui::PopStyleVar();
    }
    else if (!lyrics_only)
    {
        draw_list_page(state, layout, list_w);
    }

    if (layout.lyrics_open)
    {
        if (lyrics_only)
        {
            ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
            draw_lyrics_panel(state, player, layout.body_size.x, layout.body_size.y);
        }
        else
        {
            ImGui::SetCursorPos(ImVec2(layout.body_size.x - lyrics_w, 0.0f));
            draw_lyrics_panel(state, player, layout.lyrics_w, layout.body_size.y);
        }
    }

    ImGui::End();
}
