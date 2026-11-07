#include "tasks.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include <SDL2/SDL.h>
#include <GL/gl.h>
#include <cstdio>
#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <map>
#include <set>
#include <vector>
#include <string>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <csignal>
#ifndef P100_WINDOWS
#include <unistd.h>
#endif
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <random>
#include <cmath>
#include <unordered_set>
#include "version.h"
#include "accent.h"
#include "paths.h"
#include "ytdlp.h"
#include "mpvplayer.h"
#include "crossfade.h"
#include "ui.h"
#include "ui_backdrop.h"
#include "texture.h"
#include "mpris.h"
#include "ui_notify.h"
#include "updater.h"
#include "library.h"
#include "diagnostics.h"
#include "writeback.h"
#include "discord_rpc.h"
#include "power.h"

// ---- libnotify (Linux desktop notifications; no-op on Windows) ----
#ifndef P100_WINDOWS
#include <libnotify/notify.h>
#endif

#define DLOG(...) do { fputs("[p100] ", stderr); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)
#define DNOW() ((long long)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()))

// ----------------------------------------------------------------------
// Global state and player
// ----------------------------------------------------------------------
static AppState g_state;
static std::unique_ptr<MpvPlayer> g_player;
// MPRIS holds a reference to this pointer, so it follows player role swaps
// during crossfade (see maybe_start_crossfade).
static MpvPlayer *g_player_raw = nullptr;
// Second mpv instance used to fade in the next track while the main one
// fades out; after the swap the roles reverse for the next crossfade.
static std::unique_ptr<MpvPlayer> g_fade_player;
static class MprisPlayer *g_mpris = nullptr;
// The main and fade player event handlers, stored so they can be re-bound
// when the two players swap roles at the end of a crossfade (each MpvPlayer
// keeps the callback it was given, but roles move between the objects).
static std::function<void(const std::string &)> g_main_event_cb;
static std::function<void(const std::string &)> g_fade_event_cb;
static std::mutex g_data_mutex;

// SIGTERM/SIGINT request a clean shutdown (save session, stop workers, tear
// down GL) instead of leaving the process to be killed mid-write.
static volatile std::sig_atomic_t g_quit_requested = 0;
static void handle_quit_signal(int)
{
    g_quit_requested = 1;
}

// Party HTTP workers call this after pushing work into the party inbox. It
// must wake SDL_WaitEventTimeout() without touching ImGui from a worker
// thread, so it posts an empty user event; the main loop then drains the
// inbox on its own thread.
static void party_wake_event()
{
    SDL_Event event{};
    event.type = SDL_USEREVENT;
    SDL_PushEvent(&event);
}

// ----------------------------------------------------------------------
// Global UI scale (Settings -> "UI scale" + per-aspect sliders).
// The layout heights below are globals so they can track the sliders live,
// and ui_px() (ui.h) scales every other explicit pixel value by the layout
// factor. FontScaleMain handles text and ScaleAllSizes the widget metrics.
// ----------------------------------------------------------------------
float g_ui_layout_scale = 1.0f; // effective layout factor (master * layout)
std::unordered_map<std::string, float> g_ui_elem; // resolved per-element multipliers
static std::string g_imgui_ini_path;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void apply_ui_scale(const Settings &s)
{
    ui_theme_apply(s, ImGui::GetIO());
    g_ui_layout_scale = ui_m().layout_scale;
    // Low power mode uses the "motion off" profile for this session without
    // touching the user's saved animation preference.
    g_ui_animation_level = power::low_power()
                               ? 0
                               : std::max(0, std::min(3, s.animations));
    power::configure(s.low_power, s.low_power_on_battery);
    power::refresh();

    g_ui_elem.clear();
    for (const auto &kv : s.ui_elements)
        g_ui_elem[kv.first] = clampf(kv.second, 0.25f, 3.0f);

}

struct PlayRequest
{
    std::string path;   // local file or resolved stream URL
    std::string id;     // video id (for the offline cache)
    bool remote;        // true when path is a network stream to be recorded
    std::uint64_t gen = 0; // play generation the request was issued for
};
static std::queue<PlayRequest> g_play_queue;
static std::mutex g_play_queue_mutex;
// Bumped by begin_play_track(): every selection invalidates resolves still
// in flight for an earlier selection. A slow (non-cached) resolve that
// finishes after the user/EOF moved on must not start playing its stale
// track on top of the current one.
static std::atomic<std::uint64_t> g_play_generation{0};

// Pre-resolved stream URL for the upcoming track, so advancing at end of
// track doesn't wait on a network round-trip.
static std::map<std::string, std::string> g_prefetched_urls; // video_id -> stream URL
static std::mutex g_prefetch_mutex;

// Endless radio: one refill attempt per playing track. The result is
// published through AppState and consumed on the main thread.
static bool g_endless_waiting = false;
static std::string g_endless_seed;
// Crossfade state and pure decisions live in playback/crossfade.*. The mpv
// role swap and event handling stay here with the rest of the playback loop.
static crossfade::State g_cf;

// Window visibility: when minimized/hidden the UI is not visible, so the
// render path is skipped entirely and the loop only wakes for events and
// low-rate housekeeping (playback, saves, MPRIS keep running).
static bool g_window_hidden = false;
// Dirty flags for the periodic persistence pass: history / play counts /
// play log only change on track starts and commits, so the 10 s save pass
// skips them (and the settings file) unless something actually changed.
static std::atomic<bool> g_history_dirty{false};
static std::atomic<bool> g_counts_dirty{false};

// Watchdog for the resolve path: loading is cleared by mpv's start/error
// events; if neither ever fires (worker died, backend wedged) this caps how
// long the 60 FPS spinner cadence can be pinned.
static std::chrono::steady_clock::time_point g_loading_since;
static constexpr double kLoadingWatchdogSec = 30.0;

// ---- Discord Rich Presence ----
// Env var wins over the Settings field so a headless/production install can
// force an id without editing settings.json.
static void discord_apply_client_id()
{
    const char *env = getenv("P100_DISCORD_CLIENT_ID");
    discord_rpc_start(env && *env ? env : g_state.settings.discord_client_id);
}

// While a crossfade runs, the incoming track lives on the helper player.
// UI/MPRIS/Discord should describe that player, not the one fading out.
static MpvPlayer *display_player()
{
    if ((g_cf.pending.load() || g_cf.active.load()) && g_fade_player &&
        g_state.crossfade_progress >= 0.5f)
        return g_fade_player.get();
    return g_player.get();
}

static void update_discord_presence()
{
    DiscordPresence p;
    p.active = g_state.playing && !g_state.current_track_id.empty();
    p.title = g_state.current_title;
    p.artist = g_state.current_artist;
    p.image_url = g_state.current_thumbnail_url;
    if (p.active)
    {
        MpvPlayer *dp = display_player();
        const double dur = dp ? dp->cached_duration() : 0.0;
        const double pos = dp ? std::max(0.0, dp->cached_time_pos()) : 0.0;
        if (dur > 0.0)
        {
            const long long now_ts = (long long)time(nullptr);
            p.start_ts = now_ts - (long long)pos;
            p.end_ts = p.start_ts + (long long)dur;
        }
    }
    discord_rpc_update(p);
}

// Cancel an in-flight crossfade: restore the main player's volume and
// silence/stop the helper. Safe to call when no fade is running.
static void stop_fade()
{
    if (!g_fade_player)
        return;
    if (g_cf.pending || g_cf.active)
        g_player->set_volume(g_cf.user_volume);
    g_cf.pending = false;
    g_cf.active = false;
    g_cf.manual = false;
    g_cf.track = Track{};
    g_cf.index = -1;
    g_fade_player->set_stream_record("");
    g_fade_player->stop();
    g_fade_player->set_volume(0);
}

static bool g_endless_attempted = false;

// Fresh stream re-resolution after an uncached track fails to load.
static bool g_force_fresh_resolve = false;
static int g_play_retry_count = 0;

// Touchscreen: SDL's synthetic mouse events are disabled, so taps and
// one-finger drags are translated here. A tap becomes a real click across
// two frames; a long press becomes a right-click.
struct TouchGesture
{
    bool active = false;
    SDL_FingerID id = 0;
    float start_x = 0.0f, start_y = 0.0f;
    float last_x = 0.0f, last_y = 0.0f;
    Uint32 start_ticks = 0;
    Uint32 last_move_ticks = 0;
    bool scrolling = false;
    bool long_fired = false;
    float velocity = 0.0f; // pixels/second in scroll direction
};

static TouchGesture g_touch;
static float g_touch_fling_velocity = 0.0f;
static bool g_touch_fling_active = false;
static Uint32 g_touch_fling_last_ms = 0;

// Wheel/touchpad scrolling — browser model. A virtual scroll target
// integrates wheel deltas 1:1 (plus post-release kinetic fling); the rendered
// position chases the target with a critically-damped filter every frame, so
// unevenly-spaced touchpad packets become continuous sub-pixel motion and
// even tiny flicks glide. Children report edge clamping back via
// scroll_clamp_feedback() so the target stays in the reachable range.
static float g_scroll_target = 0.0f;        // virtual scroll position, px
static float g_scroll_pos = 0.0f;           // chased render position, px
static float g_wheel_velocity = 0.0f;      // active fling speed, px/s
static float g_wheel_input_velocity = 0.0f; // recent event speed, px/s
static bool g_wheel_inertia_active = false;
static Uint32 g_wheel_last_event_ms = 0;
static Uint32 g_wheel_last_frame_ms = 0;
static int g_wheel_event_count = 0;

static constexpr float kWheelPixelsPerDelta = 70.0f;
static constexpr float kWheelChaseTau = 0.05f; // input→display settle, s
static constexpr float kWheelFlingDecay = 3.0f;
static constexpr float kWheelFlingMin = 5.0f;
static constexpr float kWheelFlingMax = 5000.0f;
static constexpr Uint32 kWheelReleaseMs = 80;
static constexpr Uint32 kWheelGestureGapMs = 120;

// Horizontal two-finger swipe (back/forward navigation). See the
// SDL_MOUSEWHEEL handler for the threshold semantics.
static float g_hwheel_accum = 0.0f;
static Uint32 g_hwheel_last_ms = 0;
static constexpr float kHWheelTrigger = 2.0f; // wheel deltas per navigation step
static constexpr Uint32 kHWheelGapMs = 150;

static void reset_wheel_scroll()
{
    g_scroll_target = 0.0f;
    g_scroll_pos = 0.0f;
    g_wheel_velocity = 0.0f;
    g_wheel_input_velocity = 0.0f;
    g_wheel_inertia_active = false;
    g_wheel_last_event_ms = 0;
    g_wheel_event_count = 0;
}

enum class TouchClickPhase
{
    None,
    Down,
    Up
};

static TouchClickPhase g_touch_click_phase = TouchClickPhase::None;
static int g_touch_click_button = 0;
static ImVec2 g_touch_click_pos;
// ImGui::GetTime() of the last tap we synthesized. Track rows treat a recent
// tap as the "play" gesture (the double-click idiom has no touch analogue);
// see ui_touch_tap_recent().
double g_touch_click_time = -1.0e6;

static void queue_touch_click(float x, float y, int button)
{
    if (g_touch_click_phase != TouchClickPhase::None)
        return;
    g_touch_click_time = ImGui::GetTime();
    g_touch_click_phase = TouchClickPhase::Down;
    g_touch_click_button = button;
    g_touch_click_pos = ImVec2(x, y);
}

// Feed one half of a synthetic click per frame. Sending down and up in the
// same NewFrame would leave ImGui with no pressed transition, so the down
// lands now and the up lands on the next loop iteration.
static void process_touch_click()
{
    ImGuiIO &io = ImGui::GetIO();
    if (g_touch_click_phase == TouchClickPhase::Down)
    {
        io.AddMousePosEvent(g_touch_click_pos.x, g_touch_click_pos.y);
        io.AddMouseButtonEvent(g_touch_click_button, true);
        g_touch_click_phase = TouchClickPhase::Up;
    }
    else if (g_touch_click_phase == TouchClickPhase::Up)
    {
        io.AddMouseButtonEvent(g_touch_click_button, false);
        g_touch_click_phase = TouchClickPhase::None;
    }
}

static void handle_touch_event(const SDL_Event &event, SDL_Window *window)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return;
    auto px = [&](float nx) { return nx * (float)ww; };
    auto py = [&](float ny) { return ny * (float)wh; };

    if (event.type == SDL_FINGERDOWN)
    {
        if (g_touch.active)
            return;
        g_touch.active = true;
        g_touch.id = event.tfinger.fingerId;
        g_touch.start_x = g_touch.last_x = px(event.tfinger.x);
        g_touch.start_y = g_touch.last_y = py(event.tfinger.y);
        g_touch.start_ticks = SDL_GetTicks();
        g_touch.last_move_ticks = g_touch.start_ticks;
        g_touch.scrolling = false;
        g_touch.long_fired = false;
        g_touch.velocity = 0.0f;
        g_touch_fling_active = false;
        g_touch_fling_velocity = 0.0f;
        reset_wheel_scroll();
        ImGui::GetIO().AddMousePosEvent(g_touch.last_x, g_touch.last_y);
        return;
    }

    if (!g_touch.active || event.tfinger.fingerId != g_touch.id)
        return;

    if (event.type == SDL_FINGERMOTION)
    {
        const float x = px(event.tfinger.x);
        const float y = py(event.tfinger.y);
        const float dx = x - g_touch.start_x;
        const float dy = y - g_touch.start_y;
        if (!g_touch.scrolling && dx * dx + dy * dy > 14.0f * 14.0f)
            g_touch.scrolling = true;
        const Uint32 now_ms = SDL_GetTicks();
        const float pixel_dy = y - g_touch.last_y;
        if (g_touch.scrolling)
        {
            const float dt = (now_ms - g_touch.last_move_ticks) / 1000.0f;
            if (dt > 0.001f && dt < 0.2f)
            {
                const float instant = -pixel_dy / dt;
                // Low-pass the flick velocity so one jittery sample doesn't
                // dominate the momentum after the finger lifts.
                g_touch.velocity = g_touch.velocity * 0.65f + instant * 0.35f;
            }
            else if (dt >= 0.2f)
            {
                g_touch.velocity = -pixel_dy / std::max(dt, 0.001f);
            }
            // Direct pixel scroll: content follows the finger exactly.
            add_scroll_pixels(-pixel_dy);
        }
        g_touch.last_move_ticks = now_ms;
        g_touch.last_x = x;
        g_touch.last_y = y;
        ImGui::GetIO().AddMousePosEvent(x, y);
        return;
    }

    if (event.type == SDL_FINGERUP)
    {
        const float x = px(event.tfinger.x);
        const float y = py(event.tfinger.y);
        const bool tap = !g_touch.scrolling && !g_touch.long_fired;
        const Uint32 now_ms = SDL_GetTicks();
        float velocity = g_touch.velocity;
        // Holding still before lifting should not fling.
        if (now_ms - g_touch.last_move_ticks > 120)
            velocity = 0.0f;
        velocity = std::max(-5000.0f, std::min(velocity, 5000.0f));
        g_touch.active = false;
        if (g_touch.scrolling && !g_touch.long_fired && std::fabs(velocity) >= 80.0f)
        {
            g_touch_fling_velocity = velocity;
            g_touch_fling_active = true;
            g_touch_fling_last_ms = now_ms;
        }
        if (tap)
            queue_touch_click(x, y, ImGuiMouseButton_Left);
        return;
    }
}

static void update_touch_long_press()
{
    if (!g_touch.active || g_touch.scrolling || g_touch.long_fired)
        return;
    if (SDL_GetTicks() - g_touch.start_ticks < 550)
        return;
    g_touch.long_fired = true;
    queue_touch_click(g_touch.last_x, g_touch.last_y, ImGuiMouseButton_Right);
}

// Kinetic touch scrolling: keep moving after the finger lifts, with an
// exponential decay similar to a browser's fling.
static bool update_touch_inertia()
{
    if (!g_touch_fling_active)
        return false;
    if (g_touch.active)
    {
        g_touch_fling_active = false;
        g_touch_fling_velocity = 0.0f;
        return false;
    }
    const Uint32 now_ms = SDL_GetTicks();
    float dt = (now_ms - g_touch_fling_last_ms) / 1000.0f;
    g_touch_fling_last_ms = now_ms;
    if (dt <= 0.0f)
        return true;
    if (dt > 0.05f)
        dt = 0.05f;
    add_scroll_pixels(g_touch_fling_velocity * dt);
    g_touch_fling_velocity *= std::exp(-3.2f * dt);
    if (std::fabs(g_touch_fling_velocity) < 20.0f)
    {
        g_touch_fling_velocity = 0.0f;
        g_touch_fling_active = false;
        return false;
    }
    return true;
}

