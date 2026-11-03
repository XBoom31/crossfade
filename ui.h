#pragma once

#include "imgui.h"
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include "ytdlp.h"
#include "ui_theme.h"

// Forward declarations
class MpvPlayer;

// ----------------------------------------------------------------------
// Accent colour (system portal, then today's date-seeded fallback).
// g_accent is the base colour; the others are derived for hover/active and
// for accent-coloured text on the black background.
// ----------------------------------------------------------------------
extern ImVec4 g_accent;
extern ImVec4 g_accent_hover;
extern ImVec4 g_accent_active;
extern ImVec4 g_accent_text;

static inline ImVec4 accent_alpha(const ImVec4 &c, float a)
{
    return ImVec4(c.x, c.y, c.z, a);
}

// ----------------------------------------------------------------------
    // Navigation tabs
    enum class Tab
    {
        Home,
        Downloads,
        Radio,
        Liked,
        Playlists,
        Albums,
        Artists,
        History,
        Stats,
        Queue,
        Album,
        Artist,
        Settings,
        YouTube,
        Smart
    };

    // ----------------------------------------------------------------------
    // Playlist source
    enum class PlaylistSource
    {
        HOME,
        SEARCH,
        DOWNLOADS,
        RADIO,
        LIKED,
        PLAYLIST,
        HISTORY,
        STATS,
        ALBUM,
        ARTIST,
        YOUTUBE_LIKED,
        YOUTUBE_PLAYLIST,
        SMART
    };

// ----------------------------------------------------------------------
// Playlist definition
struct Playlist
{
    std::string name;
    std::vector<Track> tracks;
};

// ----------------------------------------------------------------------
// A saved album (persisted to ~/.config/p100/albums.json). Only the
// metadata needed to render the albums grid is stored; opening the album
// fetches the current track list from the backend.
struct SavedAlbum
{
    std::string browse_id;
    std::string title;
    std::string artist;
    std::string year;
    std::string duration;
    std::string thumbnail_url;
    int track_count = 0;
};

// A saved artist (persisted to ~/.config/p100/artists.json).
struct SavedArtist
{
    std::string browse_id;
    std::string name;
    std::string thumbnail_url;
    std::string subscribers;
    std::string monthly_listeners;
};

// ----------------------------------------------------------------------
// One play event (persisted to ~/.config/p100/play_log.json). Kept small
// on purpose; title/artist/thumbnail are snapshotted at play time so the
// stats tab has metadata even for songs no longer in any list.
struct PlayEvent
{
    std::string id;
    std::string title;
    std::string artist;
    std::string thumbnail_url;
    int64_t ts = 0; // unix seconds
};

// Ring-cap for the in-memory / on-disk event log (oldest are dropped).
static constexpr int kPlayLogCap = 5000;

// ----------------------------------------------------------------------
// Derived listening stats for the active period. Rebuilt on the main
// thread whenever stats_dirty/period changes (see rebuild_stats()).
struct StatsSnapshot
{
    std::vector<Track> tracks; // ranked by plays desc (url = watch link)
    std::vector<int> plays;    // parallel to tracks
    std::string top_artist;
    int top_artist_plays = 0;
    int total_plays = 0;
    int unique_songs = 0;
    int unique_artists = 0;
    // Extended dashboard data.
    std::vector<std::pair<std::string, int>> top_artists; // name, plays
    int hour_plays[24] = {0};
    int weekday_plays[7] = {0};
    int peak_hour = -1;
    int peak_weekday = -1;
    int streak_days = 0;
    int unique_days = 0;
    float repeat_rate = 0.0f;
    double anim_start = 0.0;
};

