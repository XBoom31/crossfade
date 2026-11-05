#include "ui.h"
#include "tasks.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ytdlp.h"
#include "paths.h"
#include "persist_util.h"
#include "ui_notify.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
std::string saved_podcasts_path()
{
    return p100_config_path("saved_podcasts.json");
}

void load_saved_podcasts(AppState &state)
{
    if (state.saved_podcasts_loaded)
        return;
    state.saved_podcasts_loaded = true;
    std::ifstream in(saved_podcasts_path());
    if (!in)
        return;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int depth = 0;
    size_t start = std::string::npos;
    for (size_t i = 0; i < json.size(); ++i)
    {
        if (json[i] == '{')
        {
            if (depth++ == 0)
                start = i;
        }
        else if (json[i] == '}' && depth > 0)
        {
            if (--depth == 0 && start != std::string::npos)
            {
                const std::string obj = json.substr(start, i - start + 1);
                PodcastShow show;
                show.browse_id = persist::extract_str_field(obj, "browse_id");
                show.title = persist::extract_str_field(obj, "title");
                show.author = persist::extract_str_field(obj, "author");
                show.thumbnail_url = persist::extract_str_field(obj, "thumbnail_url");
                show.description = persist::extract_str_field(obj, "description");
                show.saved = true;
                if (!show.browse_id.empty() && !show.title.empty())
                    state.saved_podcasts.push_back(std::move(show));
            }
        }
    }
}

void save_saved_podcasts(const AppState &state)
{
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(saved_podcasts_path()).parent_path(), ec);
    std::ofstream out(saved_podcasts_path(), std::ios::trunc);
    if (!out)
        return;
    out << "[";
    for (size_t i = 0; i < state.saved_podcasts.size(); ++i)
    {
        const auto &s = state.saved_podcasts[i];
        if (i)
            out << ",";
        out << "{\"browse_id\":\"" << persist::json_escape(s.browse_id)
            << "\",\"title\":\"" << persist::json_escape(s.title)
            << "\",\"author\":\"" << persist::json_escape(s.author)
            << "\",\"thumbnail_url\":\"" << persist::json_escape(s.thumbnail_url)
            << "\",\"description\":\"" << persist::json_escape(s.description)
            << "\"}";
    }
    out << "]";
}

bool is_show_saved(const AppState &state, const std::string &browse_id)
{
    return std::any_of(state.saved_podcasts.begin(), state.saved_podcasts.end(),
                       [&](const PodcastShow &s) { return s.browse_id == browse_id; });
}

void toggle_saved_show(AppState &state, const PodcastShow &show)
{
    if (show.browse_id.empty())
        return;
    auto it = std::find_if(state.saved_podcasts.begin(), state.saved_podcasts.end(),
                           [&](const PodcastShow &s) { return s.browse_id == show.browse_id; });
    if (it != state.saved_podcasts.end())
    {
        state.saved_podcasts.erase(it);
        ui_notify(UiNotifyType::Info, "Removed from your shows", show.title, 4.0f);
    }
    else
    {
        PodcastShow saved = show;
        saved.saved = true;
        state.saved_podcasts.push_back(std::move(saved));
        ui_notify(UiNotifyType::Success, "Saved to your shows", show.title, 4.0f);
    }
    save_saved_podcasts(state);
    state.repaint_requested = true;
}

std::vector<PodcastShow> all_shows(AppState &state)
{
    load_saved_podcasts(state);
    std::vector<PodcastShow> out = state.saved_podcasts;
    auto library = std::atomic_load(&state.podcast_shows);
    if (library)
    {
        for (const auto &s : *library)
        {
            if (s.title == "Episodes for Later")
                continue;
            if (is_show_saved(state, s.browse_id))
                continue;
            out.push_back(s);
        }
    }
    return out;
}

std::atomic<bool> g_podcast_downloading{false};
std::atomic<int> g_podcast_download_done{0};
std::atomic<int> g_podcast_download_total{0};
std::string g_podcast_download_show;

void download_episode_async(const std::string &id)
{
    if (id.empty() || !find_cached_audio_path(id).empty())
        return;
    tasks::spawn([id]() {
        const std::string path = get_cached_audio_path(id);
        g_downloads_dirty = true;
        if (path.empty())
            ui_notify(UiNotifyType::Error, "Episode download failed",
                      "The audio could not be fetched.", 6.0f);
        else
            ui_notify(UiNotifyType::Success, "Episode downloaded",
                      "Available offline in Downloads.", 3.5f);
    });
}