// Wheel/touchpad scrolling. Input is drained with a short time constant while
// events are arriving; kinetic inertia is only started after the pad goes
// quiet, using the speed measured from the events themselves.
static bool update_wheel_inertia()
{
    const Uint32 now_ms = SDL_GetTicks();
    const bool input_active = (now_ms - g_wheel_last_event_ms) <= kWheelReleaseMs;
    if (!input_active && !g_wheel_inertia_active &&
        std::fabs(g_scroll_target - g_scroll_pos) < 0.05f)
        return false;

    float dt = (now_ms - g_wheel_last_frame_ms) / 1000.0f;
    g_wheel_last_frame_ms = now_ms;
    if (dt <= 0.0f)
        return true;
    if (dt > 0.05f)
        dt = 0.05f;

    // Edge feedback from the last frame: the hovered child clamped the
    // scroll, so pull the target back into the reachable range and stop any
    // fling pushing into the edge.
    const float unapplied = take_scroll_clamp_feedback();
    if (unapplied != 0.0f)
    {
        g_scroll_target += unapplied;
        if (g_wheel_inertia_active)
        {
            g_wheel_velocity = 0.0f;
            g_wheel_inertia_active = false;
        }
    }

    if (input_active)
    {
        g_wheel_inertia_active = false;
        g_wheel_velocity = 0.0f;
    }
    else if (!g_wheel_inertia_active)
    {
        const float fling = std::max(-kWheelFlingMax,
                                     std::min(g_wheel_input_velocity, kWheelFlingMax));
        if (std::fabs(fling) >= kWheelFlingMin)
        {
            g_wheel_velocity = fling;
            g_wheel_inertia_active = true;
        }
        g_wheel_input_velocity = 0.0f;
        g_wheel_event_count = 0;
    }

    if (g_wheel_inertia_active)
    {
        g_scroll_target += g_wheel_velocity * dt;
        g_wheel_velocity *= std::exp(-kWheelFlingDecay * dt);
        if (std::fabs(g_wheel_velocity) < kWheelFlingMin)
        {
            g_wheel_velocity = 0.0f;
            g_wheel_inertia_active = false;
        }
    }

    // Critically-damped chase: the rendered position follows the target,
    // turning discrete wheel packets into continuous sub-pixel motion.
    const float chase = 1.0f - std::exp(-dt / kWheelChaseTau);
    const float new_pos = g_scroll_pos + (g_scroll_target - g_scroll_pos) * chase;
    add_scroll_pixels(new_pos - g_scroll_pos);
    g_scroll_pos = new_pos;

    if (!input_active && !g_wheel_inertia_active &&
        std::fabs(g_scroll_target - g_scroll_pos) < 0.05f)
    {
        reset_wheel_scroll();
        return false;
    }
    return true;
}

// Path of the in-progress stream recording (empty when not recording).
static std::string g_recording_path; // final destination
static std::string g_recording_temp; // where mpv is writing right now
static std::string g_recording_id;
// Source duration of the track being recorded (mpv's "duration" property of
// the remote stream), captured after its start event. Compared against the
// finished recording's duration before it's accepted into the cache.
static bool g_recording_started = false;
static double g_recording_duration = 0.0;
static bool notify_init_done = false;

// Session-restore seek that had to wait for the demuxer cache: seeking past
// the cached region would leave a hole in the stream recording, so the jump
// is issued once the target is covered (or when the deadline passes).
static double g_pending_restore_seek = -1.0;
static std::string g_pending_restore_seek_id;
static std::uint64_t g_pending_restore_seek_gen = 0;
static std::chrono::steady_clock::time_point g_pending_restore_seek_deadline;

// ----------------------------------------------------------------------
// Notification helper
// ----------------------------------------------------------------------
static void show_notification(const std::string &title, const std::string &body, const std::string &icon_path = "")
{
#ifndef P100_WINDOWS
    if (!notify_init_done || !g_state.settings.show_notifications)
        return;

    static NotifyNotification *n = nullptr;
    if (!n)
    {
        n = notify_notification_new(title.c_str(), body.c_str(),
                                    icon_path.empty() ? nullptr : icon_path.c_str());
        notify_notification_set_timeout(n, 5000);
    }
    else
    {
        notify_notification_update(n, title.c_str(), body.c_str(),
                                   icon_path.empty() ? nullptr : icon_path.c_str());
        notify_notification_set_timeout(n, 5000);
    }

    GError *error = nullptr;
    if (!notify_notification_show(n, &error))
    {
        if (error && strstr(error->message, "MaxNotificationsExceeded") == nullptr)
        {
            fprintf(stderr, "Notification error: %s\n", error->message);
        }
        if (error)
            g_error_free(error);
    }
#endif
    (void)title;
    (void)body;
    (void)icon_path;
}

// ----------------------------------------------------------------------
// Playlist helpers. Returns an immutable snapshot of the current source
// list. Network-populated lists are shared_ptrs swapped atomically by
// worker threads, so callers get a stable copy they can hold across
// frames. Main-thread vectors (liked, playlists, history, downloads)
// are wrapped with a non-owning alias (alias_of).
// ----------------------------------------------------------------------
static TrackListPtr get_playlist()
{
    switch (g_state.current_playlist_source)
    {
    case PlaylistSource::HOME:
        return std::atomic_load(&g_state.recommendations);
    case PlaylistSource::SEARCH:
        return std::atomic_load(&g_state.search_results);
    case PlaylistSource::DOWNLOADS:
        return alias_of(g_state.downloaded_tracks);
    case PlaylistSource::RADIO:
        return std::atomic_load(&g_state.radio_results);
    case PlaylistSource::LIKED:
        return alias_of(g_state.liked_tracks);
    case PlaylistSource::YOUTUBE_PLAYLIST:
        return std::atomic_load(&g_state.youtube_playlist_tracks);
    case PlaylistSource::PLAYLIST:
        if (g_state.selected_playlist_index >= 0 && g_state.selected_playlist_index < (int)g_state.playlists.size())
            return alias_of(g_state.playlists[g_state.selected_playlist_index].tracks);
        return nullptr;
    case PlaylistSource::ALBUM:
        return alias_of(g_state.album_tracks);
    case PlaylistSource::ARTIST:
        return alias_of(g_state.artist_songs);
    case PlaylistSource::HISTORY:
        if (g_state.history_view && !g_state.history_view->empty())
            return g_state.history_view;
        return alias_of(g_state.playback_history);
    case PlaylistSource::STATS:
        return alias_of(g_state.stats.tracks);
    case PlaylistSource::SMART:
        return std::atomic_load(&g_state.smart_view);
    default:
        return nullptr;
    }
}