// ----------------------------------------------------------------------
// Settings structure
struct Settings
{
    float crossfade_duration = 5.0f;
    // Manual-skip crossfade: blend when the user presses Next/Previous (or
    // optionally selects a track), but only when the target is already in the
    // local audio cache. Kept separate from the natural end-of-track fade
    // because skip transitions want a much shorter blend.
    bool skip_crossfade_enabled = false;
    float skip_crossfade_duration = 2.0f;
    // Which manual actions use the skip fade:
    //   0 = Next/Previous only
    //   1 = Next/Previous + track clicks
    //   2 = all manual track changes (also queue rows)
    int skip_crossfade_scope = 0;
    bool volume_normalization = false;
    bool show_notifications = true;
    // Endless music: when the active list and queue run out, append tracks
    // from a radio seeded by the last played song.
    bool endless_radio = false;
    std::string ytdl_browser = "firefox";
    // Backend authentication: "browser" uses browser.json cookies, "oauth"
    // uses browseroauth.json. Applied at backend startup.
    std::string auth_mode = "browser";
    bool lyrics_panel_open = true; // bottom-bar toggle state (persisted)
    // Seconds to shift the synced-lyrics highlight. Positive delays it (lyrics
    // shown later), negative advances it. Applied to both the active-line
    // search and click-to-seek.
    float lyrics_offset = 0.0f;
    // Horizontal width of the lyrics panel in logical units (persisted).
    // Multiplied by the layout scale and clamped at runtime; dragged by the
    // divider between the track list and the lyrics panel.
    float lyrics_panel_width = 380.0f;
    // Global UI scale. Multiplies text size, section heights and widget
    // metrics (1.0 = the app's normal look). Persisted in settings.json.
    float ui_scale = 1.0f;
    // Per-aspect multipliers layered on top of the global scale, so the
    // three parts of the UI can be tweaked independently.
    float ui_text_scale = 1.0f;   // font sizes
    float ui_widget_scale = 1.0f; // paddings, spacing, frame/scrollbar metrics
    float ui_layout_scale = 1.0f; // section heights, album art, buttons, gaps
    // Animation intensity: 0 = off, 1 = subtle, 2 = standard, 3 = playful.
    int animations = 2;
    // Day mode: true = light theme, false = dark theme. Persisted in settings.json.
    bool day_mode = false;
    // Custom accent colour as "#rrggbb". Empty means automatic: follow the
    // desktop accent (XDG portal) or the date-seeded daily colour. P100_ACCENT
    // still takes precedence over both.
    std::string accent_color;
    // Ambient backdrop: tint the background with the playing album's dominant
    // colour (or the app accent when nothing is loaded). Fades smoothly between
    // tracks. `album_backdrop_strength` scales the glow (0.1 - 1.0).
    bool album_backdrop = false;
    float album_backdrop_strength = 0.65f;
    // Glowing separators: a soft album-coloured light streak under each
    // section header, plus album-tinted structural hairlines. Uses the same
    // dominant-colour tint as the backdrop, but works with the backdrop off.
    bool glow_separators = true;
    float glow_separator_strength = 0.75f;
    // Slowly rotate the backdrop's light arrangement every so often, so the
    // glow drifts to a different corner instead of sitting still.
    bool backdrop_drift = true;
    // Playback modes. Repeat: 0 = off, 1 = all, 2 = one. Repeat-all is the
    // default so existing installs keep the old wrap-around behaviour.
    bool shuffle_enabled = false;
    int repeat_mode = 1;
    // Per-element multipliers, keyed by the names in ui_element_defs()
    // (e.g. "bottom_art", "lyrics_art"). Missing key => 1.0. Multiplied on top
    // of ui_layout_scale so a single element can be nudged on its own.
    std::unordered_map<std::string, float> ui_elements;
    // Free-form notes / todo list shown as a multiline textbox in Settings.
    // Persisted in settings.json (JSON-escaped, may contain newlines).
    std::string notes;
    // Download cache size cap in GB; 0 = unlimited (default).
    float download_cap_gb = 0.0f;

    // Experimental: the backend extracts stream URLs in-process (yt-dlp as a
    // module) instead of spawning a yt-dlp subprocess per resolve, making
    // uncached songs start faster. Turn off if playback misbehaves. Applied
    // when the backend processes start (i.e. next app launch).
    bool warm_stream_resolver = true;

    // Playback rate multiplier (0.5 - 2.0). Applied live to both mpv
    // instances so it survives the crossfade role swap.
    float playback_speed = 1.0f;