void download_all_episodes(AppState &state)
{
    if (g_podcast_downloading.exchange(true))
        return;
    std::vector<std::string> ids;
    ids.reserve(state.podcast_tracks.size());
    for (const auto &t : state.podcast_tracks)
        if (!t.id.empty())
            ids.push_back(t.id);
    g_podcast_download_show = state.podcast_selected.title;
    g_podcast_download_done = 0;
    g_podcast_download_total = (int)ids.size();
    AppState *sp = &state;
    tasks::spawn([sp, ids]() {
        int done = 0;
        for (const auto &id : ids)
        {
            if (find_cached_audio_path(id).empty())
                get_cached_audio_path(id);
            g_downloads_dirty = true;
            g_podcast_download_done = ++done;
            sp->repaint_requested = true;
        }
        g_podcast_downloading = false;
        ui_notify(UiNotifyType::Success, "Podcast downloaded",
                  std::to_string(done) + " episodes are available offline.", 4.5f);
        sp->repaint_requested = true;
    });
}

Track episode_track(const PodcastEpisode &e, const PodcastShow &show)
{
    Track t;
    t.id = e.video_id;
    t.title = e.title;
    t.artist = show.author.empty() ? show.title : show.author;
    t.thumbnail_url = e.thumbnail_url.empty() ? show.thumbnail_url : e.thumbnail_url;
    t.album = show.title;
    t.duration_sec = e.duration_sec;
    t.url = "https://www.youtube.com/watch?v=" + e.video_id;
    return t;
}

std::vector<Track> tracks_for(const PodcastShow &show,
                              const std::vector<PodcastEpisode> &episodes)
{
    std::vector<Track> out;
    out.reserve(episodes.size());
    for (const auto &e : episodes)
        if (!e.video_id.empty())
            out.push_back(episode_track(e, show));
    return out;
}

void load_show(AppState &state, const PodcastShow &show)
{
    if (show.browse_id.empty())
        return;
    if (state.search_buffer[0] != '\0')
        ui_reset_search(state);
    state.podcast_selected = show;
    state.podcast_details_loading = true;
    state.podcast_episodes.reset();
    state.podcast_tracks.clear();
    state.repaint_requested = true;

    AppState *sp = &state;
    const std::string id = show.browse_id;
    tasks::spawn([sp, id]() {
        PodcastDetails details = fetch_podcast_details(id);
        if (sp->podcast_selected.browse_id != id)
            return;
        if (details.show.title.empty())
            details.show = sp->podcast_selected;
        auto episodes = std::make_shared<const std::vector<PodcastEpisode>>(details.episodes);
        auto tracks = tracks_for(details.show, details.episodes);
        std::atomic_store(&sp->podcast_episodes, episodes);
        sp->podcast_selected = details.show;
        sp->podcast_tracks = std::move(tracks);
        sp->podcast_details_loading = false;
        sp->repaint_requested = true;
    });
}

void play_list(AppState &state, const std::vector<Track> &tracks, int index)
{
    if (index < 0 || index >= (int)tracks.size())
        return;
    state.podcast_tracks = tracks;
    ui_play_track(state, tracks[index], index);
}

void draw_browse_hero()
{
    const UiMetrics &m = ui_m();
    const float h = std::max(112.0f * m.layout_scale, m.control_h * 2.3f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + ImGui::GetContentRegionAvail().x, p0.y + h);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ui_overlay_u32(0.035f), m.radius_lg);
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + (p1.x - p0.x) * 0.55f, p1.y),
                                ui_accent_u32(0.10f), ui_accent_u32(0.0f),
                                ui_accent_u32(0.0f), ui_accent_u32(0.0f));
    dl->AddRect(p0, p1, ui_with_alpha(ui_border_u32(), 0.8f), m.radius_lg, 0, 1.0f);
    const float pad = m.space_4;
    ui_draw_icon(dl, UiIcon::Podcast,
                 ImVec2(p0.x + pad + 20.0f * m.layout_scale, p0.y + h * 0.5f),
                 42.0f * m.layout_scale, ui_accent_text_u32(), 1.7f);
    const float tx = p0.x + pad + 52.0f * m.layout_scale;
    const float fs = 28.0f * m.text_scale;
    dl->AddText(g_ui_fonts.semibold, fs, ImVec2(tx, p0.y + pad),
                ui_text_u32(), "Podcasts");
    ui_push_small();
    ImGui::SetCursorScreenPos(ImVec2(tx, p0.y + pad + fs + 2.0f * m.layout_scale));
    ImGui::PushTextWrapPos(p1.x - pad);
    ImGui::TextColored(ui_c().text_muted,
                       "Shows, episodes and saved listens from your YouTube Music library.");
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, h));
}

