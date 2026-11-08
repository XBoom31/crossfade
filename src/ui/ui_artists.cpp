#include "tasks.h"
#include "ui.h"
#include "ytdlp.h"
#include "texture.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ui_notify.h"
#include <thread>
#include <algorithm>
#include <cstring>

// The Saved Artists tab mirrors the Radio/Stats motion design: one start
// stamp drives a header fade, a staggered spring-settle on each card and
// in_view() gating so cards below the fold animate only once scrolled in.
static float clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

using ArtistsAnim = UiPageAnim;


static void apply_artist(AppState &state, const ArtistInfo &artist)
{
    state.artist_browse_id = artist.browse_id;
    state.artist_name = artist.name;
    state.artist_description = artist.description;
    state.artist_subscribers = artist.subscribers;
    state.artist_monthly_listeners = artist.monthly_listeners;
    state.artist_thumbnail_url = artist.thumbnail_url;
    state.artist_songs = artist.songs;
    state.artist_songs_browse_id = artist.songs_browse_id;
    state.artist_songs_offset = (int)artist.songs.size();
    state.artist_songs_has_more = !artist.songs_browse_id.empty();
    state.artist_songs_loading = false;
    state.artist_albums.clear();
    for (const auto &a : artist.albums)
    {
        SavedAlbum album;
        album.browse_id = a.browse_id;
        album.title = a.title;
        album.artist = artist.name;
        album.year = a.year;
        album.thumbnail_url = a.thumbnail_url;
        state.artist_albums.push_back(std::move(album));
    }
}

static void play_artist_track(AppState &state, int index)
{
    if (index < 0 || index >= (int)state.artist_songs.size())
        return;
    const auto &t = state.artist_songs[index];
    state.current_title = t.title;
    state.current_artist = t.artist;
    state.current_thumbnail_url = t.thumbnail_url.empty() ? state.artist_thumbnail_url : t.thumbnail_url;
    state.current_track_id = t.id;
    state.current_album_id = t.album_id;
    state.current_artist_id = t.artist_id.empty() ? state.artist_browse_id : t.artist_id;
    state.current_track_url = t.url;
    state.current_track_index = index;
    state.queue_replace_from_source = true;
    state.queue_replace_index = index;
    state.queue_replace_id = t.id;
    state.current_playlist_source = PlaylistSource::ARTIST;

    state.pending_play_url = playable_track_url(t.id, t.url);
    state.pending_play_title = t.title;
    state.pending_play_artist = t.artist;
    state.pending_play_thumbnail_url = state.current_thumbnail_url;
    state.pending_video_id = t.id;
    state.pending_video_url = playable_track_url(t.id, t.url);
    state.pending_manual_play = true;
    state.loading = true;
    state.loading_seq.fetch_add(1, std::memory_order_relaxed);
}

// Background artist fetch shared by open_artist_view and the Retry button on
// the inline error state. Publishes to the artist_* fields on completion.
static void fetch_artist_into(AppState &state, const std::string &browse_id)
{
    ArtistInfo artist;
    if (fetch_artist(browse_id, artist))
    {
        const bool same_artist = state.artist_browse_id == browse_id ||
                                 state.artist_browse_id == artist.browse_id;
        const std::vector<Track> previous = same_artist ? state.artist_songs
                                                         : std::vector<Track>{};
        const int previous_offset = state.artist_songs_offset;
        const bool previous_has_more = state.artist_songs_has_more;

        prefetch_urls_for(artist.songs, 5);
        apply_artist(state, artist);

        // A background refresh publishes only the header's top songs. If the
        // user already expanded this artist, keep those extra rows.
        if (same_artist && !previous.empty())
        {
            for (const Track &old : previous)
            {
                if (old.id.empty())
                    continue;
                const bool exists = std::any_of(
                    state.artist_songs.begin(), state.artist_songs.end(),
                    [&](const Track &t) { return t.id == old.id; });
                if (!exists)
                    state.artist_songs.push_back(old);
            }
            state.artist_songs_offset =
                std::max((int)artist.songs.size(), previous_offset);
            state.artist_songs_has_more =
                !state.artist_songs_browse_id.empty() && previous_has_more;
        }

        ArtistInfo cached = artist;
        cached.songs = state.artist_songs;
        save_artist_cache(cached);

        state.artist_failed = false;
        state.artist_error_msg.clear();
    }
    else
    {
        BackendError err = take_backend_error();
        // Only flag the failure when there's nothing to show; a stale cached
        // artist on screen just gets the toast.
        if (state.artist_songs.empty() && state.artist_albums.empty() && err)
        {
            state.artist_failed = true;
            state.artist_error_msg = backend_error_user_message(err);
            ui_notify_backend_error(err);
        }
    }
    state.artist_loading = false;
    state.repaint_requested = true;
}