    // 5-band equalizer (mpv lavfi `equalizer` chain) + volume normalization.
    // Band centres: 60 / 230 / 910 / 3600 / 14000 Hz; gains in dB (-12..+12).
    // Both compose the single `af` string pushed to mpv.
    bool eq_enabled = false;
    float eq_gains[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    // Discord Rich Presence: application client id (empty = off). Also
    // overridable via P100_DISCORD_CLIENT_ID.
    std::string discord_client_id;

    // Server write-back master switch (off by default): mirror likes and
    // playlist edits to the signed-in YouTube Music account.
    bool server_sync = false;

    // Used by the periodic-save dirty check in the main loop.
    bool operator==(const Settings &o) const
    {
        bool eq_same = true;
        for (int i = 0; i < 5; ++i)
            eq_same = eq_same && eq_gains[i] == o.eq_gains[i];
        return crossfade_duration == o.crossfade_duration &&
               skip_crossfade_enabled == o.skip_crossfade_enabled &&
               skip_crossfade_duration == o.skip_crossfade_duration &&
               skip_crossfade_scope == o.skip_crossfade_scope &&
               volume_normalization == o.volume_normalization &&
               show_notifications == o.show_notifications &&
               endless_radio == o.endless_radio && ytdl_browser == o.ytdl_browser &&
               auth_mode == o.auth_mode && lyrics_panel_open == o.lyrics_panel_open &&
               lyrics_offset == o.lyrics_offset &&
               lyrics_panel_width == o.lyrics_panel_width && ui_scale == o.ui_scale &&
               ui_text_scale == o.ui_text_scale &&
               ui_widget_scale == o.ui_widget_scale &&
               ui_layout_scale == o.ui_layout_scale && animations == o.animations &&
               day_mode == o.day_mode && accent_color == o.accent_color &&
               album_backdrop == o.album_backdrop &&
               album_backdrop_strength == o.album_backdrop_strength &&
               glow_separators == o.glow_separators &&
               glow_separator_strength == o.glow_separator_strength &&
               backdrop_drift == o.backdrop_drift &&
               shuffle_enabled == o.shuffle_enabled &&
               repeat_mode == o.repeat_mode && ui_elements == o.ui_elements &&
               notes == o.notes && download_cap_gb == o.download_cap_gb &&
               warm_stream_resolver == o.warm_stream_resolver &&
               playback_speed == o.playback_speed && eq_enabled == o.eq_enabled &&
               eq_same && discord_client_id == o.discord_client_id &&
               server_sync == o.server_sync;
    }
};

// Fine-grained UI element the user can resize individually. `key` is used
// both in settings.json ("ui_elem_<key>") and by ui_elem() at the call site.
struct UiElementDef
{
    const char *key;
    const char *label;
};

// Canonical, ordered list of tunable elements (defined in ui_settings.cpp).
const std::vector<UiElementDef> &ui_element_defs();

// ----------------------------------------------------------------------
// Session state (persistent)
struct SessionState
{
    std::string track_id;
    std::string track_title;
    std::string track_artist;
    std::string track_thumbnail;
    std::string track_url;
    std::string track_album_id;
    std::string track_artist_id;
    std::string source;        // "home", "search", "downloads", "radio", "liked", "playlist"
    std::string playlist_name; // if source == "playlist"
    int index = -1;
    double position = 0.0;
    int volume = 50;
    bool playing = false;
    std::string search_query;
};

// ----------------------------------------------------------------------
// Lyrics display payload. Bundles the track the lyrics belong to with the
// lines themselves so a reader always sees a consistent pair (published
// atomically by the fetch worker, like the track lists).
struct LyricsView
{
    std::string track_id;
    Lyrics lyrics;
};

// ----------------------------------------------------------------------
// Application state
struct AppState
{
    // Current tab
    Tab current_tab = Tab::Home;
    bool sidebar_collapsed = false;
    // Network-fed toggles / list source. Atomic: backend search threads
    // (and radio completion) set this from worker threads.
    std::atomic<PlaylistSource> current_playlist_source{PlaylistSource::HOME};

    // Network-populated track lists. These are swapped atomically by the
    // worker threads that fetch them, so any reader must take a snapshot
    // via std::atomic_load() and operate on that copy -- never on a raw
    // pointer into the vector (it can be freed underneath you).
    std::shared_ptr<const std::vector<Track>> recommendations;
    // Inline error state for the Home feed: set when a fetch fails, so the
    // tab can show what went wrong (and a Retry) instead of a bare empty list.
    bool home_failed = false;
    std::string home_error_msg;
    std::shared_ptr<const std::vector<Track>> search_results;
    std::vector<Track> downloaded_tracks;
    std::shared_ptr<const std::vector<Track>> radio_results;
    std::vector<Track> liked_tracks;
    std::vector<Track> playback_history;
    // Stable display snapshot of the history taken when the History tab is
    // entered, so a click's move-to-front doesn't reshuffle the visible list
    // mid-visit. Refreshed on the next tab (re-)entry.
    std::shared_ptr<const std::vector<Track>> history_view;
    // Smart playlists ("On Repeat" / "Recently Played"): computed from play
    // counts / play log when the tab is entered, same frozen-snapshot idiom
    // as history_view. smart_mode: 0 = On Repeat, 1 = Recently Played.
    std::shared_ptr<const std::vector<Track>> smart_view;
    int smart_mode = 0;
    // Albums the user explicitly saved. Main-thread only; persisted
    // immediately whenever the list changes.
    std::vector<SavedAlbum> saved_albums;
    std::vector<SavedArtist> saved_artists;