bool podcast_grid(AppState &state, const std::vector<PodcastShow> &shows, const char *scope)
{
    if (shows.empty())
        return false;
    const UiMetrics &m = ui_m();
    const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float min_card = 190.0f * m.layout_scale;
    int cols = std::max(1, (int)((avail + m.space_4) / (min_card + m.space_4)));
    cols = std::min(cols, 5);
    const float gap = m.space_4;
    const float card_w = (avail - gap * (float)(cols - 1)) / (float)cols;
    const float title_h = 19.0f * m.text_scale * 1.25f;
    const float meta_h = 16.0f * m.text_scale * 1.25f;
    const float card_h = card_w + m.space_3 + title_h + m.space_1 + meta_h + m.space_2;
    const float row_pitch = card_h + m.card_gap;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    bool any = false;
    for (int i = 0; i < (int)shows.size(); ++i)
    {
        const auto &s = shows[i];
        const int row = i / cols;
        const int col = i % cols;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + (float)col * (card_w + gap),
                                         origin.y + (float)row * row_pitch));
        ImGui::PushID((scope + s.browse_id).c_str());
        MediaCardResult r = ui_media_card("##podcast_show", s.title,
                                          s.author.empty() ? "Podcast" : s.author,
                                          s.thumbnail_url, card_w, card_w, false);
        ImGui::PopID();
        if (r.clicked || r.double_clicked)
        {
            load_show(state, s);
            any = true;
        }
    }
    const int rows = ((int)shows.size() + cols - 1) / cols;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (float)rows * row_pitch));
    ImGui::Dummy(ImVec2(1.0f, m.space_1));
    return any;
}
} // namespace