// Pull the next five songs from the artist's full songs shelf. The backend
// caches the full list, so repeated clicks are cheap.
static void load_more_artist_songs(AppState &state, const std::string &artist_id,
                                   const std::string &browse_id, int offset)
{
    std::vector<Track> more;
    const bool ok = fetch_artist_songs(browse_id, offset, 5, more);
    if (state.artist_browse_id != artist_id ||
        state.artist_songs_browse_id != browse_id)
        return; // the user navigated to another artist while this was loading
    if (ok)
    {
        int added = 0;
        for (const Track &t : more)
        {
            if (t.id.empty())
                continue;
            const bool exists = std::any_of(
                state.artist_songs.begin(), state.artist_songs.end(),
                [&](const Track &existing) { return existing.id == t.id; });
            if (!exists)
            {
                state.artist_songs.push_back(t);
                ++added;
            }
        }
        state.artist_songs_offset = offset + (int)more.size();
        if ((int)more.size() < 5)
            state.artist_songs_has_more = false;
        if (added > 0)
            prefetch_urls_for(more, more.size());

        ArtistInfo cached;
        if (load_artist_cache(state.artist_browse_id, cached))
        {
            cached.songs = state.artist_songs;
            save_artist_cache(cached);
        }
    }
    else
    {
        BackendError err = take_backend_error();
        if (err)
            ui_notify_backend_error(err);
    }
    state.artist_songs_loading = false;
    state.repaint_requested = true;
}

void open_artist_view(AppState &state, const std::string &browse_id,
                      const std::string &display_name)
{
    if (browse_id.empty())
        return;
    if (state.current_tab == Tab::Artist && state.artist_browse_id == browse_id)
        return;

    ui_invalidate_forward(state);
    state.artist_return_tab = state.current_tab;
    state.artist_return_source = state.current_playlist_source;
    if (state.artist_return_source == PlaylistSource::PLAYLIST)
        state.artist_return_playlist_index = state.selected_playlist_index;
    else
        state.artist_return_playlist_index = -1;

    state.artist_browse_id = browse_id;
    state.artist_name = display_name;
    state.artist_description.clear();
    state.artist_subscribers.clear();
    state.artist_monthly_listeners.clear();
    state.artist_thumbnail_url.clear();
    state.artist_songs.clear();
    state.artist_albums.clear();
    state.artist_songs_browse_id.clear();
    state.artist_songs_offset = 0;
    state.artist_songs_has_more = false;
    state.artist_songs_loading = false;

    ArtistInfo cached;
    bool had_cache = load_artist_cache(browse_id, cached);
    if (had_cache)
    {
        apply_artist(state, cached);
        prefetch_urls_for(state.artist_songs, 5);
        if (!display_name.empty())
            state.artist_name = display_name;
    }
    state.artist_loading = !had_cache;
    state.artist_failed = false;
    state.artist_error_msg.clear();
    state.current_tab = Tab::Artist;
    state.current_playlist_source = PlaylistSource::ARTIST;

    tasks::spawn("artist-fetch", [&state, browse_id]() {
        fetch_artist_into(state, browse_id);
    });
}

