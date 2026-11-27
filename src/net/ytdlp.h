#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <unordered_map>

struct Track
{
    std::string id;
    std::string title;
    std::string artist;
    std::string artist_id;
    std::string album;
    std::string album_id;
    int track_number = 0;
    std::string thumbnail_url;
    int duration_sec = 0;
    std::string url; // full YouTube URL
    // Party mode provenance. Optional and always defaulted for tracks that
    // did not come from a guest, so old saved queues/lists keep working.
    std::string requested_by; // guest display name ("" when not from a party)
    std::string guest_id;     // ephemeral per-party guest id (never persisted server-side)
    bool from_party = false;
    int party_round = 0;      // fair-queue round assigned at insertion
    // True for tracks materialized from the host's active source list (a
    // playlist, album, etc.). Guest requests insert before these, but after
    // explicit "Play next" host items.
    bool from_queue_source = false;
    // True for a row that comes from the offline video cache (Downloads).
    bool is_video = false;
};

// Lyrics for one track. `ok` is false when the fetch failed (backend /
// network unreachable) so the caller can decide not to cache; `none` is a
// definitive "this track has no lyrics" result from the backend (cacheable).
// `times` is parallel to `lines` and holds start times in seconds (only
// meaningful when `synced` is true).
struct LyricWord
{
    int start_ms = 0;
    int end_ms = 0;
    std::string text;
};

// A line with optional word/syllable timings and the extras the immersive
// view draws underneath it. `times`/`lines` below remain the simple view used
// by the docked panel and older cache files.
struct RichLyricLine
{
    int start_ms = 0;
    int end_ms = 0;
    std::string text;
    std::vector<LyricWord> words;
    std::string translation;
    std::string romanization;
};

struct Lyrics
{
    bool ok = false;
    bool none = false;
    bool synced = false;
    std::string source;
    std::string language;
    int version = 0; // 2 = rich word timings/translations
    std::vector<int> times;
    std::vector<std::string> lines;
    std::vector<RichLyricLine> rich;
};

// ---- Studio: waveform + beat analysis ----
// One resolution of the waveform pyramid. `mins`/`maxs` are parallel and hold
// the sample range in each bucket (normalised to roughly [-1, 1]).
struct StudioWaveLevel
{
    int buckets = 0;
    std::vector<float> mins;
    std::vector<float> maxs;
};

struct StudioAnalysis
{
    std::string id; // beat this analysis belongs to
    bool ok = false;
    double duration = 0.0;
    double bpm = 0.0;
    float confidence = 0.0f;
    std::vector<double> beats;
    std::vector<double> downbeats;
    std::vector<StudioWaveLevel> waves; // coarse -> fine
};

// Analyse an audio file (waveform pyramid + beat grid), caching the result at
// `cache_path`. Blocking: call from a worker, never the UI thread. Reads the
// cache directly when it already exists. Returns false on decode failure.
bool fetch_studio_analysis(const std::string &id, const std::string &audio_path,
                           const std::string &cache_path, StudioAnalysis &out);
// Parse a wave-cache file written by the backend. Exposed for tests.
bool load_studio_analysis_file(const std::string &path, StudioAnalysis &out);

bool start_backend();
// True when the running backend started without a usable sign-in and is
// limited to public browsing (search, albums, artists, radio, podcasts).
bool backend_is_guest();
// Whether start_backend() also launches the play/prefetch workers.
void set_optional_backends_enabled(bool enabled);
// While parked, commands fail fast instead of waiting for a backend that is
// intentionally not running yet.
void set_backend_deferred(bool deferred);
// Per-channel status/counters for the diagnostics bundle.
std::string backend_diagnostics();
// Cancel an in-flight startup wait (called before joining the boot thread
// during shutdown).
void cancel_backend_startup();
void stop_backend();

