#include "ui_components.h"
#include "ui.h"
#include "ui_theme.h"
#include "ui_backdrop.h"
#include "ytdlp.h"
#include "tasks.h"
#include "texture.h"
#include "writeback.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>

static float button_text_room(const char *label);

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Per-row hover fade state, keyed by the row's ImGui id. Rows only exist
// while visible, so this stays small.
static std::unordered_map<ImGuiID, float> g_row_hover;

static std::string fmt_duration(const Track &t)
{
    if (t.duration_sec <= 0)
        return "";
    int m = t.duration_sec / 60;
    int s = t.duration_sec % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

static void draw_art(ImDrawList *dl, const std::string &url, ImVec2 pmin, ImVec2 pmax,
                     float rounding, bool circular, float alpha = 1.0f)
{
    if (alpha <= 0.001f)
        return;

    // Draw the placeholder first: while a thumbnail is still loading (or
    // fading in) the tile keeps its shape and icon instead of flashing empty.
    const ImU32 bg = ui_with_alpha(ImGui::GetColorU32(ui_c().surface_hover), alpha);
    const ImU32 bd = ui_with_alpha(ui_border_u32(), alpha);
    const ImVec2 centre((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f);
    if (circular)
    {
        const float radius = (pmax.x - pmin.x) * 0.5f;
        dl->AddCircleFilled(centre, radius, bg, 48);
        dl->AddCircle(centre, radius, bd, 48, 1.0f);
    }
    else
    {
        dl->AddRectFilled(pmin, pmax, bg, rounding);
        dl->AddRect(pmin, pmax, bd, rounding, 0, 1.0f);
    }
    ui_draw_icon(dl, UiIcon::Album, centre, (pmax.x - pmin.x) * 0.34f,
                 ui_with_alpha(ui_text_muted_u32(), alpha), 1.4f);

    if (url.empty())
        return;

    TextureInfo ti = texture_info(url);
    if (!ti.id)
        return;

    const float fade = ui_anim_texture_alpha(url);
    const ImU32 tint = IM_COL32(255, 255, 255, (int)(255.0f * alpha * fade));
    if (tint == 0)
        return;

    float u0, v0, u1, v1;
    cover_uv(ti, pmax.x - pmin.x, pmax.y - pmin.y, u0, v0, u1, v1);
    dl->AddImageRounded((ImTextureID)(intptr_t)ti.id, pmin, pmax,
                        ImVec2(u0, v0), ImVec2(u1, v1), tint,
                        circular ? (pmax.x - pmin.x) * 0.5f : rounding);
}

void ui_play_track(AppState &state, const Track &track, int index)
{
    std::string thumb_url = track.thumbnail_url;
    if (thumb_url.empty() && !track.id.empty())
        thumb_url = "https://img.youtube.com/vi/" + track.id + "/hqdefault.jpg";

    state.current_title = track.title;
    state.current_artist = track.artist;
    state.current_thumbnail_url = thumb_url;
    state.current_track_id = track.id;
    state.current_album_id = track.album_id;
    state.current_artist_id = track.artist_id;
    state.current_track_index = index;
    state.loading = true;
 
    if (state.current_tab == Tab::Home)
        state.current_playlist_source = strlen(state.search_buffer) > 0 ? PlaylistSource::SEARCH : PlaylistSource::HOME;
    else if (state.current_tab == Tab::Downloads)
        state.current_playlist_source = PlaylistSource::DOWNLOADS;
    else if (state.current_tab == Tab::Radio)
        state.current_playlist_source = PlaylistSource::RADIO;
    else if (state.current_tab == Tab::Liked)
        state.current_playlist_source = PlaylistSource::LIKED;
    else if (state.current_tab == Tab::History)
        state.current_playlist_source = PlaylistSource::HISTORY;
    else if (state.current_tab == Tab::Playlists)
        state.current_playlist_source = PlaylistSource::PLAYLIST;
    else if (state.current_tab == Tab::Album)
        state.current_playlist_source = PlaylistSource::ALBUM;
    else if (state.current_tab == Tab::Artist)
        state.current_playlist_source = PlaylistSource::ARTIST;
    else if (state.current_tab == Tab::Stats)
        state.current_playlist_source = PlaylistSource::STATS;
    else if (state.current_tab == Tab::YouTube)
        state.current_playlist_source = PlaylistSource::YOUTUBE_PLAYLIST;
    else if (state.current_tab == Tab::Smart)
        state.current_playlist_source = PlaylistSource::SMART;
    else if (state.current_tab == Tab::Podcasts)
        state.current_playlist_source = PlaylistSource::PODCAST;
    else
        state.current_playlist_source = PlaylistSource::HOME;

    state.is_search_active = (state.current_playlist_source == PlaylistSource::SEARCH);

    // The History tab displays a snapshot taken on tab entry, so its row
    // index can drift from the live history. Re-resolve the position so
    // auto-advance continues from the right row.
    if (state.current_playlist_source == PlaylistSource::HISTORY)
    {
        for (int i = 0; i < (int)state.playback_history.size(); ++i)
        {
            if (state.playback_history[i].id == track.id)
            {
                index = i;
                break;
            }
        }
    }

    state.current_track_index = index;
    state.queue_replace_from_source = true;
    state.queue_replace_index = index;
    state.queue_replace_id = track.id;

    state.pending_play_url = track.url;
    state.current_track_url = track.url;
    state.pending_play_title = track.title;
    state.pending_play_artist = track.artist;
    state.pending_play_thumbnail_url = thumb_url;
    state.pending_video_id = track.id;
    state.pending_video_url = track.url;
    state.pending_manual_play = true;
    state.loading_seq.fetch_add(1, std::memory_order_relaxed);
}

void ui_selection_bar(AppState &state, const std::vector<Track> &visible, const char *context,
                      bool allow_like, bool allow_delete, int playlist_index)
{
    if (state.selected_track_ids.empty())
        return;

    const UiMetrics &m = ui_m();
    std::vector<Track> picked;
    for (const auto &t : visible)
        if (state.selected_track_ids.count(t.id))
            picked.push_back(t);
    if (picked.empty())
    {
        state.selected_track_ids.clear();
        state.selection_anchor.clear();
        return;
    }

    ImGui::PushID(context);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ui_accent_u32(0.10f));
    const float h = m.control_h + m.space_2 * 2.0f;
    ImGui::BeginChild("##selection_bar", ImVec2(0.0f, h),
                      ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ui_c().text, "%d selected", (int)picked.size());

    auto clear_selection = [&]() {
        state.selected_track_ids.clear();
        state.selection_anchor.clear();
    };

    ImGui::SameLine();
    if (ui_ghost_button("Queue", ImVec2(0, m.control_h)))
    {
        for (const auto &t : picked)
            queue_add_end(state, t);
        clear_selection();
    }
    ImGui::SameLine();
    if (ui_ghost_button("Playlist", ImVec2(0, m.control_h)))
        ImGui::OpenPopup("##add_to_playlist");
    if (ImGui::BeginPopup("##add_to_playlist"))
    {
        if (state.playlists.empty())
            ImGui::TextDisabled("No playlists yet");
        for (int i = 0; i < (int)state.playlists.size(); ++i)
        {
            if (ImGui::MenuItem(state.playlists[i].name.c_str()))
            {
                for (const auto &t : picked)
                    state.playlists[i].tracks.push_back(t);
                save_playlist(state.playlists[i].name, state.playlists[i].tracks);
                wb_sync_playlist(state.playlists[i].name, state.playlists[i].tracks);
                clear_selection();
            }
        }
        ImGui::EndPopup();
    }

    if (allow_like)
    {
        const bool removing = context && std::strcmp(context, "liked") == 0;
        ImGui::SameLine();
        if (ui_ghost_button(removing ? "Unlike" : "Like", ImVec2(0, m.control_h)))
        {
            for (const auto &t : picked)
            {
                const bool liked = is_track_liked(state, t.id);
                if (removing ? liked : !liked)
                    toggle_like(state, t);
            }
            clear_selection();
        }
    }

    if (allow_delete)
    {
        ImGui::SameLine();
        if (ui_ghost_button("Delete", ImVec2(0, m.control_h)))
        {
            for (const auto &t : picked)
                delete_downloaded_track(state, t.id);
            clear_selection();
        }
    }

    if (playlist_index >= 0 && playlist_index < (int)state.playlists.size())
    {
        ImGui::SameLine();
        if (ui_ghost_button("Remove", ImVec2(0, m.control_h)))
        {
            auto &tracks = state.playlists[playlist_index].tracks;
            tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const Track &t) {
                return state.selected_track_ids.count(t.id) > 0;
            }), tracks.end());
            save_playlist(state.playlists[playlist_index].name, tracks);
            wb_sync_playlist(state.playlists[playlist_index].name, tracks);
            clear_selection();
        }
    }

    ImGui::SameLine();
    if (ui_ghost_button("Clear", ImVec2(0, m.control_h)))
        clear_selection();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, m.space_1));
}

