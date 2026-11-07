#include "ui.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ui_notify.h"
#include "ytdlp.h"
#include "writeback.h"
#include "paths.h"
#include "persist_util.h"
#include "tasks.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>

// ---- Playlist export (M3U / XSPF) ----
struct ExportPopupState
{
    int index = -1;
    bool xspf = false;
    char path[512] = "";
};
static ExportPopupState s_export;

static std::string expand_tilde(const std::string &p)
{
    if (p.rfind("~/", 0) == 0)
    {
#ifdef P100_WINDOWS
        const char *home = getenv("USERPROFILE");
#else
    #ifdef P100_WINDOWS
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
#endif
        if (home && *home)
            return std::string(home) + p.substr(1);
    }
    return p;
}

static void export_playlist_file(const Playlist &pl, const std::string &path_raw, bool xspf)
{
    const std::string path = expand_tilde(path_raw);
    if (path.empty())
        return;
    std::ofstream out(path);
    if (!out.good())
    {
        ui_notify(UiNotifyType::Error, "Export failed",
                  "Could not write " + path);
        return;
    }
    out << (xspf ? playlist_xspf(pl) : playlist_m3u(pl));
    out.close();
    ui_notify(UiNotifyType::Success,
              xspf ? "Exported as XSPF" : "Exported as M3U",
              std::to_string(pl.tracks.size()) + " tracks → " + path);
}

static char s_spotify_url[512] = "";
static bool s_spotify_import_complete = false;

static void begin_spotify_import(AppState &state)
{
    if (state.spotify_import_running.load())
        return;
    const std::string link = s_spotify_url;
    if (link.empty())
        return;
    state.spotify_import_running = true;
    std::atomic_store(&state.spotify_import_result,
                      std::shared_ptr<const SpotifyImportResult>());
    AppState *sp = &state;
    tasks::spawn([sp, link]() {
        auto result = fetch_spotify_import(link);
        std::atomic_store(&sp->spotify_import_result,
                          std::make_shared<const SpotifyImportResult>(std::move(result)));
        sp->spotify_import_running = false;
        sp->repaint_requested = true;
    });
}

static void finish_spotify_import(AppState &state)
{
    auto result = std::atomic_load(&state.spotify_import_result);
    if (!result)
        return;
    if (!result->error.empty())
        return;
    if (result->tracks.empty())
        return;
    std::string name = result->name.empty() ? "Spotify import" : result->name;
    auto names = get_playlist_names();
    std::string unique = name;
    int suffix = 2;
    while (std::find(names.begin(), names.end(), unique) != names.end())
        unique = name + " (" + std::to_string(suffix++) + ")";
    state.playlists.push_back(Playlist{unique, result->tracks});
    save_playlist(unique, result->tracks);
    wb_sync_playlist(unique, result->tracks);
    state.selected_playlist_index = (int)state.playlists.size() - 1;
    state.current_playlist_source = PlaylistSource::PLAYLIST;
    ui_notify(UiNotifyType::Success, "Spotify playlist imported",
              std::to_string(result->tracks.size()) + " tracks added as \"" + unique + "\".",
              6.0f);
    std::atomic_store(&state.spotify_import_result,
                      std::shared_ptr<const SpotifyImportResult>());
}

struct SpotifyProgress
{
    int done = 0;
    int total = 0;
    std::string phase;
    std::string name;
};

static SpotifyProgress read_spotify_progress()
{
    SpotifyProgress out;
    std::ifstream in(p100_config_path("spotify_import_progress.json"));
    if (!in)
        return out;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    out.done = persist::extract_int_field(json, "done");
    out.total = persist::extract_int_field(json, "total");
    out.phase = persist::extract_str_field(json, "phase");
    out.name = persist::extract_str_field(json, "name");
    return out;
}