// ---- Backend debug log (live command tracing) ----
// Every command sent through the backend channel is recorded (bounded ring) so
// an in-app debug window can show what was sent where and how long it took.
struct BackendLogEntry
{
    uint64_t seq = 0;      // monotonic, per command
    int64_t ts_ms = 0;     // steady-clock milliseconds at completion
    std::string channel;   // "main" / "play" / "prefetch" / "meta"
    std::string command;   // the command line (clipped)
    uint64_t ms = 0;       // round-trip duration (queue + processing)
    uint64_t queue_ms = 0; // time spent waiting for the channel
    size_t pending = 0;    // commands already waiting when this one queued
    bool error = false;    // response carried an error / backend died
    size_t bytes = 0;      // response payload size
    std::string detail;    // "error" message, "timed out", … (clipped)
};

struct BackendChannelStats
{
    std::string name;
    bool ready = false;
    bool dead = false;
    uintptr_t pid = 0;
    uint64_t commands = 0;
    uint64_t errors = 0;
    uint64_t last_ms = 0;
    uint64_t max_ms = 0;
    size_t pending = 0;
};

struct BackendDebugSnapshot
{
    std::vector<BackendChannelStats> channels;
    std::vector<BackendLogEntry> log; // most recent first
};

BackendDebugSnapshot backend_debug_snapshot();
void backend_debug_clear_log();

// What went wrong, classified at the source (Python stamps {"kind": ...} on
// every error payload; the C++ side only falls back to substring guessing).
enum class BackendErrKind
{
    None = 0,  // no error since the last take
    Auth,      // signed out / stale cookies / forbidden
    Network,   // DNS, connection, timeout while reaching YouTube
    Backend,   // the backend process itself is dead / wedged / parse junk
    NotFound,  // the requested item genuinely doesn't exist
};
// NB: NotFound is produced by Python's generic classifier only when the
// message clearly says so; most "no data" cases are simply empty results.

struct BackendError
{
    BackendErrKind kind = BackendErrKind::None;
    std::string message; // raw backend text (may be multi-line)

    bool ok() const { return kind != BackendErrKind::None || !message.empty(); }
    void clear()
    {
        kind = BackendErrKind::None;
        message.clear();
    }
    explicit operator bool() const { return ok(); }
};

// Short, user-facing sentence for a backend error ("You're signed out of
// YouTube Music", "No connection to YouTube Music", ...). Falls back to the
// first line of the raw message when the kind isn't recognized.
std::string backend_error_user_message(const BackendError &err);

// Configure which browser yt-dlp should pull cookies from (mpv, the
// local yt-dlp calls and the Python backend all share this one source).
void set_ytdl_browser(const std::string &browser);

// Select the backend auth flavour: "browser" (browser.json cookies) or
// "oauth" (browseroauth.json token). Takes effect when start_backend() runs.
void set_auth_mode(const std::string &mode);

// Toggle the backend's experimental in-process stream extraction (skips the
// per-resolve yt-dlp subprocess spawn). Takes effect when start_backend() runs.
void set_warm_stream_resolver(bool enabled);

// The last backend failure with its classified kind. Cleared by
// take_backend_error(); backend_error_snapshot() reads without taking.
BackendError take_backend_error();

// Expose for liked songs persistence
std::vector<Track> parse_track_array(const std::string &json);

std::vector<Track> fetch_recommendations(bool force_fresh = false, int limit = 40);
// Append the next page of the personalized Home feed. has_more is true while
// YouTube still returned a continuation token.
std::vector<Track> fetch_recommendations_more(bool &has_more);
// Set *failed (when provided) if the backend did not answer or reported an
// error, so callers can distinguish "no matches" from "search unavailable".
std::vector<Track> fetch_search(const std::string &query, bool *failed = nullptr);
std::vector<Track> fetch_radio(const std::string &video_id);

// Party mode: resolve one video id to metadata on the main backend channel.
// Never used for stream resolution, so guest traffic cannot delay a host
// click on the play channel. Returns false and fills `error` on failure.
bool fetch_track_meta(const std::string &video_id, Track &out, std::string &error);

struct YouTubePlaylist
{
    std::string id;
    std::string title;
    std::string thumbnail_url;
    int count = 0;
};