// Materialize the active source list into the single global queue. A UI play
// action sets queue_replace_from_source; this runs once before the pending
// track is resolved. The current track is played directly and the rest of the
// list becomes Up Next, so manual skips and natural advances share one order.
static void rebuild_queue_from_source()
{
    if (!g_state.queue_replace_from_source)
        return;
    g_state.queue_replace_from_source = false;

    const std::string want_id = g_state.queue_replace_id;
    g_state.queue_replace_id.clear();
    int start = g_state.queue_replace_index;
    g_state.queue_replace_index = 0;

    g_state.play_queue.clear();
    g_state.queue_cycle.clear();

    auto playlist = get_playlist();
    if (!playlist || playlist->empty())
    {
        g_state.queue_dirty = true;
        return;
    }

    if (!want_id.empty())
    {
        // Row indices can drift (History is a snapshot), so trust the track
        // id first and use the index only as a fallback.
        for (int i = 0; i < (int)playlist->size(); ++i)
        {
            if ((*playlist)[i].id == want_id)
            {
                start = i;
                break;
            }
        }
    }
    if (start < 0 || start >= (int)playlist->size())
        start = 0;

    g_state.queue_cycle = *playlist;
    for (Track &t : g_state.queue_cycle)
        t.from_queue_source = true;

    g_state.play_queue.reserve(playlist->size() - (size_t)start);
    for (int i = start + 1; i < (int)playlist->size(); ++i)
    {
        Track t = (*playlist)[i];
        t.from_queue_source = true;
        g_state.play_queue.push_back(std::move(t));
    }

    if (g_state.settings.shuffle_enabled && g_state.play_queue.size() > 1)
    {
        static std::mt19937 rng(
            (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        std::shuffle(g_state.play_queue.begin(), g_state.play_queue.end(), rng);
    }

    g_state.queue_dirty = true;
    g_state.repaint_requested = true;
}

// ----------------------------------------------------------------------
// Background audio resolver
// ----------------------------------------------------------------------
// Why the last stream resolve failed (classified backend error), consumed by
// the mpv error handler to give the "Playback failed" toast a real reason.
static std::mutex g_play_fail_mutex;
static std::string g_play_fail_hint;

static void resolve_audio(const std::string &video_id, const std::string &video_url,
                          const std::string &title, const std::string &artist,
                          const std::string &thumb, bool fresh = false,
                          std::uint64_t gen = 0)
{
    (void)title;
    (void)artist;
    (void)thumb;
    PlayRequest req;
    req.id = video_id;
    req.gen = gen;
    req.path = find_cached_audio_path(video_id);
    req.remote = false;
    if (req.path.empty())
    {
        // Not cached: play the resolved stream URL immediately. The bytes
        // are saved into the offline cache as they stream (mpv
        // stream-record), so there's no second download.
        {
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            if (fresh)
                g_prefetched_urls.erase(video_id);
            else
            {
                auto it = g_prefetched_urls.find(video_id);
                // An empty value is an in-flight prefetch: resolving the
                // same track ourselves below is fine, the worker's result
                // will simply serve a later request.
                if (it != g_prefetched_urls.end() && !it->second.empty())
                {
                    req.path = it->second;
                    g_prefetched_urls.erase(it);
                }
            }
        }
        if (req.path.empty())
        {
            const bool user_facing = g_state.pending_manual_play.load();
            req.path = fresh ? fetch_stream_url_fresh(video_id)
                             : fetch_stream_url(video_id);
            if (req.path.empty())
            {
                // The backend couldn't resolve a URL. Tell the user why when
                // this was their own play request (speculative prefetch
                // failures stay silent, they retry later anyway).
                BackendError err = take_backend_error();
                if (err && user_facing)
                {
                    ui_notify_backend_error(err);
                    std::lock_guard<std::mutex> lock(g_play_fail_mutex);
                    g_play_fail_hint = backend_error_user_message(err);
                }
                req.path = video_url; // mpv resolves this itself as a fallback
            }
        }
        req.remote = true;
    }
    {
        std::lock_guard<std::mutex> lock(g_play_queue_mutex);
        g_play_queue.push(std::move(req));
    }
}

// Resolve upcoming stream URLs ahead of time so clicking a track or reaching
// the end of the current one starts without the ~2-4s extraction round-trip.
// Covers the next few queue/playlist entries plus the heads of freshly
// loaded lists (search results, radio, home). URLs are probed by the backend
// before being returned, so a cached entry here is a playable one.
//
// One serialized worker on the main backend channel instead of a thread per
// track: resolves run one at a time (self-throttled), never touch the play
// channel a user click depends on, and `cancel_search_id` aborts the
// remaining entries the moment a newer search supersedes them.
void prefetch_urls_for(const std::vector<Track> &tracks, size_t count,
                       int cancel_search_id)
{
    if (power::low_power())
        return; // no speculative network work in low power mode
    std::vector<std::string> ids;
    ids.reserve(std::min(count, tracks.size()));
    for (const Track &t : tracks)
    {
        if (ids.size() >= count)
            break;
        if (t.id.empty() || !find_cached_audio_path(t.id).empty())
            continue;
        {
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            if (g_prefetched_urls.count(t.id))
                continue;
            g_prefetched_urls[t.id]; // reserve the slot (empty = in flight)
        }
        ids.push_back(t.id);
    }
    if (ids.empty())
        return;

    tasks::spawn([ids = std::move(ids), cancel_search_id]()
    {
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (cancel_search_id != 0 && g_state.search_id != cancel_search_id)
            {
                // Superseded: release the slots not yet resolved (completed
                // URLs stay warmed) so a future list can warm them again.
                std::lock_guard<std::mutex> lock(g_prefetch_mutex);
                for (size_t j = i; j < ids.size(); ++j)
                    g_prefetched_urls.erase(ids[j]);
                return;
            }
            std::string url = fetch_stream_url_prefetch(ids[i]);
            std::lock_guard<std::mutex> lock(g_prefetch_mutex);
            if (url.empty())
                g_prefetched_urls.erase(ids[i]); // failed: allow a retry later
            else
                g_prefetched_urls[ids[i]] = url;
        }
    });
}

// Warm stream URLs one at a time. The backend handles commands serially, so
// firing ten independent prefetch threads (as prefetch_urls_for does) can put
// a user's "start this station" request behind a long queue. This variant
// keeps at most one speculative resolve in flight and stops as soon as the
// caller's request token changes.
void prefetch_urls_for_serial(const std::vector<Track> &tracks, size_t count,
                              const std::atomic<uint64_t> *cancel_token,
                              uint64_t token)
{
    if (power::low_power())
        return; // no speculative network work in low power mode
    std::vector<std::string> ids;
    ids.reserve(std::min(count, tracks.size()));
    for (const Track &t : tracks)
    {
        if (ids.size() >= count)
            break;
        if (t.id.empty() || !find_cached_audio_path(t.id).empty())
            continue;
        ids.push_back(t.id);
    }
    if (ids.empty())
        return;

    tasks::spawn([ids = std::move(ids), cancel_token, token]() {
        for (const std::string &id : ids)
        {
            if (cancel_token && cancel_token->load() != token)
                return;
            {
                std::lock_guard<std::mutex> lock(g_prefetch_mutex);
                if (g_prefetched_urls.count(id))
                    continue; // already warmed or another worker is on it
                g_prefetched_urls[id]; // reserve the in-flight slot
            }

            const std::string url = fetch_stream_url(id);
            {
                std::lock_guard<std::mutex> lock(g_prefetch_mutex);
                if (url.empty())
                    g_prefetched_urls.erase(id);
                else
                    g_prefetched_urls[id] = url;
            }

            if (cancel_token && cancel_token->load() != token)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
    });
}

// The tracks playback will touch next: the head of the single global queue.
static std::vector<Track> upcoming_tracks()
{
    std::vector<Track> out;
    const size_t n = std::min<size_t>(3, g_state.play_queue.size());
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        out.push_back(g_state.play_queue[i]);
    return out;
}

static void prefetch_next_track()
{
    prefetch_urls_for(upcoming_tracks(), 3);
}

// Cover sweep: a downloaded track's album cover is fetched the first time a
// row showing it is drawn, so one transient failure (offline moment, network
// hiccup) leaves it permanently blank -- the texture loader only retries a
// failed URL while that row is on screen and its backoff has expired. This
// sweep re-fetches missing covers for every downloaded track in the
// background; it is stat-only when everything is already cached.
static std::atomic<bool> g_cover_sweep_running{false};
static void cover_sweep_worker(std::vector<Track> tracks)
{
    for (const auto &t : tracks)
    {
        if (t.id.empty() || t.thumbnail_url.empty())
            continue;
        if (!thumbnail_is_cached(t.thumbnail_url))
            get_cached_thumbnail_path(t.thumbnail_url);
    }
    g_cover_sweep_running = false;
    g_state.repaint_requested = true;
}

static void maybe_sweep_missing_covers()
{
    if (power::low_power())
        return;
    if (!g_state.downloaded_loaded || g_state.downloaded_tracks.empty())
        return;
    if (g_cover_sweep_running.exchange(true))
        return;
    tasks::spawn([tracks = g_state.downloaded_tracks]() { cover_sweep_worker(tracks); });
}

// Full downloads currently running (video_id -> nothing). Guards against
// stacking duplicate downloads for the same track.
static std::set<std::string> g_downloading;
static std::mutex g_download_mutex;

// Download a track fully into the audio cache in the background. Used to
// pre-fetch the *next* track (so advancing always starts from disk, Spotify
// style) and to repair recordings invalidated by seeks/skips.
// At most this many yt-dlp downloads run at once. Each is a Python
// subprocess (CPU + network burst); letting a track change fan out several
// concurrently stole noticeable power, and the losers of the race get
// retried on the next predownload pass anyway.
static std::atomic<int> g_bg_download_count{0};
static constexpr int kMaxBgDownloads = 2;

static void background_download(const std::string &video_id)
{
    if (power::low_power())
        return; // low power: never download ahead of playback
    if (video_id.empty())
        return;
    if (!find_cached_audio_path(video_id).empty())
        return;
    {
        std::lock_guard<std::mutex> lock(g_download_mutex);
        if (!g_downloading.insert(video_id).second)
            return; // already running
        if (g_bg_download_count.load() >= kMaxBgDownloads)
        {
            g_downloading.erase(video_id);
            return; // retried by the next predownload pass
        }
    }
    g_bg_download_count.fetch_add(1);
    tasks::spawn([video_id]()
                {
        std::string path = get_cached_audio_path(video_id);
        g_bg_download_count.fetch_sub(1);
        {
            std::lock_guard<std::mutex> lock(g_download_mutex);
            g_downloading.erase(video_id);
        }
        if (!path.empty())
            g_downloads_dirty = true; });
}

// While the current song plays, make sure the next one will start from the
// local cache: resolve its URL and pull down the whole file in the
// background (a few MB, seconds at typical bandwidth).
static void predownload_next_track()
{
    if (power::low_power())
        return;
    for (const Track &t : upcoming_tracks())
        background_download(t.id);
}

// Container extension for a resolved stream URL (mpv stream-record writes
// the raw stream, so the label must match the mime type). The result is a
// best-effort guess: finalize_recording() re-checks the finished bytes with
// sniff_audio_container_ext() and renames to the real container if wrong.
static std::string stream_container_ext(const std::string &url)
{
    // Case-insensitive copy: the mime type arrives raw or percent-encoded
    // and the URL's casing is not guaranteed.
    std::string u = url;
    for (char &c : u)
        c = (char)tolower((unsigned char)c);
    auto has = [&](const char *needle)
    { return u.find(needle) != std::string::npos; };

    if (has("mime=audio%2fmp4") || has("mime=audio/mp4"))
        return "m4a";
    if (has("mime=audio%2fmpeg") || has("mime=audio/mpeg"))
        return "mp3";
    if (has("mime=audio%2fwebm") || has("mime=audio/webm"))
        return "webm";
    if (has("mime=audio%2fogg") || has("mime=audio/ogg"))
        return "ogg";

    // Fall back to the YouTube audio itag when the mime param is absent.
    if (size_t p = u.find("itag="); p != std::string::npos && p + 5 < u.size())
    {
        const int itag = atoi(u.c_str() + p + 5);
        switch (itag)
        {
        case 139: // AAC in MP4
        case 140:
        case 141:
            return "m4a";
        case 170: // Vorbis/Opus in WebM
        case 171:
        case 249:
        case 250:
        case 251:
            return "webm";
        }
    }
    return "webm"; // YouTube's default audio-only muxer
}

// The current stream played through to the end: keep the recording and
// write its metadata sidecar (off-thread; the fetch is network-bound).
static void finalize_recording()
{
    if (g_recording_path.empty())
        return;
    g_player->set_stream_record("");
    std::string path = g_recording_path;
    std::string temp = g_recording_temp;
    std::string id = g_recording_id;
    const double expected_duration = g_recording_duration;
    g_recording_path.clear();
    g_recording_temp.clear();
    g_recording_id.clear();
    g_recording_started = false;
    g_recording_duration = 0.0;
    if (g_player->was_seeked())
    {
        // A seek outside the demuxer cache makes mpv skip stream data, so
        // the file has a hole in it. Repair in the background: the user
        // listened to (most of) the track, so it deserves a cache entry.
        remove(temp.c_str());
        background_download(id);
        return;
    }
    // Validate off-thread: mpv documents that seeks/stream switches can stop
    // the recorder outright, and a truncated or empty file must never reach
    // the cache as a "downloaded" song.
    tasks::spawn([temp, path, id, expected_duration]() mutable
                {
        const double got = probe_audio_file_duration(temp);
        if (got <= 0.0 || (expected_duration > 0.0 && got < expected_duration - 5.0))
        {
            fprintf(stderr, "Discarding recording %s: duration %.1fs (source %.1fs)\n",
                    id.c_str(), got, expected_duration);
            remove(temp.c_str());
            // mpv's recorder can stop silently mid-stream (documented for
            // seeks / stream switches); the bytes are unusable but the song
            // was heard -- fetch a clean copy in the background.
            background_download(id);
            return;
        }
        // Trust the recorded bytes over the mime/itag guess: mpv picked its
        // muxer from the extension we gave it, and a wrong container under a
        // lying extension is an unplayable cache entry.
        const size_t dot = path.find_last_of('.');
        const std::string declared = dot == std::string::npos ? "" : path.substr(dot + 1);
        const std::string real_ext = sniff_audio_container_ext(temp, declared);
        if (real_ext != declared && !real_ext.empty())
            path = recorded_audio_path(id, real_ext);
        if (path.empty() || rename(temp.c_str(), path.c_str()) != 0)
            return;
        g_downloads_dirty = true;
        finalize_recorded_audio(id, path); });
}

// A track was skipped/stopped before finishing: drop the partial recording.
// If the track had actually started playing (not an instant skip-through),
// repair it in the background so the cache still ends up with the song.
static void discard_recording()
{
    if (g_recording_temp.empty())
        return;
    g_player->set_stream_record("");
    remove(g_recording_temp.c_str());
    const bool heard = (g_state.listen_track_id == g_recording_id &&
                        g_state.listen_seconds >= 2.0);
    if (heard)
        background_download(g_recording_id);
    g_recording_path.clear();
    g_recording_temp.clear();
    g_recording_id.clear();
    g_recording_started = false;
    g_recording_duration = 0.0;
}

// ----------------------------------------------------------------------
// Data fetchers
// ----------------------------------------------------------------------
static void commit_play_count()
{
    if (g_state.current_track_id.empty())
        return;

    ++g_state.play_counts[g_state.current_track_id];

    PlayEvent ev;
    ev.id = g_state.current_track_id;
    ev.title = g_state.current_title;
    ev.artist = g_state.current_artist;
    ev.thumbnail_url = g_state.current_thumbnail_url;
    ev.ts = (int64_t)time(nullptr);
    g_state.play_log.push_back(ev);
    if ((int)g_state.play_log.size() > kPlayLogCap)
        g_state.play_log.erase(g_state.play_log.begin(),
                               g_state.play_log.end() - kPlayLogCap);
    g_state.stats_dirty = true;
    g_counts_dirty = true;
}

static void load_recommendations(bool force = false)
{
    auto tracks = fetch_recommendations(force);
    BackendError err = take_backend_error();
    if (err)
        ui_notify_backend_error(err);
    // Don't clobber a good list with an empty one (e.g. a refresh that hit
    // a transient backend error) -- keep what's on screen and surface the
    // error instead.
    if (!tracks.empty())
    {
        auto snapshot = std::make_shared<const std::vector<Track>>(std::move(tracks));
        std::atomic_store(&g_state.recommendations, snapshot);
        g_state.home_failed = false;
        g_state.home_error_msg.clear();
        // Publish the list first: the header spinner should stop as soon as
        // the new recommendations are on screen, not while we warm the first
        // few URLs in the background.
        g_state.home_refreshing = false;
        g_state.repaint_requested = true;
        prefetch_urls_for(*snapshot, 3);
    }
    else if (err)
    {
        // Nothing on screen (or the refresh failed): remember why, so the
        // Home tab can show an inline error + Retry instead of an empty list.
        g_state.home_failed = true;
        g_state.home_error_msg = backend_error_user_message(err);
    }
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_state.status_text = "Ready.";
    g_state.home_refreshing = false;
    g_state.backend_status = err;
    g_state.repaint_requested = true;
}

static void artist_search_and_update(const std::string &query, int expect_id)
{
    if (query.empty())
    {
        std::atomic_store(&g_state.artist_search_results,
                          std::make_shared<const std::vector<ArtistSummary>>());
        g_state.artist_search_loading = false;
        return;
    }
    auto artists = fetch_artist_search(query);
    if (g_state.search_id == expect_id && g_state.current_tab == Tab::Artists)
    {
        std::atomic_store(&g_state.artist_search_results,
                          std::make_shared<const std::vector<ArtistSummary>>(std::move(artists)));
        g_state.artist_search_loading = false;
    }
    BackendError err = take_backend_error();
    if (err)
        ui_notify_backend_error(err);
    {
        std::lock_guard<std::mutex> lock(g_data_mutex);
        g_state.backend_status = err;
    }
    g_state.repaint_requested = true;
}

static bool text_matches_query(const std::string &haystack, const std::string &needle)
{
    if (needle.empty())
        return true;
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [](char a, char b) {
                           return std::tolower((unsigned char)a) ==
                                  std::tolower((unsigned char)b);
                       }) != haystack.end();
}

static void merge_local_search_results(const std::string &query, SearchAllResult &results)
{
    std::unordered_set<std::string> seen;

    auto local_tracks = library::search_tracks(query, 60);
    std::vector<Track> tracks;
    tracks.reserve(local_tracks.size() + results.songs.size());
    for (auto &t : local_tracks)
        if (seen.insert(t.id).second)
            tracks.push_back(std::move(t));
    for (auto &t : results.songs)
        if (seen.insert(t.id).second)
            tracks.push_back(std::move(t));
    results.songs = std::move(tracks);

    std::unordered_set<std::string> seen_albums;
    std::vector<AlbumSummary> albums;
    for (const auto &a : library::load_saved_albums())
    {
        if (!text_matches_query(a.title, query) && !text_matches_query(a.artist, query))
            continue;
        AlbumSummary s;
        s.browse_id = a.browse_id;
        s.title = a.title;
        s.artist = a.artist;
        s.year = a.year;
        s.thumbnail_url = a.thumbnail_url;
        s.track_count = a.track_count;
        if (seen_albums.insert(s.browse_id).second)
            albums.push_back(std::move(s));
    }
    for (auto &a : results.albums)
        if (seen_albums.insert(a.browse_id).second)
            albums.push_back(std::move(a));
    results.albums = std::move(albums);

    std::unordered_set<std::string> seen_artists;
    std::vector<ArtistSummary> artists;
    for (const auto &a : library::load_saved_artists())
    {
        if (!text_matches_query(a.name, query))
            continue;
        ArtistSummary s;
        s.browse_id = a.browse_id;
        s.name = a.name;
        s.thumbnail_url = a.thumbnail_url;
        s.subscribers = a.subscribers;
        if (seen_artists.insert(s.browse_id).second)
            artists.push_back(std::move(s));
    }
    for (auto &a : results.artists)
        if (seen_artists.insert(a.browse_id).second)
            artists.push_back(std::move(a));
    results.artists = std::move(artists);
}

static void search_all_and_update(const std::string &query, int expect_id)
{
    if (query.empty())
    {
        std::atomic_store(&g_state.search_results,
                          std::make_shared<const std::vector<Track>>());
        std::atomic_store(&g_state.search_album_results,
                          std::make_shared<const std::vector<AlbumSummary>>());
        std::atomic_store(&g_state.search_artist_results,
                          std::make_shared<const std::vector<ArtistSummary>>());
        g_state.search_loading = false;
        return;
    }

    SearchAllResult results;
    bool ok = fetch_search_all(query, results);
    if (!ok)
    {
        // Older/partially-updated backends may not know search_all yet.
        results.songs = fetch_search(query);
        ok = !results.songs.empty();
    }


    if (g_state.search_id == expect_id)
    {
        // Local library rows are authoritative and instant; put them first,
        // then append online results that aren't already present.
        merge_local_search_results(query, results);
        // Warm the URLs of the visible results so clicking one starts fast.
        // Depth matches the rows a search page shows; warming runs on the
        // main backend channel and aborts as soon as search_id changes.
        prefetch_urls_for(results.songs, 10, expect_id);
        std::atomic_store(&g_state.search_results,
                          std::make_shared<const std::vector<Track>>(std::move(results.songs)));
        std::atomic_store(&g_state.search_album_results,
                          std::make_shared<const std::vector<AlbumSummary>>(std::move(results.albums)));
        std::atomic_store(&g_state.search_artist_results,
                          std::make_shared<const std::vector<ArtistSummary>>(std::move(results.artists)));
        g_state.current_playlist_source = PlaylistSource::SEARCH;
        g_state.search_loading = false;
        // Success clears any previous failure state; a genuinely empty result
        // (no hits) keeps search_failed false so "no results" stays honest.
        if (ok)
        {
            g_state.search_failed = false;
            g_state.search_error_msg.clear();
        }
    }
    BackendError err = take_backend_error();
    if (err)
    {
        ui_notify_backend_error(err);
        if (g_state.search_id == expect_id)
        {
            g_state.search_failed = true;
            g_state.search_error_msg = backend_error_user_message(err);
        }
    }
    std::lock_guard<std::mutex> lock(g_data_mutex);
    g_state.backend_status = err;
    g_state.repaint_requested = true;
}

// Lyrics: disk cache first (works offline, including for tracks playing
// from the local audio cache), then a metadata-only backend request that
// asks for the lyrics alone -- no stream resolution, no downloads.
static void load_lyrics_worker(std::string track_id, int expect_seq)
{
    Lyrics lx;
    if (!load_lyrics_cache(track_id, lx))
    {
        if (fetch_lyrics(track_id, lx))
            save_lyrics_cache(track_id, lx); // caches real lyrics and "none" results
        else
            lx.ok = false; // fetch failed: don't cache, retry on next play/open
    }

    // Discard if a newer request superseded this one.
    if (g_state.lyrics_seq.load() != expect_seq)
        return;

    auto view = std::make_shared<LyricsView>();
    view->track_id = std::move(track_id);
    view->lyrics = std::move(lx);
    std::atomic_store(&g_state.lyrics, std::shared_ptr<const LyricsView>(std::move(view)));
    g_state.lyrics_loading = false;
    g_state.repaint_requested = true;
}

static void begin_play_track(const Track &t, int index)
{
    if (t.id.empty())
        return;
    g_endless_waiting = false;
    stop_fade();
    // Invalidate resolves still in flight for the previous selection; this
    // request's generation is captured when the resolver thread is spawned.
    g_play_generation.fetch_add(1);
    if (!g_force_fresh_resolve)
        g_play_retry_count = 0;
    g_state.loading = true;
    g_loading_since = std::chrono::steady_clock::now();
    g_state.current_title = t.title;
    g_state.current_artist = t.artist;
    g_state.current_thumbnail_url = t.thumbnail_url;
    g_state.current_track_id = t.id;
    g_state.current_album_id = t.album_id;
    g_state.current_artist_id = t.artist_id;
    g_state.current_track_url = t.url; // <-- persistent
    g_state.status_text = "Loading...";
    int resolved_index = index;
    if (resolved_index < 0)
    {
        // Queue entries do not carry a source index. Re-find the row when the
        // active list still contains this track so list highlighting survives
        // a skip.
        auto playlist = get_playlist();
        if (playlist)
        {
            for (int i = 0; i < (int)playlist->size(); ++i)
            {
                if ((*playlist)[i].id == t.id)
                {
                    resolved_index = i;
                    break;
                }
            }
        }
    }
    g_state.current_track_index = resolved_index;

    g_state.pending_manual_play = true;
    g_state.pending_play_url = t.url;
    g_state.pending_play_title = t.title;
    g_state.pending_play_artist = t.artist;
    g_state.pending_play_thumbnail_url = t.thumbnail_url;
    g_state.pending_video_id = t.id;
    g_state.pending_video_url = t.url;
}

static void replay_current_track()
{
    Track t;
    t.id = g_state.current_track_id;
    t.title = g_state.current_title;
    t.artist = g_state.current_artist;
    t.artist_id = g_state.current_artist_id;
    t.album_id = g_state.current_album_id;
    t.thumbnail_url = g_state.current_thumbnail_url;
    t.url = g_state.current_track_url;
    begin_play_track(t, g_state.current_track_index);
}

static bool advance_playback(bool manual = false);

// Start one endless-radio refill when the remaining queue/source is short.
// Returns true if a refill is loading (or was already in flight).
static bool maybe_start_endless_refill()
{
    if (!g_state.settings.endless_radio || g_state.settings.repeat_mode != 0 ||
        g_state.settings.shuffle_enabled)
        return false;
    if (g_state.current_track_id.empty())
        return false;
    if (g_endless_seed != g_state.current_track_id)
    {
        g_endless_seed = g_state.current_track_id;
        g_endless_attempted = false;
    }
    if (g_state.endless_loading.load())
        return true;
    if (g_endless_attempted)
        return false;
    if (g_state.play_queue.size() >= 100)
        return false;

    const int remaining = (int)g_state.play_queue.size();
    if (remaining > 3)
        return false;

    g_endless_attempted = true;
    g_state.endless_loading = true;
    const std::string seed = g_state.current_track_id;
    tasks::spawn([seed]()
                {
        std::vector<Track> results;
        try {
            results = fetch_radio(seed);
        } catch (...) {}
        std::atomic_store(&g_state.endless_results,
                          std::make_shared<const std::vector<Track>>(std::move(results)));
        g_state.endless_ready = true;
        g_state.endless_loading = false;
        g_state.repaint_requested = true; });
    return true;
}

// Append deduped endless-radio results to the queue. When playback already
// stopped at the end of the source list, start the first new entry now.
static void consume_endless_results()
{
    if (!g_state.endless_ready.exchange(false))
        return;
    auto results = std::atomic_load(&g_state.endless_results);
    bool appended = false;

    if (g_state.settings.endless_radio && results)
    {
        std::unordered_set<std::string> seen;
        if (!g_state.current_track_id.empty())
            seen.insert(g_state.current_track_id);
        for (const auto &t : g_state.play_queue)
            if (!t.id.empty())
                seen.insert(t.id);
        for (const auto &t : g_state.playback_history)
            if (!t.id.empty())
                seen.insert(t.id);

        for (const auto &t : *results)
        {
            if (t.id.empty() || seen.count(t.id))
                continue;
            if (g_state.play_queue.size() >= 100)
                break;
            seen.insert(t.id);
            g_state.play_queue.push_back(t);
            appended = true;
        }
        g_state.endless_loading = false;
    }

    if (appended)
    {
        g_state.queue_dirty = true;
        g_state.repaint_requested = true;
        prefetch_next_track();
    }

    if (g_endless_waiting)
    {
        g_endless_waiting = false;
        if (appended)
        {
            if (!advance_playback())
                g_state.status_text = "Ready.";
        }
        else if (!g_state.endless_loading.load())
        {
            g_state.status_text = "Ready.";
        }
    }
}

// Refill the single global queue from the source-list snapshot used for
// repeat-all / shuffle. Manual "Play next" pins and guest requests already in
// the queue are not part of the cycle; they were one-shot entries.
static void refill_queue_from_cycle()
{
    if (g_state.queue_cycle.empty())
        return;
    g_state.play_queue = g_state.queue_cycle;
    if (g_state.settings.shuffle_enabled && g_state.play_queue.size() > 1)
    {
        static std::mt19937 rng(
            (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
        std::shuffle(g_state.play_queue.begin(), g_state.play_queue.end(), rng);
    }
    g_state.queue_dirty = true;
}

// Decide what plays next without starting it: the head of the global queue.
// `allow_refill` lets natural/manual advance wrap the source cycle when repeat
// is on. Returns false when playback should stop.
struct NextPick
{
    Track track;
};
static bool select_next_track(bool allow_refill, bool consume, NextPick &out)
{
    if (g_state.play_queue.empty() && allow_refill &&
        g_state.settings.repeat_mode != 0)
        refill_queue_from_cycle();

    if (g_state.play_queue.empty())
        return false;

    out.track = g_state.play_queue.front();
    if (consume)
    {
        g_state.play_queue.erase(g_state.play_queue.begin());
        g_state.queue_dirty = true;
    }
    return true;
}

// Shared per-track bookkeeping for a fade transition. The helper player's
// events are not routed through the main "start" handler, so metadata,
// history, MPRIS and the notification are updated here when the fade begins.
static void adopt_fade_track(const Track &consumed, int consumed_index)
{
    g_state.loading = false;
    // Keep the outgoing track around for the first half of the fade. Lyrics
    // and their metadata stay in sync with the bottom bar's midpoint switch.
    g_state.crossfade_from_track = Track{};
    g_state.crossfade_from_track.id = g_state.current_track_id;
    g_state.crossfade_from_track.title = g_state.current_title;
    g_state.crossfade_from_track.artist = g_state.current_artist;
    g_state.crossfade_from_track.thumbnail_url = g_state.current_thumbnail_url;
    g_state.crossfade_from_track.url = g_state.current_track_url;
    g_state.crossfade_from_track.album_id = g_state.current_album_id;
    g_state.crossfade_from_track.artist_id = g_state.current_artist_id;
    g_state.current_title = consumed.title;
    g_state.current_artist = consumed.artist;
    g_state.current_thumbnail_url = consumed.thumbnail_url;
    g_state.current_track_id = consumed.id;
    g_state.current_album_id = consumed.album_id;
    g_state.current_artist_id = consumed.artist_id;
    g_state.current_track_url = consumed.url;
    g_state.status_text = "Playing";
    g_state.current_track_index = consumed_index;
    g_state.listen_track_id = consumed.id;
    g_state.listen_seconds = 0.0;
    g_state.listen_counted = false;
    // Lyrics stay on the outgoing track until the visual midpoint; the render
    // loop queues the incoming lyrics when crossfade_progress crosses 0.5.

    // History/notification bookkeeping, mirroring the main "start" path.
    g_history_dirty = true;
    {
        Track history_entry;
        history_entry.id = consumed.id;
        history_entry.title = consumed.title;
        history_entry.artist = consumed.artist;
        history_entry.thumbnail_url = consumed.thumbnail_url;
        history_entry.url = consumed.url;
        history_entry.duration_sec = consumed.duration_sec;
        for (auto it = g_state.playback_history.begin();
             it != g_state.playback_history.end(); ++it)
        {
            if (it->id == history_entry.id)
            {
                g_state.playback_history.erase(it);
                break;
            }
        }
        g_state.playback_history.insert(g_state.playback_history.begin(), history_entry);
        if (g_state.playback_history.size() > 200)
            g_state.playback_history.resize(200);
    }
    if (g_mpris)
    {
        g_mpris->update_metadata(consumed, (double)consumed.duration_sec);
        g_mpris->update_playback_status(true);
        update_discord_presence();
    }
    {
        std::string title = consumed.title.empty() ? "Unknown Track" : consumed.title;
        std::string artist = consumed.artist.empty() ? "Unknown Artist" : consumed.artist;
        // Stat-only: never a synchronous curl on the main thread (the async
        // pipeline fetches the art; the notification falls back to a generic
        // icon until it lands).
        std::string thumb_path = thumbnail_is_cached(consumed.thumbnail_url)
                                     ? get_cached_thumbnail_path(consumed.thumbnail_url)
                                     : "";
        if (thumb_path.empty())
            thumb_path = "audio-x-generic";
        show_notification(title, "by " + artist, thumb_path);
    }
}

// A manual skip can crossfade only while audio is actually playing and the
// target already exists as a complete file in the local audio cache.
static bool can_start_manual_crossfade(const Track &t)
{
    if (!g_state.settings.skip_crossfade_enabled)
        return false;
    if (g_state.settings.skip_crossfade_duration <= 0.0f)
        return false;
    if (!g_state.playing.load() || !g_player || !g_player->is_playing())
        return false;
    if (t.id.empty())
        return false;
    return true;
}

// Promote the helper player (the track that faded in) to be the main player
// and retire the outgoing one. Shared by timer-driven manual fades and by
// selections that arrive while a fade is already in flight.
static void promote_fade_player_to_main()
{
    if (!g_fade_player)
        return;
    g_cf.pending = false;
    g_cf.active = false;
    g_cf.manual = false;

    g_player.swap(g_fade_player);
    g_player_raw = g_player.get();
    // Callbacks live on the objects, not the roles: move them with the swap.
    g_player->set_event_callback(g_main_event_cb);
    g_fade_player->set_event_callback(g_fade_event_cb);
    g_player->set_volume(g_cf.user_volume);
    g_fade_player->set_volume(0);
    g_fade_player->set_stream_record("");
    g_fade_player->stop();

    g_cf.track = Track{};
    g_cf.index = -1;
    g_state.playing = true;
    if (!g_state.current_track_id.empty() &&
        g_state.lyrics_requested_id != g_state.current_track_id)
    {
        g_state.lyrics_requested_id = g_state.current_track_id;
        g_state.lyrics_dirty = true;
    }
    g_state.repaint_requested = true;
}

// Finish a timer-driven manual fade: promote the incoming helper player to
// be the main player and retire the (cut-short) outgoing one.
static void finish_manual_crossfade()
{
    if (!g_fade_player)
        return;
    promote_fade_player_to_main();
    g_state.loading = false;
    g_state.status_text = "Playing";

    prefetch_next_track();
    predownload_next_track();
    maybe_start_endless_refill();
}

// A manual selection (track click, queue row, next/prev) arrived while a
// crossfade is already in flight. Settle that transition first: otherwise
// the helper player can keep playing the old "next" track while the new
// selection is loaded into the main player, leaving an orphaned player the
// app no longer tracks.
static void settle_inflight_crossfade()
{
    if (!g_fade_player)
        return;
    if (g_cf.active.load())
    {
        // The faded-in track is already audible, so make it the outgoing
        // track for the new selection instead of letting the helper linger.
        discard_recording();
        promote_fade_player_to_main();
    }
    else if (g_cf.pending.load())
    {
        // The incoming track never became audible: cancel it and keep the
        // current main player, which can switch to the new selection.
        stop_fade();
    }
}

static void start_manual_crossfade(const Track &t, int index,
                                   const std::string &cached_path, double seconds)
{
    // Restart semantics: a second Next during a fade cancels the running one
    // and starts a fresh fade toward the newly picked track.
    stop_fade();
    // The outgoing track is being skipped, so drop its partial recording now
    // (while the listen timer still refers to it -- discard_recording repairs
    // it in the background when enough was heard).
    discard_recording();
    // Invalidate resolves still in flight for an earlier selection.
    g_play_generation.fetch_add(1);
    g_state.pending_manual_play = false;
    g_state.pending_play_url.clear();

    g_cf.track = t;
    g_cf.index = index;
    adopt_fade_track(t, index);

    g_cf.user_volume = (float)g_player->get_property_double("volume");
    if (g_cf.user_volume <= 0.0f)
        g_cf.user_volume = 100.0f;
    g_fade_player->set_stream_record("");
    g_fade_player->set_volume(0);
    g_cf.seconds = seconds;
    g_cf.manual = true;
    g_cf.pending = true;
    g_cf.start = std::chrono::steady_clock::now();
    g_fade_player->play(cached_path);

    prefetch_next_track();
    predownload_next_track();
    maybe_start_endless_refill();
}

// Returns true when the transition was handed to a manual crossfade; false
// means the caller should fall back to the normal instant track switch.
static bool try_start_manual_crossfade(const Track &t, int index)
{
    if (!can_start_manual_crossfade(t))
        return false;

    // Don't start a fade that the outgoing file would finish before the
    // incoming one reaches full volume; an instant switch is cleaner there.
    const double dur = g_player->cached_duration();
    const double pos = g_player->cached_time_pos();
    if (dur > 0.0 && pos >= 0.0 && dur - pos < 0.5)
        return false;

    const std::string cached = find_cached_audio_path(t.id);
    if (cached.empty())
        return false;

    double cf = (double)g_state.settings.skip_crossfade_duration;
    if (t.duration_sec > 0)
        cf = std::min(cf, t.duration_sec * 0.5);
    if (dur > 0.0 && pos >= 0.0)
        cf = std::min(cf, dur - pos);
    if (cf < 0.1)
        cf = 0.1;

    start_manual_crossfade(t, index, cached, cf);
    return true;
}

static void maybe_start_crossfade()
{
    // Keep mpv's crossfade option in sync with the settings slider (unused
    // by mpv 0.41+, which has no crossfade support, but harmless).
    if (g_cf.applied_duration != g_state.settings.crossfade_duration)
    {
        g_cf.applied_duration = g_state.settings.crossfade_duration;
        g_player->set_crossfade(g_cf.applied_duration);
    }

    // Playback speed: applied to BOTH instances so it survives the crossfade
    // role swap.
    static float g_applied_speed = -1.0f;
    if (g_applied_speed != g_state.settings.playback_speed)
    {
        g_applied_speed = g_state.settings.playback_speed;
        g_player->set_speed((double)g_applied_speed);
        g_fade_player->set_speed((double)g_applied_speed);
    }

    // Audio filters (normalization + EQ): one chain on both instances.
    static std::string g_applied_af;
    static bool g_af_first = true;
    std::string af = crossfade::audio_filter(g_state.settings);
    if (g_af_first || af != g_applied_af)
    {
        g_af_first = false;
        g_applied_af = af;
        g_player->set_af(af);
        g_fade_player->set_af(af);
    }

    // Manual skip fades are timer-driven: the outgoing file does not reach
    // EOF, so promote the helper once the ramp has run its course.
    if (g_cf.manual.load())
    {
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - g_cf.start)
                                   .count();
        switch (crossfade::plan_manual(g_cf.manual, g_cf.pending, g_cf.active, elapsed,
                                       g_cf.seconds))
        {
        case crossfade::ManualAction::Cancel:
            g_cf.manual = false;
            break;
        case crossfade::ManualAction::Finish:
            finish_manual_crossfade();
            return;
        case crossfade::ManualAction::StartFailed:
        {
            // The cached file never started (vanished, decoder error): fall
            // back to the normal switch instead of leaving silence.
            Track target = g_cf.track;
            const int target_index = g_cf.index;
            g_cf.track = Track{};
            g_cf.index = -1;
            stop_fade();
            if (!target.id.empty())
                begin_play_track(target, target_index);
            return;
        }
        case crossfade::ManualAction::None:
            break;
        }
    }

    // Drive the fade: perceptually even, constant-power ramp on the two
    // players, and mirror the app's play/pause intent onto the helper.
    // The ramp runs at ~20 Hz -- plenty for a smooth loudness glide; writing
    // mpv properties on every 16 ms frame only burns wakeups.
    if ((g_cf.pending || g_cf.active) && g_fade_player)
    {
        static std::chrono::steady_clock::time_point s_last_ramp;
        const auto ramp_now = std::chrono::steady_clock::now();
        if (ramp_now - s_last_ramp < std::chrono::milliseconds(50))
            return;
        s_last_ramp = ramp_now;
        const double cf = std::max(g_cf.seconds, 0.1);
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - g_cf.start)
                                   .count();
        const double t = std::min(elapsed / cf, 1.0);
        const crossfade::Gains g = crossfade::gains(t);
        g_fade_player->set_volume(g.incoming * g_cf.user_volume);
        g_player->set_volume(g.outgoing * g_cf.user_volume);
        g_fade_player->set_paused(!g_state.playing.load());
    }

    if (g_state.settings.crossfade_duration <= 0.0f)
        return;
    if (g_cf.pending || g_cf.active)
        return;
    if (!g_state.playing.load() || g_state.loading)
        return;
    if (g_state.settings.repeat_mode == 2)
        return; // repeat-one replays via replay_current_track()

    const double dur = g_player->cached_duration();
    if (dur <= 0.0)
        return;
    // Never spend more than half the track on the fade: a 20 s setting on a
    // 15 s interlude would otherwise start the next track straight away.
    const double cf = std::min((double)g_state.settings.crossfade_duration, dur * 0.5);
    if (cf <= 0.0)
        return;
    const double pos = g_player->cached_time_pos();
    if (pos <= 0.0 || dur - pos > cf)
        return;

    NextPick pick;
    if (!select_next_track(false, false, pick))
        return;
    // Crossfade only plays the next track from the local cache (the
    // predownload worker usually has it ready): playing a resolved remote
    // URL here would bypass the stream-recording machinery and risk the
    // 403s of stale googlevideo links. When it isn't cached yet, the
    // normal advance at end-of-file handles it without a fade.
    std::string src = find_cached_audio_path(pick.track.id);
    crossfade::NaturalInputs fade_inputs;
    fade_inputs.crossfade_duration = g_state.settings.crossfade_duration;
    fade_inputs.track_duration = dur;
    fade_inputs.position = pos;
    fade_inputs.playing = g_state.playing;
    fade_inputs.loading = g_state.loading;
    fade_inputs.already_fading = g_cf.pending || g_cf.active;
    fade_inputs.repeat_one = g_state.settings.repeat_mode == 2;
    fade_inputs.target_cached = !src.empty();
    if (!crossfade::should_start_natural(fade_inputs))
        return;

    // Consume exactly the peeked pick.
    if (g_state.play_queue.empty() ||
        g_state.play_queue.front().id != pick.track.id)
        return; // queue changed underneath us
    g_state.play_queue.erase(g_state.play_queue.begin());
    g_state.queue_dirty = true;
    const Track &consumed = pick.track;
    const int consumed_index = -1;

    g_cf.track = consumed;
    g_cf.index = consumed_index;

    adopt_fade_track(consumed, consumed_index);

    // Start the next track silently on the helper player; the per-frame
    // volume ramp below fades the two into each other.
    g_cf.user_volume = (float)g_player->get_property_double("volume");
    if (g_cf.user_volume <= 0.0f)
        g_cf.user_volume = 100.0f;
    g_fade_player->set_stream_record("");
    g_fade_player->set_volume(0);
    g_cf.seconds = cf;
    g_cf.manual = false; // natural end-of-track fade
    g_cf.pending = true;
    g_cf.start = std::chrono::steady_clock::now();
    g_fade_player->play(src);

    prefetch_next_track();
    predownload_next_track();
    maybe_start_endless_refill();
}

// Advance playback: pop the head of the single global queue. Manual skips and
// natural end-of-track use exactly the same order, so guest requests can no
// longer be stranded by pressing Next.
static bool advance_playback(bool manual)
{
    NextPick pick;
    if (select_next_track(true, true, pick))
    {
        if (manual && try_start_manual_crossfade(pick.track, -1))
            return true;
        begin_play_track(pick.track, -1);
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------
int app_run(int argc, char **argv)
{
    bool autoplay_last = false;
    bool cli_low_power = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--version")
        {
            std::printf("%s\n", CROSSFADE_VERSION);
            return 0;
        }
        if (arg == "--autoplay-last")
            autoplay_last = true;
        if (arg == "--low-power")
            cli_low_power = true;
    }
    DLOG("boot: app_run at %lld", DNOW());
    ensure_cache_dir();

    // ---- Load settings ----
    g_state.settings = load_settings();
    if (cli_low_power)
        power::set_cli_override(true);
    power::configure(g_state.settings.low_power, g_state.settings.low_power_on_battery);
    power::refresh();
    diagnostics::init();
    const bool safe_mode = diagnostics::safe_mode_requested() || g_state.settings.safe_mode;
    if (safe_mode)
    {
        // Override in memory only: the user's saved preferences stay intact.
        g_state.settings.backdrop_shader = false;
        g_state.settings.album_backdrop = false;
        g_state.settings.warm_stream_resolver = false;
        diagnostics::log("app", "safe mode enabled");
    }
    writeback_set_enabled(g_state.settings.server_sync);
    discord_apply_client_id();

    // ---- Local library database ----
    {
        std::string library_error;
        if (!library::init(library_error))
        {
            std::fprintf(stderr, "Library database unavailable, using legacy JSON: %s\n",
                         library_error.c_str());
            diagnostics::log("library", "DB unavailable, using legacy JSON: " + library_error);
        }
        else
        {
            diagnostics::log("library", "SQLite library ready");
        }
    }

    // ---- Load up-next queue and recent searches ----
    g_state.play_queue = load_play_queue();
    // Persisted rows came from the single global queue; keep guest requests
    // ahead of them instead of treating the whole restored list as "Play
    // next" pins.
    for (Track &t : g_state.play_queue)
        t.from_queue_source = true;
    g_state.search_recent = load_search_recent();

    // ---- Accent colour (custom setting, system portal or daily fallback) ----
    update_accent(g_state.settings);

    // ---- libnotify init ----
#ifndef P100_WINDOWS
    notify_init("crossfade");
    notify_init_done = true;
#endif

    // ---- Load liked tracks ----
    g_state.liked_tracks = load_liked_tracks();

    // ---- Load saved albums ----
    g_state.saved_albums = load_saved_albums();
    g_state.saved_artists = load_saved_artists();

    // ---- Load playback history ----
    g_state.playback_history = load_playback_history();

    // ---- Load play counts ----
    g_state.play_counts = load_play_counts();

    // ---- Load play event log (drives the Most Played / stats tab) ----
    g_state.play_log = load_play_log();

    // ---- Load playlists ----
    auto names = get_playlist_names();
    for (const auto &name : names)
    {
        Playlist pl;
        pl.name = name;
        pl.tracks = load_playlist_tracks(name);
        g_state.playlists.push_back(pl);
    }

    // ---- Load session ----
    load_session(g_state);

    // ---- SDL init ----
    // Touch is translated to ImGui input manually (tap/scroll/long-press),
    // so suppress SDL's synthetic touch-to-mouse events.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
    party_init();
    party_set_wake(party_wake_event);
    if (!safe_mode)
        updater::init(party_wake_event);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window *window = SDL_CreateWindow("crossfade",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 820,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window, 860, 600);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context)
    {
        fprintf(stderr, "OpenGL 3.2 core context unavailable: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    // ---- ImGui init ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Keep window layout in the same place as the rest of the user data so
    // it travels with a copied ~/.config/p100.
    g_imgui_ini_path = p100_config_path("imgui.ini");
    io.IniFilename = g_imgui_ini_path.c_str();
    // Touch taps jitter: two taps on the same row easily exceed the default
    // 6 px window, which would make double-tap start unreliable.
    io.MouseDoubleClickMaxDist = 48.0f;

    ui_theme_init_fonts(io, p100_asset_dir() + "/fonts");
    g_state.title_font = g_ui_fonts.semibold;

    apply_ui_scale(g_state.settings);

    // ---- Backends ----
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    if (!ImGui_ImplOpenGL3_Init("#version 130"))
    {
        fprintf(stderr,
                "OpenGL 3.2+ is required (found no working GL driver). "
                "Update your GPU driver or use a machine with hardware/llvmpipe OpenGL.\n");
        return 1;
    }

    // ---- Mpv ----
    // Single cookie source everywhere: mpv, local yt-dlp calls and the
    // Python backend all use the browser chosen in Settings.
    set_ytdl_browser(g_state.settings.ytdl_browser);
    set_auth_mode(g_state.settings.auth_mode);
    set_warm_stream_resolver(g_state.settings.warm_stream_resolver);
    // Download cache cap (0 = unlimited). Re-applied live when the user
    // changes the combo in Settings.
    set_audio_cache_cap((double)g_state.settings.download_cap_gb * 1024.0 * 1024.0 * 1024.0);
    // Cap eviction prefers your least-listened songs: rank cached tracks by
    // their play count (0 for never-played), oldest file breaks ties.
    set_audio_cache_rank([](const std::string &video_id) -> int
    {
        auto it = g_state.play_counts.find(video_id);
        return it != g_state.play_counts.end() ? it->second : 0;
    });
    g_player = std::make_unique<MpvPlayer>(g_state.settings.ytdl_browser);
    g_player_raw = g_player.get();
    g_fade_player = std::make_unique<MpvPlayer>(g_state.settings.ytdl_browser);
    // The helper player fades in the next track during a crossfade. Its
    // only routed event is "start": from that moment the outgoing track's
    // "end" promotes the helper instead of advancing normally.
    g_fade_event_cb = [](const std::string &event)
    {
        if (event == "start")
            g_cf.active = g_cf.pending.exchange(false);
    };
    g_fade_player->set_event_callback(g_fade_event_cb);

    ui_notify_set_open_settings([]()
    {
        ui_invalidate_forward(g_state);
        g_state.current_tab = Tab::Settings;
    });

    start_writeback_worker();

    if (!start_backend())
    {
        fprintf(stderr, "Failed to start backend.\n");
        // Don't hard-exit: keep the UI up so the auth-failure banner
        // can tell the user to re-run `ytmusicapi browser`.
        BackendError err = take_backend_error();
        if (!err)
        {
            err.kind = BackendErrKind::Backend;
            err.message =
                "The YouTube Music backend failed to start. Check the auth file in Settings.";
        }
        ui_notify_backend_error(err);
        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            g_state.backend_status = err;
        }
        g_state.repaint_requested = true;
    }

    // Recordings interrupted by a crash/quit are dead weight in .tmp.
    cleanup_incomplete_recordings();

    // Load the last radio station (and warm its first tracks) so the Radio
    // tab is usable immediately. With no cache, build a station from the
    // user's most-played songs in the background.
    ui_radio_init(g_state);

    // ---- MPRIS ----
    auto mpris = std::make_unique<MprisPlayer>(g_state, g_player_raw);
    g_mpris = mpris.get();

    // ---- Session restore ----
    if (g_state.session_restore_pending)
    {
        const auto &r = g_state.session_restore;
        auto source_from_str = [](const std::string &s)
        {
            if (s == "search")
                return PlaylistSource::SEARCH;
            if (s == "downloads")
                return PlaylistSource::DOWNLOADS;
            if (s == "radio")
                return PlaylistSource::RADIO;
            if (s == "liked")
                return PlaylistSource::LIKED;
            if (s == "playlist")
                return PlaylistSource::PLAYLIST;
            if (s == "album")
                return PlaylistSource::ALBUM;
            if (s == "artist")
                return PlaylistSource::ARTIST;
            if (s == "podcast")
                return PlaylistSource::PODCAST;
            return PlaylistSource::HOME;
        };
        g_state.current_playlist_source = source_from_str(r.source);
    if (g_state.current_playlist_source == PlaylistSource::PLAYLIST && !r.playlist_name.empty())
    {
        for (int i = 0; i < (int)g_state.playlists.size(); ++i)
            if (g_state.playlists[i].name == r.playlist_name)
            {
                g_state.selected_playlist_index = i;
                break;
            }
    }
        if (!r.search_query.empty())
            strncpy(g_state.search_buffer, r.search_query.c_str(), sizeof(g_state.search_buffer) - 1);
        g_state.current_title = r.track_title;
        g_state.current_artist = r.track_artist;
        g_state.current_thumbnail_url = r.track_thumbnail;
        g_state.current_track_id = r.track_id;
        g_state.current_album_id = r.track_album_id;
        g_state.current_artist_id = r.track_artist_id;
        if (g_state.current_album_id.empty() && g_state.current_playlist_source == PlaylistSource::ALBUM)
            g_state.current_album_id = g_state.album_browse_id;
        if (g_state.current_artist_id.empty() && g_state.current_playlist_source == PlaylistSource::ARTIST)
            g_state.current_artist_id = g_state.artist_browse_id;
        g_state.current_track_url = r.track_url;
        g_state.current_track_index = r.index;
        g_state.status_text = "Restoring session...";
        // Seed the play/pause intent from the saved state now (not when the
        // track finishes resolving) so a click during the resolve window is
        // not overwritten later.
        g_state.playing = r.playing || autoplay_last;
        g_state.loading = true;
        g_loading_since = std::chrono::steady_clock::now();
        g_state.pending_manual_play = true;
        g_state.pending_play_url = r.track_url;
        g_state.pending_play_title = r.track_title;
        g_state.pending_play_artist = r.track_artist;
        g_state.pending_play_thumbnail_url = r.track_thumbnail;
        g_state.pending_video_id = r.track_id;
        g_state.pending_video_url = r.track_url;
    }

    g_state.home_refreshing = true;
    tasks::spawn([]() { load_recommendations(false); });

#ifndef P100_WINDOWS
    std::signal(SIGTERM, handle_quit_signal);
    std::signal(SIGINT, handle_quit_signal);
#endif

    // ---- Event callback ----
    g_main_event_cb = [&](const std::string &event)
    {
        if (event == "start") {
            static bool logged_first_audio = false;
            if (!logged_first_audio)
            {
                DLOG("boot: first audio at %lld", DNOW());
                logged_first_audio = true;
            }
            g_state.loading = false;
            g_state.status_text = "Playing";
            g_state.repaint_requested = true;

            bool was_manual = g_state.pending_manual_play.exchange(false);
            if (was_manual) {
                // UI already updated for the track we asked for.
            } else {
                // Unexpected auto-start: the metadata set by begin_play_track()
                // is the closest guess and must be kept — do NOT advance the
                // index here. A stale (superseded) resolve playing after a
                // newer one already started used to land in this branch and
                // overwrite the indicator with the *next* list entry while a
                // different song was audible.
                g_state.playing = true;
            }

            // Start timing this listen. The play count / stats entry is
            // only committed once 10s of actual playback have elapsed (see
            // the main loop), so skipped tracks don't count.
            g_state.listen_track_id = g_state.current_track_id;
            g_state.listen_seconds = 0.0;
            g_state.listen_counted = false;

            // The track now under recording reports its duration from here
            // on; finalize_recording() validates the finished file against
            // it (may still be 0 until the header is parsed -> polled in the
            // main loop until it shows up).
            if (!g_recording_path.empty())
            {
                g_recording_started = true;
                const double d = g_player->get_property_double("duration");
                if (d > 0.0)
                    g_recording_duration = d;
            }

            if (g_state.session_restore_pending) {
                g_state.session_restore_pending = false;
                const double restore_pos = g_state.session_restore.position;
                if (restore_pos > 0)
                {
                    // The stream just started, so the cache almost certainly
                    // doesn't cover the saved position yet; seeking there
                    // now would skip data and break this track's recording.
                    // Wait (bounded) until the target is inside the cached
                    // region, then jump without hurting the recording.
                    if (g_recording_path.empty() || g_player->is_position_cached(restore_pos))
                        g_player->seek(restore_pos);
                    else
                    {
                        g_pending_restore_seek = restore_pos;
                        g_pending_restore_seek_id = g_state.current_track_id;
                        g_pending_restore_seek_gen = g_player->seek_generation();
                        g_pending_restore_seek_deadline =
                            std::chrono::steady_clock::now() + std::chrono::seconds(15);
                    }
                }
                int vol = g_state.session_restore.volume;
                if (vol >= 0 && vol <= 100)
                    g_player->set_volume(vol);
            }
            // Make mpv match the app's play/pause intent. The saved state
            // was folded into g_state.playing when the track was queued, and
            // any play/pause the user pressed while it was still resolving
            // is honored here, so a single click always sticks.
            g_player->set_paused(!g_state.playing.load());
            if (!g_state.playing)
                g_state.status_text = "Paused";

            Track current_track;
            current_track.id = g_state.current_track_id;
            current_track.title = g_state.current_title;
            current_track.artist = g_state.current_artist;
            current_track.thumbnail_url = g_state.current_thumbnail_url;
            current_track.url = g_state.current_track_url;
            double duration = g_player->get_property_double("duration");
            mpris->update_metadata(current_track, duration);
            mpris->update_playback_status(g_state.playing);
            update_discord_presence();
            double position = g_player->get_property_double("time-pos");
            mpris->update_position(position);
            double vol = g_player->get_property_double("volume");
            mpris->update_volume(vol / 100.0);

            std::string title = g_state.current_title;
            std::string artist = g_state.current_artist;
            if (title.empty()) title = "Unknown Track";
            if (artist.empty()) artist = "Unknown Artist";
            std::string thumb_path = thumbnail_is_cached(g_state.current_thumbnail_url)
                                         ? get_cached_thumbnail_path(g_state.current_thumbnail_url)
                                         : "";
            if (thumb_path.empty()) thumb_path = "audio-x-generic";
            show_notification(title, "by " + artist, thumb_path);

            // ---- Record playback history ----
            g_history_dirty = true;
            {
                Track history_entry;
                history_entry.id = g_state.current_track_id;
                history_entry.title = g_state.current_title;
                history_entry.artist = g_state.current_artist;
                history_entry.thumbnail_url = g_state.current_thumbnail_url;
                history_entry.url = g_state.current_track_url;
                history_entry.duration_sec = (int)duration;
                // Insert at front (most-recent-first). Drop an earlier
                // entry for the same track first: history keeps one row per
                // track, so repeats don't stack duplicates.
                for (auto it = g_state.playback_history.begin();
                     it != g_state.playback_history.end(); ++it)
                {
                    if (it->id == history_entry.id)
                    {
                        g_state.playback_history.erase(it);
                        break;
                    }
                }
                g_state.playback_history.insert(g_state.playback_history.begin(), history_entry);
                // Cap at 200 entries
                if (g_state.playback_history.size() > 200)
                    g_state.playback_history.resize(200);
            }

            prefetch_next_track();
            predownload_next_track();

            // Mark the track for a lyrics lookup. The actual fetch happens
            // in the main loop and only while the lyrics panel is open, so
            // playing with the panel closed costs nothing.
            g_state.lyrics_requested_id = g_state.current_track_id;
            g_state.lyrics_dirty = true;

            maybe_start_endless_refill();

        } else if (event == "end") {
            DLOG("EVENT end");
            if (g_cf.active.exchange(false)) {
                // The helper player already faded the next track in; this
                // EOF is just the outgoing file reaching its end. Finalize
                // its recording, then promote the helper to be the main
                // player (roles swap, so the next crossfade reuses the
                // freshly retired instance).
                finalize_recording();
                g_cf.manual = false;
                g_player.swap(g_fade_player);
                g_player_raw = g_player.get();
                // The callbacks live on the MpvPlayer objects, not the
                // roles: move them with the swap or the new main player
                // would keep swallowing its own end-of-file events.
                g_player->set_event_callback(g_main_event_cb);
                g_fade_player->set_event_callback(g_fade_event_cb);
                g_player->set_volume(g_cf.user_volume);
                g_fade_player->set_volume(0);
                g_cf.track = Track{};
                g_cf.index = -1;
                if (!g_state.current_track_id.empty() &&
                    g_state.lyrics_requested_id != g_state.current_track_id)
                {
                    g_state.lyrics_requested_id = g_state.current_track_id;
                    g_state.lyrics_dirty = true;
                }
                g_state.repaint_requested = true;
                if (g_state.sleep_at_end_of_track)
                {
                    // "Stop after this track": the next track already faded
                    // in, so pause it immediately.
                    g_state.sleep_at_end_of_track = false;
                    g_player->set_paused(true);
                    g_state.playing = false;
                    mpris->update_playback_status(false);
                    update_discord_presence();
                    ui_notify(UiNotifyType::Info, "Sleep timer",
                              "Paused at end of track.");
                }
                return;
            }
            // Fade was issued but the helper never started (e.g. its file
            // vanished): cancel it so the normal advance doesn't overlap.
            if (g_cf.pending.exchange(false))
            {
                const bool manual = g_cf.manual.exchange(false);
                Track target = g_cf.track;
                const int target_index = g_cf.index;
                g_fade_player->set_stream_record("");
                g_fade_player->stop();
                if (manual)
                {
                    // The outgoing file ended before the helper started:
                    // finalize it and start the target through the normal
                    // path so the transition isn't lost.
                    g_cf.track = Track{};
                    g_cf.index = -1;
                    finalize_recording();
                    if (!target.id.empty())
                        begin_play_track(target, target_index);
                    return;
                }
            }
            // A manual selection is already pending (its resolve is still
            // running): let it take over instead of advancing. Advancing
            // here is what makes a slow non-cached resolve finish after
            // this EOF and overwrite the track that was actually picked.
            if (g_state.pending_manual_play.load() || !g_state.pending_play_url.empty())
            {
                finalize_recording();
                return;
            }
            g_state.playing = false;
            g_state.repaint_requested = true;
            mpris->update_playback_status(false);
            update_discord_presence();
            // Track finished on its own: keep the stream recording, then
            // advance to the next entry explicitly (so the UI metadata
            // always matches what is actually playing).
            finalize_recording();
            if (g_state.sleep_at_end_of_track)
            {
                g_state.sleep_at_end_of_track = false;
                ui_notify(UiNotifyType::Info, "Sleep timer",
                          "Paused at end of track.");
            }
            else if (g_state.settings.repeat_mode == 2)
            {
                replay_current_track();
            }
            else if (!advance_playback())
            {
                if (g_state.settings.endless_radio && g_state.settings.repeat_mode == 0)
                {
                    g_endless_waiting = true;
                    if (maybe_start_endless_refill() || g_state.endless_ready.load())
                        g_state.status_text = "Loading endless radio...";
                    else
                    {
                        g_endless_waiting = false;
                        g_state.status_text = "Ready.";
                    }
                }
                else
                {
                    g_state.status_text = "Ready.";
                }
            }
        } else if (event == "stop" || event.rfind("error", 0) == 0) {
            DLOG("EVENT %s", event.c_str());
            stop_fade();
            const bool started_playing = (g_state.listen_track_id == g_state.current_track_id &&
                                          g_state.listen_seconds >= 2.0);
            const bool load_failed = event.rfind("error", 0) == 0 &&
                                     !g_state.current_track_id.empty() &&
                                     find_cached_audio_path(g_state.current_track_id).empty() &&
                                     !started_playing;
            if (load_failed && g_play_retry_count < 2)
            {
                ++g_play_retry_count;
                g_force_fresh_resolve = true;
                g_state.status_text = "Retrying stream...";
                g_state.repaint_requested = true;
                replay_current_track();
            }
            else
            {
                if (event.rfind("error", 0) == 0)
                {
                    // Prefer the classified reason from the failed resolve
                    // (auth / network / backend) over a generic message.
                    std::string hint;
                    {
                        std::lock_guard<std::mutex> lock(g_play_fail_mutex);
                        hint = std::move(g_play_fail_hint);
                        g_play_fail_hint.clear();
                    }
                    ui_notify(UiNotifyType::Warning, "Playback failed",
                              hint.empty()
                                  ? (g_state.current_title.empty()
                                         ? "The stream could not be loaded. Try again in a moment."
                                         : "Couldn't play \"" + g_state.current_title +
                                               "\". Check your connection or try again.")
                                  : hint,
                              8.0f);
                }
                g_state.loading = false;
                g_state.playing = false;
                g_state.pending_manual_play = false;
                g_state.status_text = "Ready.";
                g_state.repaint_requested = true;
                g_state.listen_track_id.clear();
                // NB: don't discard the recording here. A manual skip fires
                // this *after* the next track has already begun recording, so
                // the consume path / shutdown are responsible for cleanup.
                mpris->update_playback_status(false);
                update_discord_presence();
            }
    }
    };
    g_player->set_event_callback(g_main_event_cb);

    // ---- Ultra‑low‑power main loop ----
    // The render rate is capped per activity level instead of tracking the
    // input clock:
    //   * scroll easing / loading spinner → 60 FPS (visible motion)
    //   * steady playback                 → ~10 FPS (only the seek bar
    //     moves, a fraction of a pixel per frame -- 10 FPS is visually
    //     identical to 60 for that and costs ~6x less CPU)
    //   * fully idle                      → repaint only when a real event
    //     arrives, a thumbnail lands, or background data is published (a
    //     ~1 s periodic tick is the safety net for anything unflagged, and
    //     it keeps MPRIS / media-key commands from lagging more than a
    //     second since glib callbacks only run at the top of the loop).
    // SDL_WaitEventTimeout() parks the thread in the kernel between frames,
    // so a full ImGui layout + GL redraw does NOT run every ~1 ms merely
    // because media is playing. That was the dominant CPU/GPU drain: the
    // loop below used wait_ms=1 while playing, i.e. a fresh layout + swap
    // on every wakeup.
    bool running = true;
    bool show_demo = false;
    auto last_save_time = std::chrono::steady_clock::now();
    auto last_frame_time = last_save_time;
    auto next_frame = last_save_time;
    auto last_activity_time = last_save_time;

    diagnostics::start_watchdog();
    DLOG("boot: loop at %lld", DNOW());
    while (running && !g_quit_requested)
    {
        // Tell the hang watchdog this thread is alive. If the loop stops
        // advancing (deadlock or a synchronous backend call on the UI thread),
        // the watchdog dumps the stuck stack after a minute.
        diagnostics::heartbeat();
#ifndef P100_WINDOWS
        while (g_main_context_iteration(g_main_context_default(), FALSE))
        {
        }
#endif

        // Battery state is cheap to poll (cached for 30 s). A transition
        // requests one repaint so the Settings "Power" row and the frame
        // budget update immediately.
        power::refresh();
        const bool low_power = power::low_power();
        static bool s_last_low_power = low_power;
        if (low_power != s_last_low_power)
        {
            s_last_low_power = low_power;
            g_state.repaint_requested = true;
        }

        // Target frame interval for the current activity. Full 60 FPS is
        // reserved for visible animation (scroll easing, loading spinner)
        // and for a short grace period after any input: a scroll ease-out
        // is driven by frame-to-frame state, so if we drop back to the idle
        // cadence the instant the wheel stops, the remaining glide is
        // rendered at ~1 FPS and looks janky. Keeping 60 FPS for ~1 s after
        // the last event (mouse motion included, since that is what the
        // user naturally does to "wake" it) lets the ease finish smoothly.
        // Both *is_*_animating() consume their counter, so each must be
        // queried exactly once per iteration -- call them unconditionally
        // (|| would short-circuit and skip the second one's reset).
        auto now = std::chrono::steady_clock::now();

        // Roll the accent colour over at local midnight without a restart.
        static time_t last_accent_check = 0;
        time_t wall_now = time(NULL);
        if (wall_now - last_accent_check >= 60)
        {
            last_accent_check = wall_now;
            update_accent(g_state.settings);
            ui_theme_refresh_accent();
        }

        bool lyrics_animating = is_lyrics_animating();
        bool ui_animating = is_ui_animating();
        // Toast motion at full rate; expiry wake-ups are handled separately
        // via ui_notify_needs_frame() in the repaint check below.
        bool notify_animating = ui_notify_animating();
        bool animating = lyrics_animating || ui_animating ||
                         notify_animating;
        if (animating)
            last_activity_time = now; // keep the fast cadence until motion settles
        bool recently_active =
            (now - last_activity_time) < std::chrono::milliseconds(1000);

        long long interval_ms = 1000;
        if (animating || g_state.loading || recently_active ||
            g_cf.pending.load() || g_cf.active.load())
            interval_ms = low_power ? 33 : 16;
        else if (g_state.playing)
            interval_ms = low_power ? 250 : 100;
        else if (low_power)
            interval_ms = 2000;

        // Wake at the earlier of the scheduled frame and one current
        // interval from now, so a state change (play started while idle,
        // animated scroll finished) never sleeps past the next deadline.
        auto deadline = std::min(next_frame, now + std::chrono::milliseconds(interval_ms));
        long long wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        if (wait_ms < 0)
            wait_ms = 0;

        SDL_Event event;
        bool have_event = SDL_WaitEventTimeout(&event, (Uint32)wait_ms);

        if (have_event)
        {
            do
            {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT)
                    running = false;
                if (event.type == SDL_USEREVENT)
                    g_state.repaint_requested = true;
                if (event.type == SDL_WINDOWEVENT)
                {
                    if (event.window.event == SDL_WINDOWEVENT_CLOSE &&
                        event.window.windowID == SDL_GetWindowID(window))
                        running = false;
                    // Track visibility so an invisible window never burns GPU
                    // on frames nobody can see.
                    switch (event.window.event)
                    {
                    case SDL_WINDOWEVENT_MINIMIZED:
                    case SDL_WINDOWEVENT_HIDDEN:
                        g_window_hidden = true;
                        break;
                    case SDL_WINDOWEVENT_RESTORED:
                    case SDL_WINDOWEVENT_SHOWN:
                    case SDL_WINDOWEVENT_EXPOSED:
                        if (g_window_hidden)
                            g_state.repaint_requested = true;
                        g_window_hidden = false;
                        break;
                    }
                }
                // Accumulate wheel deltas ourselves — ImGui 1.87+ accumulates
                // io.MouseWheel during NewFrame() from an internal event queue,
                // which is too late for smooth_scroll_begin_frame().
                if (event.type == SDL_MOUSEWHEEL)
                {
#if SDL_VERSION_ATLEAST(2, 0, 18)
                    const float delta = event.wheel.preciseY;
#else
                    const float delta = (float)event.wheel.y;
#endif
                    if (delta != 0.0f)
                    {
                        // Wheel-up is negative scroll. Deltas integrate into
                        // the virtual target 1:1; the chase filter in
                        // update_wheel_inertia() renders them continuously.
                        g_touch_fling_active = false;
                        g_touch_fling_velocity = 0.0f;

                        const Uint32 now_ms = SDL_GetTicks();
                        const Uint32 gap_ms = now_ms - g_wheel_last_event_ms;
                        const float pixels = -delta * kWheelPixelsPerDelta;

                        if (gap_ms <= kWheelGestureGapMs)
                        {
                            if (gap_ms > 0)
                            {
                                const float instant =
                                    pixels / std::max(gap_ms / 1000.0f, 0.001f);
                                g_wheel_input_velocity =
                                    g_wheel_input_velocity * 0.65f + instant * 0.35f;
                            }
                            ++g_wheel_event_count;
                        }
                        else
                        {
                            g_wheel_input_velocity = 0.0f;
                            g_wheel_last_frame_ms = now_ms;
                            g_wheel_event_count = 1;
                        }

                        g_scroll_target += pixels;
                        g_wheel_last_event_ms = now_ms;
                        g_wheel_inertia_active = false;
                        g_wheel_velocity = 0.0f;
                    }

                    // Horizontal touchpad swipe: two-finger navigation, like
                    // a browser. Deltas accumulate during one swipe (events
                    // arriving closer than kHWheelGapMs); crossing the
                    // threshold fires once and re-arms, so a long swipe can
                    // navigate several steps. Field-tested: a rightward
                    // finger swipe sends wheel.x > 0, so that is Forward and
                    // wheel.x < 0 is Back.
#if SDL_VERSION_ATLEAST(2, 0, 18)
                    const float xdelta = event.wheel.preciseX;
#else
                    const float xdelta = (float)event.wheel.x;
#endif
                    if (xdelta != 0.0f)
                    {
                        // A toast under the cursor owns the horizontal
                        // gesture: it tracks the fingers (swipe-to-dismiss)
                        // instead of triggering back/forward navigation.
                        // Same delta direction as nav: positive xdelta is a
                        // rightward finger swipe, so the toast follows it.
                        if (ui_notify_swipe_feed(xdelta * kWheelPixelsPerDelta))
                        {
                            g_state.repaint_requested = true;
                        }
                        else
                        {
                        const Uint32 now_ms = SDL_GetTicks();
                        if (now_ms - g_hwheel_last_ms > kHWheelGapMs ||
                            (g_hwheel_accum != 0.0f && g_hwheel_accum * xdelta < 0.0f))
                        {
                            // New swipe: enough time passed since the last
                            // event, or the direction flipped.
                            g_hwheel_accum = 0.0f;
                        }
                        g_hwheel_accum += xdelta;
                        g_hwheel_last_ms = now_ms;
                        while (std::abs(g_hwheel_accum) >= kHWheelTrigger)
                        {
                            const bool back = g_hwheel_accum < 0.0f;
                            g_hwheel_accum -=
                                back ? -kHWheelTrigger : kHWheelTrigger;
                            if (back)
                                ui_go_back(g_state);
                            else
                                ui_go_forward(g_state);
                            g_state.repaint_requested = true;
                        }
                        }
                    }
                }
                if (event.type == SDL_MOUSEBUTTONDOWN)
                {
                    // A click means the user is taking control; stop any
                    // momentum so the clicked target doesn't drift away.
                    reset_wheel_scroll();
                    g_hwheel_accum = 0.0f;
                    g_touch_fling_active = false;
                    g_touch_fling_velocity = 0.0f;
                }
                if (event.type == SDL_FINGERDOWN ||
                    event.type == SDL_FINGERMOTION ||
                    event.type == SDL_FINGERUP)
                {
                    handle_touch_event(event, window);
                }
                if (event.type == SDL_KEYDOWN)
                {
                    const SDL_Keymod mod = (SDL_Keymod)event.key.keysym.mod;
                    const bool ctrl = (mod & KMOD_CTRL) != 0;
                    const bool shift = (mod & KMOD_SHIFT) != 0;
                    const SDL_Keycode key = event.key.keysym.sym;

                    if (ctrl && shift && key == SDLK_d)
                    {
                        show_demo = !show_demo;
                    }
                    else if (key == SDLK_ESCAPE && !ctrl && !shift)
                    {
                        if (g_state.party_fullscreen)
                            g_state.party_fullscreen = false;
                        else if (g_state.show_shortcuts)
                            g_state.show_shortcuts = false;
                        else if (g_state.current_tab != Tab::Album &&
                                 g_state.current_tab != Tab::Artist &&
                                 g_state.current_tab != Tab::Queue &&
                                 g_state.current_tab != Tab::Settings &&
                                 (g_state.search_buffer[0] || g_state.local_search_active ||
                                  g_state.artist_search_active))
                            ui_reset_search(g_state);
                        else
                            ui_go_back(g_state);
                        g_state.repaint_requested = true;
                    }
                    else if (ImGui::GetIO().WantTextInput)
                    {
                        // Let the focused text field keep the remaining keys.
                    }
                    else if (key == SDLK_l && !ctrl && !shift)
                    {
                        g_state.settings.lyrics_panel_open = !g_state.settings.lyrics_panel_open;
                        save_settings(g_state.settings);
                        if (g_state.settings.lyrics_panel_open)
                        {
                            g_state.lyrics_requested_id = g_state.current_track_id;
                            g_state.lyrics_refresh_requested = true;
                        }
                        g_state.repaint_requested = true;
                    }
                    else if (key == SDLK_s && !ctrl && !shift)
                    {
                        g_state.sidebar_collapsed = !g_state.sidebar_collapsed;
                        g_state.repaint_requested = true;
                    }
                    else if (key == SDLK_q && !ctrl && !shift)
                    {
                        if (g_state.current_tab != Tab::Queue)
                        {
                            g_state.pre_queue_tab = g_state.current_tab;
                            ui_invalidate_forward(g_state);
                        }
                        g_state.current_tab = Tab::Queue;
                        g_state.repaint_requested = true;
                    }
                    else if (key == SDLK_p && !ctrl && !shift)
                    {
                        g_state.current_tab = Tab::Party;
                        g_state.repaint_requested = true;
                    }
                    else if (shift && key == SDLK_SLASH)
                    {
                        g_state.show_shortcuts = !g_state.show_shortcuts;
                        g_state.repaint_requested = true;
                    }
                    else if (ctrl && key == SDLK_COMMA)
                    {
                        ui_invalidate_forward(g_state);
                        g_state.current_tab = Tab::Settings;
                        g_state.repaint_requested = true;
                    }
                    else if (key == SDLK_SLASH || (ctrl && key == SDLK_f))
                    {
                        if (g_state.current_tab != Tab::Home &&
                            g_state.current_tab != Tab::Downloads &&
                            g_state.current_tab != Tab::Liked &&
                            g_state.current_tab != Tab::Artists &&
                            g_state.current_tab != Tab::Playlists &&
                            g_state.current_tab != Tab::Podcasts)
                        {
                            ui_invalidate_forward(g_state);
                            g_state.current_tab = Tab::Home;
                            g_state.current_playlist_source = PlaylistSource::HOME;
                        }
                        g_state.focus_search_requested = true;
                        g_state.repaint_requested = true;
                    }
                    else if (key == SDLK_SPACE && !event.key.repeat)
                    {
                        if (g_state.playing)
                            g_state.user_pause_requested = true;
                        else
                            g_state.user_play_requested = true;
                        g_state.repaint_requested = true;
                    }
                    else if (key == SDLK_LEFT || key == SDLK_RIGHT)
                    {
                        if (ctrl)
                        {
                            if (key == SDLK_LEFT)
                                g_state.prev_requested = true;
                            else
                                g_state.next_requested = true;
                        }
                        else
                        {
                            const double pos = g_player->get_property_double("time-pos");
                            const double dur = g_player->get_property_double("duration");
                            if (pos >= 0.0 && dur > 0.0)
                            {
                                const double step = shift ? 20.0 : 5.0;
                                const double target = key == SDLK_LEFT ? pos - step : pos + step;
                                g_player->seek(std::max(0.0, std::min(dur, target)));
                            }
                        }
                        g_state.repaint_requested = true;
                    }
                    else if (key == SDLK_UP || key == SDLK_DOWN)
                    {
                        const bool fading =
                            g_cf.pending.load() || g_cf.active.load();
                        const double cur = fading
                                               ? (double)g_cf.user_volume
                                               : g_player->get_property_double("volume");
                        const double vol = key == SDLK_UP ? std::min(100.0, cur + (shift ? 10.0 : 5.0))
                                                          : std::max(0.0, cur - (shift ? 10.0 : 5.0));
                        if (fading)
                            g_cf.user_volume = (float)vol;
                        else
                            g_player->set_volume((int)vol);
                        g_state.repaint_requested = true;
                    }
                }
            } while (SDL_PollEvent(&event));

            // Any real input (wheel, motion, click, key…) restarts the
            // 60 FPS grace window so interactive feedback stays smooth.
            last_activity_time = std::chrono::steady_clock::now();
        }

        update_touch_long_press();
        process_touch_click();
        const bool touch_flinging = update_touch_inertia();
        const bool wheel_flinging = update_wheel_inertia();
        if (touch_flinging || wheel_flinging)
            last_activity_time = std::chrono::steady_clock::now();

        g_player->process_events();
        g_fade_player->process_events();

        // A background worker published data and/or a decoded thumbnail is
        // waiting for a GPU upload -- force a repaint now (not at the next
        // idle tick) so the screen reflects it immediately. A toast that
        // expired while the loop was idle also requests a frame here (which
        // starts its fade-out; the fade itself flips ui_notify_animating()).
        bool repaint_now = g_state.repaint_requested.exchange(false) ||
                           has_pending_textures() || ui_notify_needs_frame();
        // While the window is hidden, repaint requests must not spin the
        // loop: textures stay queued until the window is shown again.
        const bool render_wake = repaint_now && !g_window_hidden;

        // ---- Non‑event work (frame‑paced) ----
        // Only run when a frame is due (deadline reached) or something
        // actually changed (input event, data/thumbnail arrival). Between
        // the two the thread stays parked in those wait calls above.
        now = std::chrono::steady_clock::now();
        if (!have_event && !render_wake && now < deadline)
            continue;

        {

            // Only count a listen once 10s of real playback have elapsed.
            // Wall-clock is only accumulated while playing, so pauses and
            // skipped tracks don't count.
            double frame_dt = std::chrono::duration<double>(now - last_frame_time).count();
            last_frame_time = now;
            if (g_state.playing && !g_state.listen_counted && !g_state.listen_track_id.empty())
            {
                g_state.listen_seconds += frame_dt;
                if (g_state.listen_seconds >= 10.0)
                {
                    commit_play_count();
                    g_state.listen_counted = true;
                }
            }

            // Resolve watchdog: mpv's start/error events normally clear
            // loading; don't let a wedged resolve pin the fast cadence (and
            // the spinner) forever.
            //
            // The UI sets `loading` directly when a row is clicked (it has no
            // access to this clock), so stamp the start time here on a rising
            // edge. Without that, a click on an instantly-playable cached
            // track inherits the *previous* load's timestamp and the watchdog
            // fires on the very next frame -- nagging about a stall while the
            // track is already playing.
            {
                static bool s_loading_prev = false;
                static uint64_t s_loading_seq_prev = 0;
                const bool loading_now = g_state.loading.load();
                const uint64_t seq = g_state.loading_seq.load(std::memory_order_relaxed);
                // Either a new selection arrived (seq bump, even if the flag
                // was already set) or loading rose from false: both restart
                // the clock.
                if (seq != s_loading_seq_prev || (loading_now && !s_loading_prev))
                    g_loading_since = now;
                s_loading_prev = loading_now;
                s_loading_seq_prev = seq;
            }

            if (g_state.loading &&
                std::chrono::duration<double>(now - g_loading_since).count() >
                    kLoadingWatchdogSec)
            {
                // A stale flag on a healthy stream should clear silently; only
                // warn when nothing is actually audible.
                const bool audible = g_player->is_playing();
                g_state.loading = false;
                g_state.status_text = audible ? "Playing" : "Ready.";
                g_state.repaint_requested = true;
                if (!audible)
                {
                    ui_notify(UiNotifyType::Warning, "Playback stalled",
                              "The track took too long to load.", 8.0f);
                }
            }

            // Update MPRIS position (only if playing)
            if (g_state.playing)
            {
                // Cached values (mpv observers drained above) -- no sync API
                // round-trip per frame.
                MpvPlayer *dp = display_player();
                const double pos = dp ? dp->cached_time_pos() : -1.0;
                if (pos >= 0)
                    mpris->update_position(pos);
                if (dp)
                    mpris->update_volume(dp->cached_volume() / 100.0);
            }

            // Append endless-radio tracks to Up Next without changing tabs.
            consume_endless_results();

            // Deferred session-restore seek: fire once the target sits inside
            // the demuxer cache (so the recording stays contiguous). Dropped
            // if the track or any user seek changed meanwhile.
            if (g_pending_restore_seek >= 0.0)
            {
                const bool same_track = g_state.current_track_id == g_pending_restore_seek_id;
                const bool untouched = g_player->seek_generation() == g_pending_restore_seek_gen;
                if (!same_track || !untouched)
                    g_pending_restore_seek = -1.0;
                else if (g_player->is_position_cached(g_pending_restore_seek) ||
                         std::chrono::steady_clock::now() >= g_pending_restore_seek_deadline)
                {
                    const double pos = g_pending_restore_seek;
                    g_pending_restore_seek = -1.0;
                    g_player->seek(pos);
                }
            }

            // A cached song landed on disk (recording finalized / sidecar
            // written / orphan swept): refresh Downloads so it shows up
            // without restarting the app.
            if (g_downloads_dirty.exchange(false))
            {
                if (g_state.downloaded_loaded)
                    load_downloaded_tracks(g_state);
                maybe_sweep_missing_covers();
                g_state.repaint_requested = true;
            }

            // Capture the source duration of the track being recorded (it
            // shows up shortly after the start event); finalize compares the
            // recorded file's duration against it.
            if (!g_recording_path.empty() && g_recording_started && g_recording_duration <= 0.0)
            {
                const double d = g_player->get_property_double("duration");
                if (d > 0.0)
                    g_recording_duration = d;
            }

            // A UI play action replaces Up Next with the new source list
            // before any pending resolve or crossfade decision.
            rebuild_queue_from_source();

            // Crossfade: when the current track is about to end, append
            // the next (cached) one to mpv's playlist so audio-crossfade
            // can overlap the two. Also applies the settings value.
            maybe_start_crossfade();

            // Handle pending manual play
            {
                std::lock_guard<std::mutex> lock(g_data_mutex);
                if (!g_state.pending_play_url.empty())
                {
                    // A click/next/queue pick can land mid-crossfade. Settle
                    // that fade before resolving the new track so the helper
                    // player cannot be left playing an orphaned track.
                    settle_inflight_crossfade();
                    bool fade_started = false;
                    // Optional skip crossfade for track clicks (scope >= 1).
                    // Session restore never takes this path because the main
                    // player is not audible yet, which can_start_manual_crossfade
                    // requires.
                    if (g_state.settings.skip_crossfade_enabled &&
                        g_state.settings.skip_crossfade_scope >= 1 &&
                        g_state.pending_manual_play.load())
                    {
                        Track target;
                        target.id = g_state.pending_video_id;
                        target.title = g_state.pending_play_title;
                        target.artist = g_state.pending_play_artist;
                        target.thumbnail_url = g_state.pending_play_thumbnail_url;
                        target.url = g_state.pending_play_url;
                        target.album_id = g_state.current_album_id;
                        target.artist_id = g_state.current_artist_id;
                        if (auto playlist = get_playlist())
                        {
                            for (const auto &row : *playlist)
                            {
                                if (row.id == target.id)
                                {
                                    target.duration_sec = row.duration_sec;
                                    target.album_id = row.album_id;
                                    target.artist_id = row.artist_id;
                                    break;
                                }
                            }
                        }
                        if (try_start_manual_crossfade(target, g_state.current_track_index))
                        {
                            g_force_fresh_resolve = false;
                            g_state.pending_play_url.clear();
                            g_state.pending_manual_play = false;
                            fade_started = true;
                        }
                    }

                    if (!fade_started)
                    {
                        const bool fresh = g_force_fresh_resolve;
                        g_force_fresh_resolve = false;
                        if (!fresh)
                            g_play_retry_count = 0;
                        DLOG("spawn resolve id=%s url=%s%s", g_state.pending_video_id.c_str(),
                             g_state.pending_play_url.c_str(), fresh ? " (fresh)" : "");
                        const std::string pending_id = g_state.pending_video_id;
                        const std::string pending_url = g_state.pending_video_url;
                        const std::string pending_title = g_state.pending_play_title;
                        const std::string pending_artist = g_state.pending_play_artist;
                        const std::string pending_thumb = g_state.pending_play_thumbnail_url;
                        const std::uint64_t pending_gen = g_play_generation.load();
                        tasks::spawn([pending_id, pending_url, pending_title, pending_artist,
                                      pending_thumb, fresh, pending_gen]() {
                            resolve_audio(pending_id, pending_url, pending_title,
                                          pending_artist, pending_thumb, fresh, pending_gen);
                        });
                        g_state.pending_play_url.clear();
                    }
                }
            }

            // Consume resolved audio
            {
                std::lock_guard<std::mutex> lock(g_play_queue_mutex);
                while (!g_play_queue.empty())
                {
                    PlayRequest req = std::move(g_play_queue.front());
                    g_play_queue.pop();

                    // A newer selection superseded this resolve (slow
                    // non-cached extraction finishing after the user clicked
                    // again or the old track hit EOF): drop it instead of
                    // replacing the track that is actually wanted now.
                    if (req.gen != g_play_generation.load())
                    {
                        DLOG("drop stale play request id=%s (gen %llu, now %llu)",
                             req.id.c_str(),
                             (unsigned long long)req.gen,
                             (unsigned long long)g_play_generation.load());
                        continue;
                    }

                    // A crossfade may have started while this resolve was in
                    // flight (the non-cached target takes a network round-trip).
                    // Settle it first so the helper player doesn't keep playing
                    // a stale "next" track underneath this request.
                    if (g_cf.pending.load() || g_cf.active.load())
                        settle_inflight_crossfade();

                    // Anything still recording here was skipped, not
                    // finished (finished tracks are finalized in the "end"
                    // handler), so drop the partial file.
                    discard_recording();

                    if (req.remote)
                    {
                        std::string ext = stream_container_ext(req.path);
                        // Both paths "" out when the cache dirs can't be
                        // created; play without recording instead of writing
                        // into a directory that doesn't exist.
                        g_recording_temp = recorded_audio_temp_path(req.id, ext);
                        g_recording_path = g_recording_temp.empty() ? ""
                                                                    : recorded_audio_path(req.id, ext);
                        if (g_recording_temp.empty() || g_recording_path.empty())
                        {
                            fprintf(stderr, "Recording disabled for %s (cache dir unavailable)\n",
                                    req.id.c_str());
                            g_recording_temp.clear();
                            g_recording_path.clear();
                            g_recording_id.clear();
                            g_player->set_stream_record("");
                        }
                        else
                        {
                            g_recording_id = req.id;
                            g_recording_started = false;
                            g_recording_duration = 0.0;
                            // Record into the .tmp subdir (not scanned by
                            // Downloads) and move it into place on finish.
                            g_player->set_stream_record(g_recording_temp);
                        }
                    }
                    else
                    {
                        g_player->set_stream_record("");
                    }

                    g_player->play(req.path);
                    // For a normal (non-restore) play, the intent is simply
                    // "play". During a session restore the intent was seeded
                    // from the saved state at startup (and may have been
                    // changed by a play/pause click while resolving), so it
                    // must not be clobbered here.
                    if (!g_state.session_restore_pending)
                        g_state.playing = true;
                    // Keep loading=true until mpv's START_FILE event so a
                    // failed (non-cached) stream still has loading set and the
                    // error-retry path can fire. START_FILE clears it.
                    g_state.status_text = g_state.playing ? "Playing" : "Paused";
                }
            }

            // Queue panel: start a row immediately (it leaves the queue).
            if (g_state.queue_play_index >= 0)
            {
                const int qi = g_state.queue_play_index;
                g_state.queue_play_index = -1;
                if (qi >= 0 && qi < (int)g_state.play_queue.size())
                {
                    Track next = g_state.play_queue[qi];
                    // Starting a queue row consumes it and everything before
                    // it; those entries were skipped by the host.
                    g_state.play_queue.erase(g_state.play_queue.begin(),
                                             g_state.play_queue.begin() + qi + 1);
                    g_state.queue_dirty = true;
                    if (g_state.settings.skip_crossfade_enabled &&
                        g_state.settings.skip_crossfade_scope >= 2 &&
                        try_start_manual_crossfade(next, -1))
                    {
                        // Skip fade started; the queue bookkeeping above is
                        // already done.
                    }
                    else
                    {
                        begin_play_track(next, -1);
                    }
                }
            }

            // UI volume edits: during a fade this updates the target used by
            // both players' ramp, so the slider never fights the gain curve.
            if (g_state.volume_request >= 0)
            {
                const int v = std::clamp(g_state.volume_request, 0, 100);
                g_state.volume_request = -1;
                g_cf.user_volume = (float)v;
                if (!(g_cf.pending.load() || g_cf.active.load()))
                    g_player->set_volume(v);
                g_state.repaint_requested = true;
            }

            // Next / Prev
            if (g_state.next_requested)
            {
                g_state.next_requested = false;
                advance_playback(true);
            }
            if (g_state.prev_requested)
            {
                g_state.prev_requested = false;
                // Previous walks actual playback history now that the queue
                // is the only ordering source. The current track sits at the
                // front of that history, so play the first different entry.
                Track prev;
                bool found = false;
                for (const Track &h : g_state.playback_history)
                {
                    if (h.id.empty() || h.id == g_state.current_track_id)
                        continue;
                    prev = h;
                    found = true;
                    break;
                }
                if (found)
                {
                    if (!try_start_manual_crossfade(prev, -1))
                        begin_play_track(prev, -1);
                }
            }

            // Refresh
            if (g_state.refresh_requested)
            {
                g_state.refresh_requested = false;
                g_state.home_refreshing = true;
                g_state.status_text = "Loading...";
                tasks::spawn([]() { load_recommendations(true); });
            }

            // Search: the UI debounces and snapshots the query; we run the
            // actual fetch here on a worker and publish results atomically,
            // discarding stale responses via search_id.
            if (g_state.search_dirty)
            {
                g_state.search_dirty = false;
                std::string query = g_state.queued_search;
                int expect_id = g_state.search_id;
                tasks::spawn([query, expect_id]() { search_all_and_update(query, expect_id); });
            }
            if (g_state.auth_refresh_requested)
            {
                g_state.auth_refresh_requested = false;
                stop_backend();
                start_backend();
                g_state.repaint_requested = true;
            }
            if (g_state.podcast_search_dirty)
            {
                g_state.podcast_search_dirty = false;
                std::string query = g_state.queued_podcast_search;
                int expect_id = g_state.podcast_search_id;
                tasks::spawn([query, expect_id]() {
                    auto shows = fetch_podcast_search(query);
                    if (g_state.podcast_search_id != expect_id)
                        return;
                    std::atomic_store(&g_state.podcast_search_results,
                                      std::make_shared<const std::vector<PodcastShow>>(
                                          std::move(shows)));
                    g_state.podcast_search_loading = false;
                    g_state.repaint_requested = true;
                });
            }
            if (g_state.artist_search_dirty)
            {
                g_state.artist_search_dirty = false;
                std::string query = g_state.queued_artist_search;
                int expect_id = g_state.search_id;
                tasks::spawn([query, expect_id]() { artist_search_and_update(query, expect_id); });
            }

            // Lyrics: prefetch the current track's lyrics in the background
            // as soon as a track starts, so they are ready the moment the
            // lyrics panel is opened. Runs entirely off the audio path (disk
            // cache first, then a metadata-only backend call), so cached
            // tracks get lyrics too.
            {
                bool want = g_state.lyrics_refresh_requested.exchange(false);
                if (g_state.lyrics_dirty)
                {
                    g_state.lyrics_dirty = false;
                    want = true;
                }
                if (want && !g_state.lyrics_requested_id.empty())
                {
                    auto snap = std::atomic_load(&g_state.lyrics);
                    bool have = snap && snap->track_id == g_state.lyrics_requested_id && snap->lyrics.ok;
                    if (!have)
                    {
                        g_state.lyrics_seq++;
                        g_state.lyrics_loading = true;
                        const std::string lyrics_id = g_state.lyrics_requested_id;
                        const int lyrics_seq = (int)g_state.lyrics_seq;
                        tasks::spawn([lyrics_id, lyrics_seq]() {
                            load_lyrics_worker(lyrics_id, lyrics_seq);
                        });
                    }
                }
            }

            // Sleep timer: ramp the volume down over the final 5 s, then
            // pause and restore the user's level.
            if (g_state.sleep_armed)
            {
                auto remain = std::chrono::duration_cast<std::chrono::seconds>(
                                  g_state.sleep_deadline - now)
                                  .count();
                if (remain <= 0)
                {
                    g_state.sleep_armed = false;
                    g_state.sleep_fade_started = false;
                    if (g_state.sleep_volume_restore >= 0)
                        g_player->set_volume(g_state.sleep_volume_restore);
                    g_state.sleep_volume_restore = -1;
                    g_state.user_pause_requested = true;
                    ui_notify(UiNotifyType::Info, "Sleep timer",
                              "Paused — good night.");
                }
                else if (remain <= 5 && g_state.playing)
                {
                    if (!g_state.sleep_fade_started)
                    {
                        g_state.sleep_volume_restore =
                            (int)g_player->get_property_double("volume");
                        g_state.sleep_fade_started = true;
                    }
                    float base = (float)g_state.sleep_volume_restore;
                    g_player->set_volume((int)(base * (float)remain / 5.0f));
                    g_state.repaint_requested = true;
                }
            }

            // Play/Pause (explicit state, never a blind toggle)
            if (g_state.user_play_requested)
            {
                DLOG("USER play (pending=%d)", (int)g_state.session_restore_pending);
                g_player->set_paused(false);
                g_state.playing = true;
                g_state.user_play_requested = false;
                mpris->update_playback_status(true);
                update_discord_presence();
            }
            if (g_state.user_pause_requested)
            {
                DLOG("USER pause");
                g_player->set_paused(true);
                g_state.playing = false;
                g_state.user_pause_requested = false;
                mpris->update_playback_status(false);
                update_discord_presence();
            }

            // Save session periodically. Each file is written only when its
            // content actually changed -- an idle app used to rewrite five
            // files every 10 s for no reason (disk wakeups + wear).
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_save_time).count() >= 10)
            {
                // Pick up Settings changed while running (Discord id, write-back).
                writeback_set_enabled(g_state.settings.server_sync);
                discord_apply_client_id();

                // Periodic cover sweep: heals downloads whose cover fetch
                // failed transiently at some point (backlogged ones heal on
                // the first pass; the timer starts already due for that).
                static auto last_cover_sweep = now - std::chrono::minutes(15);
                if (std::chrono::duration_cast<std::chrono::minutes>(now - last_cover_sweep).count() >= 15)
                {
                    last_cover_sweep = now;
                    maybe_sweep_missing_covers();
                }

                MpvPlayer *dp = display_player();
                const double pos = dp ? dp->get_property_double("time-pos") : 0.0;
                const int vol = (g_cf.pending || g_cf.active)
                                    ? (int)g_cf.user_volume
                                    : (int)g_player->get_property_double("volume");
                std::string sess_sig = g_state.current_track_id + '|' +
                                       g_state.current_title + '|' +
                                       g_state.current_artist + '|' +
                                       std::to_string((int)pos) + '|' +
                                       std::to_string(vol) + '|' +
                                       (g_state.playing ? "p" : "s") + '|' +
                                       std::to_string((int)g_state.current_playlist_source.load()) + '|' +
                                       std::to_string(g_state.current_track_index) + '|' +
                                       g_state.search_buffer;
                static std::string s_last_sess_sig;
                static int s_last_vol = -1;
                if (sess_sig != s_last_sess_sig || s_last_vol != vol)
                {
                    save_session(g_state, *display_player(),
                                 (g_cf.pending || g_cf.active)
                                     ? (int)g_cf.user_volume
                                     : -1);
                    s_last_sess_sig = sess_sig;
                    s_last_vol = vol;
                }

                // Settings: the notes box only saves on blur, and a blur that
                // happens while the panel is being swapped out (tab click)
                // would otherwise wait for the next pass -- so still compare
                // and flush on any real change.
                static Settings s_last_saved;
                static bool s_settings_saved = false;
                if (!s_settings_saved || !(g_state.settings == s_last_saved))
                {
                    save_settings(g_state.settings);
                    s_last_saved = g_state.settings;
                    s_settings_saved = true;
                }

                if (g_history_dirty.exchange(false))
                    save_playback_history(g_state.playback_history);
                if (g_counts_dirty.exchange(false))
                {
                    save_play_counts(g_state.play_counts);
                    save_play_log(g_state.play_log);
                }
                if (g_state.queue_dirty.exchange(false))
                    save_play_queue(g_state.play_queue);
                last_save_time = now;
            }

            // Party: drain guest add/undo requests here, on the main thread.
            // HTTP workers only ever enqueue immutable requests; this call is
            // the single place where guest songs enter state.play_queue.
            if (party_running())
            {
                const double party_pos = g_player->cached_time_pos();
                const double party_dur = g_player->cached_duration();
                party_tick(g_state, party_pos, party_dur, !g_state.playing.load());
            }

            // ---- Rendering decision ----
            // Housekeeping above runs on its own cadence; the full ImGui
            // layout + GL swap only happens when something is actually
            // visible to render: an input event, animation, a requested
            // repaint, loading feedback, the moving seek bar while playing,
            // or a 5 s safety frame that catches any unflagged state drift.
            // A minimized/hidden window never renders at all.
            static auto last_render_time = std::chrono::steady_clock::now();
            const bool want_render =
                (have_event || repaint_now || animating || g_state.loading ||
                 recently_active || g_cf.pending.load() ||
                 g_cf.active.load() || (g_state.playing) ||
                 (now - last_render_time) > std::chrono::seconds(5)) &&
                !g_window_hidden;
            if (want_render)
            {
            // Re-scale the ImGui style only when something that feeds it
            // changed. Re-running ScaleAllSizes() and the full palette
            // assignment on every frame was pure per-frame CPU work.
            static Settings s_last_applied_settings;
            static bool s_settings_applied = false;
            static bool s_applied_low_power = low_power;
            if (!s_settings_applied ||
                !(g_state.settings == s_last_applied_settings) ||
                low_power != s_applied_low_power ||
                ui_theme_animating())
            {
                apply_ui_scale(g_state.settings);
                s_last_applied_settings = g_state.settings;
                s_settings_applied = true;
                s_applied_low_power = low_power;
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            smooth_scroll_begin_frame(); // capture & zero io.MouseWheel BEFORE NewFrame consumes it
            ImGui::NewFrame();           // now sees io.MouseWheel == 0 → no default scroll

            process_texture_queue();

            // Mirror crossfade state for the backdrop transition. The colour
            // wash eases over the same window as the audio blend.
            g_state.crossfade_active = g_cf.pending.load() || g_cf.active.load();
            g_state.crossfade_seconds = (float)g_cf.seconds;
            if (g_state.crossfade_active)
            {
                const double elapsed = std::chrono::duration<double>(now - g_cf.start).count();
                g_state.crossfade_progress =
                    (float)std::clamp(elapsed / std::max(g_cf.seconds, 0.1), 0.0, 1.0);
                g_state.crossfade_user_volume = (int)std::lround(g_cf.user_volume);
                if (g_fade_player)
                {
                    g_state.crossfade_position =
                        std::max(0.0, g_fade_player->cached_time_pos());
                    g_state.crossfade_duration =
                        std::max(0.0, g_fade_player->cached_duration());
                }
                if (g_state.crossfade_duration <= 0.0 && g_cf.track.duration_sec > 0)
                    g_state.crossfade_duration = (double)g_cf.track.duration_sec;

                // Lyrics switch at the same midpoint as the bottom bar.
                if (g_state.crossfade_progress >= 0.5f &&
                    g_state.lyrics_requested_id != g_state.current_track_id)
                {
                    g_state.lyrics_requested_id = g_state.current_track_id;
                    g_state.lyrics_dirty = true;
                    g_state.lyrics_loading = true;
                }
            }
            else
            {
                g_state.crossfade_progress = 0.0f;
                g_state.crossfade_position = 0.0;
                g_state.crossfade_duration = 0.0;
            }

            ui_backdrop_update(g_state);
            ui_backdrop_draw();

            draw_sidebar(g_state);
            draw_content_header(g_state);
            draw_main_ui(g_state, *g_player, true);
            draw_bottom_bar(g_state, *g_player);
            draw_party_chip(g_state);
            updater::poll();
            ui_notify_draw();
            // Draw the QR takeover after the toasts so it is a true top-most
            // surface. The modal already blocks clicks to everything behind it.
            draw_party_fullscreen(g_state);

            if (show_demo)
            {
                ImGui::Begin("Unicode Test", &show_demo);
                ImGui::Text("English: Hello World");
                ImGui::Text("Cyrillic: Привет, мир!");
                ImGui::Text("Romanian: Bună ziua, lume!");
                ImGui::Text("Romanian diacritics: ă â î ș ț");
                ImGui::End();
            }

            if (g_state.show_shortcuts)
            {
                ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(430.0f * ui_m().layout_scale, 0.0f), ImGuiCond_Always);
                ImGui::Begin("Keyboard shortcuts", &g_state.show_shortcuts,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings);
                ImGui::TextColored(ui_c().text_muted, "Playback");
                ImGui::Text("Space            Play / pause");
                ImGui::Text("Left / Right     Seek -5s / +5s");
                ImGui::Text("Shift + arrows   Seek -20s / +20s");
                ImGui::Text("Ctrl + Left/Right Previous / next track");
                ImGui::Text("Up / Down        Volume");
                ImGui::Spacing();
                ImGui::Text("L                Toggle lyrics panel");
                ImGui::Text("S  Toggle sidebar");
                ImGui::Text("Q  Up Next queue");
                ImGui::Text("P  Party mode");
                ImGui::Spacing();
                ImGui::TextColored(ui_c().text_muted, "Navigation");
                ImGui::Text("/ or Ctrl+F      Focus search");
                ImGui::Text("Esc              Back / clear search");
                ImGui::Text("?                Toggle this help");
                ImGui::End();
            }

            ImGui::Render();
            glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
            glClearColor(ui_c().bg.x, ui_c().bg.y, ui_c().bg.z, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ui_backdrop_render_shader();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            SDL_GL_SwapWindow(window);
            last_render_time = std::chrono::steady_clock::now();
            }

            // Don't spin: schedule the next frame at a whole interval out.
            next_frame = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms);
        }
    }

    diagnostics::stop_watchdog();
    DLOG("shutdown: save session");
    // ---- Save session on shutdown ----
    save_session(g_state, *g_player,
                 (g_cf.pending || g_cf.active) ? (int)g_cf.user_volume : -1);
    save_settings(g_state.settings); // catch edits made after the last widget blur
    save_playback_history(g_state.playback_history);
    save_play_counts(g_state.play_counts);
    save_play_log(g_state.play_log);
    save_play_queue(g_state.play_queue);

    // Drop any half-recorded stream (it wasn't finished).
    discard_recording();

    // ---- Cleanup ----
    DLOG("shutdown: party stop");
    party_stop();
    DLOG("shutdown: backend stop");
    stop_backend();
    DLOG("shutdown: writeback");
    stop_writeback_worker();
    DLOG("shutdown: library");
    library::shutdown();
    DLOG("shutdown: tasks");
    tasks::shutdown();
    DLOG("shutdown: textures");
    clear_texture_cache();
    DLOG("shutdown: players");
    g_player.reset();
    g_fade_player.reset();
    g_player_raw = nullptr;
    mpris.reset();
    DLOG("shutdown: imgui gl");
    ImGui_ImplOpenGL3_Shutdown();
    DLOG("shutdown: imgui sdl");
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    DLOG("shutdown: gl context");
    SDL_GL_DeleteContext(gl_context);
    DLOG("shutdown: window");
    SDL_DestroyWindow(window);
    DLOG("shutdown: sdl quit");
    SDL_Quit();
    DLOG("shutdown: notify");

#ifndef P100_WINDOWS
    if (notify_init_done)
        notify_uninit();
#endif
    DLOG("shutdown: done");
    // GLib/GDBus and SDL can leave atexit/static teardown waiting on helper
    // threads after main returns. Every resource we own has been released
    // above, so leave the process deterministically instead of hanging on a
    // library's static destructor.
    std::fflush(nullptr);
    std::_Exit(0);
}