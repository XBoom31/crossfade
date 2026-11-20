#pragma once

#include "types.h"
#include "studio_project.h"

#include <mutex>

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
    // Local audio files the user added (Studio). Main-thread only; loaded on
    // demand and persisted through library.db. Kept separate from the
    // YouTube-backed lists so they can never be stream-resolved or downloaded.
    std::vector<Track> local_tracks;
    bool local_tracks_loaded = false;
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

    // YouTube account library (fetched from the backend). The track snapshot
    // is atomic; the playlist vector, id/error strings and flags are guarded
    // by youtube_mutex (draw_youtube_panel holds it for the frame).
    mutable std::recursive_mutex youtube_mutex;
    std::vector<YouTubePlaylist> youtube_playlists;
    std::shared_ptr<const std::vector<Track>> youtube_playlist_tracks;
    std::string youtube_playlist_loaded_id;
    std::atomic<bool> youtube_playlists_loading{false};
    std::atomic<bool> youtube_playlist_loading{false};
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
    // Background album fetches mutate the album_* fields above; draw_album_panel
    // holds this mutex for the whole frame so a fetch can never replace the
    // vector while rows are being drawn (that race caused a real segfault).
    std::recursive_mutex album_mutex;
    bool album_loading = false;
    // Set when the album fetch failed with no cached copy to show.
    bool album_failed = false;
    std::string album_error_msg;
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
    // Background artist fetches publish the fields above from a worker;
    // draw_artist_panel holds this mutex for the whole frame so a fetch can
    // never replace artist_songs/artist_albums while rows are being drawn
    // (same pattern as album_mutex). Mutable so const readers (session save,
    // get_playlist) can take it too.
    mutable std::recursive_mutex artist_mutex;
    Tab artist_return_tab = Tab::Home;
    PlaylistSource artist_return_source = PlaylistSource::HOME;
    int artist_return_playlist_index = -1;
    std::shared_ptr<const std::vector<ArtistSummary>> artist_search_results;
    std::string queued_artist_search;
    std::atomic<bool> artist_search_dirty{false};
    std::atomic<bool> artist_search_loading{false};
    bool artist_search_active = false;

    // Settings -> Shortcuts: action id waiting for a key press, empty when idle.
    std::string rebind_shortcut_id;

    // Search
    char search_buffer[512] = "";
    bool search_dirty = false;
    std::string queued_search; // snapshot taken when the debounce fires
    std::atomic<int> search_id{0};
    // Only one search worker runs at a time. New queries coalesce into
    // queued_search instead of stacking up behind an in-flight request
    // (the backend answers one command at a time).
    std::atomic<bool> search_worker_active{false};
    // Set when the search fetch failed, so results area shows why + Retry.
    bool search_failed = false;
    std::string search_error_msg;
    // 0 = songs, 1 = albums, 2 = artists, 3 = playlists.
    int search_filter = 0;
    std::atomic<bool> search_loading{false};
    std::vector<std::string> search_recent;
    std::shared_ptr<const std::vector<AlbumSummary>> search_album_results;
    std::shared_ptr<const std::vector<ArtistSummary>> search_artist_results;
    // YouTube Music's "Top result" card after the local policy has run.
    std::shared_ptr<const TopResult> search_top;
    // Typeahead suggestions for the search bar (Home). Published by a worker;
    // requests carry a sequence id so stale replies are dropped.
    std::shared_ptr<const SearchSuggestions> search_suggestions;
    bool search_suggest_dirty = false;
    std::string queued_search_suggest;
    std::atomic<int> search_suggest_id{0};
    std::atomic<bool> search_suggest_loading{false};
    // Category shelves are filled by the two-request search only thinly
    // (3-6 rows); selecting the Albums/Artists chip tops them up once per
    // search_id with a filtered fetch.
    std::atomic<int> search_albums_fetched_id{0};
    std::atomic<int> search_artists_fetched_id{0};
    // 0 = none, 1 = albums, 2 = artists; set by the chip handlers and
    // consumed by the main loop once the main search has settled.
    std::atomic<int> search_category_dirty{0};

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
    // Row hover piped from ui_track_row to the main loop: after a short dwell
    // on a row its stream URL is warmed speculatively. UI thread only.
    std::string hovered_track_id;
    double hovered_track_since = 0.0;
    int hovered_track_frame = 0;
    // Backend debug window (live command log). Session-only; never persisted.
    bool show_backend_debug = false;
    // Party view: fullscreen QR takeover. Session-only; never persisted.
    bool party_fullscreen = false;
    // Lyrics immersive view: full-window, effects-driven lyrics takeover.
    // Session-only; never persisted.
    bool lyrics_immersive = false;
    // Set by the Party tab's "Party settings" shortcut so the Settings page
    // scrolls straight to the Party section on the next frame.
    bool settings_focus_party = false;
    // Set by the Home/YouTube guest prompt so Settings opens the sign-in card.
    bool settings_focus_auth = false;

    // Backend diagnostics shown in Settings.
    bool backend_running = false;
    std::string backend_auth_file;

    // Local search (Downloads, Liked, Playlists)
    std::vector<Track> local_search_results;
    bool local_search_active = false;
    Tab local_search_tab = Tab::Home; // which tab the local filter belongs to

    // Status / errors
    std::string status_text = "Loading...";
    // Workers must not assign status_text (a UI-thread string). They set this
    // flag instead and the main loop applies it on the UI thread.
    std::atomic<bool> status_ready_pending{false};
    // Set by UI actions that must exit the process (e.g. import-after-exit).
    std::atomic<bool> quit_requested{false};
    // Last classified backend failure (kind + raw message). Persists until a
    // later success clears it, so Settings can always show what's wrong.
    BackendError backend_status;
    mutable std::mutex backend_status_mutex;
    // backend_status is copied by fetch workers and read by the UI. Every
    // access goes through these helpers; g_data_mutex does not cover readers.
    BackendError backend_status_snapshot() const
    {
        std::lock_guard<std::mutex> lock(backend_status_mutex);
        return backend_status;
    }
    void set_backend_status(const BackendError &status)
    {
        std::lock_guard<std::mutex> lock(backend_status_mutex);
        backend_status = status;
    }
    // Home / search / Studio inline error state: written by fetch workers,
    // read by the drawer. One leaf mutex, never held across network or file
    // I/O.
    mutable std::mutex fetch_error_mutex;
    // Human-readable message shown inline by the tab that failed; kept in
    // sync with backend_status by the fetch workers.
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
    // Set when the Stats tab is clicked (even if it is already active), so
    // re-clicking it refreshes the page without waiting for a tab switch.
    bool stats_refresh_requested = false;
    // Download metadata repair: a worker publishes id -> Track here; the main
    // loop applies it to play_log (unknown artists) without touching UI-owned
    // vectors from the worker.
    std::shared_ptr<const std::unordered_map<std::string, Track>> repaired_meta;
    std::atomic<bool> metadata_repair_pending{false};

    // Pending play request
    std::string pending_play_url;
    std::string pending_play_title;
    std::string pending_play_artist;
    std::string pending_play_thumbnail_url;
    std::string pending_video_id;
    std::string pending_video_url;
    std::atomic<bool> pending_manual_play{false};

    // Video surface (the main content area showing the current track's
    // video). `video_active` is owned by the main loop: the UI only raises
    // the two request flags and the loop drives mpv + the render surface.
    bool video_active = false;
    bool video_toggle_requested = false;
    bool video_close_requested = false;
    // Set while the video stream is resolving/starting (the page shows a
    // spinner until the first frame arrives).
    std::atomic<bool> video_loading{false};
    // Human-readable note shown on the video page (e.g. why it fell back to
    // the track's own upload).
    std::string video_note;
    // Title of the video actually shown (the matched music video's title
    // when there is one, otherwise the track title).
    std::string current_video_title;
    // Fade/scale of the video page (0..1). UI-owned; the main loop keeps
    // rendering while it is above zero so the close fade can finish.
    float video_page_alpha = 0.0f;
    // Full-window "theater" mode for the video page.
    bool video_theater = false;
    // Tab that was active when the video page opened; changing tabs closes it.
    Tab video_tab = Tab::Home;
    // Set by a "play with video" action: open the video once playback starts.
    bool video_open_on_start = false;

    // Right-click "Download": the UI queues the request and the main loop
    // starts the background worker (and shows the toast).
    std::string download_request_id;
    std::string download_request_title;
    // Same for "Download video" (offline video cache).
    std::string video_download_request_id;
    std::string video_download_request_title;

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

    // Studio (songwriting): the selected beat, its waveform/beat analysis and
    // the in-Studio YouTube search. Main-thread owned except the snapshots
    // published by workers. `studio_analysis_dirty` is consumed by the main
    // loop exactly like lyrics_dirty, so the heavy work stays off the UI thread.
    Track studio_beat;
    std::shared_ptr<const StudioAnalysis> studio_analysis;
    std::atomic<bool> studio_analyzing{false};
    std::atomic<int> studio_analysis_seq{0};
    std::string studio_analysis_requested_id;
    bool studio_analysis_dirty = false;
    std::string studio_analysis_error;
    std::shared_ptr<const std::vector<Track>> studio_search_results;
    std::atomic<bool> studio_search_loading{false};
    std::atomic<int> studio_search_seq{0};
    std::string studio_queued_search;
    bool studio_search_dirty = false;
    std::string studio_search_error;

    // Current songwriting project for studio_beat: cues plus lyrics. Saved
    // with a short debounce after edits (studio_project_dirty).
    SongProject studio_project;
    bool studio_project_loaded = false;
    bool studio_project_dirty = false;
    double studio_project_edit_time = 0.0;
    // One-step-many undo for songwriting edits (capped snapshots).
    std::vector<SongProject> studio_undo;

    // Podcasts (custom panel; loaded on demand).
    // Workers publish the snapshot pointers atomically; the plain fields
    // (selected show, tracks, loading flags) are guarded by podcast_mutex,
    // which draw_podcasts_panel holds for the frame.
    mutable std::recursive_mutex podcast_mutex;
    std::shared_ptr<const std::vector<PodcastShow>> podcast_shows;
    std::shared_ptr<const std::vector<PodcastShow>> podcast_search_results;
    std::shared_ptr<const std::vector<PodcastEpisode>> podcast_episodes;
    std::shared_ptr<const std::vector<PodcastEpisode>> podcast_later;
    std::vector<Track> podcast_tracks;
    std::vector<PodcastShow> saved_podcasts;
    bool saved_podcasts_loaded = false;
    PodcastShow podcast_selected;
    bool podcast_library_loaded = false;
    bool podcast_library_loading = false;
    bool podcast_details_loading = false;
    bool podcast_search_loading = false;
    bool podcast_search_dirty = false;
    bool auth_refresh_requested = false;
    // Sign-in helper state (Settings -> YouTube Music). The helper runs as a
    // detached child and writes login-result.txt; the UI polls that and
    // login.log so a webview window can stay open without blocking the loop.
    std::atomic<bool> auth_login_running{false};
    int auth_login_exit = 0;
    std::string auth_login_log;
    std::atomic<bool> auth_probe_running{false};
    std::mutex auth_probe_mutex;
    std::string auth_probe_output;
    std::shared_ptr<const SpotifyImportResult> spotify_import_result;
    std::atomic<bool> spotify_import_running{false};
    std::atomic<bool> spotify_login_running{false};
    std::atomic<bool> spotify_open_import_requested{false};
    std::shared_ptr<const std::vector<SpotifyPlaylistSummary>> spotify_playlists;
    std::shared_ptr<const std::string> spotify_library_error;
    std::atomic<bool> spotify_library_running{false};
    std::atomic<bool> spotify_library_loaded{false};
    std::string queued_podcast_search;
    int podcast_search_id = 0;

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