std::vector<YouTubePlaylist> fetch_youtube_playlists();
std::vector<Track> fetch_youtube_playlist(const std::string &playlist_id);
// Background offline pre-cache variant: uses the speculative prefetch channel
// so it never sits in front of an interactive request on the main channel.
std::vector<Track> fetch_youtube_playlist_prefetch(const std::string &playlist_id);
// Drop account-scoped caches after sign-in / account switch.
void clear_recommendations_cache();
void clear_youtube_playlist_caches();
// True when a cache file predates the current auth file (belongs to an older
// or anonymous session).
bool cache_older_than_auth(const std::string &path);

// ------------------ Server write-back ------------------
// Mutations of the signed-in YouTube Music account. All are blocking and
// network-bound: call them from the write-back worker (writeback.h), never
// on the UI thread. Failures are recorded like every other backend command;
// report them with ui_notify_backend_error(take_backend_error()).
bool backend_rate_song(const std::string &video_id, const char *rating); // "LIKE"/"INDIFFERENT"/"DISLIKE"
std::string backend_create_playlist(const std::string &title,
                                    const std::vector<std::string> &video_ids); // returns server playlist id, "" on failure
bool backend_delete_playlist(const std::string &playlist_id);
bool backend_rename_playlist(const std::string &playlist_id, const std::string &title);
bool backend_add_playlist_items(const std::string &playlist_id,
                                const std::vector<std::string> &video_ids);
// items are (videoId, setVideoId); entries with an empty setVideoId are skipped.
bool backend_remove_playlist_items(
    const std::string &playlist_id,
    const std::vector<std::pair<std::string, std::string>> &items);
// after_item_id empty means "move to top".
bool backend_move_playlist_item(const std::string &playlist_id,
                                const std::string &set_video_id,
                                const std::string &after_item_id);
// Current server contents as (videoId, setVideoId) pairs in server order
// (setVideoId is required for removals and reorders). Empty on failure.
std::vector<std::pair<std::string, std::string>> backend_playlist_items(
    const std::string &playlist_id);

// Parse the pairs out of a `playlist` backend response (ytmusicapi's
// get_playlist JSON). Exposed separately so it can be unit-tested without a
// live backend: the response nests artists/album/thumbnails before
// "setVideoId", which a nearest-brace scan gets wrong.
std::vector<std::pair<std::string, std::string>> parse_playlist_item_pairs(
    const std::string &response);

// ------------------ Browse (charts & moods) ------------------

// ------------------ Download cache size cap ------------------
// bytes <= 0 means unlimited (the default). Enforced after each finished
// download by evicting the oldest recordings; the currently-playing track is
// never evicted. audio_cache_clear() wipes everything except keep_id.
void set_audio_cache_cap(double bytes);
// Optional eviction ranking: return how "wanted" a cached track is (e.g. its
// play count). Least-wanted tracks are evicted first; ties fall back to the
// oldest file. Call once at startup; without it eviction is age-based.
void set_audio_cache_rank(int (*rank_fn)(const std::string &video_id));
double audio_cache_usage_bytes();
void audio_cache_clear(const std::string &keep_id = "");
// Test hook: run the size-cap eviction immediately. Production calls this
// after a download finishes; tests need deterministic control.
void audio_cache_enforce_cap_now(const std::string &keep_id = "");

// Cache helpers for the YouTube tab (raw JSON on disk).
std::string youtube_playlists_cache_path();
std::string youtube_playlist_cache_path(const std::string &playlist_id);
bool read_text_file(const std::string &path, std::string &out);
void write_text_file(const std::string &path, const std::string &data);
std::vector<YouTubePlaylist> parse_youtube_playlists_response(const std::string &json);
std::vector<Track> parse_youtube_playlist_tracks_response(const std::string &json);

struct Album
{
    std::string browse_id;
    std::string title;
    std::string artist;
    std::string year;
    std::string duration;
    std::string description;
    std::string thumbnail_url;
    std::string audio_playlist_id;
    int track_count = 0;
    std::vector<Track> tracks;
};