    // YouTube account library (fetched from the backend). Main-thread only.
    std::vector<YouTubePlaylist> youtube_playlists;
    std::shared_ptr<const std::vector<Track>> youtube_liked;
    std::shared_ptr<const std::vector<Track>> youtube_playlist_tracks;
    std::string youtube_playlist_loaded_id;
    std::atomic<bool> youtube_liked_loading{false};
    std::atomic<bool> youtube_playlists_loading{false};
    std::atomic<bool> youtube_playlist_loading{false};
    bool youtube_liked_loaded = false;
    bool youtube_playlists_loaded = false;
    bool youtube_playlists_refreshed = false;
    bool youtube_playlist_loaded_from_cache = false;
    std::string youtube_playlist_refreshed_id;
    std::string youtube_playlist_failed_id; // id whose last network fetch failed
    std::string youtube_playlist_error_msg;
    // Pre-cache all playlist contents (not just the one the user opens).
    std::atomic<bool> youtube_playlist_contents_precaching{false};
    bool youtube_playlist_contents_precached = false;
    // Radio station state. The seed metadata is main-thread only; the
    // request counter guards against a slow fetch overwriting a newer
    // station the user picked while it was in flight.
    std::string radio_seed_id;
    std::string radio_seed_title;
    std::string radio_seed_artist;
    std::string radio_seed_thumbnail_url;
    int radio_seed_plays = 0;
    std::atomic<bool> radio_loading{false};
    std::atomic<bool> radio_failed{false};
    std::atomic<uint64_t> radio_request{0};
    // Endless-radio refill results (consumed on the main thread and appended
    // to play_queue without switching tabs).
    std::shared_ptr<const std::vector<Track>> endless_results;
    std::atomic<bool> endless_ready{false};
    std::atomic<bool> endless_loading{false};
    bool downloaded_loaded = false;

    int stats_week_offset = 0; // 0 = this week, 1 = previous week, ...
    // Set by background workers when they publish new screen-relevant data
    // (recommendations, search results, radio, thumbnails, ...). The main
    // loop checks it so an otherwise idle app repaints the moment new
    // content lands instead of waiting out the idle frame interval.
    std::atomic<bool> repaint_requested{false};

    // Home endless scroll state.
    std::atomic<bool> home_loading_more{false};
    // True while a Home recommendation fetch/refresh is in flight; the header
    // uses it to show a spinner instead of the Refresh label.
    std::atomic<bool> home_refreshing{false};
    bool home_has_more = true;

    // Playlists
    std::vector<Playlist> playlists;
    int selected_playlist_index = -1;

    // One-shot forward navigation target. ui_go_back() fills it with the
    // view it is leaving; ui_go_forward() restores it. Any new navigation
    // (tab switch, opening another album/artist) invalidates it.
    struct NavSnapshot
    {
        Tab tab = Tab::Home;
        PlaylistSource source = PlaylistSource::HOME;
        int playlist_index = -1;
        std::string album_id;
        std::string artist_id;
    };
    NavSnapshot nav_forward;
    bool nav_forward_valid = false;

    // Album detail view (drill-down from a track)
    std::string album_browse_id;
    std::string album_title;
    std::string album_artist;
    std::string album_year;
    std::string album_duration;
    std::string album_description;
    std::string album_thumbnail_url;
    std::vector<Track> album_tracks;
    bool album_loading = false;
    // Set when the album fetch failed with no cached copy to show.
    bool album_failed = false;
    std::string album_error_msg;
    std::atomic<bool> album_ready{false};
    // Album audio download progress (one album at a time). The worker only
    // touches the atomics; the browse id is main-thread bookkeeping.
    std::atomic<bool> album_downloading{false};
    std::atomic<int> album_download_done{0};
    std::atomic<int> album_download_total{0};
    std::string album_download_browse_id;
    Tab album_return_tab = Tab::Home;
    PlaylistSource album_return_source = PlaylistSource::HOME;
    int album_return_playlist_index = -1;

    // Artist detail view (drill-down from a track or artist search).
    std::string artist_browse_id;
    std::string artist_name;
    std::string artist_description;
    std::string artist_subscribers;
    std::string artist_monthly_listeners;
    std::string artist_thumbnail_url;
    std::vector<Track> artist_songs;
    std::vector<SavedAlbum> artist_albums;
    bool artist_loading = false;
    // Set when the artist fetch failed with no cached copy to show.
    bool artist_failed = false;
    std::string artist_error_msg;
    std::atomic<bool> artist_ready{false};
    Tab artist_return_tab = Tab::Home;
    PlaylistSource artist_return_source = PlaylistSource::HOME;
    int artist_return_playlist_index = -1;
    std::shared_ptr<const std::vector<ArtistSummary>> artist_search_results;
    std::string queued_artist_search;
    std::atomic<bool> artist_search_dirty{false};
    std::atomic<bool> artist_search_loading{false};
    bool artist_search_active = false;

