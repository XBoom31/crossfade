#include "tasks.h"
#include "ui.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_notify.h"
#include "ui_theme.h"
#include "ytdlp.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

void draw_youtube_panel(AppState &state)
{
    const UiMetrics &m = ui_m();

    // Account playlists (the "Liked Music" auto-playlist is included).
    if (!state.youtube_playlists_loaded)
    {
        std::string raw;
        if (read_text_file(youtube_playlists_cache_path(), raw) && !raw.empty())
        {
            auto cached = parse_youtube_playlists_response(raw);
            if (!cached.empty())
            {
                state.youtube_playlists = std::move(cached);
                state.youtube_playlists_loaded = true;
            }
        }
    }

    if (!state.youtube_playlists_loaded && !state.youtube_playlists_loading.load())
    {
        state.youtube_playlists_loading = true;
        tasks::spawn("youtube", [&state]() {
            auto pls = fetch_youtube_playlists();
            BackendError err = take_backend_error();
            if (!pls.empty())
            {
                state.youtube_playlists = std::move(pls);
            }
            else if (err)
            {
                ui_notify_backend_error(err);
            }
            // Mark loaded even on failure so the empty-state spinner stops;
            // the left pane shows "No playlists found." + retry instead.
            state.youtube_playlists_loaded = true;
            state.youtube_playlists_loading = false;
            state.repaint_requested = true;
        });
    }
    else if (state.youtube_playlists_loaded && !state.youtube_playlists_loading.load() && !state.youtube_playlists_refreshed)
    {
        state.youtube_playlists_refreshed = true;
        state.youtube_playlists_loading = true;
        tasks::spawn("youtube", [&state]() {
            auto pls = fetch_youtube_playlists();
            BackendError err = take_backend_error();
            if (!pls.empty())
            {
                state.youtube_playlists = std::move(pls);
            }
            else if (err)
            {
                // Keep the cached list on screen; just tell the user why the
                // refresh didn't happen.
                ui_notify_backend_error(err);
                state.youtube_playlists_refreshed = false; // retry next visit
            }
            state.youtube_playlists_loading = false;
            state.repaint_requested = true;
        });
    }

    // Pre-cache all playlist contents in the background so they are
    // available offline without the user having to open each one.
    if (state.youtube_playlists_loaded && !state.youtube_playlists.empty() &&
        !state.youtube_playlist_contents_precaching.load() &&
        !state.youtube_playlist_contents_precached)
    {
        state.youtube_playlist_contents_precaching = true;
        std::vector<std::string> ids;
        for (const auto &pl : state.youtube_playlists)
            if (!pl.id.empty())
                ids.push_back(pl.id);
        tasks::spawn("youtube-playlist", [&state, ids]() {
            for (const auto &pid : ids)
            {
                // Skip if already cached.
                std::string raw;
                if (read_text_file(youtube_playlist_cache_path(pid), raw) && !raw.empty())
                    continue;
                auto tracks = fetch_youtube_playlist(pid);
                if (tracks.empty())
                {
                    // A genuinely empty playlist also lands here, but with no
                    // backend error. On a real failure (network down, auth
                    // expired) stop hammering the backend playlist-by-playlist
                    // and leave the rest for the next session.
                    BackendError err = take_backend_error();
                    if (err)
                    {
                        ui_notify_backend_error(err);
                        break;
                    }
                }
            }
            state.youtube_playlist_contents_precached = true;
            state.youtube_playlist_contents_precaching = false;
            state.repaint_requested = true;
        });
    }

    if (state.youtube_playlists_loading.load() && state.youtube_playlists.empty())
    {
        ui_empty_state(UiIcon::Playlist, "Loading playlists", "Fetching from your YouTube account");
        return;
    }

    static int selected = -1;
    if (selected >= (int)state.youtube_playlists.size())
        selected = -1;

    const float total_w = ImGui::GetContentRegionAvail().x;
    const float gap = m.space_4;
    float left_w = 260.0f * m.layout_scale;
    if (total_w < 600.0f * m.layout_scale)
        left_w = std::max(180.0f * m.layout_scale, total_w * 0.36f);
    left_w = std::min(left_w, total_w * 0.44f);

    ImGui::BeginChild("YTPlaylistList", ImVec2(left_w, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();

    ui_section_header("Your YouTube Playlists", nullptr);
    for (int i = 0; i < (int)state.youtube_playlists.size(); ++i)
    {
        const YouTubePlaylist &pl = state.youtube_playlists[i];
        const float h = std::max(m.control_h, m.row_art * 0.86f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(i);
        ImGui::InvisibleButton("##yt_playlist_item", ImVec2(w, h),
                               ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool active = i == selected;
        if (clicked)
        {
            selected = i;
            state.youtube_playlist_loaded_id.clear(); // force reload
            state.youtube_playlist_failed_id.clear(); // retry any earlier failure
            state.youtube_playlist_error_msg.clear();
        }
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (active)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_accent_u32(0.13f), m.radius_sm);
        else if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_overlay_u32(0.045f), m.radius_sm);
        ui_draw_icon(dl, UiIcon::Playlist, ImVec2(pos.x + m.space_3 + 9.0f * m.layout_scale, pos.y + h * 0.5f),
                     18.0f * m.layout_scale, active ? ui_accent_text_u32() : ui_text_muted_u32(), 1.5f);
        const float tx = pos.x + m.space_3 + 22.0f * m.layout_scale;
        const float tw = w - (tx - pos.x) - m.space_2;
        std::string name = ui_ellipsize(g_ui_fonts.medium, 19.0f, pl.title.empty() ? "Untitled" : pl.title, tw);
        dl->AddText(g_ui_fonts.medium, 19.0f * m.text_scale,
                    ImVec2(tx, pos.y + (h - 19.0f * m.text_scale * 1.2f) * 0.5f),
                    active ? ui_accent_text_u32() : (hovered ? ui_text_u32() : ui_text_dim_u32()), name.c_str());
        if (pl.count > 0)
        {
            std::string c = std::to_string(pl.count);
            ImVec2 ts = g_ui_fonts.body->CalcTextSizeA(14.0f * m.text_scale, FLT_MAX, 0.0f, c.c_str());
            dl->AddText(g_ui_fonts.body, 14.0f * m.text_scale,
                        ImVec2(pos.x + w - ts.x - m.space_2, pos.y + (h - 14.0f * m.text_scale * 1.2f) * 0.5f),
                        ui_text_muted_u32(), c.c_str());
        }
        ImGui::PopID();
    }

    if (state.youtube_playlists.empty() && !state.youtube_playlists_loading.load())
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_5));
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "No playlists found.");
        ImGui::PopFont();
        if (state.youtube_playlists_loaded && ImGui::SmallButton("Retry"))
        {
            // Force both the initial load and the refresh to run again.
            state.youtube_playlists_loaded = false;
            state.youtube_playlists_refreshed = false;
            state.repaint_requested = true;
        }
    }

    ImGui::EndChild();

    ImGui::SameLine(0.0f, gap);
    ImGui::BeginChild("YTPlaylistTracks", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();

    if (selected < 0 || selected >= (int)state.youtube_playlists.size())
    {
        ui_empty_state(UiIcon::Playlist, "Select a playlist", "Your YouTube playlists live here");
    }
    else
    {
        const YouTubePlaylist &pl = state.youtube_playlists[selected];
        if (state.youtube_playlist_loaded_id != pl.id)
        {
            state.youtube_playlist_loaded_from_cache = false;
            std::string raw;
            if (read_text_file(youtube_playlist_cache_path(pl.id), raw) && !raw.empty())
            {
                auto cached = parse_youtube_playlist_tracks_response(raw);
                if (!cached.empty())
                {
                    std::atomic_store(&state.youtube_playlist_tracks,
                                      std::make_shared<const std::vector<Track>>(std::move(cached)));
                    state.youtube_playlist_loaded_id = pl.id;
                    state.youtube_playlist_loaded_from_cache = true;
                }
            }
        }

        if (state.youtube_playlist_loaded_id != pl.id &&
            state.youtube_playlist_failed_id != pl.id && !state.youtube_playlist_loading.load())
        {
            state.youtube_playlist_loading = true;
            std::string pid = pl.id;
            tasks::spawn("youtube-playlist", [&state, pid]() {
                auto tracks = fetch_youtube_playlist(pid);
                BackendError err = take_backend_error();
                if (!tracks.empty() || !err)
                {
                    std::atomic_store(&state.youtube_playlist_tracks,
                                      std::make_shared<const std::vector<Track>>(std::move(tracks)));
                    state.youtube_playlist_loaded_id = pid;
                    if (state.youtube_playlist_failed_id == pid)
                    {
                        state.youtube_playlist_failed_id.clear();
                        state.youtube_playlist_error_msg.clear();
                    }
                }
                else
                {
                    // Leave any cached tracks on screen; record the failure so
                    // the UI can offer a retry instead of a bare spinner.
                    state.youtube_playlist_failed_id = pid;
                    state.youtube_playlist_error_msg = backend_error_user_message(err);
                    ui_notify_backend_error(err);
                }
                state.youtube_playlist_loading = false;
                state.repaint_requested = true;
            });
        }
        else if (state.youtube_playlist_loaded_id == pl.id && !state.youtube_playlist_loading.load() &&
                 state.youtube_playlist_loaded_from_cache &&
                 state.youtube_playlist_refreshed_id != pl.id)
        {
            state.youtube_playlist_refreshed_id = pl.id;
            state.youtube_playlist_loading = true;
            std::string pid = pl.id;
            tasks::spawn("youtube-playlist", [&state, pid]() {
                auto tracks = fetch_youtube_playlist(pid);
                BackendError err = take_backend_error();
                if (!tracks.empty())
                {
                    std::atomic_store(&state.youtube_playlist_tracks,
                                      std::make_shared<const std::vector<Track>>(std::move(tracks)));
                    state.youtube_playlist_loaded_id = pid;
                    if (state.youtube_playlist_failed_id == pid)
                    {
                        state.youtube_playlist_failed_id.clear();
                        state.youtube_playlist_error_msg.clear();
                    }
                }
                else if (err)
                {
                    // Offline refresh: keep showing the saved tracks.
                    ui_notify_backend_error(err);
                    state.youtube_playlist_refreshed_id.clear(); // retry next visit
                }
                state.youtube_playlist_loading = false;
                state.repaint_requested = true;
            });
        }

        // Only show the spinner when there is nothing on screen yet. If
        // cached tracks are already displayed, the background refresh swaps
        // them in silently — no blocking spinner on a cache hit.
        if (state.youtube_playlist_loading.load() && state.youtube_playlist_loaded_id != pl.id)
        {
            ui_empty_state(UiIcon::Playlist, "Loading tracks", "Fetching playlist");
        }
        else if (state.youtube_playlist_loaded_id != pl.id &&
                 state.youtube_playlist_failed_id == pl.id && !state.youtube_playlist_error_msg.empty())
        {
            // No cache available and the network fetch failed: say so and
            // offer a retry (clicking the playlist in the list also retries).
            ui_empty_state(UiIcon::Playlist, "Couldn't load playlist", state.youtube_playlist_error_msg.c_str());
            const UiMetrics &mm = ui_m();
            std::string label = "Retry";
            ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + (avail.x - ts.x) * 0.5f - mm.space_3,
                                             ImGui::GetCursorScreenPos().y + mm.space_4));
            if (ImGui::Button("Retry"))
            {
                state.youtube_playlist_failed_id.clear();
                state.youtube_playlist_error_msg.clear();
                state.repaint_requested = true;
            }
        }
        else
        {
            auto snap = std::atomic_load(&state.youtube_playlist_tracks);
            const std::vector<Track> *tracks = (state.youtube_playlist_loaded_id == pl.id && snap) ? snap.get() : nullptr;
            int count = tracks ? (int)tracks->size() : 0;

            std::string meta = pl.title + "  ·  " + std::to_string(count) + (count == 1 ? " track" : " tracks");
            ui_section_header(pl.title.empty() ? "Playlist" : pl.title.c_str(), meta.c_str());

            if (!tracks || tracks->empty())
            {
                ui_empty_state(UiIcon::Playlist, "No tracks", "This playlist is empty");
            }
            else
            {
                const double reveal_start = ui_anim_track_list_reveal("yt_playlist", *tracks);
                const float row_pitch = ui_track_row_pitch(44.0f * m.layout_scale);
                ImGuiListClipper clipper;
                clipper.Begin(count, row_pitch);
                while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const Track &track = (*tracks)[i];
                    TrackRowOptions opt;
                    opt.show_duration = true;
                    opt.art_size = 44.0f * m.layout_scale;
                    // This view plays on single click; keep row-internal
                    // (double-click / touch-tap) playback off so a touch tap
                    // can't fire ui_play_track twice.
                    opt.auto_play_double_click = false;
                    opt.reveal_start = reveal_start;
                    opt.reveal_index = i;
                    TrackRowResult row = ui_track_row(state, track, i, opt);
                    if (row.clicked && !row.ctrl_clicked && !row.shift_clicked)
                    {
                        state.current_playlist_source = PlaylistSource::YOUTUBE_PLAYLIST;
                        ui_play_track(state, track, i);
                    }
                }
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_4));
    ImGui::EndChild();
}