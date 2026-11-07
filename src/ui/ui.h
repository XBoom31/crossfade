#pragma once

#include "types.h"
#include "app_state.h"

// layout factor. See apply_ui_scale() in main.cpp.
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
void draw_party_panel(AppState &state);
void draw_podcasts_panel(AppState &state);
void close_podcast_show(AppState &state);
void load_podcast_library(AppState &state);
// Fullscreen QR takeover (Esc or the close button returns to the app).
void draw_party_fullscreen(AppState &state);
// Small bottom-bar chip shown only while a party is running.
void draw_party_chip(AppState &state);

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
std::string playlist_m3u(const Playlist &playlist);
std::string playlist_xspf(const Playlist &playlist);

// Lyrics cache (~/.config/p100/lyrics/<video_id>.json). One file per track;
// loading never touches the network. A definitive "no lyrics" result is
// cached too (as a "none" marker) so it isn't re-fetched every play.
std::string lyrics_dir();
std::string lyrics_cache_path(const std::string &video_id);
bool load_lyrics_cache(const std::string &video_id, Lyrics &out);
void save_lyrics_cache(const std::string &video_id, const Lyrics &lyrics);

// Shared UI helpers
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
// Pure Tab -> list source mapping (side-effect-free; callers still do their
// own loading/refresh work).
PlaylistSource playlist_source_for_tab(Tab tab);

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
void smooth_scroll_child();

// Lyrics panel animation signal. The panel sets this while its auto-centre
// scroll / fade-in is running; the main loop consumes it (like
// UI motion) to keep rendering at 60 FPS until the motion settles.
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
float ui_anim_clamp01f(float v);
float ui_anim_ease_out_back(float t);
void ui_anim_request();

// Shared entrance clock used by the page panels: one start stamp drives the
// hero pop, staggered rows/chips and section fades, and in_view() keeps
// content below the fold from animating until it scrolls in.
struct UiPageAnim
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
        return ui_anim_ease_out(ui_anim_clamp01f((float)((now - start - d) / du)));
    }

    float pop(float delay, float dur) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float d = ui_anim_delay(delay);
        const float du = ui_anim_duration(dur);
        if (du <= 0.0001f)
            return 1.0f;
        return ui_anim_ease_out_back(ui_anim_clamp01f((float)((now - start - d) / du)));
    }

    float in_view(float screen_y, float span = 120.0f) const
    {
        if (!ui_animations_enabled())
            return 1.0f;
        const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y;
        return ui_anim_ease_out(ui_anim_clamp01f((bottom - screen_y) / ui_px(span)));
    }
};
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

// Alpha multiplier for a piece of scroll content at the top/bottom edge of
// the current window. Fades the content itself (rows, cards, lyric lines)
// instead of painting a background-coloured overlay over it.
float ui_content_edge_alpha(float top_y, float bottom_y);

// ----------------------------------------------------------------------
// UI drawing functions
// ----------------------------------------------------------------------
void draw_sidebar(AppState &state);
// Warm stream URLs for the first `count` uncached tracks of a list (main.cpp).
// Serialized on the dedicated prefetch backend channel (never the main or play
// channel); pass `cancel_search_id` to abort remaining entries when search_id
// moves on.
void prefetch_urls_for(const std::vector<Track> &tracks, size_t count,
                       int cancel_search_id = 0);
// Serial variant: one backend request at a time, cancelled when `cancel_token`
// no longer equals `token`. Used by Radio so speculative warming never puts a
// long queue in front of the user's next station request.
void prefetch_urls_for_serial(const std::vector<Track> &tracks, size_t count,
                              const std::atomic<uint64_t> *cancel_token,
                              uint64_t token);
void draw_content_header(AppState &state);
void draw_main_ui(AppState &state, MpvPlayer &player);
void draw_playlists_panel(AppState &state);
void draw_albums_panel(AppState &state);
void draw_album_panel(AppState &state);
void draw_artists_panel(AppState &state);
void draw_artist_panel(AppState &state);
void draw_youtube_panel(AppState &state);
void draw_bottom_bar(AppState &state, MpvPlayer &player);
void draw_lyrics_panel(AppState &state, MpvPlayer &player, float width, float height);