    // Search
    char search_buffer[512] = "";
    bool search_dirty = false;
    std::string queued_search; // snapshot taken when the debounce fires
    std::atomic<int> search_id{0};
    // Set when the search fetch failed, so results area shows why + Retry.
    bool search_failed = false;
    std::string search_error_msg;
    // 0 = songs, 1 = albums, 2 = artists, 3 = playlists.
    int search_filter = 0;
    std::atomic<bool> search_loading{false};
    std::vector<std::string> search_recent;
    std::shared_ptr<const std::vector<AlbumSummary>> search_album_results;
    std::shared_ptr<const std::vector<ArtistSummary>> search_artist_results;

    // Multi-select in Liked / Downloads / Playlists. IDs are kept per list;
    // selection_context is cleared whenever the user changes lists.
    std::unordered_set<std::string> selected_track_ids;
    std::string selection_context;
    std::string selection_anchor;
    int liked_sort = 0;
    int downloads_sort = 0;

    // Keyboard/shortcut state.
    bool focus_search_requested = false;
    bool show_shortcuts = false;

    // Backend diagnostics shown in Settings.
    bool backend_running = false;
    std::string backend_auth_file;

    // Local search (Downloads, Liked, Playlists)
    std::vector<Track> local_search_results;
    bool local_search_active = false;
    Tab local_search_tab = Tab::Home; // which tab the local filter belongs to

    // Status / errors
    std::string status_text = "Loading...";
    // Last classified backend failure (kind + raw message). Persists until a
    // later success clears it, so Settings can always show what's wrong.
    BackendError backend_status;
    // Human-readable message shown inline by the tab that failed; kept in
    // sync with backend_status by the fetch workers.
    std::string backend_error;
    std::string current_title = "Nothing playing";
    std::string current_artist = "";
    std::string current_thumbnail_url;
    std::string current_track_id;
    std::string current_album_id;
    std::string current_artist_id;
    std::atomic<bool> loading{false};
    // Bumped whenever the UI hands a new play request to the main loop. The
    // resolve watchdog stamps its start time from this, so a click that lands
    // while an older load is still in flight doesn't inherit that older clock
    // (which would make the watchdog fire immediately).
    std::atomic<uint64_t> loading_seq{0};
    std::atomic<bool> playing{false};

    // Mirror of the main loop's crossfade state, refreshed each frame so UI
    // effects (the ambient backdrop) can stretch their own transition to the
    // exact length of an in-flight crossfade. `crossfade_progress` is 0..1.
    bool crossfade_active = false;
    float crossfade_seconds = 0.0f;
    float crossfade_progress = 0.0f;

    // Listening stats (persisted to ~/.config/p100/play_counts.json)
    std::unordered_map<std::string, int> play_counts;

    // Play event log + derived stats (main thread only)
    std::vector<PlayEvent> play_log;
    StatsSnapshot stats;
    int stats_period = 0; // 0 = all time, 1 = this month, 2 = this week
    bool stats_dirty = true;

    // Pending play request
    std::string pending_play_url;
    std::string pending_play_title;
    std::string pending_play_artist;
    std::string pending_play_thumbnail_url;
    std::string pending_video_id;
    std::string pending_video_url;
    std::atomic<bool> pending_manual_play{false};

    // Playback queue. `play_queue` holds only upcoming tracks; the current
    // track lives in the normal current_* fields. `queue_return_index` is
    // the source-list position to resume from once the queue drains.
    std::vector<Track> play_queue;
    int queue_return_index = -1;
    std::atomic<bool> queue_dirty{false};
    // Set by the Queue panel to start a specific queue row now.
    int queue_play_index = -1;
    int current_track_index = -1;
    bool is_search_active = false;
    Tab pre_queue_tab = Tab::Home;

    // Listen-time accounting: a play is only counted after 10s of actual
    // playback, so skipped tracks don't inflate the counter/stats.
    std::string listen_track_id;
    double listen_seconds = 0.0;
    bool listen_counted = false;

    // User controls
    bool user_pause_requested = false;
    bool user_play_requested = false;
    bool refresh_requested = false;
    bool next_requested = false;
    bool prev_requested = false;

    // Sleep timer (session-only). `sleep_at_end_of_track` pauses at the next
    // natural track end instead of a wall-clock deadline; `sleep_fade_started`
    // captures the user's volume so the final 5 s can ramp down and restore.
    bool sleep_armed = false;
    bool sleep_at_end_of_track = false;
    bool sleep_fade_started = false;
    int sleep_volume_restore = -1;
    std::chrono::steady_clock::time_point sleep_deadline{};

    // Context menu
    std::string context_track_id;
    std::string context_track_title;
    std::string context_track_artist;
    std::string context_track_thumbnail;
    std::string context_track_url;

