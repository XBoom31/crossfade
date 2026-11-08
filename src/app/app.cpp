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
#include "app_internal.h"
#include "session.h"

// ---- libnotify (Linux desktop notifications; no-op on Windows) ----
#ifndef P100_WINDOWS
#include <libnotify/notify.h>
#endif

// DLOG / DNOW live in app_internal.h so the split app modules share them.

// ----------------------------------------------------------------------
// Global state and player
// ----------------------------------------------------------------------
AppState g_state;
std::unique_ptr<MpvPlayer> g_player;
// MPRIS holds a reference to this pointer, so it follows player role swaps
// during crossfade (see maybe_start_crossfade).
MpvPlayer *g_player_raw = nullptr;
// Second mpv instance used to fade in the next track while the main one
// fades out; after the swap the roles reverse for the next crossfade.
std::unique_ptr<MpvPlayer> g_fade_player;
class MprisPlayer *g_mpris = nullptr;
// The main and fade player event handlers, stored so they can be re-bound
// when the two players swap roles at the end of a crossfade (each MpvPlayer
// keeps the callback it was given, but roles move between the objects).
std::function<void(const std::string &)> g_main_event_cb;
std::function<void(const std::string &)> g_fade_event_cb;
std::mutex g_data_mutex;

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

    static std::unordered_map<std::string, float> s_applied_elements;
    if (s_applied_elements != s.ui_elements)
    {
        s_applied_elements = s.ui_elements;
        g_ui_elem.clear();
        for (const auto &kv : s.ui_elements)
            g_ui_elem[kv.first] = clampf(kv.second, 0.25f, 3.0f);
    }
}

std::queue<PlayRequest> g_play_queue;
std::mutex g_play_queue_mutex;
// Bumped by begin_play_track(): every selection invalidates resolves still
// in flight for an earlier selection. A slow (non-cached) resolve that
// finishes after the user/EOF moved on must not start playing its stale
// track on top of the current one.
std::atomic<std::uint64_t> g_play_generation{0};

// Pre-resolved stream URL for the upcoming track, so advancing at end of
// track doesn't wait on a network round-trip.
std::map<std::string, std::string> g_prefetched_urls; // video_id -> stream URL
std::mutex g_prefetch_mutex;

// Endless radio: one refill attempt per playing track. The result is
// published through AppState and consumed on the main thread.
bool g_endless_waiting = false;
std::string g_endless_seed;
// Crossfade state and pure decisions live in playback/crossfade.*. The mpv
// role swap and event handling stay here with the rest of the playback loop.
crossfade::State g_cf;

// Window visibility: when minimized/hidden the UI is not visible, so the
// render path is skipped entirely and the loop only wakes for events and
// low-rate housekeeping (playback, saves, MPRIS keep running).
static bool g_window_hidden = false;
// Dirty flags for the periodic persistence pass: history / play counts /
// play log only change on track starts and commits, so the 10 s save pass
// skips them (and the settings file) unless something actually changed.
std::atomic<bool> g_history_dirty{false};
std::atomic<bool> g_counts_dirty{false};

// Watchdog for the resolve path: loading is cleared by mpv's start/error
// events; if neither ever fires (worker died, backend wedged) this caps how
// long the 60 FPS spinner cadence can be pinned.
std::chrono::steady_clock::time_point g_loading_since;
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
MpvPlayer *display_player()
{
    if ((g_cf.pending.load() || g_cf.active.load()) && g_fade_player &&
        g_state.crossfade_progress >= 0.5f)
        return g_fade_player.get();
    return g_player.get();
}

void update_discord_presence()
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
bool g_endless_attempted = false;

// Fresh stream re-resolution after an uncached track fails to load.
bool g_force_fresh_resolve = false;
int g_play_retry_count = 0;

// Path of the in-progress stream recording (empty when not recording).
std::string g_recording_path; // final destination
std::string g_recording_temp; // where mpv is writing right now
std::string g_recording_id;
// Source duration of the track being recorded (mpv's "duration" property of
// the remote stream), captured after its start event. Compared against the
// finished recording's duration before it's accepted into the cache.
bool g_recording_started = false;
double g_recording_duration = 0.0;
bool notify_init_done = false;

// Session-restore seek that had to wait for the demuxer cache: seeking past
// the cached region would leave a hole in the stream recording, so the jump
// is issued once the target is covered (or when the deadline passes).
double g_pending_restore_seek = -1.0;
std::string g_pending_restore_seek_id;
std::uint64_t g_pending_restore_seek_gen = 0;
std::chrono::steady_clock::time_point g_pending_restore_seek_deadline;

