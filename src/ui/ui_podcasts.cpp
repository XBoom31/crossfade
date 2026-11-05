#include "ui.h"
#include "tasks.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ytdlp.h"
#include "paths.h"
#include "persist_util.h"
#include "library.h"
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
std::string legacy_saved_podcasts_path()
{
    return p100_config_path("saved_podcasts.json");
}

// One-time import from the pre-0.3 JSON file. New saves live in library.db
// alongside likes, playlists and downloaded-track metadata.
void migrate_legacy_saved_podcasts(AppState &state)
{
    std::ifstream in(legacy_saved_podcasts_path());
    if (!in)
        return;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int depth = 0;
    size_t start = std::string::npos;
    std::vector<PodcastShow> migrated;
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
                    migrated.push_back(std::move(show));
            }
        }
    }
    if (!migrated.empty())
    {
        state.saved_podcasts = migrated;
        library::save_saved_podcasts(state.saved_podcasts);
    }
    std::error_code ec;
    std::filesystem::remove(legacy_saved_podcasts_path(), ec);
}

void load_saved_podcasts(AppState &state)
{
    if (state.saved_podcasts_loaded)
        return;
    state.saved_podcasts_loaded = true;
    if (library::available())
    {
        state.saved_podcasts = library::load_saved_podcasts();
        if (state.saved_podcasts.empty())
            migrate_legacy_saved_podcasts(state);
    }
    else
    {
        // DB unavailable: the UI still works for this session, but there is
        // no JSON side-channel to silently drift out of sync.
        state.saved_podcasts.clear();
    }
}

void save_saved_podcasts(const AppState &state)
{
    if (library::available())
        library::save_saved_podcasts(state.saved_podcasts);
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

struct PodcastAnim
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
        return ui_anim_ease_out(std::max(0.0f, std::min(1.0f,
                                                         (float)((now - start - d) / du))));
    }

    float pop(float delay, float dur) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float t = stagger(delay, dur);
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.0f;
        const float u = t - 1.0f;
        return 1.0f + c3 * u * u * u + c1 * u * u;
    }

    float in_view(float screen_y, float span = 120.0f) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
        return ui_anim_ease_out(std::max(0.0f, std::min(1.0f,
                                                         (bottom - screen_y) / ui_px(span))));
    }
};

struct PodcastAction
{
    std::string label;
    UiIcon icon;
    bool primary;
    bool enabled;
};