bool fetch_album(const std::string &browse_id, Album &out);
bool fetch_album_for_track(const std::string &video_id, std::string &browse_id, std::string &album_name);

struct ArtistAlbum
{
    std::string browse_id;
    std::string title;
    std::string year;
    std::string thumbnail_url;
    std::string type;
};

struct ArtistInfo
{
    std::string browse_id;
    std::string name;
    std::string description;
    std::string subscribers;
    std::string monthly_listeners;
    std::string thumbnail_url;
    std::vector<Track> songs;
    std::string songs_browse_id; // full songs shelf (pass to artist_songs)
    std::vector<ArtistAlbum> albums;
};

struct ArtistSummary
{
    std::string browse_id;
    std::string name;
    std::string thumbnail_url;
    std::string subscribers;
};

struct AlbumSummary
{
    std::string browse_id;
    std::string title;
    std::string artist;
    std::string year;
    std::string thumbnail_url;
    int track_count = 0;
};

// YouTube Music's "Top result" card for a query. The backend supplies the
// server's own intent match; src/app/search_top.cpp validates it against the
// shelves and the local library before the UI shows it.
enum class TopKind
{
    None,
    Song,
    Album,
    Artist
};

struct TopResult
{
    TopKind kind = TopKind::None;
    std::string id; // video id / album browse id / artist channel id
    std::string title;
    std::string artist;
    std::string artist_id;
    std::string album;
    std::string album_id;
    std::string year;
    std::string duration; // "3:58" when known
    int duration_sec = 0;
    std::string subscribers;
    std::string thumbnail_url;
    bool video = false; // song card that YouTube classes as a video
    int track_count = 0;
};

struct SearchAllResult
{
    TopResult top;
    std::vector<Track> songs;
    std::vector<AlbumSummary> albums;
    std::vector<ArtistSummary> artists;
};

// Typeahead data for the search bar.
struct SearchSuggestionQuery
{
    std::string text;
    bool history = false; // from the account's search history
};

struct SearchSuggestionItem
{
    std::string kind; // "song" / "video" / "album" / "artist"
    std::string id;   // video id or browse id
    std::string title;
    std::string artist;
    std::string album;
    std::string subtitle;
    std::string thumbnail_url;
};

struct SearchSuggestions
{
    std::vector<SearchSuggestionQuery> queries;
    std::vector<SearchSuggestionItem> items;
};

struct PodcastShow
{
    std::string browse_id;
    std::string title;
    std::string author;
    std::string thumbnail_url;
    std::string description;
    bool saved = false;
};

struct PodcastEpisode
{
    std::string video_id;
    std::string title;
    std::string author;
    std::string thumbnail_url;
    std::string date;
    std::string description;
    int duration_sec = 0;
};

struct PodcastDetails
{
    PodcastShow show;
    std::vector<PodcastEpisode> episodes;
};

struct SpotifyImportResult
{
    std::string name;
    std::string error;
    std::vector<Track> tracks;
    // "Title - Artist" entries the matcher refused (wrong artist, cover,
    // karaoke, live version, ...). Shown to the user instead of importing
    // whatever the search happened to return first.
    std::vector<std::string> unmatched;
    int skipped = 0;
    int total = 0;
};

struct SpotifyPlaylistSummary
{
    std::string id;
    std::string kind; // "liked" or "playlist"
    std::string name;
    std::string owner;
    std::string art;
    int count = 0;
};

// Podcasts (YouTube Music's podcast show / episode model).
std::vector<PodcastShow> fetch_podcast_library();
std::vector<PodcastShow> fetch_podcast_search(const std::string &query);
PodcastDetails fetch_podcast_details(const std::string &browse_id);
std::vector<PodcastEpisode> fetch_episodes_later();
SpotifyImportResult fetch_spotify_import(const std::string &link);
std::vector<SpotifyPlaylistSummary> fetch_spotify_library(std::string &error);