void ui_track_context_menu(AppState &state, const Track &track, bool allow_delete,
                           bool show_download, bool *download_requested,
                           bool *download_remove)
{
    if (!ImGui::BeginPopup("TrackContextMenu"))
        return;

    const float s = ui_m().scale;
    const float art = 38.0f * s;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    draw_art(dl, track.thumbnail_url, p, ImVec2(p.x + art, p.y + art), ui_m().radius_sm, false);
    ImGui::Dummy(ImVec2(art, art));
    ImGui::SameLine();
    ui_push_medium(20.0f);
    ImGui::TextUnformatted(track.title.empty() ? "Unknown Title" : track.title.c_str());
    ImGui::PopFont();
    ui_push_small();
    ImGui::TextColored(ui_c().text_dim, "%s", track.artist.c_str());
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const bool liked = is_track_liked(state, track.id);
    if (ImGui::MenuItem(liked ? "Unlike" : "Like"))
        toggle_like(state, track);

    if (ImGui::MenuItem("Play next"))
        queue_add_next(state, track);
    if (ImGui::MenuItem("Add to queue"))
        queue_add_end(state, track);
    {
        auto qit = std::find_if(state.play_queue.begin(), state.play_queue.end(),
                                [&](const Track &t) { return t.id == track.id; });
        if (qit != state.play_queue.end() && ImGui::MenuItem("Remove from queue"))
            queue_remove_at(state, (int)(qit - state.play_queue.begin()));
    }

    if (ImGui::MenuItem("Start Radio"))
    {
        start_radio_from_track(state, track);
    }

    if (!track.album_id.empty() || !track.album.empty() || !track.id.empty())
    {
        if (ImGui::MenuItem("View album"))
            open_album_from_track(state, track);
    }

    if (!track.album_id.empty())
    {
        const bool saved = is_album_saved(state, track.album_id);
        if (ImGui::MenuItem(saved ? "Remove saved album" : "Save album"))
        {
            if (saved)
                remove_saved_album(state, track.album_id);
            else
            {
                SavedAlbum a;
                a.browse_id = track.album_id;
                a.title = track.album.empty() ? "Unknown Album" : track.album;
                a.artist = track.artist;
                a.thumbnail_url = track.thumbnail_url;
                save_album(state, a);
            }
        }
    }

    if (!track.artist_id.empty() || !track.id.empty())
    {
        if (ImGui::MenuItem("View artist"))
            open_artist_from_track(state, track);
    }

    if (!track.artist_id.empty())
    {
        const bool saved = is_artist_saved(state, track.artist_id);
        if (ImGui::MenuItem(saved ? "Remove saved artist" : "Save artist"))
        {
            if (saved)
                remove_saved_artist(state, track.artist_id);
            else
            {
                SavedArtist a;
                a.browse_id = track.artist_id;
                a.name = track.artist.empty() ? "Unknown Artist" : track.artist;
                a.thumbnail_url = track.thumbnail_url;
                save_artist(state, a);
            }
        }
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Add to playlist"))
    {
        for (int i = 0; i < (int)state.playlists.size(); ++i)
        {
            const auto &pl = state.playlists[i];
            if (ImGui::MenuItem(pl.name.c_str()))
            {
                state.playlists[i].tracks.push_back(track);
                save_playlist(pl.name, state.playlists[i].tracks);
                wb_sync_playlist(pl.name, state.playlists[i].tracks);
            }
        }
        if (!state.playlists.empty())
            ImGui::Separator();
        static char new_name[128] = "";
        ImGui::SetNextItemWidth(180.0f * s);
        ImGui::InputTextWithHint("##new_playlist", "New playlist", new_name, sizeof(new_name));
        if (ImGui::MenuItem("Create playlist"))
        {
            std::string name = new_name;
            if (!name.empty())
            {
                Playlist pl;
                pl.name = name;
                pl.tracks.push_back(track);
                state.playlists.push_back(pl);
                save_playlist(name, pl.tracks);
                wb_sync_playlist(name, pl.tracks);
                new_name[0] = '\0';
            }
        }
        ImGui::EndMenu();
    }

    if (state.current_tab == Tab::Playlists && state.selected_playlist_index >= 0 &&
        state.selected_playlist_index < (int)state.playlists.size())
    {
        ImGui::Separator();
        if (ImGui::MenuItem("Remove from playlist"))
        {
            auto &tracks = state.playlists[state.selected_playlist_index].tracks;
            tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                        [&](const Track &t) { return t.id == track.id; }),
                         tracks.end());
            save_playlist(state.playlists[state.selected_playlist_index].name, tracks);
            wb_sync_playlist(state.playlists[state.selected_playlist_index].name, tracks);
        }
    }

    if (show_download)
    {
        ImGui::Separator();
        const bool cached = !find_cached_audio_path(track.id).empty();
        if (ImGui::MenuItem(cached ? "Remove download" : "Download episode"))
        {
            if (download_requested)
                *download_requested = true;
            if (download_remove)
                *download_remove = cached;
        }
    }

    if (allow_delete)
    {
        ImGui::Separator();
        if (ImGui::MenuItem("Delete download"))
            delete_downloaded_track(state, track.id);
    }
    ImGui::EndPopup();
}