// Radio-style hero, shared by the Podcasts library, search results and show
// pages. Returns the index of the clicked action, or -1.
static int podcast_hero(const AppState &state, const std::string &title,
                        const std::string &subtitle, const std::string &eyebrow,
                        const std::string &art, UiIcon fallback,
                        const std::vector<std::string> &chips,
                        const std::vector<PodcastAction> &actions,
                        double start, double now)
{
    const UiMetrics &m = ui_m();
    const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const bool compact = avail < 620.0f * m.layout_scale;
    const bool narrow = avail < 460.0f * m.layout_scale;
    const float hero_h = (narrow ? 272.0f : 210.0f) * m.layout_scale;
    const float pad = m.space_4;
    float art_size = narrow ? std::min(avail - pad * 2.0f, avail * 0.30f)
                            : std::min(hero_h - pad * 2.0f, avail * (compact ? 0.18f : 0.30f));
    art_size = std::max(art_size, 44.0f * m.layout_scale);
    const float title_base = narrow ? 21.0f : (compact ? 24.5f : 32.0f);
    const float sub_base = narrow ? 13.5f : (compact ? 14.5f : 17.0f);
    const float text_gap = compact ? m.space_4 : m.space_5;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float art_x = narrow ? origin.x + (avail - art_size) * 0.5f : origin.x + pad;
    const float art_y = origin.y + pad;
    ImGui::Dummy(ImVec2(avail, hero_h));

    PodcastAnim anim{now, start};
    const float t = anim.pop(0.0f, 0.62f);
    const float lift = (1.0f - t) * ui_anim_offset(10.0f);
    const float scale = 0.965f + 0.035f * t;
    const ImVec2 centre(origin.x + avail * 0.5f, origin.y + hero_h * 0.5f);
    auto C = [&](ImVec2 p) {
        return ImVec2(centre.x + (p.x - centre.x) * scale,
                      centre.y + (p.y - centre.y) * scale);
    };

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 art_min(art_x, art_y + lift);
    const ImVec2 art_max(art_x + art_size, art_y + art_size + lift);
    const ImVec2 art_centre((art_min.x + art_max.x) * 0.5f, (art_min.y + art_max.y) * 0.5f);
    dl->AddCircleFilled(art_centre, art_size * 0.78f, ui_accent_u32(0.045f * t), 48);
    dl->AddCircleFilled(art_centre, art_size * 0.62f, ui_accent_u32(0.035f * t), 40);

    const ImVec2 card_min = C(origin);
    const ImVec2 card_max = C(ImVec2(origin.x + avail, origin.y + hero_h));
    dl->AddRectFilled(card_min, card_max, ui_with_alpha(ui_col(ui_c().surface_hover), t),
                      m.radius_lg);
    dl->AddRectFilled(card_min, card_max, ui_accent_u32(0.040f * t), m.radius_lg);
    dl->AddRect(card_min, card_max,
                ui_with_alpha(ui_border_u32(), (0.45f + 0.55f * t) * t), m.radius_lg, 0, 1.0f);

    const float elapsed = (float)(now - start);
    if (ui_animations_enabled() && elapsed < 4.8f)
    {
        for (int k = 0; k < 2; ++k)
        {
            const float phase = std::fmod(elapsed * 0.55f + (float)k * 0.5f, 1.0f);
            const float grow = (6.0f + 30.0f * phase) * m.layout_scale;
            const float a = 0.16f * (1.0f - phase) * t;
            if (a > 0.005f)
                dl->AddRect(ImVec2(art_min.x - grow, art_min.y - grow),
                            ImVec2(art_max.x + grow, art_max.y + grow),
                            ui_accent_u32(a), m.radius_lg + grow, 0, 1.5f);
        }
        ui_anim_request();
    }

    dl->AddRectFilled(ImVec2(art_min.x + 3.0f * m.layout_scale, art_min.y + 4.0f * m.layout_scale),
                      ImVec2(art_max.x + 3.0f * m.layout_scale, art_max.y + 4.0f * m.layout_scale),
                      IM_COL32(0, 0, 0, (int)(120.0f * t)), m.radius_lg);
    ui_draw_cover(dl, art, art_min, art_max, m.radius_lg, 1.0f, fallback,
                  "podcast_hero", true, true);

    const float text_right = origin.x + avail - pad;
    const float text_x = narrow ? origin.x + pad
                                : std::min(art_max.x + text_gap, text_right - 1.0f);
    const float text_w = std::max(1.0f, text_right - text_x);
    const float centre_x = origin.x + avail * 0.5f;
    const float text_top = narrow ? (art_y + art_size + m.space_2 + lift)
                                  : (origin.y + pad + lift);
    const float title_y = text_top + 22.0f * m.layout_scale;
    const float title_line = title_base * m.text_scale * 1.20f;
    const float sub_y = title_y + title_line + 2.0f * m.layout_scale;
    const float sub_line = sub_base * m.text_scale * 1.24f;
    const float chips_y = sub_y + sub_line + m.space_3;
    const float chip_h = 24.0f * m.layout_scale;
    const float btn_h = m.button_h;
    const float buttons_y = origin.y + hero_h - pad - btn_h + lift;

    dl->PushClipRect(ImVec2(text_x - 2.0f, origin.y),
                     ImVec2(text_right + 2.0f, origin.y + hero_h), true);

    // Eyebrow with a live accent dot.
    {
        const float fs = 12.0f;
        const float dot_r = 3.2f * m.layout_scale;
        const float dot_a = ui_animations_enabled()
                                ? 0.55f + 0.45f * (0.5f + 0.5f * std::sin(elapsed * 3.0f))
                                : 1.0f;
        float ex = text_x;
        if (narrow)
            ex = centre_x - (dot_r * 3.0f +
                             ui_measure_width(g_ui_fonts.medium, fs, eyebrow.c_str())) * 0.5f;
        dl->AddCircleFilled(ImVec2(ex + dot_r, text_top + 8.0f * m.layout_scale), dot_r,
                            ui_accent_u32(dot_a * t), 16);
        dl->AddText(g_ui_fonts.medium, fs * m.text_scale,
                    ImVec2(ex + dot_r * 3.0f, text_top),
                    ui_with_alpha(ui_accent_text_u32(), 0.82f * t), eyebrow.c_str());
    }

    const std::string title_draw = ui_ellipsize(g_ui_fonts.semibold, title_base, title, text_w);
    const float title_x = narrow ? std::max(text_x, centre_x -
                                                       ui_measure_width(g_ui_fonts.semibold,
                                                                        title_base, title_draw) * 0.5f)
                                 : text_x;
    dl->AddText(g_ui_fonts.semibold, title_base * m.text_scale, ImVec2(title_x, title_y),
                ui_with_alpha(ui_text_u32(), t), title_draw.c_str());

    const std::string sub_draw = ui_ellipsize(g_ui_fonts.body, sub_base, subtitle, text_w);
    const float sub_x = narrow ? std::max(text_x, centre_x -
                                                   ui_measure_width(g_ui_fonts.body, sub_base,
                                                                    sub_draw) * 0.5f)
                               : text_x;
    dl->AddText(g_ui_fonts.body, sub_base * m.text_scale, ImVec2(sub_x, sub_y),
                ui_with_alpha(ui_text_dim_u32(), 0.9f * t), sub_draw.c_str());

    if (chips_y + chip_h <= buttons_y - 2.0f * m.layout_scale)
    {
        float total_w = 0.0f;
        size_t fit = 0;
        for (const std::string &chip : chips)
        {
            const float cw = ui_measure_width(g_ui_fonts.body, 13.0f, chip.c_str()) +
                             20.0f * m.layout_scale;
            const float add = cw + (fit > 0 ? m.space_2 : 0.0f);
            if (total_w + add > text_w)
                break;
            total_w += add;
            ++fit;
        }
        float cx = narrow ? centre_x - total_w * 0.5f : text_x;
        for (size_t i = 0; i < fit; ++i)
        {
            const float cw = ui_meta_chip(dl, m, chips[i].c_str(), ImVec2(cx, chips_y),
                                          i == 0, t);
            cx += cw + m.space_2;
        }
    }
    dl->PopClipRect();

    // Actions: lay them out from the text column; drop the ones that don't fit.
    std::vector<float> widths;
    float total_actions = 0.0f;
    size_t action_fit = 0;
    for (const PodcastAction &a : actions)
    {
        const float w = std::max(118.0f * m.layout_scale,
                                 ui_measure_width(g_ui_fonts.medium, 16.0f, a.label.c_str()) +
                                     48.0f * m.layout_scale);
        const float add = w + (action_fit > 0 ? m.space_2 : 0.0f);
        if (total_actions + add > text_w)
            break;
        widths.push_back(w);
        total_actions += add;
        ++action_fit;
    }
    float bx = narrow ? centre_x - total_actions * 0.5f : text_x;
    int clicked = -1;
    for (size_t i = 0; i < action_fit; ++i)
    {
        if (ui_pill_button(("podcast_hero_" + std::to_string(i)).c_str(),
                           actions[i].label.c_str(), actions[i].icon,
                           ImVec2(bx, buttons_y), ImVec2(widths[i], btn_h),
                           actions[i].primary, t, actions[i].enabled))
            clicked = (int)i;
        bx += widths[i] + m.space_2;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + hero_h + m.space_5));
    return clicked;
}

