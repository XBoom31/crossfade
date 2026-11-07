#include "tasks.h"
#include "ui.h"
#include "ytdlp.h"
#include "texture.h"
#include "mpvplayer.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ui_notify.h"
#include <thread>
#include <algorithm>
#include <cstring>

// The Saved Albums tab shares the Radio/Stats motion design: one start stamp
// drives a header fade, a staggered spring-settle on each card and in_view()
// gating so cards below the fold animate only once scrolled into sight.
static float clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

using AlbumsAnim = UiPageAnim;


static void play_album_track(AppState &state, int index)
{
    if (index < 0 || index >= (int)state.album_tracks.size())
        return;
    const auto &t = state.album_tracks[index];
    state.current_title = t.title;
    state.current_artist = t.artist;
    state.current_thumbnail_url = t.thumbnail_url.empty() ? state.album_thumbnail_url : t.thumbnail_url;
    state.current_track_id = t.id;
    state.current_album_id = t.album_id.empty() ? state.album_browse_id : t.album_id;
    state.current_artist_id = t.artist_id;
    state.current_track_url = t.url;
    state.current_track_index = index;
    state.queue_replace_from_source = true;
    state.queue_replace_index = index;
    state.queue_replace_id = t.id;
    state.current_playlist_source = PlaylistSource::ALBUM;

    state.pending_play_url = t.url;
    state.pending_play_title = t.title;
    state.pending_play_artist = t.artist;
    state.pending_play_thumbnail_url = state.current_thumbnail_url;
    state.pending_video_id = t.id;
    state.pending_video_url = t.url;
    state.pending_manual_play = true;
    state.loading = true;
    state.loading_seq.fetch_add(1, std::memory_order_relaxed);
}

static void start_album_download(AppState &state, const std::string &browse_id)
{
    if (browse_id.empty() || state.album_tracks.empty())
        return;
    if (state.album_downloading.exchange(true))
        return;

    std::vector<std::string> ids;
    ids.reserve(state.album_tracks.size());
    for (const auto &t : state.album_tracks)
        if (!t.id.empty())
            ids.push_back(t.id);

    if (ids.empty())
    {
        state.album_downloading = false;
        return;
    }

    state.album_download_browse_id = browse_id;
    state.album_download_done = 0;
    state.album_download_total = (int)ids.size();

    tasks::spawn([&state, ids]() {
        int done = 0;
        for (const auto &id : ids)
        {
            if (find_cached_audio_path(id).empty())
                get_cached_audio_path(id);
            g_downloads_dirty = true;
            state.album_download_done = ++done;
            state.repaint_requested = true;
        }
        g_downloads_dirty = true;
        state.album_downloading = false;
        state.repaint_requested = true;
    });
}

// Background album fetch shared by open_album_view and the Retry button on
// the inline error state. Publishes to the album_* fields on completion.
static void fetch_album_into(AppState &state, const std::string &browse_id)
{
    Album album;
    if (fetch_album(browse_id, album))
    {
        prefetch_urls_for(album.tracks, album.tracks.size());
        save_album_cache(album);
        std::lock_guard<std::recursive_mutex> lock(state.album_mutex);
        state.album_browse_id = album.browse_id.empty() ? browse_id : album.browse_id;
        state.album_title = album.title;
        state.album_artist = album.artist;
        state.album_year = album.year;
        state.album_duration = album.duration;
        state.album_description = album.description;
        state.album_thumbnail_url = album.thumbnail_url;
        state.album_tracks = std::move(album.tracks);
        state.album_failed = false;
        state.album_error_msg.clear();
        state.repaint_requested = true;
    }
    else
    {
        BackendError err = take_backend_error();
        std::lock_guard<std::recursive_mutex> lock(state.album_mutex);
        // Only flag the failure when there's nothing to show; a stale cached
        // album on screen just gets the toast.
        if (state.album_tracks.empty() && err)
        {
            state.album_failed = true;
            state.album_error_msg = backend_error_user_message(err);
            ui_notify_backend_error(err);
        }
        state.repaint_requested = true;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(state.album_mutex);
        state.album_loading = false;
    }
}