// Row pitch (height + spacing) for a ui_track_row() with the given options,
// so callers can drive an ImGuiListClipper without duplicating the height
// math (it must match ui_track_row's internals).
float ui_track_row_pitch(float art_size, bool compact)
{
    const UiMetrics &m = ui_m();
    const float art = art_size > 0.0f ? art_size : ui_elem("list_thumb", compact ? 40.0f : 48.0f);
    const float title_line = (compact ? 21.0f : 24.0f) * m.text_scale * 1.22f;
    const float meta_line = (compact ? 16.5f : 18.0f) * m.text_scale * 1.24f;
    const float row_scale = ui_elem("list_row", 1.0f);
    const float pad_y = (compact ? m.space_1 : m.row_pad_y) * row_scale;
    const float row_h = std::max(art, title_line + m.space_1 + meta_line) + pad_y * 2.0f;
    return row_h + ImGui::GetStyle().ItemSpacing.y;
}


// ----------------------------------------------------------------------
// Hero pill button + cover tile, shared by the Radio and list-page heroes.
// ----------------------------------------------------------------------
static std::unordered_map<ImGuiID, float> g_pill_hover;
static std::unordered_map<ImGuiID, double> g_pill_pulse;

float ui_pill_hover_value(ImGuiID id, bool hovered)
{
    ui_prune_hover_map(g_pill_hover);
    float &h = g_pill_hover[id];
    const float target = hovered ? 1.0f : 0.0f;
    h = ui_animations_enabled() ? ui_anim_approach(h, target, 16.0f) : target;
    if (std::fabs(h - target) > 0.004f)
        ui_anim_request();
    return h;
}

float ui_pill_pulse_value(ImGuiID id, double now, bool clicked)
{
    if (clicked)
    {
        g_pill_pulse[id] = now;
        ui_anim_request();
    }
    auto it = g_pill_pulse.find(id);
    if (it == g_pill_pulse.end())
        return 1.0f;
    const float p = (float)((now - it->second) / 0.45);
    if (p >= 0.0f && p < 1.0f)
        ui_anim_request();
    return p;
}

void ui_draw_cover(ImDrawList *dl, const std::string &url, ImVec2 pmin, ImVec2 pmax,
                       float rounding, float alpha, UiIcon fallback, const char *scope,
                       bool instant, bool skip_texture_fade)
{
    if (instant)
        alpha = 1.0f; // hero art: never dimmed by the entrance/reveal fades
    if (alpha <= 0.001f)
        return;

    const ImU32 bg = ui_with_alpha(ImGui::GetColorU32(ui_c().surface_hover), alpha);
    const ImU32 bd = ui_with_alpha(ui_border_u32(), alpha);
    dl->AddRectFilled(pmin, pmax, bg, rounding);
    dl->AddRect(pmin, pmax, bd, rounding, 0, 1.0f);

    const ImVec2 centre((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f);
    // The fallback icon is only for "nothing to show" (no URL at all). When a
    // real cover URL exists, the tile stays a plain surface until the texture
    // is ready -- a placeholder flashing over a cached cover is worse than a
    // quiet empty tile.
    if (url.empty())
        ui_draw_icon(dl, fallback, centre, (pmax.x - pmin.x) * 0.30f,
                     ui_with_alpha(ui_text_muted_u32(), alpha), 1.4f);

    // Never downgrade: if `url` has no ready texture, fall back to the last
    // texture that rendered for this scope (a hero's cover URL can briefly
    // point at an unloaded image while a background list re-scan or station
    // refresh runs). And when the URL does switch to a loaded texture, the
    // old image cross-fades out underneath -- the first-sighting reveal fade
    // of the new URL must never expose the placeholder.
    static std::unordered_map<std::string, std::string> s_scope_cover;
    const std::string scope_key = scope ? scope : "";
    std::string use = url;
    TextureInfo ti = url.empty() ? TextureInfo{} : texture_info(url);
    std::string prev_url;
    TextureInfo prev_ti{};
    auto it = s_scope_cover.find(scope_key);
    if (it != s_scope_cover.end() && it->second != url)
    {
        prev_url = it->second;
        prev_ti = texture_info(prev_url);
        if (!ti.id && prev_ti.id)
        {
            use = prev_url; // new one not ready: keep the old art on screen
            ti = prev_ti;
            prev_ti = TextureInfo{};
        }
    }
    if (ti.id)
        s_scope_cover[scope_key] = use;
    if (!ti.id)
        return;

    // Hero tiles (instant) skip the first-sighting reveal fade entirely: a
    // cached cover must be fully opaque on its very first frame.
    const float fade = (instant || skip_texture_fade)
                           ? 1.0f
                           : ui_anim_texture_alpha(use);

    // Outgoing cover fades out under the incoming one.
    if (prev_ti.id && fade < 1.0f)
    {
        float pu0, pv0, pu1, pv1;
        if (cover_uv(prev_ti, pmax.x - pmin.x, pmax.y - pmin.y, pu0, pv0, pu1, pv1))
        {
            const ImU32 ptint =
                IM_COL32(255, 255, 255, (int)(255.0f * alpha * (1.0f - fade)));
            if (ptint != 0)
                dl->AddImageRounded((ImTextureID)(intptr_t)prev_ti.id, pmin, pmax,
                                    ImVec2(pu0, pv0), ImVec2(pu1, pv1), ptint, rounding);
        }
    }

    float u0, v0, u1, v1;
    if (!cover_uv(ti, pmax.x - pmin.x, pmax.y - pmin.y, u0, v0, u1, v1))
        return;
    const ImU32 tint = IM_COL32(255, 255, 255, (int)(255.0f * alpha * fade));
    if (tint == 0)
        return;
    dl->AddImageRounded((ImTextureID)(intptr_t)ti.id, pmin, pmax, ImVec2(u0, v0), ImVec2(u1, v1),
                        tint, rounding);
}

// A small outlined pill used for the hero metadata ("ENDLESS", track count,
// duration). Returns its width so the caller can pack chips left to right.
float ui_meta_chip(ImDrawList *dl, const UiMetrics &m, const char *text, ImVec2 pos,
                             bool accent, float alpha)
{
    ImFont *font = g_ui_fonts.body;
    const float fs = 13.0f * m.text_scale;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
    const float pad_x = 10.0f * m.layout_scale;
    const float w = ts.x + pad_x * 2.0f;
    const float h = 24.0f * m.layout_scale;
    const float rounding = h * 0.5f;

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
                      accent ? ui_accent_u32(0.14f * alpha)
                             : ui_overlay_u32(0.045f * alpha),
                      rounding);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h),
                accent ? ui_accent_u32(0.32f * alpha) : ui_with_alpha(ui_border_u32(), 0.65f * alpha),
                rounding, 0, 1.0f);
    dl->AddText(font, fs,
                ImVec2(pos.x + pad_x, pos.y + (h - ts.y) * 0.5f),
                ui_with_alpha(accent ? ui_accent_text_u32() : ui_text_dim_u32(), alpha),
                text);
    return w;
}