bool fetch_artist(const std::string &browse_id, ArtistInfo &out);
bool fetch_artist_songs(const std::string &songs_browse_id, int offset, int count,
                        std::vector<Track> &out);
bool fetch_artist_for_track(const std::string &video_id, std::string &artist_id, std::string &artist_name);
std::vector<ArtistSummary> fetch_artist_search(const std::string &query);
std::vector<AlbumSummary> fetch_search_albums(const std::string &query);
SearchSuggestions fetch_search_suggestions(const std::string &query);
bool fetch_search_all(const std::string &query, SearchAllResult &out);

// Backend/auth diagnostics for Settings.
bool backend_running();
BackendError backend_error_snapshot();
std::string backend_auth_file_path();
std::string backend_reauth_command();
std::string backend_script_path();
std::string backend_python_path();
// The same interpreter as pythonw.exe when available (no console window for
// GUI helpers such as the sign-in window).
std::string backend_pythonw_path();

// Parse a lyrics JSON payload (from the backend or the on-disk cache) into
// `out`. Returns false if the payload is an error object. Recognizes the
// "none" marker (definitive no-lyrics) and the synced flag.
bool parse_lyrics_json(const std::string &json, Lyrics &out);

// Fetch lyrics for a video id from the backend. This is a metadata-only
// request (watch playlist + lyrics); it never resolves a stream or
// downloads audio, so it is safe to use for locally cached tracks.
// On success `out.ok` is set (and maybe `out.none`). Returns false when the
// backend could not be reached or reported an error.
bool fetch_lyrics(const std::string &video_id, Lyrics &out);

// Look up a previously downloaded file without triggering a download.
std::string find_cached_audio_path(const std::string &video_id);
// Local paths in imported libraries may point at another OS/home. Prefer an
// existing local file, then the current cache copy, then the YouTube URL.
std::string playable_track_url(const std::string &id, const std::string &url);// Download (if needed) and return the local audio file path.
std::string get_cached_audio_path(const std::string &video_id);
// Persist the metadata sidecar for an already-cached audio file. Podcast
// downloads use it to keep the show/artist association in the Downloads list
// (yt-dlp's generic metadata does not know which show an episode belongs to).
// No-op when the file is missing or the track is being deleted.
void save_cached_track_metadata(const Track &track, const std::string &audio_path);
// Artwork pulled from stale session/history rows can belong to a different
// track. Prefer a sidecar whose URL contains the video id, otherwise fall back
// to an id-derived thumbnail so the cover is always tied to the track.
std::string authoritative_thumbnail_url(const std::string &video_id,
                                        const std::string &fallback = "");
// Resolve a direct streaming URL via the backend ("yt-dlp -g").
std::string fetch_stream_url(const std::string &video_id);

// Native backend switch. The environment variable P100_BACKEND=python always
// wins (useful for bug reports and CI); otherwise this runtime flag decides
// whether the native resolver is tried before the Python backend.
void set_native_backend_enabled(bool enabled);
bool native_backend_enabled();
// Speculative variant for background warming: uses a dedicated prefetch
// backend channel, so it competes with neither a user click's resolve nor a
// search/metadata request on the main channel.
std::string fetch_stream_url_prefetch(const std::string &video_id);
// Resolve several ids in one round trip. `background` routes the batch to the
// meta worker (lyrics/background prefetch) so the hover/hero workspaces keep
// the fast channel to themselves. Failed ids are omitted.
std::unordered_map<std::string, std::string> fetch_stream_urls_prefetch(
    const std::vector<std::string> &video_ids, bool background = false);
// Same, but bypass and replace the backend's cached URL (used after a
// player-side load failure, which often means the googlevideo URL expired).
std::string fetch_stream_url_fresh(const std::string &video_id);

