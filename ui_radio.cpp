#include "ui.h"
#include "texture.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ytdlp.h"
#include "paths.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

constexpr int kMaxSeeds = 8;
constexpr size_t kPrefetchCount = 10;

struct RadioSeed
{
    Track track;
    int plays = 0;
};

static float clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float ease_out_back(float t)
{
    t = clamp01f(t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

// Radio's motion clocks mirror the Stats page: one start stamp drives the
// hero pop, the seed-tile stagger and the section fades, and in_view() keeps
// lower sections from animating until they scroll into the viewport.
struct RadioAnim
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
        return ui_anim_ease_out(clamp01f((float)((now - start - d) / du)));
    }

    float pop(float delay, float dur) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float d = ui_anim_delay(delay);
        const float du = ui_anim_duration(dur);
        if (du <= 0.0001f)
            return 1.0f;
        return ease_out_back(clamp01f((float)((now - start - d) / du)));
    }

    float in_view(float screen_y, float span = 120.0f) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
        return ui_anim_ease_out(clamp01f((bottom - screen_y) / ui_px(span)));
    }
};


// ----------------------------------------------------------------------
// Artwork / text helpers
// ----------------------------------------------------------------------



// ----------------------------------------------------------------------
// Seed collection: most-played first, with graceful fallbacks so a fresh
// account still gets a station from Liked / History / recommendations.
// ----------------------------------------------------------------------
static void ensure_thumb(Track &t)
{
    if (t.thumbnail_url.empty() && !t.id.empty())
        t.thumbnail_url = "https://img.youtube.com/vi/" + t.id + "/hqdefault.jpg";
}

static void merge_meta(std::unordered_map<std::string, Track> &meta, const Track &t)
{
    if (t.id.empty())
        return;
    auto it = meta.find(t.id);
    if (it == meta.end())
    {
        meta.emplace(t.id, t);
        return;
    }
    Track &d = it->second;
    if (d.title.empty()) d.title = t.title;
    if (d.artist.empty()) d.artist = t.artist;
    if (d.artist_id.empty()) d.artist_id = t.artist_id;
    if (d.album.empty()) d.album = t.album;
    if (d.album_id.empty()) d.album_id = t.album_id;
    if (d.thumbnail_url.empty()) d.thumbnail_url = t.thumbnail_url;
    if (d.duration_sec == 0) d.duration_sec = t.duration_sec;
}

static std::vector<RadioSeed> collect_radio_seeds(const AppState &state)
{
    std::unordered_map<std::string, Track> meta;
    std::unordered_map<std::string, int> plays;

    for (const auto &ev : state.play_log)
    {
        if (ev.id.empty())
            continue;
        Track t;
        t.id = ev.id;
        t.title = ev.title;
        t.artist = ev.artist;
        t.thumbnail_url = ev.thumbnail_url;
        merge_meta(meta, t);
        ++plays[ev.id];
    }

    // play_counts is the all-time total and survives the play_log ring cap.
    for (const auto &kv : state.play_counts)
    {
        if (kv.first.empty())
            continue;
        int &p = plays[kv.first];
        p = std::max(p, kv.second);
    }

    for (const auto &t : state.liked_tracks)
        merge_meta(meta, t);
    for (const auto &t : state.playback_history)
        merge_meta(meta, t);
    for (const auto &t : state.downloaded_tracks)
        merge_meta(meta, t);
    for (const auto &t : state.stats.tracks)
        merge_meta(meta, t);
    if (auto recs = std::atomic_load(&state.recommendations))
        for (const auto &t : *recs)
            merge_meta(meta, t);
    if (auto yt = std::atomic_load(&state.youtube_liked))
        for (const auto &t : *yt)
            merge_meta(meta, t);

    std::vector<RadioSeed> out;
    out.reserve(plays.size());
    for (const auto &kv : plays)
    {
        auto it = meta.find(kv.first);
        if (it == meta.end() || it->second.title.empty())
            continue;
        Track t = it->second;
        ensure_thumb(t);
        out.push_back(RadioSeed{t, kv.second});
    }

    if (out.empty())
    {
        // No play counts yet: seed from the nearest thing to taste.
        auto add = [&](const std::vector<Track> &v) {
            for (const auto &src : v)
            {
                if (src.id.empty() || src.title.empty())
                    continue;
                Track t = src;
                ensure_thumb(t);
                int p = 0;
                auto pit = plays.find(t.id);
                if (pit != plays.end())
                    p = pit->second;
                out.push_back(RadioSeed{t, p});
            }
        };
        add(state.liked_tracks);
        add(state.playback_history);
        if (auto recs = std::atomic_load(&state.recommendations))
            add(*recs);
    }

    std::sort(out.begin(), out.end(), [](const RadioSeed &a, const RadioSeed &b) {
        if (a.plays != b.plays)
            return a.plays > b.plays;
        if (a.track.title != b.track.title)
            return a.track.title < b.track.title;
        return a.track.id < b.track.id;
    });

    // De-duplicate while keeping the play-count-sorted order.
    std::vector<RadioSeed> unique;
    unique.reserve(std::min(out.size(), (size_t)kMaxSeeds));
    std::unordered_map<std::string, bool> seen;
    for (auto &s : out)
    {
        if (s.track.id.empty() || seen.count(s.track.id))
            continue;
        seen[s.track.id] = true;
        unique.push_back(std::move(s));
        if ((int)unique.size() >= kMaxSeeds)
            break;
    }
    return unique;
}