bool podcast_grid(AppState &state, const std::vector<PodcastShow> &shows,
                  const char *scope, double start, double now)
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
    PodcastAnim anim{now, start};
    bool any = false;
    for (int i = 0; i < (int)shows.size(); ++i)
    {
        const auto &show = shows[i];
        const int row = i / cols;
        const int col = i % cols;
        const float t = anim.stagger(0.10f + (float)i * 0.025f, 0.40f);
        const float lift = (1.0f - t) * ui_anim_offset(8.0f);
        ImGui::SetCursorScreenPos(ImVec2(origin.x + (float)col * (card_w + gap),
                                         origin.y + (float)row * row_pitch + lift));
        ImGui::PushID((scope + show.browse_id).c_str());
        MediaCardResult r = ui_media_card("##podcast_show", show.title,
                                          show.author.empty() ? "Podcast" : show.author,
                                          show.thumbnail_url, card_w, card_w, false,
                                          false, t);
        ImGui::PopID();
        if (r.clicked || r.double_clicked)
        {
            load_show(state, show);
            any = true;
        }
    }
    const int rows = ((int)shows.size() + cols - 1) / cols;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + (float)rows * row_pitch));
    ImGui::Dummy(ImVec2(1.0f, m.space_1));
    const double total = (double)ui_anim_delay(0.10f + (float)shows.size() * 0.025f) +
                         (double)ui_anim_duration(0.40f);
    if (now - start < total)
        ui_anim_request();
    return any;
}
} // namespace

void load_podcast_library(AppState &state)
{
    if (state.podcast_library_loaded || state.podcast_library_loading)
        return;
    state.podcast_library_loading = true;
    AppState *sp = &state;
    tasks::spawn([sp]() {
        auto shows = fetch_podcast_library();
        auto later = fetch_episodes_later();
        std::atomic_store(&sp->podcast_shows,
                          std::make_shared<const std::vector<PodcastShow>>(std::move(shows)));
        std::atomic_store(&sp->podcast_later,
                          std::make_shared<const std::vector<PodcastEpisode>>(std::move(later)));
        sp->podcast_library_loaded = true;
        sp->podcast_library_loading = false;
        sp->repaint_requested = true;
    });
}