    // Lyrics (side panel). Fetched independently of audio resolution (disk
    // cache first, then a metadata-only backend call) so it also works for
    // tracks playing from the local audio cache. `lyrics` is an atomic
    // snapshot of {track_id, lines}; the request flags are main-thread only.
    std::shared_ptr<const LyricsView> lyrics;
    std::atomic<int> lyrics_seq{0};                    // bumped per spawn (stale-guard)
    std::atomic<bool> lyrics_loading{false};
    std::atomic<bool> lyrics_refresh_requested{false}; // set by bottom-bar toggle
    std::string lyrics_requested_id;                   // track to fetch (main thread)
    bool lyrics_dirty = false;                         // set on track start (main thread)

    // Settings
    Settings settings;

    SessionState session_restore;
    bool session_restore_pending = false;
    std::string current_track_url;

    ImFont *title_font = nullptr;
};

// ----------------------------------------------------------------------
// Layout constants
// ----------------------------------------------------------------------
// These are globals (not constexpr) so the settings scale sliders can update
// them live; ui_px() scales an arbitrary explicit pixel value by the same
// layout factor. See apply_ui_scale() in main.cpp.
extern float g_ui_scale;        // master scale (informational)
extern float g_ui_layout_scale; // effective layout factor (master * layout)
inline float ui_px(float v) { return v * g_ui_layout_scale; }

// Resolved per-element multipliers (settings.ui_elements, clamped), refreshed
// by apply_ui_scale(). ui_elem() scales `base` by the layout factor AND the
// element's own multiplier, so individual elements can be tuned on their own.
extern std::unordered_map<std::string, float> g_ui_elem;
inline float ui_elem(const char *name, float base)
{
    float m = 1.0f;
    auto it = g_ui_elem.find(name);
    if (it != g_ui_elem.end())
        m = it->second;
    return base * g_ui_layout_scale * m;
}

extern float kTopNavH;
extern float kSearchBarH;
extern float kBottomBarH;
static constexpr float kLyricsPanelW = 380.0f;

struct ScopedNoMinSize
{
    ScopedNoMinSize() { ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f)); }
    ~ScopedNoMinSize() { ImGui::PopStyleVar(); }
};

// ----------------------------------------------------------------------
// Helper functions (declared for use across UI files)
// ----------------------------------------------------------------------

// Liked songs
std::string liked_file_path();
void save_liked_tracks(const std::vector<Track> &tracks);
std::vector<Track> load_liked_tracks();

// Saved albums
std::string saved_albums_file_path();
void save_saved_albums(const std::vector<SavedAlbum> &albums);
std::vector<SavedAlbum> load_saved_albums();
bool is_album_saved(const AppState &state, const std::string &browse_id);
// Adds the album, or refreshes its metadata if the browse id is already saved.
void save_album(AppState &state, const SavedAlbum &album);
void remove_saved_album(AppState &state, const std::string &browse_id);

// Saved artists
std::string saved_artists_file_path();
void save_saved_artists(const std::vector<SavedArtist> &artists);
std::vector<SavedArtist> load_saved_artists();
bool is_artist_saved(const AppState &state, const std::string &browse_id);
void save_artist(AppState &state, const SavedArtist &artist);
void remove_saved_artist(AppState &state, const std::string &browse_id);

// Album data cache (~/.config/p100/album_cache/<browse_id>.json). Stores the
// full album header + ordered track list so reopening an album is instant and
// works offline. save_album_cache() is safe to call from worker threads.
std::string album_cache_dir();
std::string album_cache_path(const std::string &browse_id);
bool load_album_cache(const std::string &browse_id, Album &out);
void save_album_cache(const Album &album);
// Fetch an album in the background and write it to the cache (no-op when the
// cache already exists). Used after saving an album.
void prefetch_album_cache(const std::string &browse_id);

// Artist data cache (~/.config/p100/artist_cache/<channel_id>.json).
std::string artist_cache_dir();
std::string artist_cache_path(const std::string &browse_id);
bool load_artist_cache(const std::string &browse_id, ArtistInfo &out);
void save_artist_cache(const ArtistInfo &artist);
void prefetch_artist_cache(const std::string &browse_id);

// Playback history
std::string history_file_path();
void save_playback_history(const std::vector<Track> &tracks);
std::vector<Track> load_playback_history();

// Play counts (times each song has been played)
std::string play_counts_file_path();
std::unordered_map<std::string, int> load_play_counts();
void save_play_counts(const std::unordered_map<std::string, int> &counts);
int play_count_for(const AppState &state, const std::string &id);

// Play event log + listening stats
std::string play_log_file_path();
std::vector<PlayEvent> load_play_log();
void save_play_log(const std::vector<PlayEvent> &events);
void rebuild_stats(AppState &state);
void draw_stats_panel(AppState &state);
void draw_queue_panel(AppState &state);