void open_artist_from_track(AppState &state, const Track &track)
{
    if (!track.artist_id.empty())
    {
        open_artist_view(state, track.artist_id, track.artist);
        return;
    }
    if (track.id.empty())
        return;

    ui_invalidate_forward(state);

    state.artist_return_tab = state.current_tab;
    state.artist_return_source = state.current_playlist_source;
    if (state.artist_return_source == PlaylistSource::PLAYLIST)
        state.artist_return_playlist_index = state.selected_playlist_index;
    else
        state.artist_return_playlist_index = -1;

    state.artist_browse_id.clear();
    state.artist_name = track.artist;
    state.artist_description.clear();
    state.artist_subscribers.clear();
    state.artist_monthly_listeners.clear();
    state.artist_thumbnail_url = track.thumbnail_url;
    state.artist_songs.clear();
    state.artist_albums.clear();
    state.artist_songs_browse_id.clear();
    state.artist_songs_offset = 0;
    state.artist_songs_has_more = false;
    state.artist_songs_loading = false;
    state.artist_loading = true;
    state.artist_failed = false;
    state.artist_error_msg.clear();
    state.current_tab = Tab::Artist;
    state.current_playlist_source = PlaylistSource::ARTIST;

    tasks::spawn("artist-open", [&state, track]() {
        std::string artist_id, artist_name;
        if (fetch_artist_for_track(track.id, artist_id, artist_name))
        {
            state.artist_browse_id = artist_id;
            if (!artist_name.empty())
                state.artist_name = artist_name;
            ArtistInfo cached;
            if (load_artist_cache(artist_id, cached))
            {
                apply_artist(state, cached);
                prefetch_urls_for(state.artist_songs, 5);
            }
            ArtistInfo artist;
            if (fetch_artist(artist_id, artist))
            {
                prefetch_urls_for(artist.songs, 5);
                save_artist_cache(artist);
                apply_artist(state, artist);
            }
            else
            {
                BackendError err = take_backend_error();
                if (err)
                {
                    state.artist_failed = state.artist_songs.empty() &&
                                          state.artist_albums.empty();
                    state.artist_error_msg = backend_error_user_message(err);
                    ui_notify_backend_error(err);
                }
            }
        }
        else
        {
            BackendError err = take_backend_error();
            if (err)
            {
                state.artist_failed = state.artist_songs.empty() &&
                                      state.artist_albums.empty();
                state.artist_error_msg = backend_error_user_message(err);
                ui_notify_backend_error(err);
            }
        }
        state.artist_loading = false;
        state.repaint_requested = true;
    });
}

void close_artist_view(AppState &state)
{
    Tab ret = state.artist_return_tab;
    state.current_tab = ret;
    if (ret == Tab::Playlists)
    {
        state.current_playlist_source = PlaylistSource::PLAYLIST;
        if (state.artist_return_playlist_index >= 0 &&
            state.artist_return_playlist_index < (int)state.playlists.size())
            state.selected_playlist_index = state.artist_return_playlist_index;
    }
    else if (ret == Tab::Artists || ret == Tab::Albums)
        state.current_playlist_source = state.artist_return_source;
    else
        state.current_playlist_source = playlist_source_for_tab(ret);
}

static SavedArtist artist_from_summary(const ArtistSummary &s)
{
    SavedArtist a;
    a.browse_id = s.browse_id;
    a.name = s.name;
    a.thumbnail_url = s.thumbnail_url;
    a.subscribers = s.subscribers;
    return a;
}

static void save_current_artist(AppState &state)
{
    if (state.artist_browse_id.empty())
        return;
    SavedArtist a;
    a.browse_id = state.artist_browse_id;
    a.name = state.artist_name.empty() ? "Unknown Artist" : state.artist_name;
    a.thumbnail_url = state.artist_thumbnail_url;
    a.subscribers = state.artist_subscribers;
    a.monthly_listeners = state.artist_monthly_listeners;
    save_artist(state, a);
}

static void refresh_saved_artist_metadata(AppState &state)
{
    if (state.artist_loading || state.artist_browse_id.empty() || !is_artist_saved(state, state.artist_browse_id))
        return;
    for (auto &a : state.saved_artists)
    {
        if (a.browse_id != state.artist_browse_id)
            continue;
        bool changed = false;
        if (!state.artist_name.empty() && a.name != state.artist_name) { a.name = state.artist_name; changed = true; }
        if (!state.artist_thumbnail_url.empty() && a.thumbnail_url != state.artist_thumbnail_url) { a.thumbnail_url = state.artist_thumbnail_url; changed = true; }
        if (!state.artist_subscribers.empty() && a.subscribers != state.artist_subscribers) { a.subscribers = state.artist_subscribers; changed = true; }
        if (!state.artist_monthly_listeners.empty() && a.monthly_listeners != state.artist_monthly_listeners) { a.monthly_listeners = state.artist_monthly_listeners; changed = true; }
        if (changed)
            save_saved_artists(state.saved_artists);
        break;
    }
}