// ----------------------------------------------------------------------
// Notification helper
// ----------------------------------------------------------------------
void show_notification(const std::string &title, const std::string &body, const std::string &icon_path)
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
// Materialize the active source list into the single global queue. A UI play
// action sets queue_replace_from_source; this runs once before the pending
// track is resolved. The current track is played directly and the rest of the
// list becomes Up Next, so manual skips and natural advances share one order.
// ----------------------------------------------------------------------
// Background audio resolver
// ----------------------------------------------------------------------
// Why the last stream resolve failed (classified backend error), consumed by
// the mpv error handler to give the "Playback failed" toast a real reason.
std::mutex g_play_fail_mutex;
std::string g_play_fail_hint;

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
// Warm stream URLs one at a time. The backend handles commands serially, so
// firing ten independent prefetch threads (as prefetch_urls_for does) can put
// a user's "start this station" request behind a long queue. This variant
// keeps at most one speculative resolve in flight and stops as soon as the
// caller's request token changes.
// The tracks playback will touch next: the head of the single global queue.
// Cover sweep: a downloaded track's album cover is fetched the first time a
// row showing it is drawn, so one transient failure (offline moment, network
// hiccup) leaves it permanently blank -- the texture loader only retries a
// failed URL while that row is on screen and its backoff has expired. This
// sweep re-fetches missing covers for every downloaded track in the
// background; it is stat-only when everything is already cached.
std::atomic<bool> g_cover_sweep_running{false};
// Full downloads currently running (video_id -> nothing). Guards against
// stacking duplicate downloads for the same track.
std::set<std::string> g_downloading;
std::mutex g_download_mutex;

// Download a track fully into the audio cache in the background. Used to
// pre-fetch the *next* track (so advancing always starts from disk, Spotify
// style) and to repair recordings invalidated by seeks/skips.
// At most this many yt-dlp downloads run at once. Each is a Python
// subprocess (CPU + network burst); letting a track change fan out several
// concurrently stole noticeable power, and the losers of the race get
// retried on the next predownload pass anyway.
std::atomic<int> g_bg_download_count{0};

// While the current song plays, make sure the next one will start from the
// local cache: resolve its URL and pull down the whole file in the
// background (a few MB, seconds at typical bandwidth).
// Container extension for a resolved stream URL (mpv stream-record writes
// the raw stream, so the label must match the mime type). The result is a
// best-effort guess: finalize_recording() re-checks the finished bytes with
// sniff_audio_container_ext() and renames to the real container if wrong.
// The current stream played through to the end: keep the recording and
// write its metadata sidecar (off-thread; the fetch is network-bound).
// A track was skipped/stopped before finishing: drop the partial recording.
// If the track had actually started playing (not an instant skip-through),
// repair it in the background so the cache still ends up with the song.
// ----------------------------------------------------------------------
// Data fetchers
// ----------------------------------------------------------------------
// Lyrics: disk cache first (works offline, including for tracks playing
// from the local audio cache), then a metadata-only backend request that
// asks for the lyrics alone -- no stream resolution, no downloads.
// Start one endless-radio refill when the remaining queue/source is short.
// Returns true if a refill is loading (or was already in flight).
// Append deduped endless-radio results to the queue. When playback already
// stopped at the end of the source list, start the first new entry now.
// Refill the single global queue from the source-list snapshot used for
// repeat-all / shuffle. Manual "Play next" pins and guest requests already in
// the queue are not part of the cycle; they were one-shot entries.
// Decide what plays next without starting it: the head of the global queue.
// `allow_refill` lets natural/manual advance wrap the source cycle when repeat
// is on. Returns false when playback should stop.
// Shared per-track bookkeeping for a fade transition. The helper player's
// events are not routed through the main "start" handler, so metadata,
// history, MPRIS and the notification are updated here when the fade begins.
// A manual skip can crossfade only while audio is actually playing and the
// target already exists as a complete file in the local audio cache.
// Promote the helper player (the track that faded in) to be the main player
// and retire the outgoing one. Shared by timer-driven manual fades and by
// selections that arrive while a fade is already in flight.
// Finish a timer-driven manual fade: promote the incoming helper player to
// be the main player and retire the (cut-short) outgoing one.
// A manual selection (track click, queue row, next/prev) arrived while a
// crossfade is already in flight. Settle that transition first: otherwise
// the helper player can keep playing the old "next" track while the new
// selection is loaded into the main player, leaving an orphaned player the
// app no longer tracks.
// Returns true when the transition was handed to a manual crossfade; false
// means the caller should fall back to the normal instant track switch.
// Advance playback: pop the head of the single global queue. Manual skips and
// natural end-of-track use exactly the same order, so guest requests can no
// longer be stranded by pressing Next.
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

    // Bootstrap the Python backend in parallel with the rest of startup. It
    // takes ~1 s of Python imports; waiting for it here used to dominate boot.
    set_ytdl_browser(g_state.settings.ytdl_browser);
    set_auth_mode(g_state.settings.auth_mode);
    set_warm_stream_resolver(g_state.settings.warm_stream_resolver);
    std::atomic<int> backend_boot_state{0}; // 0 starting, 1 ready, -1 failed
    std::thread backend_boot_thread([&backend_boot_state]() {
        backend_boot_state.store(start_backend() ? 1 : -1, std::memory_order_release);
    });

    // SQLite + saved-data loading runs in parallel with SDL/GL window setup.
    // It costs ~160 ms and does not touch UI or graphics state.
    std::thread data_boot_thread([]() {
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

        g_state.liked_tracks = load_liked_tracks();
        g_state.saved_albums = load_saved_albums();
        g_state.saved_artists = load_saved_artists();
        g_state.playback_history = load_playback_history();
        g_state.play_counts = load_play_counts();
        g_state.play_log = load_play_log();

        auto names = get_playlist_names();
        for (const auto &name : names)
        {
            Playlist pl;
            pl.name = name;
            pl.tracks = load_playlist_tracks(name);
            g_state.playlists.push_back(pl);
        }
    });

    // Early graphics failures still need to tear down the background workers
    // and join every thread before app_run returns.
    auto fatal_exit = [&](int code) {
        cancel_backend_startup();
        if (backend_boot_thread.joinable())
            backend_boot_thread.join();
        if (data_boot_thread.joinable())
            data_boot_thread.join();
        stop_backend();
        return code;
    };

    // ---- Accent colour (custom setting, system portal or daily fallback) ----
    update_accent(g_state.settings);

    // ---- libnotify init ----