bool ui_pill_button(const char *id, const char *label, UiIcon icon, ImVec2 pos, ImVec2 size,
                       bool primary, float alpha, bool enabled)
{
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    const ImGuiID uid = ImGui::GetID("##radio_pill");
    if (!enabled)
        ImGui::BeginDisabled();
    ImGui::InvisibleButton("##radio_pill", size);
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool clicked = enabled && ImGui::IsItemClicked();
    if (!enabled)
        ImGui::EndDisabled();
    ImGui::PopID();

    const float press_a = enabled ? alpha : alpha * 0.42f;
    const float hv = ui_pill_hover_value(uid, hovered);
    const float pulse = ui_pill_pulse_value(uid, ImGui::GetTime(), clicked);

    const float press = (pulse >= 0.0f && pulse < 0.4f)
                            ? std::sin(std::clamp(pulse / 0.4f, 0.0f, 1.0f) * 3.14159265f) * 0.02f
                            : 0.0f;
    const float scale = enabled ? (1.0f + 0.015f * hv - press) : 1.0f;

    const ImVec2 centre(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    auto P = [&](ImVec2 p) {
        return ImVec2(centre.x + (p.x - centre.x) * scale, centre.y + (p.y - centre.y) * scale);
    };

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float rounding = size.y * 0.5f;

    if (pulse >= 0.0f && pulse < 1.0f)
    {
        const float pr = ui_anim_ease_out(pulse);
        const float grow = size.y * 0.22f * pr;
        dl->AddRect(P(ImVec2(pos.x - grow, pos.y - grow)),
                    P(ImVec2(pos.x + size.x + grow, pos.y + size.y + grow)),
                    ui_accent_u32(0.45f * (1.0f - pr) * press_a), rounding + grow, 0, 1.5f);
    }

    if (primary)
    {
        if (hv > 0.01f)
            dl->AddRectFilled(P(ImVec2(pos.x - 3.0f, pos.y - 3.0f)),
                              P(ImVec2(pos.x + size.x + 3.0f, pos.y + size.y + 3.0f)),
                              ui_accent_u32(0.16f * hv * press_a), rounding + 3.0f);
        dl->AddRectFilled(P(pos), P(ImVec2(pos.x + size.x, pos.y + size.y)),
                          ui_accent_u32((0.88f + 0.12f * hv) * press_a), rounding);
    }
    else
    {
        dl->AddRectFilled(P(pos), P(ImVec2(pos.x + size.x, pos.y + size.y)),
                          ImGui::GetColorU32(ImVec4(1, 1, 1, (0.035f + 0.045f * hv) * press_a)),
                          rounding);
        dl->AddRect(P(pos), P(ImVec2(pos.x + size.x, pos.y + size.y)),
                    ui_with_alpha(ui_border_u32(), (0.7f + 0.3f * hv) * press_a),
                    rounding, 0, 1.0f);
    }

    ImFont *font = g_ui_fonts.medium;
    const float fs = 16.0f * ui_m().text_scale;
    const ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
    const float icon_size = 15.0f * ui_m().layout_scale;
    const float gap = 8.0f * ui_m().layout_scale;
    const float group_w = icon_size + gap + ts.x;
    const float gx = pos.x + (size.x - group_w) * 0.5f;
    const float gy = pos.y + (size.y - ts.y) * 0.5f;

    const ImU32 text_col = primary ? ui_with_alpha(ui_col(ui_c().on_accent), press_a)
                                   : ui_with_alpha(hv > 0.5f ? ui_text_u32() : ui_text_dim_u32(), press_a);
    const ImU32 icon_col = primary ? text_col
                                   : ui_with_alpha(hv > 0.5f ? ui_accent_text_u32() : ui_text_muted_u32(), press_a);
    ui_draw_icon(dl, icon, ImVec2(gx + icon_size * 0.5f, centre.y), icon_size, icon_col, 1.7f);
    dl->AddText(font, fs, ImVec2(gx + icon_size + gap, gy), text_col, label);

    if (hovered)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

bool ui_touch_tap_recent()
{
    return ImGui::GetTime() - g_touch_click_time < 1.0;
}

TrackRowResult ui_track_row(AppState &state, const Track &track, int index, const TrackRowOptions &opt)
{
    TrackRowResult result;
    const UiMetrics &m = ui_m();
    ImGui::PushID(index);
    const float row_x = ImGui::GetCursorScreenPos().x;
    const float row_y = ImGui::GetCursorScreenPos().y;
    const float full_w = ImGui::GetContentRegionAvail().x;
    if (full_w <= 1.0f)
    {
        ImGui::PopID();
        return result;
    }

    const float base_art = opt.art_size > 0.0f ? opt.art_size : ui_elem("list_thumb", opt.compact ? 40.0f : 48.0f);
    const float title_base = opt.compact ? 21.0f : 24.0f;
    const float meta_base = opt.compact ? 16.5f : 18.0f;
    const float title_line = title_base * m.text_scale * 1.22f;
    const float meta_line = meta_base * m.text_scale * 1.24f;
    const float row_scale = ui_elem("list_row", 1.0f);
    const float pad_y = (opt.compact ? m.space_1 : m.row_pad_y) * row_scale;

    const float rank_w = opt.show_rank ? 34.0f * m.layout_scale : 0.0f;
    const float more_w = 32.0f * m.layout_scale;
    const float del_w_full = opt.show_delete ? 82.0f * m.layout_scale : 0.0f;
    const float dur_w_full = opt.show_duration ? 58.0f * m.layout_scale : 0.0f;
    const float right_text_w_full = opt.right_text.empty()
                                        ? 0.0f
                                        : ui_measure_width(g_ui_fonts.body, meta_base, opt.right_text) + m.space_2;

    // At narrow list widths the fixed paddings, artwork, duration and action
    // buttons leave almost no room for the title. Switch to a tighter row:
    // smaller thumb, less horizontal padding, and fold away the secondary
    // columns. The context menu still exposes duration/delete actions.
    const float normal_actions_w = more_w + del_w_full + m.space_2;
    const float normal_hit_w = std::max(1.0f, full_w - normal_actions_w);
    const float normal_text_x = row_x + m.row_pad_x + rank_w + base_art + m.space_3;
    const float normal_text_w = std::max(1.0f, row_x + normal_hit_w - normal_text_x -
                                                   std::max(dur_w_full, right_text_w_full) - m.space_2);
    const bool tight = normal_text_w < 120.0f * m.layout_scale;

    const float row_pad_x = tight ? 8.0f * m.layout_scale : m.row_pad_x;
    const float text_gap = tight ? m.space_2 : m.space_3;
    const float art = tight ? std::min(base_art, 40.0f * m.layout_scale) : base_art;
    const bool show_duration = opt.show_duration && !tight;
    const bool show_delete_inline = opt.show_delete && !tight;
    const bool show_right_text = !tight && !opt.right_text.empty();
    const float del_w = show_delete_inline ? del_w_full : 0.0f;
    const float dur_w = show_duration ? dur_w_full : 0.0f;
    const float right_text_w = show_right_text ? right_text_w_full : 0.0f;
    const float right_w = std::max(dur_w, right_text_w);
    const float actions_w = more_w + del_w + m.space_2;
    const float hit_w = std::max(1.0f, full_w - actions_w);
    const float text_x = row_x + row_pad_x + rank_w + art + text_gap;
    const float text_w = std::max(1.0f, row_x + hit_w - text_x - right_w - m.space_2);
    const float row_h = std::max(art, title_line + m.space_1 + meta_line) + pad_y * 2.0f;

    double reveal_start = opt.reveal_start;
    bool stagger_reveal = true;
    int stagger_index = opt.reveal_index;
    if (ui_animations_enabled() &&
        ImGui::IsRectVisible(ImVec2(row_x, row_y), ImVec2(row_x + full_w, row_y + row_h)))
    {
        int scroll_order = 0;
        const double scroll_start =
            ui_anim_scroll_reveal((int)state.current_tab, index, track.id, &scroll_order);
        if (scroll_start > reveal_start + 0.0005)
        {
            reveal_start = scroll_start;
            stagger_reveal = opt.stagger_scroll_reveal;
            stagger_index = scroll_order;
        }
    }

    float appear = 1.0f;
    if (ui_animations_enabled() && reveal_start > 0.0)
    {
        const float dur = std::max(0.001f, ui_anim_duration(0.22f));
        const float delay = stagger_reveal
                                ? ui_anim_delay(0.018f * (float)std::min(stagger_index, 16))
                                : 0.0f;
        float t = (float)((ImGui::GetTime() - reveal_start - delay) / dur);
        t = clampf(t, 0.0f, 1.0f);
        appear = ui_anim_ease_out(t);
        if (t < 1.0f)
            ui_anim_request();
    }
    const float slide = (1.0f - appear) * ui_anim_offset(ui_px(7.0f));
    // Fade the row itself at a scroll edge; this leaves the backdrop untouched
    // instead of painting a sampled background band over it.
    appear *= ui_content_edge_alpha(row_y, row_y + row_h);
    const float draw_y = row_y + slide;

    ImGui::SetCursorScreenPos(ImVec2(row_x, row_y));
    ImGui::InvisibleButton("##track_row", ImVec2(hit_w, row_h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    const bool right_clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    result.hovered = hovered;
    result.double_clicked = double_clicked;
    result.clicked = hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left);
    result.ctrl_clicked = result.clicked && ImGui::GetIO().KeyCtrl;
    result.shift_clicked = result.clicked && ImGui::GetIO().KeyShift;
    if (opt.draggable && opt.drag_type)
    {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        {
            ImGui::SetDragDropPayload(opt.drag_type, &index, sizeof(int));
            ImGui::TextUnformatted(track.title.empty() ? "Unknown Title" : track.title.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(opt.drag_type))
            {
                result.drop_received = true;
                result.drag_from = *(const int *)payload->Data;
                result.drop_to = index;
            }
            ImGui::EndDragDropTarget();
        }
    }

    ui_prune_hover_map(g_row_hover);
    float &hover_t = g_row_hover[ImGui::GetItemID()];
    if (ui_animations_enabled())
    {
        const float target = hovered ? 1.0f : 0.0f;
        hover_t = ui_anim_approach(hover_t, target, 14.0f);
        if (std::fabs(hover_t - target) > 0.002f)
            ui_anim_request();
    }
    else
    {
        hover_t = hovered ? 1.0f : 0.0f;
    }
    const float vis_hover = hover_t * appear;

    if (right_clicked)
        ImGui::OpenPopup("TrackContextMenu");

    const bool current = (!state.current_track_id.empty() && track.id == state.current_track_id);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 row_min(row_x, draw_y);
    const ImVec2 row_max(row_x + full_w, draw_y + row_h);
    if (current)
        dl->AddRectFilled(row_min, row_max,
                          ui_accent_u32((0.125f + 0.035f * vis_hover) * appear), m.radius_md);
    else if (opt.selected)
        dl->AddRectFilled(row_min, row_max, ui_accent_u32(0.085f * appear), m.radius_md);
    else if (vis_hover > 0.001f)
        dl->AddRectFilled(row_min, row_max,
                          ui_overlay_u32(0.045f * vis_hover), m.radius_md);
    if (current || opt.selected)
        dl->AddRectFilled(ImVec2(row_x, draw_y + m.space_2),
                          ImVec2(row_x + 2.0f * m.layout_scale, draw_y + row_h - m.space_2),
                          ui_accent_u32(appear), 1.0f);
    if (opt.selected && !current)
        dl->AddRect(row_min, row_max, ui_accent_u32(0.22f * appear), m.radius_md, 0, 1.0f);

    if (opt.show_rank)
    {
        char rank[16];
        std::snprintf(rank, sizeof(rank), "%d", opt.rank);
        ImFont *rf = g_ui_fonts.medium;
        float fs = 18.0f * m.text_scale;
        ImVec2 rs = rf->CalcTextSizeA(fs, FLT_MAX, 0, rank);
        dl->AddText(rf, fs, ImVec2(row_x + row_pad_x + (rank_w - rs.x) * 0.5f, draw_y + (row_h - rs.y) * 0.5f),
                    ui_with_alpha(current ? ui_accent_text_u32() : ui_text_muted_u32(), appear), rank);
    }

    const float art_x = row_x + row_pad_x + rank_w;
    const float art_y = draw_y + (row_h - art) * 0.5f;
    ImVec2 amin(art_x, art_y);
    ImVec2 amax(art_x + art, art_y + art);
    draw_art(dl, track.thumbnail_url, amin, amax, m.radius_sm, false, appear);
    if (vis_hover > 0.001f && opt.show_art_play)
    {
        dl->AddRectFilled(amin, amax, IM_COL32(0, 0, 0, (int)(115.0f * vis_hover)), m.radius_sm);
        ui_draw_icon(dl, UiIcon::Play, ImVec2((amin.x + amax.x) * 0.5f, (amin.y + amax.y) * 0.5f),
                     art * 0.34f, IM_COL32(255, 255, 255, (int)(255.0f * vis_hover)), 1.4f);
    }

    std::string title = track.title.empty() ? "Unknown Title" : track.title;
    std::string secondary = opt.secondary;
    if (secondary.empty())
        secondary = track.artist.empty() ? "Unknown Artist" : track.artist;
    if (opt.show_plays)
    {
        int plays = play_count_for(state, track.id);
        if (plays > 0)
            secondary += "  \xC2\xB7  " + std::to_string(plays) + (plays == 1 ? " play" : " plays");
    }

    const ImVec2 clip_min(text_x - 2.0f, draw_y);
    const ImVec2 clip_max(text_x + text_w + 2.0f, draw_y + row_h);
    dl->PushClipRect(clip_min, clip_max, true);
    const float title_y = draw_y + (row_h - (title_line + m.space_1 + meta_line)) * 0.5f;
    const float title_fs = title_base * m.text_scale;
    const float meta_fs = meta_base * m.text_scale;
    std::string title_draw = ui_ellipsize(g_ui_fonts.medium, title_base, title, text_w);
    std::string meta_draw = ui_ellipsize(g_ui_fonts.body, meta_base, secondary, text_w);
    dl->AddText(g_ui_fonts.medium, title_fs, ImVec2(text_x, title_y),
                ui_with_alpha(current ? ui_accent_text_u32() : ui_text_u32(), appear), title_draw.c_str());
    dl->AddText(g_ui_fonts.body, meta_fs, ImVec2(text_x, title_y + title_line + m.space_1),
                ui_with_alpha(ui_text_dim_u32(), appear), meta_draw.c_str());
    dl->PopClipRect();

    if (show_duration)
    {
        std::string dur = fmt_duration(track);
        if (!dur.empty())
        {
            float fs = meta_base * m.text_scale;
            ImVec2 ds = g_ui_fonts.body->CalcTextSizeA(fs, FLT_MAX, 0, dur.c_str());
            float dx = row_x + hit_w - ds.x - m.space_2;
            dl->AddText(g_ui_fonts.body, fs, ImVec2(dx, draw_y + (row_h - ds.y) * 0.5f),
                        ui_with_alpha(ui_text_dim_u32(), appear), dur.c_str());
        }
    }
    else if (show_right_text)
    {
        float fs = meta_base * m.text_scale;
        std::string right = ui_ellipsize(g_ui_fonts.body, meta_base, opt.right_text, right_w);
        ImVec2 rs = g_ui_fonts.body->CalcTextSizeA(fs, FLT_MAX, 0, right.c_str());
        float dx = row_x + hit_w - rs.x - m.space_2;
        dl->AddText(g_ui_fonts.body, fs, ImVec2(dx, draw_y + (row_h - rs.y) * 0.5f),
                    ui_with_alpha(current ? ui_accent_text_u32() : ui_text_muted_u32(), appear), right.c_str());
    }
    if (opt.progress >= 0.0f)
    {
        const float ratio = clampf(opt.progress, 0.0f, 1.0f);
        const float bar_y = draw_y + row_h - 2.0f * m.layout_scale;
        dl->AddRectFilled(ImVec2(text_x, bar_y), ImVec2(text_x + text_w, bar_y + 2.0f * m.layout_scale),
                          ui_overlay_u32(0.08f * appear), 1.0f);
        if (ratio > 0.0f)
            dl->AddRectFilled(ImVec2(text_x, bar_y), ImVec2(text_x + text_w * ratio, bar_y + 2.0f * m.layout_scale),
                              ui_accent_u32(0.85f * appear), 1.0f);
    }

    const float action_x = row_x + hit_w + m.space_1;
    ImGui::SetCursorScreenPos(ImVec2(action_x, draw_y + (row_h - more_w) * 0.5f));
    if (ui_icon_button("##track_more", UiIcon::More, more_w, false,
                       ui_with_alpha(ui_text_muted_u32(), appear), "More"))
        ImGui::OpenPopup("TrackContextMenu");

    if (show_delete_inline)
    {
        ImGui::SetCursorScreenPos(ImVec2(row_x + full_w - del_w - m.space_1, draw_y + (row_h - m.control_h) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, appear);
        const bool deleted = ImGui::Button("Delete", ImVec2(del_w, m.control_h));
        ImGui::PopStyleVar();
        if (deleted)
        {
            delete_downloaded_track(state, track.id);
            result.deleted = true;
        }
    }

    ui_track_context_menu(state, track, opt.show_delete, opt.show_download,
                          &result.download_requested, &result.download_remove);

    if ((double_clicked || (result.clicked && ui_touch_tap_recent())) &&
        !result.deleted && opt.auto_play_double_click)
        ui_play_track(state, track, index);

    ImGui::SetCursorScreenPos(ImVec2(row_x, row_y + row_h + m.row_gap));
    ImGui::PopID();
    return result;
}

void ui_glow_separator(float width)
{
    const float band = std::max(ui_px(13.0f), 8.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (width <= 0.0f)
        width = ImGui::GetContentRegionAvail().x;

    const float level = ui_glow_level();
    if (width > 3.0f && level > 0.002f)
    {
        const bool light = ui_theme_is_light();
        ImVec4 tint = ui_backdrop_tint();
        // A touch of white makes the core read as emitted light rather than
        // paint. Skipped in day mode, where the tint already sits near white.
        if (!light)
        {
            tint.x += (1.0f - tint.x) * 0.16f;
            tint.y += (1.0f - tint.y) * 0.16f;
            tint.z += (1.0f - tint.z) * 0.16f;
        }

        // Day mode runs a little softer: a saturated bloom over a light
        // surface reads heavier than the same alpha over black.
        const float theme_scale = light ? 0.80f : 0.74f;
        // Cumulative alpha at the core of the bloom, and on the crisp line.
        const float glow_peak = clampf(0.55f * level, 0.0f, 0.90f) * theme_scale;
        const float core_a = clampf(0.90f * level, 0.0f, 0.95f) * theme_scale;

        const float yc = p.y + band * 0.5f;
        const float th = std::max(1.0f, ui_px(1.0f));
        // The bloom is taller than the reserved layout band: the overflow is
        // low enough alpha to read as light spilling, not as an overlay.
        const float half_h = std::max(band * 0.5f + ui_px(7.0f), th);

        // Hold most of the brightness for a short run, then taper to nothing.
        const float hold = std::min(width * 0.45f, ui_px(280.0f));
        const float x0 = p.x;
        const float xm = p.x + hold;
        const float x1 = p.x + width;

        ImDrawList *dl = ImGui::GetWindowDrawList();
        auto col = [&](float a) {
            return ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, a));
        };

        // Nested same-colour layers. Straight-alpha compositing accumulates
        // toward the centre, which gives a soft vertical falloff without a
        // shader or a blur pass. The per-layer alpha is the difference of a
        // Gaussian between neighbouring radii, so the stacked result is a
        // proper bloom (bright core, long faint tail) rather than a flat bar.
        constexpr int kLayers = 16;
        const float sigma = std::max(1.0f, half_h * 0.42f);
        auto glow_at = [&](float d) {
            const float t = d / sigma;
            return glow_peak * std::exp(-t * t);
        };
        const ImU32 off = col(0.0f);
        float outer = 0.0f;
        for (int j = kLayers; j >= 1; --j)
        {
            const float hh = half_h * ((float)j / (float)kLayers);
            const float g = glow_at(hh);
            const float a_j = g - outer;
            outer = g;
            if (a_j <= 0.0005f)
                continue;
            const ImU32 a = col(a_j);
            // Full width while holding, then a linear fade on the right.
            dl->AddRectFilledMultiColor(ImVec2(x0, yc - hh), ImVec2(xm, yc + hh),
                                        a, a, a, a);
            // ul = a, ur = 0, br = 0, bl = a.
            dl->AddRectFilledMultiColor(ImVec2(xm, yc - hh), ImVec2(x1, yc + hh),
                                        a, off, off, a);
        }

        // Crisp core line with the same hold-and-taper profile.
        const ImU32 core = col(core_a);
        dl->AddRectFilledMultiColor(ImVec2(x0, yc - th * 0.5f), ImVec2(xm, yc + th * 0.5f),
                                    core, core, core, core);
        dl->AddRectFilledMultiColor(ImVec2(xm, yc - th * 0.5f), ImVec2(x1, yc + th * 0.5f),
                                    core, off, off, core);
    }

    ImGui::Dummy(ImVec2(0.0f, band));
}

bool ui_section_header(const char *title, const char *meta, const char *action)
{
    const UiMetrics &m = ui_m();
    bool clicked = false;
    ui_push_semibold(22.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (meta && *meta)
    {
        ImGui::SameLine(0.0f, m.space_2);
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "%s", meta);
        ImGui::PopFont();
    }
    if (action && *action)
    {
        ImGui::SameLine();
        float bw = std::max(ImGui::CalcTextSize(action).x + m.space_4 * 2.0f, button_text_room(action));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - bw);
        clicked = ui_ghost_button(action, ImVec2(bw, m.control_h));
    }
    ui_glow_separator();
    return clicked;
}

static void push_button_style(bool primary, bool ghost)
{
    const UiColors &c = ui_c();
    if (primary)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, c.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c.accent_hover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, c.accent_active);
        ImGui::PushStyleColor(ImGuiCol_Text, c.on_accent);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    }
    else if (ghost)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_overlay_vec4(0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent_alpha(g_accent, 0.18f));
        ImGui::PushStyleColor(ImGuiCol_Text, c.text_dim);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ui_c().bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui_overlay_vec4(0.06f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent_alpha(g_accent, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_Text, c.text);
        ImGui::PushStyleColor(ImGuiCol_Border, c.border);
    }
}