void close_podcast_show(AppState &state)
{
    state.podcast_selected = PodcastShow{};
    state.podcast_episodes.reset();
    state.podcast_tracks.clear();
    state.repaint_requested = true;
}

void draw_podcasts_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    const float width = std::min(1080.0f * m.layout_scale, ImGui::GetContentRegionAvail().x);
    const bool searching = state.search_buffer[0] != '\0';
    const double now = ImGui::GetTime();

    // Page / show / query clocks keep the intro motion local to the thing that
    // changed: entering the tab replays it, opening a show replays the hero,
    // typing a query replays the results grid.
    static uint64_t s_epoch = (uint64_t)-1;
    static double s_start = 0.0;
    static double s_show_start = 0.0;
    static double s_search_start = 0.0;
    static std::string s_show_id;
    static std::string s_query;
    if (s_epoch != ui_page_epoch())
    {
        s_start = now;
        s_show_start = now;
        s_search_start = now;
    }
    if (s_show_id != state.podcast_selected.browse_id)
    {
        s_show_id = state.podcast_selected.browse_id;
        s_show_start = now;
    }
    if (s_query != state.search_buffer)
    {
        s_query = state.search_buffer;
        s_search_start = now;
    }
    s_epoch = ui_page_epoch();
    if (s_start <= 0.0)
        s_start = now;

    load_podcast_library(state);

    if (searching)
    {
        auto results = std::atomic_load(&state.podcast_search_results);
        const int count = results ? (int)results->size() : 0;
        const std::string art = (results && !results->empty()) ? results->front().thumbnail_url : "";
        std::string subtitle = "Results for \"" + std::string(state.search_buffer) + "\"";
        std::vector<std::string> chips;
        chips.push_back(state.podcast_search_loading
                            ? "SEARCHING"
                            : std::to_string(count) + (count == 1 ? " SHOW" : " SHOWS"));
        const std::vector<PodcastAction> actions = {
            {"Clear search", UiIcon::Close, false, true},
            {"Search again", UiIcon::Search, true, true},
        };
        const int clicked = podcast_hero(state, "Podcast results", subtitle, "SEARCH", art,
                                         UiIcon::Podcast, chips, actions, s_search_start, now);
        if (clicked == 0)
        {
            ui_reset_search(state);
            std::atomic_store(&state.podcast_search_results,
                              std::make_shared<const std::vector<PodcastShow>>());
        }
        else if (clicked == 1)
            state.focus_search_requested = true;

        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        if (state.podcast_search_loading)
        {
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
        podcast_grid(state, *results, "search_", s_search_start + 0.10, now);
        return;
    }

    if (!state.podcast_selected.browse_id.empty())
    {
        const PodcastShow show = state.podcast_selected;
        load_saved_podcasts(state);
        const bool saved = is_show_saved(state, show.browse_id);
        auto episodes = std::atomic_load(&state.podcast_episodes);
        const int count = episodes ? (int)episodes->size() : 0;
        const bool downloading = g_podcast_downloading.load() &&
                                 g_podcast_download_show == show.title;
        const bool can_play = !state.podcast_tracks.empty();

        // Clear back action, styled like the hero pills.
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 size(128.0f * m.layout_scale, m.control_h * 0.88f);
            ImGui::Dummy(size);
            if (ui_pill_button("podcast_back", "All shows", UiIcon::ChevronLeft, p, size,
                               false, 1.0f, true))
                close_podcast_show(state);
        }
        ImGui::Dummy(ImVec2(0.0f, m.space_2));

        std::vector<std::string> chips;
        if (state.podcast_details_loading && count == 0)
            chips.push_back("LOADING");
        else
            chips.push_back(std::to_string(count) + (count == 1 ? " EPISODE" : " EPISODES"));
        chips.push_back(saved ? "SAVED" : "NOT SAVED");
        if (can_play)
        {
            const int total = ui_tracks_duration(state.podcast_tracks);
            if (total > 0)
            {
                const int hours = total / 3600;
                const int mins = (total % 3600) / 60;
                char buf[32];
                if (hours > 0)
                    std::snprintf(buf, sizeof(buf), "%dh %02dm", hours, mins);
                else
                    std::snprintf(buf, sizeof(buf), "%dm", std::max(1, mins));
                chips.push_back(buf);
            }
        }

        char download_label[96];
        if (downloading)
            std::snprintf(download_label, sizeof(download_label), "Downloading %d / %d",
                          g_podcast_download_done.load(), g_podcast_download_total.load());
        else
            std::snprintf(download_label, sizeof(download_label), "Download all");
        std::vector<PodcastAction> actions = {
            {"Play latest", UiIcon::Play, true, can_play},
            {saved ? "Saved" : "Save show", saved ? UiIcon::Check : UiIcon::Heart, false, true},
            {download_label, UiIcon::Download, false, can_play && !downloading},
        };
        const int clicked = podcast_hero(state, show.title,
                                         show.author.empty() ? "Podcast show" : show.author,
                                         "PODCAST", show.thumbnail_url, UiIcon::Podcast,
                                         chips, actions, s_show_start, now);
        if (clicked == 0 && can_play)
            play_list(state, state.podcast_tracks, 0);
        else if (clicked == 1)
            toggle_saved_show(state, show);
        else if (clicked == 2 && can_play)
            download_all_episodes(state);

        if (!show.description.empty())
        {
            std::string desc = show.description;
            if (desc.size() > 260)
            {
                const size_t cut = desc.rfind(' ', 260);
                desc = desc.substr(0, cut == std::string::npos ? 260 : cut) + "...";
            }
            ui_push_small();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextColored(ui_c().text_dim, "%s", desc.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        ImGui::Dummy(ImVec2(0.0f, m.space_4));

        ui_section_header("Episodes",
                          (std::to_string(count) + (count == 1 ? " episode" : " episodes")).c_str());
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
        const uint64_t token = (uint64_t)std::hash<std::string>{}(show.browse_id);
        double reveal = ui_anim_track_list_reveal(("podcast_show:" + show.browse_id).c_str(),
                                                  state.podcast_tracks, token);
        const double gate = s_show_start + (double)ui_anim_delay(0.18f);
        if (gate > reveal)
            reveal = gate;
        for (int i = 0; i < (int)state.podcast_tracks.size(); ++i)
        {
            TrackRowOptions opt;
            opt.show_duration = true;
            opt.show_download = true;
            opt.secondary = (*episodes)[i].date;
            opt.art_size = 52.0f * m.layout_scale;
            opt.reveal_start = reveal;
            opt.reveal_index = i;
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

    std::vector<PodcastShow> shows = all_shows(state);
    auto later = std::atomic_load(&state.podcast_later);
    std::vector<std::string> chips;
    chips.push_back(std::to_string(shows.size()) + (shows.size() == 1 ? " SHOW" : " SHOWS"));
    const int saved_count = (int)state.saved_podcasts.size();
    if (saved_count > 0)
        chips.push_back(std::to_string(saved_count) + " SAVED");
    if (later && !later->empty())
        chips.push_back(std::to_string(later->size()) + " LATER");
    const std::string featured = shows.empty() ? "" : shows.front().thumbnail_url;
    const std::vector<PodcastAction> actions = {
        {"Search podcasts", UiIcon::Search, true, true},
        {"Refresh", UiIcon::Refresh, false, !state.podcast_library_loading},
    };
    const int clicked = podcast_hero(state, "Podcasts",
                                     "Shows, episodes and saved listens from YouTube Music",
                                     "YOUR LIBRARY", featured, UiIcon::Podcast, chips, actions,
                                     s_start, now);
    if (clicked == 0)
        state.focus_search_requested = true;
    else if (clicked == 1)
    {
        state.podcast_library_loaded = false;
        state.podcast_library_loading = false;
        load_podcast_library(state);
    }

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
        podcast_grid(state, shows, "saved_", s_start + 0.12, now);
    }

    if (later && !later->empty())
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_5));
        ui_section_header("Episodes for Later",
                          (std::to_string(later->size()) + " saved").c_str());
        PodcastShow none;
        auto tracks = tracks_for(none, *later);
        const uint64_t token = (uint64_t)(uintptr_t)later.get();
        double reveal = ui_anim_track_list_reveal("podcast_later", tracks, token);
        const double gate = s_start + (double)ui_anim_delay(0.30f);
        if (gate > reveal)
            reveal = gate;
        for (int i = 0; i < (int)tracks.size(); ++i)
        {
            TrackRowOptions opt;
            opt.show_duration = true;
            opt.secondary = (*later)[i].date;
            opt.art_size = 52.0f * m.layout_scale;
            opt.reveal_start = reveal;
            opt.reveal_index = i;
            TrackRowResult row = ui_track_row(state, tracks[i], i, opt);
            if (row.double_clicked)
                play_list(state, tracks, i);
        }
    }
}
