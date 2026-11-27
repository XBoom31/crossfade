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
#include "party.h"

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
        Podcasts,
        Settings,
        YouTube,
        Smart,
        Party,
        Studio
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
        PODCAST,
        YOUTUBE_PLAYLIST,
        SMART,
        STUDIO
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
struct EqBand
{
    float freq = 0.0f; // centre frequency, Hz
    float gain = 0.0f; // dB
    float q = 1.0f;    // filter Q
};

inline bool operator==(const EqBand &a, const EqBand &b)
{
    return a.freq == b.freq && a.gain == b.gain && a.q == b.q;
}

// Curves that produced an applied room correction. They are persisted with
// the settings (and with a saved room) so the Room EQ page can draw the
// measured/target/corrected response after a restart or a room switch, and
// re-fit it when the target/strength/detail controls change.
struct RoomEqCurve
{
    std::vector<float> hz;
    std::vector<float> measured_db;
    std::vector<float> target_db;
    std::vector<float> corrected_db;
    std::vector<char> valid; // per point; empty means all trusted
    std::string mic;
    float residual_before_db = 0.0f;
    float residual_after_db = 0.0f;
    float correct_lo_hz = 0.0f;
    float correct_hi_hz = 0.0f;
    float content_hi_hz = 0.0f;
    int max_bands = 12;
    int sample_rate = 48000;

    bool empty() const
    {
        return hz.empty() || measured_db.size() != hz.size() ||
               target_db.size() != hz.size() || corrected_db.size() != hz.size();
    }
};

