#include "ui.h"
#include "imgui_internal.h"
#include "ui_notify.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

// ----------------------------------------------------------------------
// Queue operations
// ----------------------------------------------------------------------

void queue_add_next(AppState &state, const Track &track)
{
    if (track.id.empty())
        return;
    Track pinned = track;
    pinned.from_queue_source = false;
    state.play_queue.insert(state.play_queue.begin(), pinned);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_add_end(AppState &state, const Track &track)
{
    if (track.id.empty())
        return;
    Track added = track;
    added.from_queue_source = false;
    state.play_queue.push_back(added);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_remove_at(AppState &state, int index)
{
    if (index < 0 || index >= (int)state.play_queue.size())
        return;
    state.play_queue.erase(state.play_queue.begin() + index);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_move(AppState &state, int from, int to)
{
    if (from < 0 || from >= (int)state.play_queue.size())
        return;
    to = std::max(0, std::min((int)state.play_queue.size() - 1, to));
    if (from == to)
        return;
    Track t = state.play_queue[from];
    state.play_queue.erase(state.play_queue.begin() + from);
    state.play_queue.insert(state.play_queue.begin() + to, std::move(t));
    state.queue_dirty = true;
    state.repaint_requested = true;
}

static std::vector<Track> g_queue_undo;
static AppState *g_queue_undo_state = nullptr;

static void undo_queue_clear()
{
    if (!g_queue_undo_state || g_queue_undo.empty())
        return;
    g_queue_undo_state->play_queue = g_queue_undo;
    g_queue_undo_state->queue_dirty = true;
    g_queue_undo_state->repaint_requested = true;
    g_queue_undo.clear();
    ui_notify(UiNotifyType::Info, "Queue restored", "Your previous Up Next is back.", 4.0f);
}

void queue_clear(AppState &state)
{
    if (state.play_queue.empty())
        return;
    g_queue_undo = state.play_queue;
    g_queue_undo_state = &state;
    state.play_queue.clear();
    state.queue_dirty = true;
    state.repaint_requested = true;
    ui_notify(UiNotifyType::Info, "Queue cleared", "Up Next is empty.", 6.0f, "Undo",
              &undo_queue_clear);
}

void ui_invalidate_forward(AppState &state)
{
    state.nav_forward_valid = false;
}

void ui_go_back(AppState &state)
{
    // Remember where we are so ui_go_forward() can bring the user back.
    state.nav_forward.tab = state.current_tab;
    state.nav_forward.source = state.current_playlist_source;
    state.nav_forward.playlist_index = state.selected_playlist_index;
    if (state.current_tab == Tab::Album)
        state.nav_forward.album_id = state.album_browse_id;
    else
        state.nav_forward.album_id.clear();
    if (state.current_tab == Tab::Artist)
        state.nav_forward.artist_id = state.artist_browse_id;
    else
        state.nav_forward.artist_id.clear();
    state.nav_forward_valid = true;

    if (state.current_tab == Tab::Album)
    {
        state.current_tab = state.album_return_tab;
        state.current_playlist_source = state.album_return_source;
        if (state.current_tab == Tab::Playlists)
            state.selected_playlist_index = state.album_return_playlist_index;
    }
    else if (state.current_tab == Tab::Artist)
    {
        state.current_tab = state.artist_return_tab;
        state.current_playlist_source = state.artist_return_source;
        if (state.current_tab == Tab::Playlists)
            state.selected_playlist_index = state.artist_return_playlist_index;
    }
    else if (state.current_tab == Tab::Queue)
    {
        state.current_tab = state.pre_queue_tab;
    }
    else if (state.current_tab == Tab::Podcasts && !state.podcast_selected.browse_id.empty())
    {
        close_podcast_show(state);
    }
    else if (state.current_tab == Tab::Settings)
    {
        state.current_tab = Tab::Home;
        state.current_playlist_source = PlaylistSource::HOME;
    }
}

void ui_go_forward(AppState &state)
{
    if (!state.nav_forward_valid)
        return;
    const AppState::NavSnapshot fwd = state.nav_forward;
    state.nav_forward_valid = false;

    switch (fwd.tab)
    {
    case Tab::Album:
        // The album detail fields may have been repurposed by a newer view;
        // reopen (cache-backed, so it is instant) when they no longer match.
        if (fwd.album_id.empty() || fwd.album_id != state.album_browse_id)
            open_album_view(state, fwd.album_id);
        else
            state.current_tab = Tab::Album;
        break;
    case Tab::Artist:
        if (fwd.artist_id.empty() || fwd.artist_id != state.artist_browse_id)
            open_artist_view(state, fwd.artist_id);
        else
            state.current_tab = Tab::Artist;
        break;
    case Tab::Queue:
        state.pre_queue_tab = state.current_tab;
        state.current_tab = Tab::Queue;
        break;
    default:
        state.current_tab = fwd.tab;
        break;
    }
    state.current_playlist_source = fwd.source;
    if (fwd.source == PlaylistSource::PLAYLIST)
        state.selected_playlist_index = fwd.playlist_index;
}

// ----------------------------------------------------------------------
// Shared UI helpers
// ----------------------------------------------------------------------

void build_smart_view(AppState &state)
{
    // Track metadata comes from the play log (title/artist/thumbnail per
    // play event); play counts only know ids. Newest entry per id wins.
    std::unordered_map<std::string, const PlayEvent *> meta;
    meta.reserve(state.play_log.size());
    for (const PlayEvent &ev : state.play_log)
        meta.emplace(ev.id, &ev); // log is newest-first: first hit is newest

    std::vector<Track> out;
    if (state.smart_mode == 0)
    {
        // On Repeat: most-played tracks, all time, top 50.
        std::vector<std::pair<int, const PlayEvent *>> ranked;
        ranked.reserve(meta.size());
        for (const auto &[id, ev] : meta)
        {
            int plays = 1;
            auto it = state.play_counts.find(id);
            if (it != state.play_counts.end() && it->second > 0)
                plays = it->second;
            ranked.emplace_back(plays, ev);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &[plays, ev] : ranked)
        {
            if ((int)out.size() >= 50)
                break;
            Track t;
            t.id = ev->id;
            t.title = ev->title;
            t.artist = ev->artist;
            t.thumbnail_url = ev->thumbnail_url;
            t.url = "https://www.youtube.com/watch?v=" + ev->id;
            out.push_back(std::move(t));
        }
    }
    else
    {
        // Recently Played: most recent distinct tracks, top 100.
        std::unordered_set<std::string> seen;
        for (const PlayEvent &ev : state.play_log)
        {
            if ((int)out.size() >= 100)
                break;
            if (!seen.insert(ev.id).second)
                continue;
            Track t;
            t.id = ev.id;
            t.title = ev.title;
            t.artist = ev.artist;
            t.thumbnail_url = ev.thumbnail_url;
            t.url = "https://www.youtube.com/watch?v=" + ev.id;
            out.push_back(std::move(t));
        }
    }
    state.smart_view = std::make_shared<const std::vector<Track>>(std::move(out));
}

// ----------------------------------------------------------------------
// Playlist source helper. Returns an immutable snapshot; network-fed
// lists are shared_ptrs swapped atomically by worker threads, and
// main-thread vectors are wrapped with a non-owning alias.
// ----------------------------------------------------------------------
TrackListPtr alias_of(const std::vector<Track> &v)
{
    return TrackListPtr(&v, [](const std::vector<Track> *) {});
}

TrackListPtr get_playlist(const AppState &state)
{
    switch (state.current_playlist_source)
    {
    case PlaylistSource::HOME:
        return std::atomic_load(&state.recommendations);
    case PlaylistSource::SEARCH:
        return std::atomic_load(&state.search_results);
    case PlaylistSource::DOWNLOADS:
        return alias_of(state.downloaded_tracks);
    case PlaylistSource::RADIO:
        return std::atomic_load(&state.radio_results);
    case PlaylistSource::LIKED:
        return alias_of(state.liked_tracks);
    case PlaylistSource::PLAYLIST:
        if (state.selected_playlist_index >= 0 && state.selected_playlist_index < (int)state.playlists.size())
            return alias_of(state.playlists[state.selected_playlist_index].tracks);
        return nullptr;
    case PlaylistSource::ALBUM:
        return alias_of(state.album_tracks);
    case PlaylistSource::ARTIST:
        return alias_of(state.artist_songs);
    case PlaylistSource::PODCAST:
        return alias_of(state.podcast_tracks);
    case PlaylistSource::YOUTUBE_PLAYLIST:
        return std::atomic_load(&state.youtube_playlist_tracks);
    case PlaylistSource::HISTORY:
        if (state.history_view && !state.history_view->empty())
            return state.history_view;
        return alias_of(state.playback_history);
    case PlaylistSource::STATS:
        return alias_of(state.stats.tracks);
    case PlaylistSource::SMART:
        if (state.smart_view)
            return state.smart_view;
        return alias_of(state.playback_history);
    default:
        return nullptr;
    }
}

void resolve_current_track_links(AppState &state)
{
    if (state.current_track_id.empty() ||
        (!state.current_album_id.empty() && !state.current_artist_id.empty()))
        return;

    const std::string &id = state.current_track_id;
    auto scan = [&](const std::vector<Track> &tracks) -> bool
    {
        for (const auto &t : tracks)
        {
            if (t.id != id)
                continue;
            if (state.current_album_id.empty())
                state.current_album_id = t.album_id;
            if (state.current_artist_id.empty())
                state.current_artist_id = t.artist_id;
            return !state.current_album_id.empty() && !state.current_artist_id.empty();
        }
        return false;
    };

    auto recommendations = std::atomic_load(&state.recommendations);
    if (recommendations && scan(*recommendations))
        return;
    auto search_results = std::atomic_load(&state.search_results);
    if (search_results && scan(*search_results))
        return;
    auto radio_results = std::atomic_load(&state.radio_results);
    if (radio_results && scan(*radio_results))
        return;
    if (scan(state.downloaded_tracks) || scan(state.liked_tracks) ||
        scan(state.playback_history) || scan(state.album_tracks) ||
        scan(state.artist_songs) || scan(state.local_search_results))
        return;
    for (const auto &pl : state.playlists)
        if (scan(pl.tracks))
            return;
}

// ----------------------------------------------------------------------
// Smooth scrolling — wheel/touch input as pixels
// ----------------------------------------------------------------------
// main.cpp turns wheel events and touch drags into pixel deltas. Each frame it
// adds them here; these pixels are applied directly to the hovered child.
// ImGui's native wheel path is disabled on those children so there is exactly
// one scroll path and no target/easing lag.
static float g_scroll_pixel_accum = 0.0f;
static float g_frame_scroll_pixels = 0.0f;
static bool g_frame_scroll_consumed = false;
static float g_scroll_clamp_unapplied = 0.0f;

// Absolute sub-pixel scroll accumulator. ImGui rounds every scroll position
// to whole pixels (ImRound64 in CalcNextScrollFromScrollTargetAndClamp), so a
// per-frame delta fed on top of GetScrollY() loses its fraction whenever it
// is under 0.5px -- slow scrolling would freeze and then step. Accumulating
// the absolute value here and feeding SetScrollY(absolute) makes the rounding
// happen on the cumulative position instead, so the fractions carry over and
// motion stays one-to-one at any speed.
static float g_scroll_applied = 0.0f;
static bool g_scroll_applied_valid = false;
static std::string g_scroll_applied_window;
static int g_scroll_applied_frame = -1;

void add_scroll_pixels(float delta_pixels)
{
    g_scroll_pixel_accum += delta_pixels;
}

// Reported by smooth_scroll_child when the hovered child hit its scroll limit
// and could not apply all requested pixels; consumed by update_wheel_inertia()
// to keep the virtual scroll target inside the reachable range.
void scroll_clamp_feedback(float unapplied)
{
    g_scroll_clamp_unapplied += unapplied;
}

float take_scroll_clamp_feedback()
{
    const float unapplied = g_scroll_clamp_unapplied;
    g_scroll_clamp_unapplied = 0.0f;
    return unapplied;
}

// Called before ImGui::NewFrame() each frame.
void smooth_scroll_begin_frame()
{
    g_frame_scroll_pixels = g_scroll_pixel_accum;
    g_scroll_pixel_accum = 0.0f;
    g_frame_scroll_consumed = false;
}

void smooth_scroll_child()
{
    if (g_frame_scroll_consumed || g_frame_scroll_pixels == 0.0f)
        return;
    if (!ImGui::IsWindowHovered())
        return;
    g_frame_scroll_consumed = true;

    // Continuous consumption: same child received pixels on the previous
    // consuming frame, so keep accumulating sub-pixel. Any discontinuity --
    // a new gesture, or the hover moving to a different child -- re-bases on
    // the child's actual scroll so external changes (scrollbar drag, tab
    // switch) are honoured.
    const char *name = ImGui::GetCurrentWindow()->Name;
    const int frame = ImGui::GetFrameCount();
    const bool continuous = g_scroll_applied_valid && name &&
                            g_scroll_applied_window == name &&
                            frame == g_scroll_applied_frame + 1;
    g_scroll_applied_window = name ? name : "";
    g_scroll_applied_valid = true;
    g_scroll_applied_frame = frame;

    const float base = continuous ? g_scroll_applied : ImGui::GetScrollY();
    const float max_scroll = ImGui::GetScrollMaxY();
    const float requested = base + g_frame_scroll_pixels;
    const float applied = std::max(0.0f, std::min(requested, max_scroll));
    g_scroll_applied = applied;
    ImGui::SetScrollY(applied);
    if (applied != requested)
        scroll_clamp_feedback(applied - requested);
}

// Lyrics panel animation flag (see ui.h). Set by draw_lyrics_panel while its
// auto-centre scroll or fade-in is in motion; consumed by the main loop to
// hold 60 FPS. Consuming (rather than just reading) means a panel that stops
// drawing -- e.g. the user switched to a non-list tab -- can't leave the loop
// pinned at the fast cadence.
static bool g_lyrics_animating = false;

void set_lyrics_animating(bool active)
{
    g_lyrics_animating = active;
}

bool is_lyrics_animating()
{
    bool active = g_lyrics_animating;
    g_lyrics_animating = false;
    return active;
}

// ----------------------------------------------------------------------
// Motion helpers
// ----------------------------------------------------------------------
int g_ui_animation_level = 2;
static bool g_ui_animating = false;

void ui_anim_request()
{
    g_ui_animating = true;
}

bool is_ui_animating()
{
    bool active = g_ui_animating;
    g_ui_animating = false;
    return active;
}

// Page-epoch: bumped whenever the visible page changes (tab switch, search
// reset). Panel entrance animations use it to decide "replay the intro",
// which used to be inferred from gaps in the ImGui frame counter. That proxy
// breaks now that the frame loop skips renders while idle (a safety frame
// would replay every intro on screen), while a page change always coincides
// with real input, so it is always observed on a live frame.
static uint64_t g_page_epoch = 0;
uint64_t ui_page_epoch()
{
    return g_page_epoch;
}
void ui_bump_page_epoch()
{
    ++g_page_epoch;
}

float ui_anim_ease_out(float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

float ui_anim_clamp01f(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float ui_anim_ease_out_back(float t)
{
    t = ui_anim_clamp01f(t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

static float anim_factor()
{
    switch (g_ui_animation_level)
    {
    case 1: return 0.70f;
    case 3: return 1.55f;
    default: return 1.0f;
    }
}

float ui_anim_duration(float base_seconds)
{
    return g_ui_animation_level > 0 ? base_seconds * anim_factor() : 0.0f;
}

float ui_anim_delay(float base_seconds)
{
    return ui_anim_duration(base_seconds);
}

float ui_anim_offset(float base_px)
{
    return g_ui_animation_level > 0 ? base_px * anim_factor() : 0.0f;
}

float ui_anim_approach(float current, float target, float speed)
{
    if (!ui_animations_enabled())
        return target;
    float dt = ImGui::GetIO().DeltaTime;
    if (!(dt > 0.0f) || dt > 0.1f)
        dt = 1.0f / 60.0f;
    const float t = 1.0f - expf(-speed * dt);
    return current + (target - current) * t;
}

namespace
{
struct WidthAnim
{
    bool init = false;
    float from = 0.0f;
    float to = 0.0f;
    float current = 0.0f;
    float last_target = -1.0f;
    double start = 0.0;
};

float anim_width(WidthAnim &st, float target, float base_duration)
{
    const double now = ImGui::GetTime();
    if (!st.init)
    {
        st.init = true;
        st.from = st.to = st.current = target;
        st.last_target = target;
        st.start = now;
        return st.current;
    }
    if (fabsf(target - st.last_target) > 0.01f)
    {
        st.from = st.current;
        st.to = target;
        st.last_target = target;
        st.start = now;
    }
    const float dur = ui_anim_duration(base_duration);
    float t = dur > 0.0f ? (float)((now - st.start) / dur) : 1.0f;
    st.current = st.from + (st.to - st.from) * ui_anim_ease_out(t);
    if (t < 1.0f)
        ui_anim_request();
    return st.current;
}
} // namespace

float ui_anim_sidebar_width(float target_width)
{
    static WidthAnim st;
    return anim_width(st, target_width, 0.26f);
}

float ui_anim_lyrics_width(float target_width)
{
    static WidthAnim st;
    return anim_width(st, target_width, 0.28f);
}

struct ListRevealState
{
    uint64_t token = 0;
    size_t count = 0;
    std::string first;
    std::string last;
    double start = -1.0;
    uint64_t epoch = (uint64_t)-1; // page epoch the list was last drawn under
};

// (last_frame removed: with idle frame-skipping, frame-number gaps no longer
// mean "not drawn recently"; page changes are tracked via ui_page_epoch.)

static std::unordered_map<std::string, ListRevealState> g_list_reveals;

void ui_anim_reset_list_reveals()
{
    g_list_reveals.clear();
}

struct ScrollRevealState
{
    int last_frame = -1;
    uint64_t epoch = (uint64_t)-1;
    double start = -1.0;
    int order = 0;
};

static std::unordered_map<std::string, ScrollRevealState> g_scroll_reveals;
// Per-scope (frame, next order) counter so rows that become visible together
// cascade instead of popping in as a block.
static std::unordered_map<int, std::pair<int, int>> g_scroll_reveal_batches;

double ui_anim_scroll_reveal(int scope, int index, const std::string &id, int *out_order)
{
    if (!ui_animations_enabled())
    {
        if (out_order)
            *out_order = 0;
        return -1.0;
    }

    std::string key = std::to_string(scope);
    key += '#';
    key += id.empty() ? std::to_string(index) : id;

    const int frame = ImGui::GetFrameCount();
    const uint64_t page_epoch = ui_page_epoch();
    ScrollRevealState &st = g_scroll_reveals[key];
    if (st.start < 0.0 || st.epoch != page_epoch)
    {
        st.start = ImGui::GetTime();
        auto &batch = g_scroll_reveal_batches[scope];
        if (batch.first != frame)
        {
            batch.first = frame;
            batch.second = 0;
        }
        st.order = batch.second++;
        ui_anim_request();
    }
    st.epoch = page_epoch;
    st.last_frame = frame;
    if (out_order)
        *out_order = st.order;
    return st.start;
}

double ui_anim_list_reveal(const char *key, uint64_t token, size_t count,
                           const std::string &first_id, const std::string &last_id)
{
    if (!ui_animations_enabled() || count == 0 || !key || !*key)
        return -1.0;

    const uint64_t page_epoch = ui_page_epoch();
    ListRevealState &st = g_list_reveals[key];
    // Restart the reveal whenever the page changed since the list was last
    // drawn (tab/view switch, returning from lyrics-only, etc.) so the
    // slide-down plays every time the list is shown, not just on the very
    // first load.
    if (st.start < 0.0 || st.epoch != page_epoch ||
        st.token != token || st.count != count ||
        st.first != first_id || st.last != last_id)
    {
        st.token = token;
        st.count = count;
        st.first = first_id;
        st.last = last_id;
        st.start = ImGui::GetTime();
    }
    st.epoch = page_epoch;

    const double elapsed = ImGui::GetTime() - st.start;
    const double total = (double)ui_anim_duration(0.22f) +
                         (double)ui_anim_delay(0.018f * 10.0f) + 0.03;
    if (elapsed < total)
        ui_anim_request();
    return st.start;
}

double ui_anim_track_list_reveal(const char *key, const std::vector<Track> &tracks,
                                 uint64_t token)
{
    if (tracks.empty())
        return -1.0;
    return ui_anim_list_reveal(key, token, tracks.size(),
                               tracks.front().id, tracks.back().id);
}

static std::unordered_map<std::string, double> g_texture_reveals;

float ui_anim_texture_alpha(const std::string &url)
{
    if (!ui_animations_enabled() || url.empty())
        return 1.0f;

    const double now = ImGui::GetTime();
    auto it = g_texture_reveals.find(url);
    if (it == g_texture_reveals.end())
    {
        it = g_texture_reveals.emplace(url, now).first;
    }
    const float dur = ui_anim_duration(0.18f);
    float t = dur > 0.0f ? (float)((now - it->second) / dur) : 1.0f;
    if (t < 1.0f)
        ui_anim_request();
    return ui_anim_ease_out(t);
}

float ui_content_edge_alpha(float top_y, float bottom_y)
{
    const float max_scroll = ImGui::GetScrollMaxY();
    if (max_scroll <= 1.0f)
        return 1.0f;

    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    if (size.y <= 1.0f)
        return 1.0f;

    const float scroll = ImGui::GetScrollY();
    const float fade = std::min(ui_px(26.0f), size.y * 0.18f);
    float alpha = 1.0f;
    if (scroll > 1.0f && top_y < pos.y + fade)
        alpha = std::min(alpha, std::clamp((top_y - pos.y) / fade, 0.0f, 1.0f));
    if (scroll < max_scroll - 1.0f && bottom_y > pos.y + size.y - fade)
        alpha = std::min(alpha, std::clamp(((pos.y + size.y) - bottom_y) / fade, 0.0f, 1.0f));
    return alpha;
}