void draw_artists_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    const bool searching = state.artist_search_active;
    // The display list is a full copy (or per-item string conversion while
    // searching); rebuilt every frame it's pure waste. Memoize on the cheap
    // things that can change it.
    static std::vector<SavedArtist> s_display;
    static bool s_searching = false;
    static size_t s_saved_size = 0;
    static const std::vector<ArtistSummary> *s_results_ptr = nullptr;
    static size_t s_results_size = 0;
    auto results = std::atomic_load(&state.artist_search_results);
    const bool stale = searching != s_searching ||
                       s_saved_size != state.saved_artists.size() ||
                       s_results_ptr != results.get() ||
                       (results && s_results_size != results->size());
    if (stale)
    {
        s_searching = searching;
        s_saved_size = state.saved_artists.size();
        s_results_ptr = results.get();
        s_results_size = results ? results->size() : 0;
        s_display.clear();
        if (searching)
        {
            if (results)
                for (const auto &r : *results)
                    s_display.push_back(artist_from_summary(r));
        }
        else
        {
            s_display = state.saved_artists;
        }
    }
    const std::vector<SavedArtist> &display = s_display;

    std::string meta = std::to_string(display.size()) + (display.size() == 1 ? " artist" : " artists");

    const double now = ImGui::GetTime();
    static uint64_t s_last_epoch = (uint64_t)-1;
    static double s_start = 0.0;
    if (s_last_epoch != ui_page_epoch())
        s_start = now; // replay when the tab reappears
    s_last_epoch = ui_page_epoch();
    if (s_start <= 0.0)
        s_start = now;
    ArtistsAnim anim;
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
        ui_section_header(searching ? "Artist Search" : "Saved Artists", meta.c_str());
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - off);
    }

    if (display.empty())
    {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim.stagger(0.08f, 0.5f));
        if (searching && state.artist_search_loading)
            ui_searching_state(UiIcon::Artist, "Searching artists", "Looking across YouTube Music");
        else if (searching)
            ui_empty_state(UiIcon::Search, "No artists found", "Try a different name");
        else
            ui_empty_state(UiIcon::Artist, "No saved artists", "Save artists from any track or artist page");
        ImGui::PopStyleVar();
        return;
    }

    ImGui::BeginChild("ArtistsScroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();

    const float gap = m.card_gap;
    const float available = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((available + gap) / (150.0f * m.layout_scale + gap)));
    cols = std::min(cols, 6);
    const float col_w = (available - gap * (cols - 1)) / cols;
    const float art = std::min(col_w, 190.0f * m.layout_scale);

    for (int i = 0; i < (int)display.size(); ++i)
    {
        const SavedArtist &artist = display[i];
        const int col = i % cols;
        if (col == 0 && i > 0)
            ImGui::Dummy(ImVec2(0.0f, m.space_1));
        if (col > 0)
            ImGui::SameLine(0.0f, gap);

        ImGui::PushID(i);
        std::string sub = artist.subscribers.empty() ? artist.monthly_listeners : artist.subscribers;
        if (sub.empty())
            sub = "Artist";
        // Spring-settle: ease-out-back lifts the card slightly past its rest
        // position before it lands, and in_view keeps off-screen rows still.
        const float t = anim.pop(0.05f + (float)i * 0.035f, 0.5f) * anim.in_view(ImGui::GetCursorScreenPos().y, 110.0f);
        const float off = (1.0f - t) * ui_anim_offset(14.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + off);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, clamp01f(t));
        MediaCardResult r = ui_media_card("##artist_card", artist.name.empty() ? "Unknown Artist" : artist.name,
                                          sub, artist.thumbnail_url, col_w, art, true, false);
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - off);
        if (r.clicked && !r.double_clicked)
            open_artist_view(state, artist.browse_id, artist.name);
        if (r.right_clicked)
            ImGui::OpenPopup("##artist_ctx");
        if (ImGui::BeginPopup("##artist_ctx"))
        {
            if (ImGui::MenuItem("Open artist"))
                open_artist_view(state, artist.browse_id, artist.name);
            const bool saved = is_artist_saved(state, artist.browse_id);
            if (ImGui::MenuItem(saved ? "Remove from saved" : "Save artist"))
            {
                if (saved)
                    remove_saved_artist(state, artist.browse_id);
                else
                    save_artist(state, artist);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_4));
    ImGui::EndChild();
}