// collect_radio_seeds() walks the whole listening history, every saved list
// and both remote feeds; that is far too much work to repeat per frame.
// The result is memoized and only recomputed when a cheap signature of the
// underlying data (list sizes + dirty flags) changes.
static std::vector<RadioSeed> radio_seeds_cached(const AppState &state)
{
    static std::vector<RadioSeed> s_cache;
    static size_t s_sig = 0;
    static bool s_valid = false;

    size_t sig = 0;
    auto mix = [&sig](size_t v) { sig = sig * 1000003u + v; };
    mix(state.play_log.size());
    mix(state.play_counts.size());
    mix(state.liked_tracks.size());
    mix(state.playback_history.size());
    mix(state.downloaded_tracks.size());
    mix(state.stats.tracks.size());
    if (auto rec = std::atomic_load(&state.recommendations))
        mix(rec->size());
    if (auto yt = std::atomic_load(&state.youtube_liked))
        mix(yt->size());
    mix(state.stats_dirty ? 1 : 0);

    if (s_valid && sig == s_sig)
        return s_cache;
    s_cache = collect_radio_seeds(state);
    s_sig = sig;
    s_valid = true;
    return s_cache;
}

// ----------------------------------------------------------------------
// Cache (<config>/radio_last.cache)
// Line 1: seed metadata, tab separated. Line 2: track array JSON.
// ----------------------------------------------------------------------
static std::string radio_cache_path()
{
    return p100_config_path("radio_last.cache");
}

static std::string radio_json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += (char)c;
            break;
        }
    }
    return out;
}

static std::string cache_field(std::string s)
{
    for (char &c : s)
        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
    return s;
}

static void save_radio_cache(const Track &seed, int plays, const std::vector<Track> &tracks)
{
    if (seed.id.empty() || tracks.empty())
        return;

    const std::string path = radio_cache_path();
    const std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);

    std::ofstream out(path);
    if (!out)
        return;

    out << cache_field(seed.id) << '\t' << cache_field(seed.title) << '\t'
        << cache_field(seed.artist) << '\t' << cache_field(seed.thumbnail_url) << '\t'
        << plays << '\n';

    out << "[";
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const Track &t = tracks[i];
        if (i)
            out << ",";
        out << "{\"id\":\"" << radio_json_escape(t.id)
            << "\",\"title\":\"" << radio_json_escape(t.title)
            << "\",\"artist\":\"" << radio_json_escape(t.artist)
            << "\",\"artist_id\":\"" << radio_json_escape(t.artist_id)
            << "\",\"album\":\"" << radio_json_escape(t.album)
            << "\",\"album_id\":\"" << radio_json_escape(t.album_id)
            << "\",\"track_number\":" << t.track_number
            << ",\"thumbnail_url\":\"" << radio_json_escape(t.thumbnail_url)
            << "\",\"duration_sec\":" << t.duration_sec
            << ",\"url\":\"" << radio_json_escape(t.url) << "\"}";
    }
    out << "]\n";
}

static bool load_radio_cache(AppState &state)
{
    std::ifstream in(radio_cache_path());
    if (!in.good())
        return false;

    std::string meta_line;
    std::string json_line;
    if (!std::getline(in, meta_line) || !std::getline(in, json_line))
        return false;

    std::vector<std::string> parts;
    size_t begin = 0;
    while (true)
    {
        const size_t tab = meta_line.find('\t', begin);
        if (tab == std::string::npos)
        {
            parts.push_back(meta_line.substr(begin));
            break;
        }
        parts.push_back(meta_line.substr(begin, tab - begin));
        begin = tab + 1;
    }
    if (parts.size() < 5 || parts[0].empty())
        return false;

    auto tracks = parse_track_array(json_line);
    if (tracks.empty())
        return false;

    state.radio_seed_id = parts[0];
    state.radio_seed_title = parts[1];
    state.radio_seed_artist = parts[2];
    state.radio_seed_thumbnail_url = parts[3];
    state.radio_seed_plays = std::max(0, atoi(parts[4].c_str()));
    if (state.radio_seed_thumbnail_url.empty())
    {
        Track s;
        s.id = state.radio_seed_id;
        ensure_thumb(s);
        state.radio_seed_thumbnail_url = s.thumbnail_url;
    }

    for (auto &t : tracks)
        ensure_thumb(t);

    std::atomic_store(&state.radio_results,
                      std::make_shared<const std::vector<Track>>(std::move(tracks)));
    state.radio_loading = false;
    state.radio_failed = false;
    return true;
}

static Track seed_from_state(const AppState &state)
{
    Track t;
    t.id = state.radio_seed_id;
    t.title = state.radio_seed_title;
    t.artist = state.radio_seed_artist;
    t.thumbnail_url = state.radio_seed_thumbnail_url;
    if (!t.id.empty())
    {
        ensure_thumb(t);
        t.url = "https://www.youtube.com/watch?v=" + t.id;
    }
    return t;
}