// Radio station page (~/.config/p100/radio_last.cache holds the last station
// so the tab opens with content and warm stream URLs).
void draw_radio_panel(AppState &state);
void ui_radio_init(AppState &state);
void start_radio_from_track(AppState &state, const Track &seed);

// Up-next queue persistence (~/.config/p100/queue.json).
std::string queue_file_path();
void save_play_queue(const std::vector<Track> &tracks);
std::vector<Track> load_play_queue();
void queue_add_next(AppState &state, const Track &track);
void queue_add_end(AppState &state, const Track &track);
void queue_remove_at(AppState &state, int index);
void queue_move(AppState &state, int from, int to);
void queue_clear(AppState &state);

// Recent searches (~/.config/p100/search_recent.txt), newest first.
std::string search_recent_file_path();
void save_search_recent(const std::vector<std::string> &queries);
std::vector<std::string> load_search_recent();

// Playlists
std::string playlists_dir();
void ensure_playlists_dir();
std::string sanitize_filename(const std::string &name);
std::string playlist_path(const std::string &name);
void save_playlist(const std::string &name, const std::vector<Track> &tracks);
std::vector<Track> load_playlist_tracks(const std::string &name);
std::vector<std::string> get_playlist_names();
void delete_playlist(const std::string &name);
void rename_playlist(const std::string &old_name, const std::string &new_name);

// Lyrics cache (~/.config/p100/lyrics/<video_id>.json). One file per track;
// loading never touches the network. A definitive "no lyrics" result is
// cached too (as a "none" marker) so it isn't re-fetched every play.
std::string lyrics_dir();
std::string lyrics_cache_path(const std::string &video_id);
bool load_lyrics_cache(const std::string &video_id, Lyrics &out);
void save_lyrics_cache(const std::string &video_id, const Lyrics &lyrics);

// Shared UI helpers
std::string truncate_text(const std::string &text, float max_width_px);
// Return to the previous logical view (album/artist/queue, or Home from
// Settings). Used by the header back button and the Escape shortcut.
void ui_go_back(AppState &state);
// Return to the view ui_go_back() last left. No-op when there is nothing
// to return to (nav_forward_valid == false).
void ui_go_forward(AppState &state);
// Drop any pending forward target. Call when the user navigates somewhere
// new so stale forward targets can't fire later.
void ui_invalidate_forward(AppState &state);
int ui_tracks_duration(const std::vector<Track> &tracks);
std::string ui_format_duration(int seconds);
void ui_reset_search(AppState &state);
void ui_set_search(AppState &state, const std::string &query);
bool is_track_liked(const AppState &state, const std::string &id);
void toggle_like(AppState &state, const Track &track);

// Smart playlists: (re)build state.smart_view from the local listening data.
// Mode 0 = "On Repeat" (most played, all time), mode 1 = "Recently Played"
// (most recent distinct tracks). Frozen snapshot, refreshed on tab entry.
void build_smart_view(AppState &state);
void load_downloaded_tracks(AppState &state);
// Fill missing album/artist ids for the current track by finding it in the
// loaded lists. Needed for sessions saved before those ids were persisted.
void resolve_current_track_links(AppState &state);
// Delete one downloaded song from disk (audio file, its .meta.json sidecar
// and any cached lyrics) and drop it from the in-memory download lists.
// Takes the id by value: callers may pass a reference into the very vector
// being erased, which would dangle the moment it shrinks.
void delete_downloaded_track(AppState &state, std::string id);

// Playlist source helpers. Returns an immutable snapshot you can hold
// across frames; the underlying vector is replaced (never mutated) by
// worker threads via atomic swap.
using TrackListPtr = std::shared_ptr<const std::vector<Track>>;
// Non-owning alias around a main-thread vector (liked/playlists/history/
// local search) so playlist access is uniform everywhere.
TrackListPtr alias_of(const std::vector<Track> &v);
TrackListPtr get_playlist(const AppState &state);

// Settings
void save_settings(const Settings &settings);
Settings load_settings();
void draw_settings_panel(AppState &state);

// Session persistence
void save_session(const AppState &state, const class MpvPlayer &player,
                  int volume_override = -1);
void load_session(AppState &state);

// Album navigation
void open_album_view(AppState &state, const std::string &browse_id);
void open_album_from_track(AppState &state, const Track &track);
void close_album_view(AppState &state);

// Artist navigation
void open_artist_view(AppState &state, const std::string &browse_id,
                      const std::string &display_name = "");
void open_artist_from_track(AppState &state, const Track &track);
void close_artist_view(AppState &state);