void draw_artist_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    refresh_saved_artist_metadata(state);

    const float art = std::min(170.0f * m.layout_scale, ImGui::GetContentRegionAvail().x * 0.34f);
    const ImVec2 hero_pos = ImGui::GetCursorScreenPos();
    TextureInfo ti = state.artist_thumbnail_url.empty() ? TextureInfo{} : texture_info(state.artist_thumbnail_url);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 center(hero_pos.x + art * 0.5f, hero_pos.y + art * 0.5f);
    if (ti.id)
    {
        float u0, v0, u1, v1;
        cover_uv(ti, art, art, u0, v0, u1, v1);
        dl->AddImageRounded((ImTextureID)(intptr_t)ti.id, hero_pos, ImVec2(hero_pos.x + art, hero_pos.y + art),
                            ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32_WHITE, art * 0.5f);
    }
    else
    {
        dl->AddCircleFilled(center, art * 0.5f, ImGui::GetColorU32(ui_c().surface_hover), 48);
        ui_draw_icon(dl, UiIcon::Artist, center, art * 0.24f, ui_text_muted_u32(), 1.4f);
    }

    const float text_x = hero_pos.x + art + m.space_5;
    const float text_w = std::max(80.0f, hero_pos.x + ImGui::GetContentRegionAvail().x - text_x);
    float y = hero_pos.y + m.space_1;
    ui_push_micro();
    ImGui::SetCursorScreenPos(ImVec2(text_x, y));
    ImGui::TextColored(ui_c().accent_text, "ARTIST");
    ImGui::PopFont();
    y += 22.0f * m.text_scale;

    std::string title = state.artist_name.empty() ? (state.artist_loading ? "Loading artist..." : "Unknown Artist") : state.artist_name;
    title = ui_ellipsize(g_ui_fonts.semibold, 34.0f, title, text_w);
    dl->AddText(g_ui_fonts.semibold, 34.0f * m.text_scale, ImVec2(text_x, y), ui_text_u32(), title.c_str());
    y += 34.0f * m.text_scale * 1.25f + m.space_1;

    std::string meta;
    if (!state.artist_subscribers.empty())
        meta += state.artist_subscribers + " subscribers";
    if (!state.artist_monthly_listeners.empty())
        meta += (meta.empty() ? "" : "  ·  ") + state.artist_monthly_listeners + " monthly listeners";
    if (!state.artist_songs.empty())
        meta += (meta.empty() ? "" : "  ·  ") + std::to_string(state.artist_songs.size()) + " popular songs";
    else if (state.artist_loading)
        meta += (meta.empty() ? "" : "  ·  ") + std::string("Loading...");
    meta = ui_ellipsize(g_ui_fonts.body, 16.0f, meta, text_w);
    dl->AddText(g_ui_fonts.body, 16.0f * m.text_scale, ImVec2(text_x, y), ui_text_muted_u32(), meta.c_str());
    y += 16.0f * m.text_scale * 1.25f + m.space_3;

    ImGui::SetCursorScreenPos(ImVec2(text_x, y));
    if (ui_primary_button("Play all", ImVec2(112.0f * m.layout_scale, m.button_h)) && !state.artist_songs.empty())
        play_artist_track(state, 0);
    if (!state.artist_browse_id.empty())
    {
        ImGui::SameLine(0.0f, m.space_2);
        const bool saved = is_artist_saved(state, state.artist_browse_id);
        if (ui_secondary_button(saved ? "Saved" : "Save artist", ImVec2(132.0f * m.layout_scale, m.button_h)))
        {
            if (saved)
                remove_saved_artist(state, state.artist_browse_id);
            else
                save_current_artist(state);
        }
    }

    const float hero_bottom = std::max(hero_pos.y + art, y + m.button_h) + m.space_4;
    ImGui::SetCursorScreenPos(ImVec2(hero_pos.x, hero_bottom));

    ImGui::BeginChild("ArtistBody", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();

    if (!state.artist_description.empty())
    {
        ui_section_header("About");
        std::string desc = state.artist_description;
        if (desc.size() > 500)
            desc = desc.substr(0, 497) + "...";
        ui_push_small();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextColored(ui_c().text_muted, "%s", desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, m.space_4));
    }

    ui_section_header("Popular songs", nullptr);
    if (state.artist_songs.empty())
    {
        if (state.artist_loading)
            ui_empty_state(UiIcon::Artist, "Loading songs", "Fetching this artist");
        else if (state.artist_failed)
        {
            if (ui_error_state(state.artist_error_msg.empty()
                                   ? "Couldn't load this artist."
                                   : state.artist_error_msg.c_str()))
            {
                // Re-run the same background fetch open_artist_view uses.
                state.artist_failed = false;
                state.artist_loading = true;
                state.repaint_requested = true;
                tasks::spawn([&state, id = state.artist_browse_id]() {
                    fetch_artist_into(state, id);
                });
            }
        }
        else
            ui_empty_state(UiIcon::Artist, "No playable songs", "This artist has no tracks available");
    }
    else
    {
        const std::string reveal_key = std::string("artist:") + state.artist_browse_id;
        const double reveal_start = ui_anim_track_list_reveal(reveal_key.c_str(), state.artist_songs);
        const float row_pitch = ui_track_row_pitch(44.0f * m.layout_scale);
        const float list_start_y = ImGui::GetCursorPosY();
        ImGuiListClipper clipper;
        clipper.Begin((int)state.artist_songs.size(), row_pitch);
        while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
        {
            TrackRowOptions opt;
            opt.show_rank = true;
            opt.rank = i + 1;
            opt.show_duration = true;
            opt.art_size = 44.0f * m.layout_scale;
            opt.reveal_start = reveal_start;
            opt.reveal_index = i;
            Track t = state.artist_songs[i];
            if (t.thumbnail_url.empty())
                t.thumbnail_url = state.artist_thumbnail_url;
            if (!t.album.empty())
                opt.secondary = t.album;
            ui_track_row(state, t, i, opt);
        }
        // The clipper only advances the cursor over visible rows; move it to
        // the real end of the list before placing the footer button. The pitch
        // includes the inter-row gap, so subtract it to sit just under the
        // final row instead of leaving an awkward empty band.
        ImGui::SetCursorPosY(list_start_y +
                             (float)state.artist_songs.size() * row_pitch -
                             ImGui::GetStyle().ItemSpacing.y + m.space_2);

        if (state.artist_songs_has_more || state.artist_songs_loading)
        {
            if (state.artist_songs_loading)
            {
                ui_push_small();
                ImGui::TextColored(ui_c().text_muted, "Loading more songs...");
                ImGui::PopFont();
            }
            else if (ui_ghost_button("Load 5 more songs",
                                     ImVec2(180.0f * m.layout_scale, m.control_h)))
            {
                state.artist_songs_loading = true;
                state.repaint_requested = true;
                tasks::spawn([&state, browse = state.artist_browse_id,
                              songs = state.artist_songs_browse_id,
                              offset = state.artist_songs_offset]() {
                    load_more_artist_songs(state, browse, songs, offset);
                });
            }
        }
    }

    if (!state.artist_albums.empty())
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_4));
        ui_section_header("Albums & Singles", nullptr);
        const float gap = m.card_gap;
        const float available = ImGui::GetContentRegionAvail().x;
        int cols = std::max(1, (int)((available + gap) / (145.0f * m.layout_scale + gap)));
        cols = std::min(cols, 6);
        const float col_w = (available - gap * (cols - 1)) / cols;
        const float card_art = std::min(col_w, 180.0f * m.layout_scale);

        for (int i = 0; i < (int)state.artist_albums.size(); ++i)
        {
            const SavedAlbum &album = state.artist_albums[i];
            const int col = i % cols;
            if (col == 0 && i > 0)
                ImGui::Dummy(ImVec2(0.0f, m.space_1));
            if (col > 0)
                ImGui::SameLine(0.0f, gap);

            ImGui::PushID(i + 10000);
            std::string am = album.year;
            if (am.empty())
                am = state.artist_name;
            MediaCardResult r = ui_media_card("##artist_album", album.title.empty() ? "Unknown Album" : album.title,
                                              am, album.thumbnail_url, col_w, card_art, false, false);
            if (r.clicked && !r.double_clicked)
                open_album_view(state, album.browse_id);
            if (r.right_clicked)
                ImGui::OpenPopup("##artist_album_ctx");
            if (ImGui::BeginPopup("##artist_album_ctx"))
            {
                if (ImGui::MenuItem("Open album"))
                    open_album_view(state, album.browse_id);
                const bool saved = is_album_saved(state, album.browse_id);
                if (ImGui::MenuItem(saved ? "Remove saved album" : "Save album"))
                {
                    if (saved)
                        remove_saved_album(state, album.browse_id);
                    else
                    {
                        SavedAlbum a = album;
                        if (a.artist.empty())
                            a.artist = state.artist_name;
                        save_album(state, a);
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_4));
    ImGui::EndChild();
}