void open_album_view(AppState &state, const std::string &browse_id)
{
    if (browse_id.empty())
        return;
    if (state.current_tab == Tab::Album)
        return;
    std::lock_guard<std::recursive_mutex> lock(state.album_mutex);
    ui_invalidate_forward(state);
    state.album_return_tab = state.current_tab;
    state.album_return_source = state.current_playlist_source;
    if (state.album_return_source == PlaylistSource::PLAYLIST)
        state.album_return_playlist_index = state.selected_playlist_index;
    else
        state.album_return_playlist_index = -1;

    state.album_browse_id = browse_id;
    state.album_title.clear();
    state.album_artist.clear();
    state.album_year.clear();
    state.album_duration.clear();
    state.album_description.clear();
    state.album_thumbnail_url.clear();
    state.album_tracks.clear();

    // Show the cached album instantly when we've opened it before; the
    // background refresh below updates it silently.
    Album cached;
    bool had_cache = load_album_cache(browse_id, cached);
    if (had_cache)
    {
        state.album_browse_id = cached.browse_id.empty() ? browse_id : cached.browse_id;
        state.album_title = cached.title;
        state.album_artist = cached.artist;
        state.album_year = cached.year;
        state.album_duration = cached.duration;
        state.album_description = cached.description;
        state.album_thumbnail_url = cached.thumbnail_url;
        state.album_tracks = std::move(cached.tracks);
        prefetch_urls_for(state.album_tracks, state.album_tracks.size());
    }
    state.album_loading = !had_cache;
    state.album_failed = false;
    state.album_error_msg.clear();
    state.current_tab = Tab::Album;
    state.current_playlist_source = PlaylistSource::ALBUM;

    tasks::spawn([&state, browse_id]() {
        fetch_album_into(state, browse_id);
    });
}

void open_album_from_track(AppState &state, const Track &track)
{
    std::lock_guard<std::recursive_mutex> state_lock(state.album_mutex);
    if (!track.album_id.empty())
    {
        if (!track.album.empty() && track.album != state.album_title)
            state.album_title = track.album;
        open_album_view(state, track.album_id);
        return;
    }
    if (track.id.empty())
        return;

    ui_invalidate_forward(state);
    state.album_return_tab = state.current_tab;
    state.album_return_source = state.current_playlist_source;
    if (state.album_return_source == PlaylistSource::PLAYLIST)
        state.album_return_playlist_index = state.selected_playlist_index;
    else
        state.album_return_playlist_index = -1;

    state.album_browse_id.clear();
    state.album_title = track.album;
    state.album_artist = track.artist;
    state.album_year.clear();
    state.album_duration.clear();
    state.album_description.clear();
    state.album_thumbnail_url = track.thumbnail_url;
    state.album_tracks.clear();
    state.album_loading = true;
    state.current_tab = Tab::Album;
    state.current_playlist_source = PlaylistSource::ALBUM;

    tasks::spawn([&state, track]() {
        std::string bid, bname;
        if (fetch_album_for_track(track.id, bid, bname))
        {
            if (!bname.empty())
                state.album_title = bname;
            Album album;
            if (fetch_album(bid, album))
            {
                prefetch_urls_for(album.tracks, album.tracks.size());
                save_album_cache(album);
                std::lock_guard<std::recursive_mutex> lock(state.album_mutex);
                state.album_browse_id = album.browse_id.empty() ? bid : album.browse_id;
                if (album.title.empty() && !bname.empty())
                    state.album_title = bname;
                else if (!album.title.empty())
                    state.album_title = album.title;
                if (album.artist.empty())
                    state.album_artist = album.artist.empty() ? track.artist : album.artist;
                else
                    state.album_artist = album.artist;
                state.album_year = album.year;
                state.album_duration = album.duration;
                state.album_description = album.description;
                if (album.thumbnail_url.empty())
                    state.album_thumbnail_url = track.thumbnail_url;
                else
                    state.album_thumbnail_url = album.thumbnail_url;
                state.album_tracks = std::move(album.tracks);
                state.album_failed = false;
                state.album_error_msg.clear();
            }
        }
        else if (state.album_tracks.empty())
        {
            BackendError err = take_backend_error();
            std::lock_guard<std::recursive_mutex> lock(state.album_mutex);
            if (err)
            {
                state.album_failed = true;
                state.album_error_msg = backend_error_user_message(err);
                ui_notify_backend_error(err);
            }
        }
        {
            std::lock_guard<std::recursive_mutex> lock(state.album_mutex);
            state.album_loading = false;
            state.repaint_requested = true;
        }
    });
}