// ----------------------------------------------------------------------
// Smooth scrolling
// The event loop integrates wheel/touch input into a pixel delta for the
// current frame. Smooth-scrolled children opt out of ImGui's native wheel
// handling (NoScrollWithMouse) and apply this delta themselves.
// ----------------------------------------------------------------------
void add_scroll_pixels(float delta_pixels);
void scroll_clamp_feedback(float unapplied);
float take_scroll_clamp_feedback();
void smooth_scroll_begin_frame();
void smooth_scroll_child(const char *window_name);
bool is_scroll_animating();

// Lyrics panel animation signal. The panel sets this while its auto-centre
// scroll / fade-in is running; the main loop consumes it (like
// is_scroll_animating) to keep rendering at 60 FPS until the motion settles.
void set_lyrics_animating(bool active);
bool is_lyrics_animating();

// ----------------------------------------------------------------------
// Motion helpers. Dear ImGui is immediate-mode, so every animation is
// driven by timestamps/state stored on our side. These helpers centralise
// the intensity setting, easing, list reveal clocks and texture fades.
// ----------------------------------------------------------------------
extern int g_ui_animation_level; // 0 off, 1 subtle, 2 standard, 3 playful
inline bool ui_animations_enabled() { return g_ui_animation_level > 0; }
float ui_anim_ease_out(float t);
float ui_anim_duration(float base_seconds);
float ui_anim_delay(float base_seconds);
float ui_anim_offset(float base_px);
float ui_anim_approach(float current, float target, float speed);
void ui_anim_request();
bool is_ui_animating();
// Page-epoch: bumped on tab switch / search reset. Panels compare their
// stored epoch against this to decide whether to replay their entrance.
uint64_t ui_page_epoch();
void ui_bump_page_epoch();
// Timestamp-driven width animations for the sidebar rail and lyrics drawer.
// Callers may invoke these several times per frame; the returned value is
// stable until the next frame.
float ui_anim_sidebar_width(float target_width);
float ui_anim_lyrics_width(float target_width);

// Returns the reveal start timestamp for a track list, resetting it when the
// content identity changes. `token` can be a shared_ptr address; size and the
// first/last ids catch appends and most refreshes as well.
double ui_anim_list_reveal(const char *key, uint64_t token, size_t count,
                           const std::string &first_id,
                           const std::string &last_id);
double ui_anim_track_list_reveal(const char *key, const std::vector<Track> &tracks,
                                 uint64_t token = 0);
// Forces every list reveal clock to restart on the next frame. Used on tab
// switches so returning to a tab replays the stagger instead of showing the
// already-finished state.
void ui_anim_reset_list_reveals();

// Returns a reveal timestamp for a row entering the visible scroll area.
// Unlike the list reveal above this repeats every time the row leaves and
// re-enters the viewport, so scrolling keeps a consistent fade-in.
// When several rows enter in the same frame they form a batch; out_order gets
// that row's position in the batch so the caller can stagger them.
double ui_anim_scroll_reveal(int scope, int index, const std::string &id,
                             int *out_order = nullptr);

// Fades a thumbnail in the first time it becomes available (per URL).
float ui_anim_texture_alpha(const std::string &url);

// Soft top/bottom gradients on a scrollable list child. Call just before
// EndChild() so the gradients sit above the rows but inside the child clip.
void draw_scroll_edge_fades();

// ----------------------------------------------------------------------
// UI drawing functions
// ----------------------------------------------------------------------
void draw_sidebar(AppState &state);
// Warm stream URLs for the first `count` uncached tracks of a list (main.cpp).
// Serialized on the main backend channel (never the play channel); pass
// `cancel_search_id` to abort remaining entries when search_id moves on.
void prefetch_urls_for(const std::vector<Track> &tracks, size_t count,
                       int cancel_search_id = 0);
// Serial variant: one backend request at a time, cancelled when `cancel_token`
// no longer equals `token`. Used by Radio so speculative warming never puts a
// long queue in front of the user's next station request.
void prefetch_urls_for_serial(const std::vector<Track> &tracks, size_t count,
                              const std::atomic<uint64_t> *cancel_token,
                              uint64_t token);
void draw_content_header(AppState &state);
void draw_main_ui(AppState &state, MpvPlayer &player, bool show_recommendations);
void draw_playlists_panel(AppState &state);
void draw_albums_panel(AppState &state);
void draw_album_panel(AppState &state);
void draw_artists_panel(AppState &state);
void draw_artist_panel(AppState &state);
void draw_youtube_panel(AppState &state);
void draw_bottom_bar(AppState &state, MpvPlayer &player);
void draw_lyrics_panel(AppState &state, MpvPlayer &player, float width, float height);