static void refresh_spotify_library(AppState &state)
{
    if (state.spotify_library_running.exchange(true))
        return;
    state.spotify_library_loaded = false;
    AppState *sp = &state;
    tasks::spawn([sp]() {
        std::string error;
        auto list = fetch_spotify_library(error);
        std::atomic_store(&sp->spotify_playlists,
                          std::make_shared<const std::vector<SpotifyPlaylistSummary>>(
                              std::move(list)));
        std::atomic_store(&sp->spotify_library_error,
                          std::make_shared<const std::string>(std::move(error)));
        sp->spotify_library_loaded = true;
        sp->spotify_library_running = false;
        sp->repaint_requested = true;
    });
}

static void import_spotify_source(AppState &state, const std::string &source)
{
    if (state.spotify_import_running.load() || source.empty())
        return;
    std::snprintf(s_spotify_url, sizeof(s_spotify_url), "%s", source.c_str());
    begin_spotify_import(state);
}

static void spotify_library_row(AppState &state, const SpotifyPlaylistSummary &entry,
                                float width)
{
    const UiMetrics &m = ui_m();
    ImGui::PushID(entry.id.c_str());
    const float row_h = 54.0f * m.layout_scale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##spotify_row", ImVec2(width, row_h));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
    {
        const std::string source =
            entry.kind == "liked" ? std::string("liked")
                                  : "https://open.spotify.com/playlist/" + entry.id;
        import_spotify_source(state, source);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hovered)
        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + row_h),
                          ui_overlay_u32(0.055f), m.radius_md);
    const float art = 38.0f * m.layout_scale;
    const ImVec2 a0(pos.x + m.space_1, pos.y + (row_h - art) * 0.5f);
    ui_draw_cover(dl, entry.art, a0, ImVec2(a0.x + art, a0.y + art), m.radius_sm,
                  1.0f, entry.kind == "liked" ? UiIcon::Heart : UiIcon::Playlist,
                  "spotify_library", false, true);
    const float tx = a0.x + art + m.space_3;
    ui_push_medium(17.0f);
    dl->AddText(g_ui_fonts.medium, 17.0f * m.text_scale,
                ImVec2(tx, pos.y + 8.0f * m.layout_scale), ui_text_u32(),
                ui_ellipsize(g_ui_fonts.medium, 17.0f, entry.name,
                             std::max(80.0f, width - art - 110.0f * m.layout_scale))
                    .c_str());
    ImGui::PopFont();
    std::string meta = entry.kind == "liked"
                           ? std::to_string(entry.count) + " liked songs"
                           : (entry.owner.empty() ? "Spotify playlist" : entry.owner);
    ui_push_small();
    dl->AddText(g_ui_fonts.body, 14.0f * m.text_scale,
                ImVec2(tx, pos.y + 30.0f * m.layout_scale), ui_text_muted_u32(),
                ui_ellipsize(g_ui_fonts.body, 14.0f, meta,
                             std::max(80.0f, width - art - 110.0f * m.layout_scale))
                    .c_str());
    ImGui::PopFont();
    dl->AddText(g_ui_fonts.medium, 14.0f * m.text_scale,
                ImVec2(pos.x + width - 52.0f * m.layout_scale,
                       pos.y + (row_h - 14.0f * m.text_scale) * 0.5f),
                ui_accent_text_u32(), "Import");
    ImGui::PopID();
}

static void open_export(AppState &state, int index, bool xspf)
{
    if (index < 0 || index >= (int)state.playlists.size())
        return;
    s_export.index = index;
    s_export.xspf = xspf;
#ifdef P100_WINDOWS
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
    const std::string def = std::string(home ? home : ".") + "/Downloads/" +
                            sanitize_filename(state.playlists[index].name) +
                            (xspf ? ".xspf" : ".m3u");
    std::snprintf(s_export.path, sizeof(s_export.path), "%s", def.c_str());
    ImGui::OpenPopup("Export playlist###ExportPlaylistPopup");
}

static Playlist g_deleted_playlist;
static int g_deleted_playlist_index = -1;
static AppState *g_deleted_playlist_state = nullptr;