void close_album_view(AppState &state)
{
    Tab ret = state.album_return_tab;
    state.current_tab = ret;
    if (ret == Tab::Playlists)
    {
        state.current_playlist_source = PlaylistSource::PLAYLIST;
        if (state.album_return_playlist_index >= 0 && state.album_return_playlist_index < (int)state.playlists.size())
            state.selected_playlist_index = state.album_return_playlist_index;
    }
    else if (ret == Tab::Albums)
        state.current_playlist_source = state.album_return_source;
    else
        state.current_playlist_source = playlist_source_for_tab(ret);
}

static void save_current_album(AppState &state)
{
    if (state.album_browse_id.empty())
        return;
    SavedAlbum a;
    a.browse_id = state.album_browse_id;
    a.title = state.album_title.empty() ? "Unknown Album" : state.album_title;
    a.artist = state.album_artist;
    a.year = state.album_year;
    a.duration = state.album_duration;
    a.thumbnail_url = state.album_thumbnail_url;
    a.track_count = (int)state.album_tracks.size();
    save_album(state, a);
}

static void refresh_saved_album_metadata(AppState &state)
{
    if (state.album_loading || state.album_browse_id.empty() || !is_album_saved(state, state.album_browse_id))
        return;
    for (auto &a : state.saved_albums)
    {
        if (a.browse_id != state.album_browse_id)
            continue;
        bool changed = false;
        if (!state.album_title.empty() && a.title != state.album_title) { a.title = state.album_title; changed = true; }
        if (!state.album_artist.empty() && a.artist != state.album_artist) { a.artist = state.album_artist; changed = true; }
        if (!state.album_year.empty() && a.year != state.album_year) { a.year = state.album_year; changed = true; }
        if (!state.album_duration.empty() && a.duration != state.album_duration) { a.duration = state.album_duration; changed = true; }
        if (!state.album_thumbnail_url.empty() && a.thumbnail_url != state.album_thumbnail_url) { a.thumbnail_url = state.album_thumbnail_url; changed = true; }
        if (!state.album_tracks.empty() && a.track_count != (int)state.album_tracks.size()) { a.track_count = (int)state.album_tracks.size(); changed = true; }
        if (changed)
            save_saved_albums(state.saved_albums);
        break;
    }
}