// Video stream pair for the main-area video surface. Both URLs belong to the
// same video id: `video` is a video-only stream, `audio` (may be empty) the
// separate audio stream, which mpv attaches with audio-add.
struct VideoStreamUrls
{
    std::string video;
    std::string audio;
    std::string error;
    // Fast path failed; the caller should retry with the full fallback walk.
    bool retry = false;
};
VideoStreamUrls fetch_video_stream_urls(const std::string &video_id, bool fresh = false,
                                        int max_height = 720, bool fast = false);

// Official music-video match for a YouTube Music song id. `matched` is false
// when the song's own upload is the best available (still a valid stream).
struct MusicVideoMatch
{
    bool ok = false;
    bool matched = false;
    // The track's own upload is already a video: no search, no A/V sync.
    bool self_video = false;
    std::string id;
    std::string title;
    std::string artist;
    int duration_sec = 0;
    std::string error;
};
MusicVideoMatch fetch_music_video(const std::string &video_id);
// Same lookup on the background channel, for parallel use with a resolve.
MusicVideoMatch fetch_music_video_prefetch(const std::string &video_id);

// Constant A/V offset for a matched music video: video_time = song_time +
// offset. `confident` is false when the cross-correlation was ambiguous (the
// caller then uses 0). Runs once per pair; the backend caches it on disk.
struct VideoSyncResult
{
    bool ok = false;
    bool confident = false;
    double offset = 0.0;
    double confidence = 0.0;
    std::string error;
};
VideoSyncResult fetch_video_sync(const std::string &song_id, const std::string &video_id,
                                 int song_duration_sec);
// Cache-only variant: instant, hits the prefetch channel. `cached` is false
// when the pair has not been analysed yet.
VideoSyncResult fetch_video_sync_cached(const std::string &song_id, const std::string &video_id);
std::vector<Track> get_cached_tracks();
// Offline video cache (video_cache/<id>.mp4 and friends).
std::string find_cached_video_path(const std::string &video_id);
// Download (or return) the cached video file. Blocking: call on a worker.
std::string get_cached_video_path(const std::string &video_id, int max_height = 720);
std::vector<Track> get_cached_videos();
void delete_cached_video(const std::string &video_id);

// Cache path (ensuring the dir exists) where a streamed recording for
// video_id should be written, e.g. <cache>/audio_cache/<id>.webm.
// Returns "" when the cache directories cannot be created.
std::string recorded_audio_path(const std::string &video_id, const std::string &ext);
// Temporary path (in <cache>/audio_cache/.tmp) for an in-progress recording.
// Must keep the real container extension so mpv's recorder picks a muxer.
// Unique per recording, so a replay never collides with a still-finalizing
// previous one. Returns "" when the cache directories cannot be created.
std::string recorded_audio_temp_path(const std::string &video_id, const std::string &ext);
// Remove leftover in-progress recordings from a previous crash/quit.
void cleanup_incomplete_recordings();
// Fetch metadata for a finished recording and write its .meta.json sidecar.
// Network-bound; safe to call from a background thread.
void finalize_recorded_audio(const std::string &video_id, const std::string &audio_path);

// Sniff the container of a finished recording from its leading bytes and
// return the extension ("webm", "m4a", "mp3", "aac", "ogg", "flac").
// `fallback` is returned when the file can't be read or isn't recognized --
// this corrects best-effort mime/itag guesses that turned out wrong.
std::string sniff_audio_container_ext(const std::string &path, const std::string &fallback);

// Set whenever cached audio changes on disk (recording finalized, sidecar
// written, orphan swept). The main loop consumes it to refresh the
// Downloads list without a restart. Defined in ytdlp.cpp.
extern std::atomic<bool> g_downloads_dirty;

// Register that a cached track is being deleted, so a finalize thread still
// fetching metadata over the network can't recreate a .meta.json next to
// files the delete just removed (and drops the recording it would
// resurrect). Call before removing the files.
void cancel_pending_metadata(const std::string &video_id);
// Runs fn while all metadata sidecar writes are blocked, so bulk deletes
// (e.g. "Clear download cache") can't race a concurrent finalize.
void with_metadata_writes_blocked(const std::function<void()> &fn);