static void undo_delete_playlist()
{
    if (!g_deleted_playlist_state || g_deleted_playlist.name.empty())
        return;
    AppState &s = *g_deleted_playlist_state;
    const int at = std::max(0, std::min((int)s.playlists.size(), g_deleted_playlist_index));
    s.playlists.insert(s.playlists.begin() + at, g_deleted_playlist);
    save_playlist(g_deleted_playlist.name, g_deleted_playlist.tracks);
    s.selected_playlist_index = at;
    s.repaint_requested = true;
    g_deleted_playlist = Playlist{};
    g_deleted_playlist_index = -1;
    ui_notify(UiNotifyType::Info, "Playlist restored", "The playlist is back.", 4.0f);
}

static void delete_playlist_at(AppState &state, int index)
{
    if (index < 0 || index >= (int)state.playlists.size())
        return;
    g_deleted_playlist = state.playlists[index];
    g_deleted_playlist_index = index;
    g_deleted_playlist_state = &state;
    delete_playlist(state.playlists[index].name);
    wb_delete_playlist(state.playlists[index].name);
    state.playlists.erase(state.playlists.begin() + index);
    if (state.selected_playlist_index >= index)
    {
        if (state.selected_playlist_index == index)
            state.selected_playlist_index = -1;
        else
            state.selected_playlist_index--;
    }
    if (state.selected_playlist_index < 0)
        state.current_playlist_source = PlaylistSource::HOME;
    ui_notify(UiNotifyType::Info, "Playlist deleted",
              g_deleted_playlist.name + " was removed.", 6.0f, "Undo",
              &undo_delete_playlist);
}