void draw_albums_panel(AppState &state)
{
    const UiMetrics &m = ui_m();

    const double now = ImGui::GetTime();
    static uint64_t s_last_epoch = (uint64_t)-1;
    static double s_start = 0.0;
    if (s_last_epoch != ui_page_epoch())
        s_start = now; // replay when the tab reappears
    s_last_epoch = ui_page_epoch();
    if (s_start <= 0.0)
        s_start = now;
    AlbumsAnim anim;
    anim.now = now;
    anim.start = s_start;
    if (ui_animations_enabled() &&
        (float)(anim.now - anim.start) < ui_anim_delay(1.0f) + ui_anim_duration(0.9f))
        ui_anim_request();

    {
        const ImVec2 p = ImGui::GetCursorPos();
        const float t = anim.stagger(0.0f, 0.45f);
        const float off = (1.0f - t) * ui_anim_offset(10.0f);
        ImGui::SetCursorPosY(p.y + off);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
        ui_section_header("Saved Albums", (std::to_string(state.saved_albums.size()) + (state.saved_albums.size() == 1 ? " album" : " albums")).c_str());
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - off);
    }

    if (state.saved_albums.empty())
    {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim.stagger(0.08f, 0.5f));
        ui_empty_state(UiIcon::Album, "No saved albums", "Save an album from any track or album page");
        ImGui::PopStyleVar();
        return;
    }

    ImGui::BeginChild("SavedAlbumsScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();

    const float gap = m.card_gap;
    const float available = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((available + gap) / (180.0f * m.layout_scale + gap)));
    cols = std::min(cols, 6);
    const float col_w = (available - gap * (cols - 1)) / cols;
    const float art = std::min(col_w, 210.0f * m.layout_scale);

    int remove_index = -1;
    for (int i = 0; i < (int)state.saved_albums.size(); ++i)
    {
        const SavedAlbum &album = state.saved_albums[i];
        const int col = i % cols;
        if (col == 0 && i > 0)
            ImGui::Dummy(ImVec2(0.0f, m.space_1));
        if (col > 0)
            ImGui::SameLine(0.0f, gap);

        ImGui::PushID(i);
        std::string meta = album.artist;
        if (!album.year.empty())
            meta += (meta.empty() ? "" : "  ·  ") + album.year;
        if (album.track_count > 0)
            meta += (meta.empty() ? "" : "  ·  ") + std::to_string(album.track_count) + " tracks";

        // Spring-settle: ease-out-back lifts the card slightly past its rest
        // position before it lands, and in_view keeps off-screen rows still.
        const float t = anim.pop(0.05f + (float)i * 0.035f, 0.5f) * anim.in_view(ImGui::GetCursorScreenPos().y, 110.0f);
        const float off = (1.0f - t) * ui_anim_offset(14.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + off);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, clamp01f(t));
        MediaCardResult r = ui_media_card("##album_card", album.title.empty() ? "Unknown Album" : album.title,
                                          meta, album.thumbnail_url, col_w, art, false, false);
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - off);
        if (r.clicked && !r.double_clicked)
            open_album_view(state, album.browse_id);
        if (r.right_clicked)
            ImGui::OpenPopup("##album_ctx");
        if (ImGui::BeginPopup("##album_ctx"))
        {
            if (ImGui::MenuItem("Open album"))
                open_album_view(state, album.browse_id);
            if (ImGui::MenuItem("Remove from saved"))
                remove_index = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_4));
    ImGui::EndChild();

    if (remove_index >= 0 && remove_index < (int)state.saved_albums.size())
        remove_saved_album(state, state.saved_albums[remove_index].browse_id);
}

