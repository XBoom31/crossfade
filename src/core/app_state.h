#pragma once

#include "types.h"

// ----------------------------------------------------------------------
// Application state
//
// Locking discipline (see docs/THREADING.md):
//  - network-fed lists are atomic shared_ptr<const vector<...>> snapshots;
//  - playback/UI scalars are atomics or UI-thread only;
//  - vectors/maps are UI-thread owned; workers publish snapshots.
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
    std::string artist_songs_browse_id;
    bool artist_songs_loading = false;
    bool artist_songs_has_more = false;
    int artist_songs_offset = 0;
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
    // Party view: fullscreen QR takeover. Session-only; never persisted.
    bool party_fullscreen = false;
    // Set by the Party tab's "Party settings" shortcut so the Settings page
    // scrolls straight to the Party section on the next frame.
    bool settings_focus_party = false;

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
    // effects (the ambient backdrop, bottom-bar metadata/progress) can stretch
    // their own transition to the exact length of an in-flight crossfade.
    // `crossfade_progress` is 0..1. Position/duration describe the incoming
    // player while the fade runs; the volume is the user's target, not the
    // temporary fade gain.
    bool crossfade_active = false;
    float crossfade_seconds = 0.0f;
    float crossfade_progress = 0.0f;
    double crossfade_position = 0.0;
    double crossfade_duration = 0.0;
    int crossfade_user_volume = 100;
    // Outgoing track snapshot, captured when a crossfade starts. The lyrics
    // panel keeps showing this track (and its lyrics) until the midpoint.
    Track crossfade_from_track;
    // UI volume edits are applied by the main loop so they update both
    // players during a crossfade. -1 means no pending change.
    int volume_request = -1;

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

    // Playback queue. `play_queue` is the single global Up Next list: it
    // contains both host source tracks and guest requests, and playback only
    // ever advances by popping its front. `queue_cycle` is the source-list
    // snapshot used to refill the queue for repeat-all/shuffle. The current
    // track lives in the normal current_* fields.
    std::vector<Track> play_queue;
    std::vector<Track> queue_cycle;
    std::atomic<bool> queue_dirty{false};
    // Set by the Queue panel to start a specific queue row now.
    int queue_play_index = -1;
    // Set by a UI play action: rebuild the global queue from the active source
    // list, starting at this index (or this track id when index is -1).
    bool queue_replace_from_source = false;
    int queue_replace_index = 0;
    std::string queue_replace_id;
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