void draw_playlists_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    static int rename_index = -1;
    static char rename_buf[128] = "";
    static char new_name[128] = "";
    bool open_rename = false;

    const float total_w = ImGui::GetContentRegionAvail().x;
    const float gap = m.space_4;
    float left_w = 245.0f * m.layout_scale;
    if (total_w < 600.0f * m.layout_scale)
        left_w = std::max(180.0f * m.layout_scale, total_w * 0.36f);
    left_w = std::min(left_w, total_w * 0.44f);

    ImGui::BeginChild("PlaylistList", ImVec2(left_w, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();

    ui_section_header("Your Library", nullptr);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(m.space_2, m.space_2));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ui_c().frame_bg);
    ImGui::PushStyleColor(ImGuiCol_Border, ui_c().border);
    ImGui::SetNextItemWidth(left_w - m.control_h - m.space_2);
    if (ImGui::InputTextWithHint("##newplaylist", "New playlist", new_name, sizeof(new_name),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
    {
        std::string name = new_name;
        auto names = get_playlist_names();
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
        {
            Playlist pl;
            pl.name = name;
            state.playlists.push_back(pl);
            save_playlist(name, {});
            wb_sync_playlist(name, {});
            state.selected_playlist_index = (int)state.playlists.size() - 1;
            state.current_playlist_source = PlaylistSource::PLAYLIST;
            new_name[0] = '\0';
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0.0f, m.space_2);
    if (ui_primary_button("+", ImVec2(m.control_h, m.control_h)))
    {
        std::string name = new_name;
        auto names = get_playlist_names();
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
        {
            Playlist pl;
            pl.name = name;
            state.playlists.push_back(pl);
            save_playlist(name, {});
            wb_sync_playlist(name, {});
            state.selected_playlist_index = (int)state.playlists.size() - 1;
            state.current_playlist_source = PlaylistSource::PLAYLIST;
            new_name[0] = '\0';
        }
    }
    ImGui::PopStyleVar();

    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    const bool spotify_connected =
        std::filesystem::exists(p100_config_path("spotify.json"));
    if (ui_secondary_button(spotify_connected ? "Import Spotify playlist"
                                              : "Import from Spotify",
                            ImVec2(196.0f * m.layout_scale, m.control_h)))
    {
        ImGui::OpenPopup("##spotify_import");
    }
    if (state.spotify_open_import_requested.exchange(false))
        ImGui::OpenPopup("##spotify_import");
    if (ImGui::BeginPopup("##spotify_import"))
    {
        if (s_spotify_import_complete)
        {
            s_spotify_import_complete = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextUnformatted("Import from Spotify");
        ui_push_small();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 430.0f * m.layout_scale);
        ImGui::TextColored(ui_c().text_muted,
                           "Pick a playlist from your Spotify library, or paste a share link. Liked Songs is included once Spotify is connected.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, m.space_2));

        if (spotify_connected)
            ImGui::TextColored(ui_c().accent_text, "Spotify connected");
        else
            ImGui::TextColored(ui_c().text_muted, "Spotify not connected");
        if (spotify_connected)
        {
            ImGui::SameLine();
            if (!state.spotify_library_running.load() &&
                ui_ghost_button("Refresh library", ImVec2(140.0f * m.layout_scale, m.control_h)))
            {
                state.spotify_library_loaded = false;
                refresh_spotify_library(state);
            }
        }

        if (spotify_connected)
        {
            if (!state.spotify_library_loaded.load() && !state.spotify_library_running.load())
                refresh_spotify_library(state);

            if (state.spotify_library_running.load())
            {
                ImGui::TextColored(ui_c().text_muted, "Loading your Spotify playlists...");
                ui_anim_request();
            }
            else
            {
                auto error = std::atomic_load(&state.spotify_library_error);
                auto library = std::atomic_load(&state.spotify_playlists);
                if (error && !error->empty())
                {
                    ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.42f, 1.0f), "%s",
                                       error->c_str());
                    if (ui_ghost_button("Retry", ImVec2(100.0f * m.layout_scale, m.control_h)))
                        refresh_spotify_library(state);
                }
                else if (library && !library->empty())
                {
                    const float list_w = 430.0f * m.layout_scale;
                    const float list_h = std::min(240.0f * m.layout_scale,
                                                  ImGui::GetIO().DisplaySize.y * 0.36f);
                    ImGui::BeginChild("##spotify_library", ImVec2(list_w, list_h),
                                      ImGuiChildFlags_None,
                                      ImGuiWindowFlags_NoBackground |
                                          ImGuiWindowFlags_NoScrollWithMouse);
                    smooth_scroll_child();
                    for (const auto &entry : *library)
                        spotify_library_row(state, entry, list_w - m.space_2);
                    ImGui::EndChild();
                }
                else
                {
                    ImGui::TextColored(ui_c().text_muted,
                                       "No Spotify playlists were found on this account.");
                }
            }
        }

        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        ImGui::SetNextItemWidth(430.0f * m.layout_scale);
        ImGui::InputTextWithHint("##spotify_url", "Paste a playlist link or ID",
                                 s_spotify_url, sizeof(s_spotify_url));
        if (state.spotify_import_running.load())
        {
            const SpotifyProgress progress = read_spotify_progress();
            if (progress.total > 0)
            {
                const float frac = std::min(1.0f, (float)progress.done /
                                                       (float)progress.total);
                const std::string label =
                    (progress.phase == "done" ? "Matched " : "Matching ") +
                    std::to_string(progress.done) + " / " +
                    std::to_string(progress.total);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui_c().accent);
                ImGui::ProgressBar(frac, ImVec2(430.0f * m.layout_scale,
                                                18.0f * m.layout_scale),
                                   label.c_str());
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::TextColored(ui_c().text_muted, "Reading the Spotify playlist...");
            }
            ui_anim_request();
        }
        auto result = std::atomic_load(&state.spotify_import_result);
        if (result && !result->error.empty())
            ImGui::TextColored(ImVec4(0.95f, 0.42f, 0.42f, 1.0f), "%s", result->error.c_str());
        if (!state.spotify_import_running.load() &&
            ui_primary_button("Import link", ImVec2(140.0f * m.layout_scale, m.control_h)))
            begin_spotify_import(state);
        ImGui::SameLine(0.0f, m.space_2);
        if (!state.spotify_login_running.load() &&
            ui_ghost_button(spotify_connected ? "Reconnect Spotify" : "Sign in to Spotify",
                            ImVec2(176.0f * m.layout_scale, m.control_h)))
        {
            const std::string script = p100_share_dir() + "/scripts/crossfade-login-helper.py";
            state.spotify_login_running = true;
            AppState *sp = &state;
            tasks::spawn([sp, script]() {
                const std::string cmd = "python3 \"" + script + "\" --spotify";
                const int rc = std::system(cmd.c_str());
                sp->spotify_login_running = false;
                if (rc == 0)
                {
                    sp->spotify_library_loaded = false;
                    sp->spotify_open_import_requested = true;
                    ui_notify(UiNotifyType::Success, "Spotify connected",
                              "Your playlists are ready to import.", 5.0f);
                }
                else
                {
                    ui_notify(UiNotifyType::Error, "Spotify sign-in failed",
                              "The Spotify window was closed before it finished.", 6.0f);
                }
                sp->repaint_requested = true;
            });
        }
        if (state.spotify_login_running.load())
            ImGui::TextColored(ui_c().text_muted, "Waiting for Spotify sign-in...");
        ImGui::EndPopup();
    }
    finish_spotify_import(state);

    ImGui::Dummy(ImVec2(0.0f, m.space_3));

    int delete_index = -1;
    for (int i = 0; i < (int)state.playlists.size(); ++i)
    {
        const Playlist &pl = state.playlists[i];
        const float h = std::max(m.control_h, m.row_art * 0.86f);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(i);
        ImGui::InvisibleButton("##playlist_item", ImVec2(w, h),
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool active = i == state.selected_playlist_index;
        if (clicked)
        {
            state.selected_playlist_index = i;
            state.current_playlist_source = PlaylistSource::PLAYLIST;
            state.local_search_active = false;
            state.local_search_results.clear();
            state.selected_track_ids.clear();
            state.selection_context.clear();
            state.selection_anchor.clear();
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("##playlist_ctx");

        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (active)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_accent_u32(0.13f), m.radius_sm);
        else if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ui_overlay_u32(0.045f), m.radius_sm);
        if (active)
            dl->AddRectFilled(ImVec2(pos.x, pos.y + m.space_1), ImVec2(pos.x + 3.0f * m.layout_scale, pos.y + h - m.space_1),
                              ui_accent_u32(1.0f), 1.5f);
        ui_draw_icon(dl, UiIcon::Playlist, ImVec2(pos.x + m.space_3 + 9.0f * m.layout_scale, pos.y + h * 0.5f),
                     18.0f * m.layout_scale, active ? ui_accent_text_u32() : ui_text_muted_u32(), 1.5f);
        const float tx = pos.x + m.space_3 + 22.0f * m.layout_scale;
        const float tw = w - (tx - pos.x) - m.space_2;
        std::string name = ui_ellipsize(g_ui_fonts.medium, 19.0f, pl.name, tw);
        dl->AddText(g_ui_fonts.medium, 19.0f * m.text_scale,
                    ImVec2(tx, pos.y + (h - 19.0f * m.text_scale * 1.2f) * 0.5f),
                    active ? ui_accent_text_u32() : (hovered ? ui_text_u32() : ui_text_dim_u32()), name.c_str());

        if (ImGui::BeginPopup("##playlist_ctx"))
        {
            if (ImGui::MenuItem("Rename"))
            {
                rename_index = i;
                std::snprintf(rename_buf, sizeof(rename_buf), "%s", pl.name.c_str());
                open_rename = true;
            }
            if (ImGui::MenuItem("Export as M3U..."))
                open_export(state, i, false);
            if (ImGui::MenuItem("Export as XSPF..."))
                open_export(state, i, true);
            if (ImGui::MenuItem("Delete"))
                delete_index = i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    if (state.playlists.empty())
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_5));
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "Create a playlist above.");
        ImGui::PopFont();
    }

    ImGui::EndChild();

    const ImVec2 line_start = ImGui::GetCursorScreenPos();
    ImGui::SameLine(0.0f, gap);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(line_start.x - gap * 0.5f, line_start.y),
                                        ImVec2(line_start.x - gap * 0.5f, line_start.y + ImGui::GetContentRegionAvail().y),
                                        ui_hairline_u32(), 1.0f);

    ImGui::BeginChild("PlaylistTracks", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child();

    if (state.selected_playlist_index < 0 || state.selected_playlist_index >= (int)state.playlists.size())
    {
        ui_empty_state(UiIcon::Playlist, "Select a playlist", "Your saved playlists live here");
    }
    else
    {
        Playlist &pl = state.playlists[state.selected_playlist_index];
        const std::vector<Track> &tracks = state.local_search_active ? state.local_search_results : pl.tracks;
        const std::string selection_context = "playlist:" + pl.name;
        if (state.selection_context != selection_context)
        {
            state.selected_track_ids.clear();
            state.selection_anchor.clear();
            state.selection_context = selection_context;
        }
        std::string meta = std::to_string(pl.tracks.size()) + (pl.tracks.size() == 1 ? " track" : " tracks");
        // Summing every track's duration each frame is O(N); memoize on the
        // cheap things that change when the playlist is edited.
        static std::string s_sum_name;
        static size_t s_sum_size = 0;
        static std::string s_sum_front, s_sum_back;
        static int s_sum_seconds = -1;
        if (pl.name != s_sum_name || pl.tracks.size() != s_sum_size ||
            (!pl.tracks.empty() && (pl.tracks.front().id != s_sum_front ||
                                    pl.tracks.back().id != s_sum_back)))
        {
            s_sum_name = pl.name;
            s_sum_size = pl.tracks.size();
            s_sum_front = pl.tracks.empty() ? "" : pl.tracks.front().id;
            s_sum_back = pl.tracks.empty() ? "" : pl.tracks.back().id;
            s_sum_seconds = ui_tracks_duration(pl.tracks);
        }
        const std::string total = ui_format_duration(s_sum_seconds);
        if (!total.empty())
            meta += "  \xC2\xB7  " + total;
        ui_section_header(pl.name.c_str(), meta.c_str());
        if (!tracks.empty())
        {
            if (ui_primary_button("Play all", ImVec2(108.0f * m.layout_scale, m.button_h)))
                ui_play_track(state, tracks.front(), 0);
            ImGui::SameLine(0.0f, m.space_2);
            if (ui_ghost_button("Rename", ImVec2(88.0f * m.layout_scale, m.button_h)))
            {
                rename_index = state.selected_playlist_index;
                std::snprintf(rename_buf, sizeof(rename_buf), "%s", pl.name.c_str());
                open_rename = true;
            }
            ImGui::Dummy(ImVec2(0.0f, m.space_2));
        }

        if (!tracks.empty())
            ui_selection_bar(state, tracks, selection_context.c_str(), true, false,
                             state.selected_playlist_index);

        if (tracks.empty())
        {
            if (state.local_search_active)
                ui_empty_state(UiIcon::Search, "No matches", "Try a different search");
            else
                ui_empty_state(UiIcon::Playlist, "Playlist is empty", "Add tracks from any song's context menu");
        }
        else
        {
            const std::string reveal_key = std::string("playlist:") + pl.name +
                                           (state.local_search_active ? ":search" : "");
            const double reveal_start = ui_anim_track_list_reveal(reveal_key.c_str(), tracks);
            const int count = (int)tracks.size();
            const float row_pitch = ui_track_row_pitch(44.0f * m.layout_scale);
            ImGuiListClipper clipper;
            clipper.Begin(count, row_pitch);
            while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Track &track = tracks[i];
                TrackRowOptions opt;
                opt.show_duration = true;
                opt.art_size = 44.0f * m.layout_scale;
                opt.reveal_start = reveal_start;
                opt.reveal_index = i;
                opt.selected = state.selected_track_ids.count(track.id) > 0;
                opt.draggable = !state.local_search_active;
                opt.drag_type = "P100_PLAYLIST";
                TrackRowResult row = ui_track_row(state, track, i, opt);

                if (row.drop_received && !state.local_search_active &&
                    row.drag_from >= 0 && row.drag_from < (int)pl.tracks.size())
                {
                    Track moved = pl.tracks[row.drag_from];
                    pl.tracks.erase(pl.tracks.begin() + row.drag_from);
                    int to = std::max(0, std::min((int)pl.tracks.size(), row.drop_to));
                    pl.tracks.insert(pl.tracks.begin() + to, std::move(moved));
                    save_playlist(pl.name, pl.tracks);
                    wb_sync_playlist(pl.name, pl.tracks);
                }

                if (row.clicked)
                {
                    if (row.shift_clicked && !state.selection_anchor.empty())
                    {
                        int anchor_idx = -1;
                        for (int j = 0; j < count; ++j)
                        {
                            if (tracks[j].id == state.selection_anchor)
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
                                state.selected_track_ids.insert(tracks[j].id);
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
            ImGui::Dummy(ImVec2(0.0f, m.space_4));
        }
    }

    ImGui::EndChild();

    if (delete_index >= 0)
        delete_playlist_at(state, delete_index);

    if (open_rename)
        ImGui::OpenPopup("Rename playlist###RenamePlaylistPopup");

    if (ImGui::BeginPopupModal("Rename playlist###RenamePlaylistPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushItemWidth(260.0f * m.layout_scale);
        const bool submitted = ImGui::InputText("##renameinput", rename_buf, sizeof(rename_buf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        bool close = submitted;
        if (!submitted)
        {
            if (ui_primary_button("Rename", ImVec2(100.0f * m.layout_scale, m.button_h)))
                close = true;
            ImGui::SameLine();
            if (ui_ghost_button("Cancel", ImVec2(90.0f * m.layout_scale, m.button_h)))
            {
                rename_index = -1;
                close = true;
            }
        }
        if (close && rename_index >= 0 && rename_index < (int)state.playlists.size())
        {
            std::string name(rename_buf);
            const std::string old = state.playlists[rename_index].name;
            auto names = get_playlist_names();
            if (!name.empty() && name != old && std::find(names.begin(), names.end(), name) == names.end())
            {
                rename_playlist(old, name);
                wb_rename_playlist(old, name);
                state.playlists[rename_index].name = name;
            }
            rename_index = -1;
            ImGui::CloseCurrentPopup();
        }
        else if (close)
        {
            rename_index = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Export playlist###ExportPlaylistPopup", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "%s",
                           (s_export.xspf ? "XSPF playlist file" : "M3U playlist file"));
        ImGui::PopFont();
        ImGui::PushItemWidth(360.0f * m.layout_scale);
        const bool submitted = ImGui::InputText("##exportpath", s_export.path,
                                                sizeof(s_export.path),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        bool do_export = submitted;
        bool close = submitted;
        if (!submitted)
        {
            if (ui_primary_button("Export", ImVec2(100.0f * m.layout_scale, m.button_h)))
            {
                do_export = true;
                close = true;
            }
            ImGui::SameLine();
            if (ui_ghost_button("Cancel", ImVec2(90.0f * m.layout_scale, m.button_h)))
                close = true;
        }
        if (close)
        {
            if (do_export && s_export.index >= 0 &&
                s_export.index < (int)state.playlists.size())
            {
                export_playlist_file(state.playlists[s_export.index], s_export.path,
                                     s_export.xspf);
            }
            s_export.index = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