static void begin_station(AppState &state, const Track &seed, int plays, bool switch_tab)
{
    if (seed.id.empty())
        return;

    const uint64_t token = state.radio_request.fetch_add(1) + 1;

    Track seed_copy = seed;
    ensure_thumb(seed_copy);

    state.radio_seed_id = seed_copy.id;
    state.radio_seed_title = seed_copy.title;
    state.radio_seed_artist = seed_copy.artist;
    state.radio_seed_thumbnail_url = seed_copy.thumbnail_url;
    state.radio_seed_plays = std::max(0, plays);

    std::atomic_store(&state.radio_results,
                      std::make_shared<const std::vector<Track>>());
    state.radio_failed = false;
    state.radio_loading = true;
    state.repaint_requested = true;

    if (switch_tab)
    {
        ui_invalidate_forward(state);
        state.current_tab = Tab::Radio;
    }

    std::thread([&state, seed_copy, plays, token]() {
        std::vector<Track> results;
        bool ok = false;
        try
        {
            results = fetch_radio(seed_copy.id);
            ok = !results.empty();
        }
        catch (...)
        {
            ok = false;
        }

        // A newer seed was chosen while this one was in flight: drop it.
        if (token != state.radio_request.load())
            return;

        if (ok)
        {
            for (auto &t : results)
            {
                ensure_thumb(t);
                if (t.url.empty() && !t.id.empty())
                    t.url = "https://www.youtube.com/watch?v=" + t.id;
            }
            prefetch_urls_for_serial(results, kPrefetchCount, &state.radio_request, token);
            save_radio_cache(seed_copy, plays, results);
        }

        std::atomic_store(&state.radio_results,
                          std::make_shared<const std::vector<Track>>(std::move(results)));
        state.radio_failed = !ok;
        state.radio_loading = false;
        state.repaint_requested = true;
    }).detach();
}

// ----------------------------------------------------------------------
// Loading skeleton
// ----------------------------------------------------------------------
static void draw_radio_skeleton(const UiMetrics &m, float avail, double now)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float art = 46.0f * m.layout_scale;
    const float title_line = 21.0f * m.text_scale * 1.22f;
    const float meta_line = 16.0f * m.text_scale * 1.24f;
    const float row_h = std::max(art, title_line + m.space_1 + meta_line) + m.row_pad_y * 2.0f;
    const float gap = m.row_gap;
    const int rows = 6;
    const float text_x = origin.x + m.row_pad_x + art + m.space_3;
    const float text_w = std::max(1.0f, avail - (text_x - origin.x) - m.row_pad_x - 60.0f);
    const float anim = ui_animations_enabled() ? 1.0f : 0.0f;

    for (int i = 0; i < rows; ++i)
    {
        const float y = origin.y + (float)i * (row_h + gap);
        const float ay = y + (row_h - art) * 0.5f;
        const float title_y = y + (row_h - (title_line + m.space_1 + meta_line)) * 0.5f;
        const float breath = 0.5f + 0.5f * sinf((float)now * 1.8f + (float)i * 0.7f);
        const float base_a = anim > 0.0f ? 0.030f + 0.024f * breath : 0.045f;

        dl->AddRectFilled(ImVec2(origin.x + m.row_pad_x, ay),
                          ImVec2(origin.x + m.row_pad_x + art, ay + art),
                          ui_overlay_u32(base_a), m.radius_sm);
        dl->AddRectFilled(ImVec2(text_x, title_y),
                          ImVec2(text_x + text_w, title_y + title_line * 0.72f),
                          ui_overlay_u32(base_a),
                          title_line * 0.36f);
        dl->AddRectFilled(ImVec2(text_x, title_y + title_line + m.space_1),
                          ImVec2(text_x + text_w * 0.58f,
                                 title_y + title_line + m.space_1 + meta_line * 0.65f),
                          ui_overlay_u32(base_a * 0.8f),
                          meta_line * 0.32f);

        if (anim > 0.0f)
        {
            const float phase = fmodf((float)now * 0.42f + (float)i * 0.17f, 1.55f) - 0.25f;
            if (phase > 0.0f && phase < 1.0f)
            {
                const float band = art * 0.20f;
                const float centre = origin.x + m.row_pad_x + art * phase;
                const float x0 = std::max(origin.x + m.row_pad_x, centre - band);
                const float x1 = std::min(origin.x + m.row_pad_x + art, centre + band);
                if (x1 > x0)
                    dl->AddRectFilled(ImVec2(x0, ay), ImVec2(x1, ay + art),
                                      ui_accent_u32(0.10f + 0.10f * (1.0f - fabsf(phase - 0.5f) * 2.0f)),
                                      m.radius_sm);
            }
        }
    }

    ImGui::Dummy(ImVec2(avail, (float)rows * (row_h + gap) + m.space_2));
    if (ui_animations_enabled())
        ui_anim_request();
}

} // namespace

// ----------------------------------------------------------------------
// Public entry points
// ----------------------------------------------------------------------
void ui_radio_init(AppState &state)
{
    if (load_radio_cache(state))
    {
        auto snapshot = std::atomic_load(&state.radio_results);
        if (snapshot)
            prefetch_urls_for_serial(*snapshot, kPrefetchCount, &state.radio_request,
                                     state.radio_request.load());
        return;
    }

    auto seeds = radio_seeds_cached(state);
    if (!seeds.empty())
        begin_station(state, seeds.front().track, seeds.front().plays, false);
}