#ifndef P100_WINDOWS
    notify_init("crossfade");
    notify_init_done = true;
#endif

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
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_Window *window = SDL_CreateWindow("crossfade",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 820,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return fatal_exit(1);
    }
    SDL_SetWindowMinimumSize(window, 860, 600);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context)
    {
        fprintf(stderr, "OpenGL 3.2 core context unavailable: %s\n", SDL_GetError());
        return fatal_exit(1);
    }
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    // Saved data is ready now (the loader ran in parallel with window/GL
    // setup); main-thread consumers can use it safely.
    if (data_boot_thread.joinable())
        data_boot_thread.join();

    // ---- Load up-next queue and recent searches ----
    g_state.play_queue = load_play_queue();
    // Persisted rows came from the single global queue; keep guest requests
    // ahead of them instead of treating the whole restored list as "Play
    // next" pins.
    for (Track &t : g_state.play_queue)
        t.from_queue_source = true;
    g_state.search_recent = load_search_recent();

    // ---- Load session ----
    load_session(g_state);

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

    // The backend keeps booting in the background: the UI comes up now and
    // the session-restore resolve waits for readiness on its worker thread.

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
        g_state.current_playlist_source = playlist_source_from_name(r.source);
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
    g_main_event_cb = [&mpris](const std::string &event)
    { handle_player_event(event, mpris.get()); };
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
    bool backend_boot_reported = false;
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
        // Report a backend startup failure once we know it; the UI is already
        // up at this point, so this only updates the banner/toast.
        if (!backend_boot_reported && backend_boot_state.load(std::memory_order_acquire) != 0)
        {
            backend_boot_reported = true;
            if (backend_boot_state.load(std::memory_order_acquire) < 0)
            {
                fprintf(stderr, "Failed to start backend.\n");
                BackendError err = take_backend_error();
                if (!err)
                {
                    err.kind = BackendErrKind::Backend;
                    err.message = "The YouTube Music backend failed to start. "
                                  "Check the auth file in Settings.";
                }
                ui_notify_backend_error(err);
                {
                    std::lock_guard<std::mutex> lock(g_data_mutex);
                    g_state.backend_status = err;
                }
                g_state.repaint_requested = true;
            }
        }
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
                    handle_wheel_event(event);

                if (event.type == SDL_MOUSEBUTTONDOWN)
                {
                    // A click means the user is taking control; stop any
                    // momentum so the clicked target doesn't drift away.
                    reset_pointer_gestures();
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
                        if (auto playlist = get_playlist(g_state))
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
                cancel_backend_startup();
                if (backend_boot_thread.joinable())
                    backend_boot_thread.join();
                stop_backend();
                backend_boot_state.store(start_backend() ? 1 : -1,
                                         std::memory_order_release);
                backend_boot_reported = false;
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
            draw_main_ui(g_state, *g_player);
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
            ui_hero_blur_process();
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
    cancel_backend_startup();
    if (backend_boot_thread.joinable())
        backend_boot_thread.join();
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