void draw_podcasts_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    const float width = std::min(1080.0f * m.layout_scale, ImGui::GetContentRegionAvail().x);
    const bool searching = state.search_buffer[0] != '\0';

    if (searching)
    {
        auto results = std::atomic_load(&state.podcast_search_results);
        const std::string search_meta =
            state.podcast_search_loading
                ? "Searching..."
                : (results ? std::to_string(results->size()) + " shows" : "0 shows");
        ui_section_header("Podcast Results", search_meta.c_str());
        if (state.podcast_search_loading)
        {
            ImGui::Dummy(ImVec2(0.0f, m.space_6));
            ImGui::TextColored(ui_c().text_muted, "Searching YouTube Music podcasts...");
            ui_anim_request();
            return;
        }
        if (!results || results->empty())
        {
            ui_empty_state(UiIcon::Podcast, "No podcasts found",
                           "Try a different show name or host.");
            return;
        }
        podcast_grid(state, *results, "search_");
        return;
    }

    if (!state.podcast_selected.browse_id.empty())
    {
        const PodcastShow &show = state.podcast_selected;
        const float cover = std::min(168.0f * m.layout_scale, width * 0.28f);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(cover, cover));
        ui_draw_cover(ImGui::GetWindowDrawList(), show.thumbnail_url, start,
                      ImVec2(start.x + cover, start.y + cover), m.radius_lg, 1.0f,
                      UiIcon::Podcast, "podcast_hero");

        ImGui::SameLine(0.0f, m.space_5);
        ImGui::BeginGroup();
        ui_push_semibold(32.0f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(120.0f, width - cover - m.space_5));
        ImGui::TextUnformatted(show.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        if (!show.author.empty())
            ImGui::TextColored(ui_c().text_muted, "%s", show.author.c_str());
        if (!show.description.empty())
        {
            ui_push_small();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(120.0f, width - cover - m.space_5));
            ImGui::TextColored(ui_c().text_dim, "%s", show.description.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        if (ui_primary_button("Play latest", ImVec2(140.0f * m.layout_scale, m.button_h)))
        {
            if (!state.podcast_tracks.empty())
                play_list(state, state.podcast_tracks, 0);
        }
        ImGui::SameLine(0.0f, m.space_2);
        {
            load_saved_podcasts(state);
            const bool saved = is_show_saved(state, show.browse_id);
            if (ui_ghost_button(saved ? "Saved" : "Save show",
                                ImVec2(130.0f * m.layout_scale, m.button_h)))
                toggle_saved_show(state, show);
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Back to shows", ImVec2(140.0f * m.layout_scale, m.button_h)))
        {
            state.podcast_selected = PodcastShow{};
            state.podcast_episodes.reset();
            state.podcast_tracks.clear();
        }

        if (!state.podcast_tracks.empty())
        {
            ImGui::Dummy(ImVec2(0.0f, m.space_2));
            const bool downloading = g_podcast_downloading.load() &&
                                     g_podcast_download_show == state.podcast_selected.title;
            char dl_label[96];
            if (downloading)
                std::snprintf(dl_label, sizeof(dl_label), "Downloading %d / %d",
                              g_podcast_download_done.load(), g_podcast_download_total.load());
            else
                std::snprintf(dl_label, sizeof(dl_label), "Download all episodes");
            if (ui_secondary_button(dl_label, ImVec2(220.0f * m.layout_scale, m.button_h)))
                download_all_episodes(state);
            ui_push_small();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextColored(ui_c().text_muted, "%s",
                               "Saved to your offline library. Right-click any episode to download just that one.");
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        ImGui::EndGroup();
        if (cover > ImGui::GetItemRectSize().y)
            ImGui::Dummy(ImVec2(0.0f, 0.0f));

        ImGui::Dummy(ImVec2(1.0f, m.space_4));
        auto episodes = std::atomic_load(&state.podcast_episodes);
        const int count = episodes ? (int)episodes->size() : 0;
        ui_section_header("Episodes", (std::to_string(count) + (count == 1 ? " episode" : " episodes")).c_str());
        if (state.podcast_details_loading)
        {
            ImGui::TextColored(ui_c().text_muted, "Loading episodes...");
            ui_anim_request();
            return;
        }
        if (!episodes || episodes->empty())
        {
            ui_empty_state(UiIcon::Podcast, "No episodes", "This show has no playable episodes.");
            return;
        }
        for (int i = 0; i < (int)state.podcast_tracks.size(); ++i)
        {
            TrackRowOptions opt;
            opt.show_duration = true;
            opt.show_download = true;
            opt.secondary = (*episodes)[i].date;
            opt.art_size = 52.0f * m.layout_scale;
            TrackRowResult row = ui_track_row(state, state.podcast_tracks[i], i, opt);
            if (row.download_requested)
            {
                if (row.download_remove)
                    delete_downloaded_track(state, state.podcast_tracks[i].id);
                else
                    download_episode_async(state.podcast_tracks[i].id);
            }
        }
        ImGui::Dummy(ImVec2(1.0f, m.space_2));
        return;
    }

    draw_browse_hero();

    std::vector<PodcastShow> shows = all_shows(state);
    const std::string shows_meta =
        state.podcast_library_loading && shows.empty()
            ? "Loading..."
            : std::to_string(shows.size()) + (shows.size() == 1 ? " show" : " shows");
    ui_section_header("Your shows", shows_meta.c_str());
    if (state.podcast_library_loading && shows.empty())
    {
        ImGui::TextColored(ui_c().text_muted, "Loading your podcast library...");
        ui_anim_request();
    }
    else if (shows.empty())
    {
        ImGui::TextColored(ui_c().text_muted,
                           "No saved shows yet. Search above for a podcast, open it and press Save show.");
    }
    else
    {
        podcast_grid(state, shows, "saved_");
    }

    auto later = std::atomic_load(&state.podcast_later);
    if (later && !later->empty())
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_5));
        ui_section_header("Episodes for Later",
                          (std::to_string(later->size()) + " saved").c_str());
        PodcastShow none;
        auto tracks = tracks_for(none, *later);
        for (int i = 0; i < (int)tracks.size(); ++i)
        {
            TrackRowOptions opt;
            opt.show_duration = true;
            opt.secondary = (*later)[i].date;
            opt.art_size = 52.0f * m.layout_scale;
            TrackRowResult row = ui_track_row(state, tracks[i], i, opt);
            if (row.double_clicked)
                play_list(state, tracks, i);
        }
    }
}