void start_radio_from_track(AppState &state, const Track &seed)
{
    if (seed.id.empty())
        return;
    begin_station(state, seed, play_count_for(state, seed.id), true);
}

void draw_radio_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    const double now = ImGui::GetTime();

    std::vector<RadioSeed> seeds = radio_seeds_cached(state);
    auto tracks = std::atomic_load(&state.radio_results);
    bool loading = state.radio_loading.load();
    bool failed = state.radio_failed.load();

    // First visit (or no cache yet): build a station from the top seed.
    if (state.radio_seed_id.empty() && !loading && !failed && !seeds.empty())
    {
        begin_station(state, seeds.front().track, seeds.front().plays, false);
        tracks = std::atomic_load(&state.radio_results);
        loading = state.radio_loading.load();
        failed = state.radio_failed.load();
    }

    // Replay the page intro whenever the tab reappears. A new seed gets its
    // own hero clock so the already-visible seed grid does not blink out.
    static uint64_t s_last_epoch = (uint64_t)-1;
    static double s_start = 0.0;
    static double s_station_start = 0.0;
    static std::string s_seed;
    if (s_last_epoch != ui_page_epoch())
    {
        s_start = now;
        s_station_start = now;
    }
    if (s_seed != state.radio_seed_id)
    {
        s_seed = state.radio_seed_id;
        s_station_start = now;
    }
    s_last_epoch = ui_page_epoch();
    if (s_start <= 0.0)
        s_start = now;
    if (s_station_start <= 0.0)
        s_station_start = now;

    RadioAnim anim;
    anim.now = now;
    anim.start = s_start;
    RadioAnim hero_anim;
    hero_anim.now = now;
    hero_anim.start = s_station_start;
    if (ui_animations_enabled() &&
        (float)(anim.now - anim.start) < ui_anim_delay(1.0f) + ui_anim_duration(0.9f))
        ui_anim_request();

    // Keep the station list behind the seed-card intro. The last tile starts
    // at 0.30 + (n-1)*0.035 and takes 0.48s; add a small breath before the
    // list header appears. On a later seed click the page clock is old, so
    // this delay is already in the past and new tracks do not feel laggy.
    const float seed_last_delay = seeds.empty()
                                      ? 0.0f
                                      : 0.30f + (float)(seeds.size() - 1) * 0.035f;
    const float seed_done_base = seeds.empty()
                                     ? 0.44f
                                     : seed_last_delay + 0.10f;

    auto header = [&](const char *title, const char *meta, const char *action, float delay) {
        const ImVec2 p = ImGui::GetCursorPos();
        const float sy = ImGui::GetCursorScreenPos().y;
        const float t = anim.stagger(delay, 0.5f) * anim.in_view(sy, 100.0f);
        const float off = (1.0f - t) * ui_anim_offset(12.0f);
        ImGui::SetCursorPosY(p.y + off);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, t);
        if (action && *action)
            ui_push_small();
        const bool clicked = ui_section_header(title, meta, action);
        if (action && *action)
            ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - off);
        return clicked;
    };

    if (state.radio_seed_id.empty() && seeds.empty())
    {
        ui_empty_state(UiIcon::Radio, "No station yet",
                       "Play a few songs and Radio will build itself from your most played");
        return;
    }

    const float avail = ImGui::GetContentRegionAvail().x;

    // ------------------------------------------------------------------
    // Hero: seed artwork, station identity, live status and actions.
    // ------------------------------------------------------------------
    const bool compact_hero = avail < 620.0f * m.layout_scale;
    // With the lyrics panel docked the content column gets very narrow. There
    // the artwork moves above the text and everything is centred, which gives
    // the station title the full card width instead of a tiny side column.
    const bool narrow_hero = avail < 460.0f * m.layout_scale;
    const float hero_h = (narrow_hero ? 272.0f : 210.0f) * m.layout_scale;
    const float pad = m.space_4;
    float art;
    if (narrow_hero)
    {
        art = std::min(avail - pad * 2.0f, avail * 0.30f);
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

    const ImVec2 hero_origin = ImGui::GetCursorScreenPos();
    const float art_x = narrow_hero ? hero_origin.x + (avail - art) * 0.5f
                                    : hero_origin.x + pad;
    const float art_y = hero_origin.y + pad;
    ImGui::Dummy(ImVec2(avail, hero_h));

    const float hero_t = hero_anim.pop(0.0f, 0.62f);
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

    // Pulsing rings around the art for the first few seconds (and again
    // whenever the user returns to the tab or picks a new seed).
    const float elapsed = (float)(now - hero_anim.start);
    if (ui_animations_enabled() && elapsed >= 0.0f && elapsed < 4.8f)
    {
        for (int k = 0; k < 2; ++k)
        {
            const float phase = fmodf(elapsed * 0.55f + (float)k * 0.5f, 1.0f);
            const float grow = (6.0f + 30.0f * phase) * m.layout_scale;
            const float a = 0.16f * (1.0f - phase) * hero_t;
            if (a > 0.005f)
                dl->AddRect(ImVec2(art_min.x - grow, art_min.y - grow),
                            ImVec2(art_max.x + grow, art_max.y + grow),
                            ui_accent_u32(a), m.radius_lg + grow, 0, 1.5f);
        }
        ui_anim_request();
    }

    // Artwork (shadow, cover, hover sheen).
    dl->AddRectFilled(ImVec2(art_min.x + 3.0f * m.layout_scale, art_min.y + 4.0f * m.layout_scale),
                      ImVec2(art_max.x + 3.0f * m.layout_scale, art_max.y + 4.0f * m.layout_scale),
                      IM_COL32(0, 0, 0, (int)(120.0f * hero_t)), m.radius_lg);
    ui_draw_cover(dl, state.radio_seed_thumbnail_url, art_min, art_max, m.radius_lg, 1.0f, UiIcon::Radio, "radio", true);

    const float text_right = hero_origin.x + avail - pad;
    const float text_x = narrow_hero ? hero_origin.x + pad
                                     : std::min(art_max.x + text_gap, text_right - 1.0f);
    const float text_w = std::max(1.0f, text_right - text_x);
    const float centre_x = hero_origin.x + avail * 0.5f;
    const float text_top = narrow_hero ? (art_y + art + m.space_2 + lift)
                                       : (hero_origin.y + pad + lift);

    std::string station_title;
    if (!state.radio_seed_artist.empty())
        station_title = state.radio_seed_artist + " Radio";
    else if (!state.radio_seed_title.empty())
        station_title = state.radio_seed_title + " Radio";
    else
        station_title = "Your Radio";

    std::string seed_line;
    if (!state.radio_seed_title.empty())
    {
        seed_line = "Seeded by " + state.radio_seed_title;
        if (state.radio_seed_plays > 0)
            seed_line += "  \xC2\xB7  " + std::to_string(state.radio_seed_plays) +
                         (state.radio_seed_plays == 1 ? " play" : " plays");
    }
    else
        seed_line = "Built from your listening";

    const float eyebrow_y = text_top;
    const float title_y = text_top + 22.0f * m.layout_scale;
    const float title_line = title_base * m.text_scale * 1.20f;
    const float sub_y = title_y + title_line + 2.0f * m.layout_scale;
    const float sub_line = sub_base * m.text_scale * 1.24f;
    const float chips_y = sub_y + sub_line + m.space_3;
    const float chip_h = 24.0f * m.layout_scale;
    const float btn_h = m.button_h;
    const float buttons_y = hero_origin.y + hero_h - pad - btn_h + lift;

    dl->PushClipRect(ImVec2(text_x - 2.0f, hero_origin.y),
                     ImVec2(text_right + 2.0f, hero_origin.y + hero_h), true);

    // "ENDLESS STATION" eyebrow with a live dot.
    {
        const char *eyebrow_text = "ENDLESS STATION";
        const float eyebrow_fs = 12.0f;
        const float dot_r = 3.2f * m.layout_scale;
        const float dot_a = ui_animations_enabled()
                                ? 0.55f + 0.45f * (0.5f + 0.5f * sinf(elapsed * 3.0f))
                                : 1.0f;
        float eyebrow_x = text_x;
        if (narrow_hero)
        {
            const float group = dot_r * 3.0f +
                                ui_measure_width(g_ui_fonts.medium, eyebrow_fs, eyebrow_text);
            eyebrow_x = centre_x - group * 0.5f;
        }
        dl->AddCircleFilled(ImVec2(eyebrow_x + dot_r, eyebrow_y + 8.0f * m.layout_scale),
                            dot_r, ui_accent_u32(dot_a * hero_t), 16);
        dl->AddText(g_ui_fonts.medium, eyebrow_fs * m.text_scale,
                    ImVec2(eyebrow_x + dot_r * 3.0f, eyebrow_y),
                    ui_with_alpha(ui_accent_text_u32(), 0.82f * hero_t), eyebrow_text);
    }

    std::string title_draw = ui_ellipsize(g_ui_fonts.semibold, title_base, station_title, text_w);
    const float title_x =
        narrow_hero ? std::max(text_x,
                               centre_x -
                                   ui_measure_width(g_ui_fonts.semibold, title_base, title_draw) * 0.5f)
                    : text_x;
    dl->AddText(g_ui_fonts.semibold, title_base * m.text_scale, ImVec2(title_x, title_y),
                ui_with_alpha(ui_text_u32(), hero_t), title_draw.c_str());

    std::string sub_draw = ui_ellipsize(g_ui_fonts.body, sub_base, seed_line, text_w);
    const float sub_x =
        narrow_hero ? std::max(text_x,
                               centre_x -
                                   ui_measure_width(g_ui_fonts.body, sub_base, sub_draw) * 0.5f)
                    : text_x;
    dl->AddText(g_ui_fonts.body, sub_base * m.text_scale, ImVec2(sub_x, sub_y),
                ui_with_alpha(ui_text_dim_u32(), 0.9f * hero_t), sub_draw.c_str());

    if (chips_y + chip_h <= buttons_y - 2.0f * m.layout_scale)
    {
        struct HeroChip
        {
            std::string label;
            bool accent;
        };
        std::vector<HeroChip> chip_list;
        chip_list.push_back({"ENDLESS", true});

        const int n = tracks ? (int)tracks->size() : 0;
        std::string track_chip;
        if (loading && n == 0)
            track_chip = "TUNING IN";
        else if (failed && n == 0)
            track_chip = "NO SIGNAL";
        else
            track_chip = std::to_string(n) + (n == 1 ? " TRACK" : " TRACKS");
        chip_list.push_back({track_chip, false});
        if (!loading && !failed && tracks && !tracks->empty())
        {
            const std::string dur = ui_format_duration(ui_tracks_duration(*tracks));
            if (!dur.empty())
                chip_list.push_back({dur, false});
        }

        // Only keep the chips that fit; then centre the group when the cover
        // sits above the text, otherwise pack from the left like before.
        const float chip_budget = text_right - text_x;
        float chip_total = 0.0f;
        size_t chip_fit = 0;
        for (const HeroChip &chip : chip_list)
        {
            const float cw = ui_measure_width(g_ui_fonts.body, 13.0f, chip.label) +
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
            const float cw = ui_meta_chip(dl, m, chip_list[i].label.c_str(),
                                             ImVec2(cx, chips_y), chip_list[i].accent, hero_t);
            cx += cw + m.space_2;
        }
    }
    dl->PopClipRect();

    // Actions. Buttons are submitted after the Dummy so they sit above the
    // card and receive hover/click normally.
    const float play_w = 152.0f * m.layout_scale;
    const float seed_w = 126.0f * m.layout_scale;
    const bool show_new = text_w >= 288.0f * m.layout_scale;
    const bool can_play = tracks && !tracks->empty() && !loading;

    if (show_new)
    {
        if (ui_pill_button("play_station", play_w > 132.0f * m.layout_scale ? "Play station" : "Play",
                       UiIcon::Play, ImVec2(text_x, buttons_y),
                       ImVec2(play_w, btn_h), true, hero_t, can_play))
        {
            state.current_playlist_source = PlaylistSource::RADIO;
            ui_play_track(state, (*tracks)[0], 0);
        }
        if (ui_pill_button("new_seed", "New seed", UiIcon::Shuffle,
                       ImVec2(text_x + play_w + m.space_2, buttons_y),
                       ImVec2(seed_w, btn_h), false, hero_t, !loading))
        {
            if (!seeds.empty())
            {
                size_t next = 0;
                for (size_t i = 0; i < seeds.size(); ++i)
                    if (seeds[i].track.id == state.radio_seed_id)
                    {
                        next = (i + 1) % seeds.size();
                        break;
                    }
                begin_station(state, seeds[next].track, seeds[next].plays, false);
                ui_anim_request();
                return;
            }
        }
    }
    else
    {
        const float w = std::min(text_w, 172.0f * m.layout_scale);
        const float bx = narrow_hero ? centre_x - w * 0.5f : text_x;
        if (ui_pill_button("play_station", w > 128.0f * m.layout_scale ? "Play station" : "Play",
                       UiIcon::Play, ImVec2(bx, buttons_y), ImVec2(w, btn_h), true,
                       hero_t, can_play))
        {
            state.current_playlist_source = PlaylistSource::RADIO;
            ui_play_track(state, (*tracks)[0], 0);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(hero_origin.x, hero_origin.y + hero_h + m.space_5));

    // ------------------------------------------------------------------
    // Start from: the most-played seeds, as tactile artwork tiles.
    // ------------------------------------------------------------------
    if (!seeds.empty())
    {
        const bool has_plays = seeds.front().plays > 0;
        header("Start from", has_plays ? "Your most played" : "From your library", nullptr, 0.24f);

        const float tile_gap = m.space_2;
        const float min_tile = 206.0f * m.layout_scale + tile_gap;
        int cols = std::max(1, std::min(4, (int)((avail + tile_gap) / min_tile)));
        const float tile_w = (avail - tile_gap * (cols - 1)) / cols;
        const float tile_h = 64.0f * m.layout_scale;
        const int rows = ((int)seeds.size() + cols - 1) / cols;

        const ImVec2 grid_origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(avail, (float)rows * (tile_h + tile_gap)));

        for (int i = 0; i < (int)seeds.size(); ++i)
        {
            const RadioSeed &seed = seeds[i];
            const int col = i % cols;
            const int row = i / cols;
            const ImVec2 pos(grid_origin.x + col * (tile_w + tile_gap),
                             grid_origin.y + row * (tile_h + tile_gap));
            const bool active = seed.track.id == state.radio_seed_id;

            ImGui::SetCursorScreenPos(pos);
            ImGui::PushID(seed.track.id.c_str());
            const ImGuiID uid = ImGui::GetID("##seed_tile");
            ImGui::InvisibleButton("##seed_tile", ImVec2(tile_w, tile_h));
            const bool hovered = ImGui::IsItemHovered();
            const bool clicked = ImGui::IsItemClicked();
            ImGui::PopID();

            const float hv = ui_pill_hover_value(uid, hovered);
            const float pulse = ui_pill_pulse_value(uid, now, clicked);
            const float t = anim.pop(0.30f + (float)i * 0.035f, 0.48f);

            const float lift2 = -2.0f * hv * m.layout_scale + (1.0f - t) * ui_anim_offset(8.0f);
            const ImVec2 rmin(pos.x, pos.y + lift2);
            const ImVec2 rmax(pos.x + tile_w, pos.y + tile_h + lift2);
            const float alpha = t;

            if (pulse >= 0.0f && pulse < 1.0f)
            {
                const float pr = ui_anim_ease_out(pulse);
                const float grow = 6.0f * pr * m.layout_scale;
                dl->AddRect(ImVec2(rmin.x - grow, rmin.y - grow),
                            ImVec2(rmax.x + grow, rmax.y + grow),
                            ui_accent_u32(0.40f * (1.0f - pr) * alpha), m.radius_md + grow, 0, 1.5f);
            }

            dl->AddRectFilled(rmin, rmax,
                              active ? ui_accent_u32(0.115f * alpha)
                                     : ImGui::GetColorU32(ImVec4(1, 1, 1, (0.028f + 0.035f * hv) * alpha)),
                              m.radius_md);
            dl->AddRect(rmin, rmax,
                        active ? ui_accent_u32(0.42f * alpha)
                               : ui_with_alpha(ui_border_u32(), (0.55f + 0.35f * hv) * alpha),
                        m.radius_md, 0, active ? 1.4f : 1.0f);
            if (active)
                dl->AddRectFilled(ImVec2(rmin.x + 2.0f * m.layout_scale, rmin.y + m.space_2),
                                  ImVec2(rmin.x + 4.0f * m.layout_scale, rmax.y - m.space_2),
                                  ui_accent_u32(alpha), 1.0f);

            const float thumb = 44.0f * m.layout_scale;
            const ImVec2 amin(rmin.x + m.space_2, rmin.y + (tile_h - thumb) * 0.5f);
            const ImVec2 amax(amin.x + thumb, amin.y + thumb);
            // Seed cards appear with the tile's pop, not with a second cover
            // fade that briefly exposes the placeholder beneath them.
            // The card pops in as before, but the cover itself must not fade:
            // it arrives at full opacity on the first visible frame.
            ui_draw_cover(dl, seed.track.thumbnail_url, amin, amax, m.radius_sm,
                          t > 0.001f ? 1.0f : 0.0f,
                          UiIcon::Album, "radio", false, true);
            if (hv > 0.01f)
            {
                dl->AddRectFilled(amin, amax, IM_COL32(0, 0, 0, (int)(110.0f * hv * alpha)),
                                  m.radius_sm);
                ui_draw_icon(dl, UiIcon::Play,
                             ImVec2((amin.x + amax.x) * 0.5f, (amin.y + amax.y) * 0.5f),
                             thumb * 0.32f, ui_with_alpha(IM_COL32(255, 255, 255, 255), hv * alpha),
                             1.4f);
            }

            const float play_wr = seed.plays > 0 ? 42.0f * m.layout_scale : 0.0f;
            const float tx = amax.x + m.space_2;
            const float tw = std::max(1.0f, rmax.x - m.space_2 - play_wr - tx);
            const float title_fs = 17.0f * m.text_scale;
            const float meta_fs = 14.0f * m.text_scale;
            const float title_th = title_fs * 1.24f;
            const float meta_th = meta_fs * 1.24f;
            const float ty = rmin.y + (tile_h - (title_th + meta_th)) * 0.5f;

            std::string title = ui_ellipsize(g_ui_fonts.medium, 17.0f,
                                             seed.track.title.empty() ? "Unknown Track" : seed.track.title,
                                             tw);
            std::string meta = ui_ellipsize(g_ui_fonts.body, 14.0f,
                                            seed.track.artist.empty() ? "Unknown Artist" : seed.track.artist,
                                            tw);
            dl->AddText(g_ui_fonts.medium, title_fs, ImVec2(tx, ty),
                        ui_with_alpha(active || hv > 0.5f ? ui_text_u32() : ui_text_dim_u32(), alpha),
                        title.c_str());
            dl->AddText(g_ui_fonts.body, meta_fs, ImVec2(tx, ty + title_th),
                        ui_with_alpha(ui_text_muted_u32(), alpha), meta.c_str());

            if (seed.plays > 0)
            {
                const std::string count = std::to_string(seed.plays);
                ImVec2 cs = g_ui_fonts.medium->CalcTextSizeA(15.0f * m.text_scale, FLT_MAX, 0.0f,
                                                            count.c_str());
                dl->AddText(g_ui_fonts.medium, 15.0f * m.text_scale,
                            ImVec2(rmax.x - m.space_2 - cs.x,
                                   rmin.y + (tile_h - cs.y) * 0.5f),
                            ui_with_alpha(active ? ui_accent_text_u32() : ui_text_muted_u32(),
                                          (0.75f + 0.25f * hv) * alpha),
                            count.c_str());
            }

            if (hovered)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (clicked)
            {
                begin_station(state, seed.track, seed.plays, false);
                ui_anim_request();
                return;
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(grid_origin.x,
                                         grid_origin.y + (float)rows * (tile_h + tile_gap) + m.space_4));
    }

    // ------------------------------------------------------------------
    // Station queue
    // ------------------------------------------------------------------
    const int track_count = tracks ? (int)tracks->size() : 0;
    std::string list_meta;
    if (loading && track_count == 0)
        list_meta = "Tuning in...";
    else if (failed && track_count == 0)
        list_meta = "No signal";
    else
    {
        list_meta = std::to_string(track_count) + (track_count == 1 ? " track" : " tracks");
        if (tracks && !tracks->empty())
        {
            const std::string dur = ui_format_duration(ui_tracks_duration(*tracks));
            if (!dur.empty())
                list_meta += "  \xC2\xB7  " + dur;
        }
    }

    const char *refresh_label = (loading || track_count == 0) ? nullptr
                                                               : (avail > 520.0f * m.layout_scale
                                                                      ? "Refresh station"
                                                                      : "Refresh");
    if (refresh_label)
    {
        float need = ui_measure_width(g_ui_fonts.semibold, 22.0f, "Station Tracks");
        if (!list_meta.empty())
            need += m.space_2 + ui_measure_width(g_ui_fonts.body, 20.0f, list_meta);
        need += ui_measure_width(g_ui_fonts.body, 20.0f, refresh_label) + m.space_4 * 2.0f +
                m.space_5;
        if (avail < need)
            refresh_label = nullptr;
    }
    if (header("Station Tracks", list_meta.c_str(), refresh_label, seed_done_base))
    {
        Track seed = seed_from_state(state);
        if (!seed.id.empty())
        {
            begin_station(state, seed, state.radio_seed_plays, false);
            ui_anim_request();
            return;
        }
    }

    if (loading && track_count == 0)
    {
        draw_radio_skeleton(m, avail, now);
    }
    else if (failed && track_count == 0)
    {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float h = 150.0f * m.layout_scale;
        const float t = anim.stagger(seed_done_base + 0.02f, 0.5f) *
                        anim.in_view(origin.y, 100.0f);
        ImGui::Dummy(ImVec2(avail, h));
        const ImVec2 rmax(origin.x + avail, origin.y + h);
        dl->AddRectFilled(origin, rmax,
                          ui_with_alpha(ui_col(ui_c().surface_hover), t), m.radius_md);
        dl->AddRect(origin, rmax, ui_with_alpha(ui_border_u32(), 0.7f * t), m.radius_md, 0, 1.0f);
        ui_draw_icon(dl, UiIcon::Radio,
                     ImVec2(origin.x + avail * 0.5f, origin.y + 34.0f * m.layout_scale),
                     26.0f * m.layout_scale, ui_with_alpha(ui_text_muted_u32(), t), 1.4f);
        auto centred = [&](const char *text, float base, float y, ImU32 col) {
            ImVec2 ts = g_ui_fonts.body->CalcTextSizeA(base * m.text_scale, FLT_MAX, 0.0f, text);
            dl->AddText(g_ui_fonts.body, base * m.text_scale,
                        ImVec2(origin.x + (avail - ts.x) * 0.5f, y), ui_with_alpha(col, t), text);
        };
        centred("This station lost the signal", 18.0f, origin.y + 52.0f * m.layout_scale,
                ui_text_u32());
        centred("Check your connection and try again", 15.0f, origin.y + 76.0f * m.layout_scale,
                ui_text_muted_u32());

        const float bw = 128.0f * m.layout_scale;
        const float bh = m.control_h;
        if (ui_pill_button("retry_station", "Try again", UiIcon::Shuffle,
                       ImVec2(origin.x + (avail - bw) * 0.5f,
                              origin.y + h - bh - m.space_3),
                       ImVec2(bw, bh), false, t, true))
        {
            Track seed = seed_from_state(state);
            if (!seed.id.empty())
            {
                begin_station(state, seed, state.radio_seed_plays, false);
                ui_anim_request();
                return;
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h + m.space_3));
    }
    else if (tracks && !tracks->empty())
    {
        const std::string reveal_key = "radio:" + state.radio_seed_id;
        const uint64_t token = (uint64_t)std::hash<std::string>{}(state.radio_seed_id);
        double reveal_start = ui_anim_track_list_reveal(reveal_key.c_str(), *tracks, token);
        const double seed_gate = s_start + (double)ui_anim_delay(seed_done_base);
        if (seed_gate > reveal_start)
        {
            reveal_start = seed_gate;
            ui_anim_request();
        }

        for (int i = 0; i < (int)tracks->size(); ++i)
        {
            const Track &track = (*tracks)[i];
            TrackRowOptions opt;
            opt.show_rank = true;
            opt.rank = i + 1;
            opt.show_duration = true;
            opt.art_size = 46.0f * m.layout_scale;
            opt.reveal_start = reveal_start;
            opt.reveal_index = i;
            opt.auto_play_double_click = false;
            TrackRowResult row = ui_track_row(state, track, i, opt);
            if (row.clicked && !row.double_clicked && !row.ctrl_clicked && !row.shift_clicked)
            {
                state.current_playlist_source = PlaylistSource::RADIO;
                ui_play_track(state, track, i);
            }
        }
        ImGui::Dummy(ImVec2(0.0f, m.space_4));
    }

    draw_scroll_edge_fades();
}