static float button_text_room(const char *label)
{
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f;
}

bool ui_primary_button(const char *label, const ImVec2 &size)
{
    const UiMetrics &m = ui_m();
    ImVec2 sz = size;
    if (sz.x <= 0.0f)
        sz.x = ImGui::CalcTextSize(label).x + m.space_5 * 2.0f;
    else
        sz.x = std::max(sz.x, button_text_room(label));
    if (sz.y <= 0.0f)
        sz.y = m.button_h;
    push_button_style(true, false);
    bool clicked = ImGui::Button(label, sz);
    ImGui::PopStyleColor(5);
    return clicked;
}

bool ui_secondary_button(const char *label, const ImVec2 &size)
{
    const UiMetrics &m = ui_m();
    ImVec2 sz = size;
    if (sz.x <= 0.0f)
        sz.x = ImGui::CalcTextSize(label).x + m.space_4 * 2.0f;
    else
        sz.x = std::max(sz.x, button_text_room(label));
    if (sz.y <= 0.0f)
        sz.y = m.button_h;
    push_button_style(false, false);
    bool clicked = ImGui::Button(label, sz);
    ImGui::PopStyleColor(5);
    return clicked;
}

bool ui_ghost_button(const char *label, const ImVec2 &size)
{
    const UiMetrics &m = ui_m();
    ImVec2 sz = size;
    if (sz.x <= 0.0f)
        sz.x = ImGui::CalcTextSize(label).x + m.space_4 * 2.0f;
    else
        sz.x = std::max(sz.x, button_text_room(label));
    if (sz.y <= 0.0f)
        sz.y = m.button_h;
    push_button_style(false, true);
    bool clicked = ImGui::Button(label, sz);
    ImGui::PopStyleColor(5);
    return clicked;
}

