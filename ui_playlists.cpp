#include "ui.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ui_notify.h"
#include "ytdlp.h"
#include "writeback.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
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

static std::string xml_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
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
    if (xspf)
    {
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\">\n"
            << "  <title>" << xml_escape(pl.name) << "</title>\n"
            << "  <trackList>\n";
        for (const Track &t : pl.tracks)
        {
            out << "    <track>\n"
                << "      <location>" << xml_escape(t.url) << "</location>\n"
                << "      <title>" << xml_escape(t.title) << "</title>\n"
                << "      <creator>" << xml_escape(t.artist) << "</creator>\n"
                << "      <album>" << xml_escape(t.album) << "</album>\n"
                << "      <duration>" << t.duration_sec * 1000 << "</duration>\n"
                << "    </track>\n";
        }
        out << "  </trackList>\n</playlist>\n";
    }
    else
    {
        out << "#EXTM3U\n";
        for (const Track &t : pl.tracks)
        {
            out << "#EXTINF:" << t.duration_sec << ","
                << t.artist << " - " << t.title << "\n"
                << t.url << "\n";
        }
    }
    out.close();
    ui_notify(UiNotifyType::Success,
              xspf ? "Exported as XSPF" : "Exported as M3U",
              std::to_string(pl.tracks.size()) + " tracks → " + path);
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

static void delete_playlist_at(AppState &state, int index)
{
    if (index < 0 || index >= (int)state.playlists.size())
        return;
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
    smooth_scroll_child("PlaylistList");

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

    draw_scroll_edge_fades();
    ImGui::EndChild();

    const ImVec2 line_start = ImGui::GetCursorScreenPos();
    ImGui::SameLine(0.0f, gap);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(line_start.x - gap * 0.5f, line_start.y),
                                        ImVec2(line_start.x - gap * 0.5f, line_start.y + ImGui::GetContentRegionAvail().y),
                                        ui_hairline_u32(), 1.0f);

    ImGui::BeginChild("PlaylistTracks", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    smooth_scroll_child("PlaylistTracks");

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

    draw_scroll_edge_fades();
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