void draw_album_panel(AppState &state)
{
    std::lock_guard<std::recursive_mutex> album_lock(state.album_mutex);
    const UiMetrics &m = ui_m();
    refresh_saved_album_metadata(state);

    const float art = std::min(170.0f * m.layout_scale, ImGui::GetContentRegionAvail().x * 0.34f);
    const ImVec2 hero_pos = ImGui::GetCursorScreenPos();
    TextureInfo ti = state.album_thumbnail_url.empty() ? TextureInfo{} : texture_info(state.album_thumbnail_url);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (ti.id)
    {
        float u0, v0, u1, v1;
        cover_uv(ti, art, art, u0, v0, u1, v1);
        dl->AddImageRounded((ImTextureID)(intptr_t)ti.id, hero_pos, ImVec2(hero_pos.x + art, hero_pos.y + art),
                            ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32_WHITE, m.radius_md);
    }
    else
    {
        dl->AddRectFilled(hero_pos, ImVec2(hero_pos.x + art, hero_pos.y + art),
                          ImGui::GetColorU32(ui_c().surface_hover), m.radius_md);
        ui_draw_icon(dl, UiIcon::Album, ImVec2(hero_pos.x + art * 0.5f, hero_pos.y + art * 0.5f),
                     art * 0.28f, ui_text_muted_u32(), 1.4f);
    }

    const float text_x = hero_pos.x + art + m.space_5;
    const float text_w = std::max(80.0f, hero_pos.x + ImGui::GetContentRegionAvail().x - text_x);
    float y = hero_pos.y + m.space_1;
    ui_push_micro();
    ImGui::SetCursorScreenPos(ImVec2(text_x, y));
    ImGui::TextColored(ui_c().accent_text, "ALBUM");
    ImGui::PopFont();
    y += 22.0f * m.text_scale;

    std::string title = state.album_title.empty() ? (state.album_loading ? "Loading album..." : "Unknown Album") : state.album_title;
    title = ui_ellipsize(g_ui_fonts.semibold, 34.0f, title, text_w);
    dl->AddText(g_ui_fonts.semibold, 34.0f * m.text_scale, ImVec2(text_x, y), ui_text_u32(), title.c_str());
    y += 34.0f * m.text_scale * 1.25f + m.space_1;

    std::string artist = state.album_artist.empty() ? "Unknown Artist" : state.album_artist;
    artist = ui_ellipsize(g_ui_fonts.medium, 20.0f, artist, text_w);
    dl->AddText(g_ui_fonts.medium, 20.0f * m.text_scale, ImVec2(text_x, y), ui_text_dim_u32(), artist.c_str());
    y += 20.0f * m.text_scale * 1.25f + m.space_1;

    std::string meta;
    if (!state.album_year.empty())
        meta += state.album_year;
    if (!state.album_tracks.empty())
        meta += (meta.empty() ? "" : "  ·  ") + std::to_string(state.album_tracks.size()) + " tracks";
    else if (state.album_loading)
        meta += (meta.empty() ? "" : "  ·  ") + std::string("Loading...");
    if (!state.album_duration.empty())
        meta += (meta.empty() ? "" : "  ·  ") + state.album_duration;
    meta = ui_ellipsize(g_ui_fonts.body, 16.0f, meta, text_w);
    dl->AddText(g_ui_fonts.body, 16.0f * m.text_scale, ImVec2(text_x, y), ui_text_muted_u32(), meta.c_str());
    y += 16.0f * m.text_scale * 1.25f + m.space_3;

    ImGui::SetCursorScreenPos(ImVec2(text_x, y));
    if (ui_primary_button("Play all", ImVec2(112.0f * m.layout_scale, m.button_h)) && !state.album_tracks.empty())
        play_album_track(state, 0);

    if (!state.album_browse_id.empty())
    {
        ImGui::SameLine(0.0f, m.space_2);
        const bool saved = is_album_saved(state, state.album_browse_id);
        if (ui_secondary_button(saved ? "Saved" : "Save album", ImVec2(132.0f * m.layout_scale, m.button_h)))
        {
            if (saved)
                remove_saved_album(state, state.album_browse_id);
            else
                save_current_album(state);
        }

        if (!state.album_tracks.empty())
        {
            ImGui::SameLine(0.0f, m.space_2);
            bool downloading_this = state.album_downloading && state.album_download_browse_id == state.album_browse_id;

            // How much of this album is already on disk. Cheap enough to
            // recheck per frame: one opendir scan per track over the cache
            // dir, and it keeps the label honest when tracks are added or
            // deleted outside this page.
            int cached_count = 0;
            for (const auto &t : state.album_tracks)
                if (!t.id.empty() && !find_cached_audio_path(t.id).empty())
                    ++cached_count;

            std::string label;
            bool is_delete = false;
            if (downloading_this)
            {
                label = "Downloading " + std::to_string(state.album_download_done.load()) + "/" +
                        std::to_string(state.album_download_total.load());
            }
            else if (cached_count == (int)state.album_tracks.size())
            {
                label = "Delete download";
                is_delete = true;
            }
            else if (cached_count > 0)
            {
                label = "Download (" + std::to_string(cached_count) + "/" +
                        std::to_string(state.album_tracks.size()) + ")";
            }
            else
            {
                label = "Download";
            }

            // Deleting the whole album's downloads is destructive enough to
            // want a second click: arm on the first press, disarm elsewhere.
            static std::string s_delete_armed_id;
            if (is_delete && s_delete_armed_id != state.album_browse_id)
                label = "Delete download?";
            if (downloading_this)
                ImGui::BeginDisabled();
            if (ui_secondary_button(label.c_str(), ImVec2(132.0f * m.layout_scale, m.button_h)))
            {
                if (is_delete)
                {
                    if (s_delete_armed_id == state.album_browse_id)
                    {
                        for (const auto &t : state.album_tracks)
                            if (!t.id.empty() && !find_cached_audio_path(t.id).empty())
                                delete_downloaded_track(state, t.id);
                        s_delete_armed_id.clear();
                    }
                    else
                    {
                        s_delete_armed_id = state.album_browse_id;
                    }
                }
                else if (!downloading_this)
                {
                    s_delete_armed_id.clear();
                    start_album_download(state, state.album_browse_id);
                }
            }
            if (downloading_this)
                ImGui::EndDisabled();
            else if (!is_delete)
                s_delete_armed_id.clear();
        }
    }

    const float hero_bottom = std::max(hero_pos.y + art, y + m.button_h) + m.space_5;
    ImGui::SetCursorScreenPos(ImVec2(hero_pos.x, hero_bottom));

    if (!state.album_description.empty())
    {
        std::string desc = state.album_description;
        if (desc.size() > 420)
            desc = desc.substr(0, 417) + "...";
        ui_push_small();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ui_c().text_muted, "%s", desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, m.space_4));
    }

    ImGui::BeginChild("AlbumTracks", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();

    if (state.album_tracks.empty())
    {
        if (state.album_loading)
            ui_empty_state(UiIcon::Album, "Loading album", "Fetching tracks");
        else if (state.album_failed)
        {
            if (ui_error_state(state.album_error_msg.empty()
                                   ? "Couldn't load this album."
                                   : state.album_error_msg.c_str()))
            {
                // Re-run the same background fetch open_album_view uses.
                state.album_failed = false;
                state.album_loading = true;
                state.repaint_requested = true;
                tasks::spawn([&state, id = state.album_browse_id]() {
                    fetch_album_into(state, id);
                });
            }
        }
        else
            ui_empty_state(UiIcon::Album, "No playable tracks", "This album has no tracks available");
        ImGui::EndChild();
        return;
    }

    const std::string reveal_key = std::string("album:") + state.album_browse_id;
    const double reveal_start = ui_anim_track_list_reveal(reveal_key.c_str(), state.album_tracks);
    const float row_pitch = ui_track_row_pitch(44.0f * m.layout_scale);
    ImGuiListClipper clipper;
    clipper.Begin((int)state.album_tracks.size(), row_pitch);
    while (clipper.Step())
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
    {
        TrackRowOptions opt;
        opt.show_rank = true;
        opt.rank = state.album_tracks[i].track_number > 0 ? state.album_tracks[i].track_number : i + 1;
        opt.show_duration = true;
        opt.art_size = 44.0f * m.layout_scale;
        opt.reveal_start = reveal_start;
        opt.reveal_index = i;
        Track row = state.album_tracks[i];
        // Album tracks from the backend carry no per-track art, so use the
        // album cover for every row (and keep it even if a stale per-track
        // url slipped through).
        if (!state.album_thumbnail_url.empty())
            row.thumbnail_url = state.album_thumbnail_url;
        ui_track_row(state, row, i, opt);
    }
    ImGui::Dummy(ImVec2(0.0f, m.space_4));
    ImGui::EndChild();
}