void ui_empty_state(UiIcon icon, const char *title, const char *subtitle)
{
    const UiMetrics &m = ui_m();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 start = ImGui::GetCursorScreenPos();
    float cy = start.y + std::max(80.0f * m.layout_scale, avail.y * 0.30f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ui_draw_icon(dl, icon, ImVec2(start.x + avail.x * 0.5f, cy), 42.0f * m.layout_scale, ui_text_muted_u32(), 1.4f);
    ui_push_medium(24.0f);
    ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::SetCursorScreenPos(ImVec2(start.x + (avail.x - ts.x) * 0.5f, cy + 34.0f * m.layout_scale));
    ImGui::TextColored(ui_c().text_dim, "%s", title);
    ImGui::PopFont();
    if (subtitle && *subtitle)
    {
        ui_push_small();
        ImVec2 ss = ImGui::CalcTextSize(subtitle);
        ImGui::SetCursorScreenPos(ImVec2(start.x + (avail.x - ss.x) * 0.5f, cy + 64.0f * m.layout_scale));
        ImGui::TextColored(ui_c().text_muted, "%s", subtitle);
        ImGui::PopFont();
    }
}

// Inline error state for a tab/list that failed to load: error icon, a
// human-readable reason and a centered Retry button. Returns true in the
// frame the user presses Retry so the caller can re-trigger its fetch.
bool ui_error_state(const char *message, const char *retry_label)
{
    ui_empty_state(UiIcon::Close, "Something went wrong", message);
    const UiMetrics &m = ui_m();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const char *label = retry_label ? retry_label : "Retry";
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 btn(std::max(ts.x + m.space_6, m.control_h * 2.0f), m.control_h);
    ImGui::SetCursorScreenPos(ImVec2(
        ImGui::GetCursorScreenPos().x + (avail.x - btn.x) * 0.5f,
        ImGui::GetCursorScreenPos().y + m.space_5));
    return ui_ghost_button(label, btn);
}

// Animated variant of ui_empty_state for in-progress searches: breathing
// accent icon inside a rotating arc ring, with cycling dots on the subtitle.
// Requests a repaint every frame while visible, since the app renders dirty
// frames only.
void ui_searching_state(UiIcon icon, const char *title, const char *subtitle)
{
    if (!ui_animations_enabled())
    {
        ui_empty_state(icon, title, subtitle);
        return;
    }

    const UiMetrics &m = ui_m();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 start = ImGui::GetCursorScreenPos();
    float cy = start.y + std::max(80.0f * m.layout_scale, avail.y * 0.30f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 centre(start.x + avail.x * 0.5f, cy);

    const double now = ImGui::GetTime();
    const float breathe = 0.5f + 0.5f * (float)std::sin(now * 1.9);
    const float icon_size = 42.0f * m.layout_scale;

    // Soft accent halo behind everything, pulsing with the icon.
    dl->AddCircleFilled(centre, icon_size * 1.05f, ui_accent_u32(0.05f + 0.05f * breathe), 64);

    // Faint full ring, with a bright arc orbiting it. The arc's span also
    // breathes so the orbit feels alive instead of metronome-flat.
    const float ring_r = icon_size * 0.8f;
    const float tau = 6.2831853f;
    const float a0 = (float)std::fmod(now * 2.1, 1.0) * tau;
    const float span = 0.22f + 0.38f * (0.5f - 0.5f * (float)std::cos(now * 1.3));
    dl->AddCircle(centre, ring_r, ui_with_alpha(ui_border_u32(), 0.55f), 64, 1.0f);
    dl->PathArcTo(centre, ring_r, a0, a0 + span * tau, 40);
    dl->PathStroke(ui_accent_u32(0.35f + 0.25f * breathe), 0, 2.2f * m.layout_scale);

    // Icon: gentle scale breathing and a tint that drifts toward the accent.
    const float scale = 1.0f + 0.06f * breathe;
    const ImU32 icon_col = ui_with_alpha(ui_accent_text_u32(), 0.65f + 0.35f * breathe);
    ui_draw_icon(dl, icon, centre, icon_size * scale, icon_col, 1.4f);

    ui_push_medium(24.0f);
    ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::SetCursorScreenPos(ImVec2(start.x + (avail.x - ts.x) * 0.5f, cy + 52.0f * m.layout_scale));
    ImGui::TextColored(ui_c().text_dim, "%s", title);
    ImGui::PopFont();
    if (subtitle && *subtitle)
    {
        static const char *dot_phrases[] = {"", ".", "..", "..."};
        const int dots = (int)std::fmod(now * 2.2, 4.0);
        std::string s = std::string(subtitle) + dot_phrases[dots];
        ui_push_small();
        ImVec2 ss = ImGui::CalcTextSize(s.c_str());
        ImGui::SetCursorScreenPos(ImVec2(start.x + (avail.x - ss.x) * 0.5f, cy + 84.0f * m.layout_scale));
        ImGui::TextColored(ui_c().text_muted, "%s", s.c_str());
        ImGui::PopFont();
    }

    ui_anim_request();
}

MediaCardResult ui_media_card(const char *id, const std::string &title, const std::string &meta,
                              const std::string &thumb_url, float width, float art_size,
                              bool circular, bool current, float alpha)
{
    MediaCardResult r;
    const UiMetrics &m = ui_m();
    const float title_base = 19.0f;
    const float meta_base = 16.0f;
    const float title_h = title_base * m.text_scale * 1.25f;
    const float meta_h = meta_base * m.text_scale * 1.25f;
    const float caption_h = title_h + (meta.empty() ? 0.0f : m.space_1 + meta_h);
    const float card_h = art_size + m.space_3 + caption_h + m.space_2;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float edge_alpha = ui_content_edge_alpha(pos.y, pos.y + card_h) *
                             std::max(0.0f, std::min(1.0f, alpha));

    ImGui::InvisibleButton(id, ImVec2(width, card_h),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    r.hovered = ImGui::IsItemHovered();
    r.clicked = r.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    r.double_clicked = r.hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    r.right_clicked = r.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float art_x = circular ? pos.x + (width - art_size) * 0.5f : pos.x;
    ImVec2 amin(art_x, pos.y);
    ImVec2 amax(art_x + art_size, pos.y + art_size);
    if (r.hovered || current)
        dl->AddRectFilled(ImVec2(pos.x - m.space_1, pos.y - m.space_1),
                          ImVec2(pos.x + width + m.space_1, pos.y + card_h + m.space_1),
                          current ? ui_accent_u32(0.13f * edge_alpha)
                                  : ui_overlay_u32(0.045f * edge_alpha), m.radius_md);
    draw_art(dl, thumb_url, amin, amax, m.radius_md, circular, edge_alpha);
    if (r.hovered)
    {
        const ImU32 scrim = IM_COL32(0, 0, 0, (int)(110.0f * edge_alpha));
        if (circular)
            dl->AddCircleFilled(ImVec2((amin.x + amax.x) * 0.5f, (amin.y + amax.y) * 0.5f), art_size * 0.5f, scrim, 48);
        else
            dl->AddRectFilled(amin, amax, scrim, m.radius_md);
        ui_draw_icon(dl, UiIcon::Play, ImVec2((amin.x + amax.x) * 0.5f, (amin.y + amax.y) * 0.5f),
                     art_size * 0.24f, ui_with_alpha(IM_COL32_WHITE, edge_alpha), 1.5f);
    }

    const float tx = pos.x;
    const float tw = width;
    const float title_y = pos.y + art_size + m.space_3;
    std::string td = ui_ellipsize(g_ui_fonts.medium, title_base, title, tw);
    std::string md = ui_ellipsize(g_ui_fonts.body, meta_base, meta, tw);
    dl->PushClipRect(ImVec2(tx, title_y), ImVec2(tx + tw, title_y + caption_h), true);
    dl->AddText(g_ui_fonts.medium, title_base * m.text_scale, ImVec2(tx, title_y),
                ui_with_alpha(current ? ui_accent_text_u32() : ui_text_u32(), edge_alpha),
                td.c_str());
    if (!meta.empty())
        dl->AddText(g_ui_fonts.body, meta_base * m.text_scale, ImVec2(tx, title_y + title_h + m.space_1),
                    ui_with_alpha(ui_text_dim_u32(), edge_alpha), md.c_str());
    dl->PopClipRect();

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + card_h + m.card_gap));
    return r;
}