inline bool operator==(const RoomEqCurve &a, const RoomEqCurve &b)
{
    return a.hz == b.hz && a.measured_db == b.measured_db &&
           a.target_db == b.target_db && a.corrected_db == b.corrected_db &&
           a.valid == b.valid && a.mic == b.mic &&
           a.residual_before_db == b.residual_before_db &&
           a.residual_after_db == b.residual_after_db &&
           a.correct_lo_hz == b.correct_lo_hz && a.correct_hi_hz == b.correct_hi_hz &&
           a.content_hi_hz == b.content_hi_hz && a.max_bands == b.max_bands &&
           a.sample_rate == b.sample_rate;
}

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
    // Video quality: 0 = 720p, 1 = 1080p, 2 = fit the window height (capped
    // at 1080). Passed to the backend's format selection.
    int video_quality = 0;
    // Backend authentication: "browser" uses browser.json cookies, "oauth"
    // uses browseroauth.json. Applied at backend startup.
    std::string auth_mode = "browser";
    bool lyrics_panel_open = true; // bottom-bar toggle state (persisted)
    // Seconds to shift the synced-lyrics highlight. Positive delays it (lyrics
    // shown later), negative advances it. Applied to both the active-line
    // search and click-to-seek.
    float lyrics_offset = 0.0f;
    // Immersive-lyrics loader style: 0 = layered waves, 1 = spectrum bars,
    // 2 = disabled. Waves and Spectrum share the same intro/gap timing rules.
    int lyrics_effect_style = 0;
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
    // Maximum refresh rate the UI renders at while animating, handling input
    // or showing video. 0 follows the display's refresh rate (up to 240 Hz);
    // a fixed value caps the fast frame budget at that many Hz. Playback and
    // idle budgets are unaffected, so higher rates only cost CPU while the
    // interface is actually moving.
    int max_refresh_hz = 0;
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
    // Height (logical px) of the scroll edge fades on track lists and the
    // lyrics panel. 0 disables them.
    float edge_fade_height = 26.0f;
    // Hero card background: 0 = off (flat card), 1 = transparent blur (frosted
    // glass), 2 = cover blur (the cover art itself, blurred). The blurred
    // texture is generated once per cover by a shader and then reused.
    int hero_backdrop = 2;
    float hero_backdrop_strength = 0.55f;
    // Render the ambient wash with a single full-screen shader pass and
    // in-shader dithering instead of stacked ImGui ellipses.
    bool backdrop_shader = true;
    // Diagnostic switch: next launch disables the shader backdrop, updater
    // checks and optional background integrations.
    bool safe_mode = false;
    // Check GitHub for a newer release once a day. The check fetches the
    // release's tiny version.txt; Linux self-updates additionally download a
    // verified release asset only when the user asks. No telemetry.
    bool check_for_updates = true;
    // Low power mode: cap the render cadence, disable motion, and skip
    // speculative stream warming / background downloads. Playback itself is
    // unaffected. `low_power_on_battery` turns it on automatically when the
    // system reports it is running on battery. P100_LOW_POWER overrides both.
    bool low_power = false;
    bool low_power_on_battery = true;
    // Low memory mode: run a single (concurrent) backend process instead of
    // four. Speculative work shares the main channel; saves a few hundred MB.
    // Applied when the backends start (next launch).
    bool low_memory_mode = false;
    // Warm start: keep a background instance for instant relaunches. The
    // window may be hidden while the GL context and (per warm_start_mode)
    // backends stay warm. 0 = everything, 1 = main backend only, 2 = UI only.
    bool warm_start = true;
    int warm_start_mode = 0;
    bool warm_close_hides = true;
    // Keep detailed task/log history for the diagnostics report. Passive:
    // a small in-memory ring plus one record per background task.
    bool diagnostics_history = true;
    // Glowing separators: a soft album-coloured light streak under each
    // section header, plus album-tinted structural hairlines. Uses the same
    // dominant-colour tint as the backdrop, but works with the backdrop off.
    // Off by default: the plain hairline keeps the list chrome quiet, and the
    // effect stays one toggle away in Settings -> Appearance.
    bool glow_separators = false;
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

    // "Prefetch the world": warm stream URLs for the whole visible list in the
    // background instead of only the hovered row. Costs bandwidth and can trip
    // YouTube rate limits; off by default.
    bool aggressive_prefetch = false;

    // Party mode settings (never the token, guest ids or guest names).
    PartySettings party;

    // Playback rate multiplier (0.5 - 2.0). Applied live to both mpv
    // instances so it survives the crossfade role swap.
    float playback_speed = 1.0f;

    // Equalizer (mpv lavfi `equalizer` chain) + volume normalization. Bands
    // are user-editable (frequency, gain, Q); the default is the historical
    // five-band layout. Both compose the single `af` string pushed to mpv.
    bool eq_enabled = false;
    // UI state for the equalizer editor: 0=5, 1=10, 2=15, 3=31, 4=custom;
    // preset: 0=Flat, 1=Bass, 2=Vocal, 3=Rock, 4=Electronic, 5=Custom.
    int eq_layout = 0;
    int eq_preset = 0;
    std::vector<EqBand> eq_bands = {
        {60.0f, 0.0f, 1.0f},
        {230.0f, 0.0f, 1.0f},
        {910.0f, 0.0f, 1.0f},
        {3600.0f, 0.0f, 1.0f},
        {14000.0f, 0.0f, 1.0f},
    };

    // Room correction measured at the listening position. Applied *after* the
    // manual equalizer as another block of peaking filters, so the two never
    // overwrite each other. Bands come from the calibration UI.
    bool room_eq_enabled = false;
    std::vector<EqBand> room_eq_bands;
    // Headroom for boosted bands; <= 0 dB. Applied as a volume trim.
    float room_eq_preamp = 0.0f;
    // Live tone trim on top of the correction (shelf filters). Does not need a
    // new measurement; -6..+6 dB.
    float room_eq_bass_db = 0.0f;
    float room_eq_treble_db = 0.0f;
    // Filter ceiling for the fit: 12, 20 or 32. More bands fit a lumpy curve
    // more closely; they do not make the correction more aggressive.
    int room_eq_max_bands = 12;
    // Microphone profile: "" (flat), "phone_generic", "custom".
    std::string room_eq_mic;
    // Correction target: "balanced" (default), "warm" or "flat".
    std::string room_eq_target = "balanced";
    // How much of the measured deviation to correct (0.5 - 1.5).
    float room_eq_strength = 1.0f;
    // Free-form label of the last successful calibration.
    std::string room_eq_label;
    // Residual deviation from the target after correction, dB RMS.
    float room_eq_residual_db = 0.0f;
    // Saved room profile currently applied ("" = unsaved correction).
    std::string room_eq_active_id;
    // Switch the saved room automatically when the output device changes.
    bool room_eq_auto_room = true;
    // Curves behind the applied correction, for the always-visible graph.
    RoomEqCurve room_eq_curve;

    // Discord Rich Presence: application client id (empty = off). Also
    // overridable via P100_DISCORD_CLIENT_ID.
    std::string discord_client_id;

    // Server write-back master switch (off by default): mirror likes and
    // playlist edits to the signed-in YouTube Music account.
    bool server_sync = false;

    // Used by the periodic-save dirty check in the main loop.
    bool operator==(const Settings &o) const
    {
        return crossfade_duration == o.crossfade_duration &&
               skip_crossfade_enabled == o.skip_crossfade_enabled &&
               skip_crossfade_duration == o.skip_crossfade_duration &&
               skip_crossfade_scope == o.skip_crossfade_scope &&
               volume_normalization == o.volume_normalization &&
               show_notifications == o.show_notifications &&
               endless_radio == o.endless_radio && ytdl_browser == o.ytdl_browser &&
               auth_mode == o.auth_mode && lyrics_panel_open == o.lyrics_panel_open &&
               lyrics_offset == o.lyrics_offset &&
               lyrics_effect_style == o.lyrics_effect_style &&
               lyrics_panel_width == o.lyrics_panel_width && ui_scale == o.ui_scale &&
               ui_text_scale == o.ui_text_scale &&
               ui_widget_scale == o.ui_widget_scale &&
               ui_layout_scale == o.ui_layout_scale && animations == o.animations &&
               max_refresh_hz == o.max_refresh_hz &&
               day_mode == o.day_mode && accent_color == o.accent_color &&
               album_backdrop == o.album_backdrop &&
               album_backdrop_strength == o.album_backdrop_strength &&
               edge_fade_height == o.edge_fade_height &&
               hero_backdrop == o.hero_backdrop &&
               hero_backdrop_strength == o.hero_backdrop_strength &&
               backdrop_shader == o.backdrop_shader &&
               safe_mode == o.safe_mode &&
               check_for_updates == o.check_for_updates &&
               low_power == o.low_power &&
               low_power_on_battery == o.low_power_on_battery &&
               low_memory_mode == o.low_memory_mode &&
               warm_start == o.warm_start &&
               warm_start_mode == o.warm_start_mode &&
               warm_close_hides == o.warm_close_hides &&
               diagnostics_history == o.diagnostics_history &&
               glow_separators == o.glow_separators &&
               glow_separator_strength == o.glow_separator_strength &&
               backdrop_drift == o.backdrop_drift &&
               shuffle_enabled == o.shuffle_enabled &&
               repeat_mode == o.repeat_mode && ui_elements == o.ui_elements &&
               notes == o.notes && download_cap_gb == o.download_cap_gb &&
               warm_stream_resolver == o.warm_stream_resolver &&
               aggressive_prefetch == o.aggressive_prefetch &&
               playback_speed == o.playback_speed && eq_enabled == o.eq_enabled &&
               eq_bands == o.eq_bands && eq_layout == o.eq_layout &&
               eq_preset == o.eq_preset &&
               room_eq_enabled == o.room_eq_enabled &&
               room_eq_bands == o.room_eq_bands &&
               room_eq_preamp == o.room_eq_preamp && room_eq_mic == o.room_eq_mic &&
               room_eq_bass_db == o.room_eq_bass_db &&
               room_eq_treble_db == o.room_eq_treble_db &&
               room_eq_max_bands == o.room_eq_max_bands &&
               room_eq_target == o.room_eq_target &&
               room_eq_strength == o.room_eq_strength &&
               room_eq_label == o.room_eq_label &&
               room_eq_residual_db == o.room_eq_residual_db &&
               room_eq_active_id == o.room_eq_active_id &&
               room_eq_auto_room == o.room_eq_auto_room &&
               room_eq_curve == o.room_eq_curve &&
               discord_client_id == o.discord_client_id &&
               server_sync == o.server_sync && party == o.party;
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
