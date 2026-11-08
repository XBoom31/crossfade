#include "ui.h"
#include "json_util.h"
#include "tasks.h"
#include <cstdlib>
#include "party_util.h"
#include "ytdlp.h"
#include "texture.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "ui_notify.h"
#include "accent.h"
#include "paths.h"
#include "writeback.h"
#include "library.h"
#include "diagnostics.h"
#include "data_transfer.h"
#include "power.h"
#include "misc/cpp/imgui_stdlib.h"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <thread>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <functional>

// ----------------------------------------------------------------------
// Undo for the destructive reset buttons
// ----------------------------------------------------------------------
namespace
{
enum class ResetKind
{
    None,
    Likes,
    Albums,
    Artists,
    Playlists
};

struct ResetSnapshot
{
    ResetKind kind = ResetKind::None;
    AppState *state = nullptr;
    std::vector<Track> likes;
    std::vector<SavedAlbum> albums;
    std::vector<SavedArtist> artists;
    std::vector<Playlist> playlists;
};

ResetSnapshot g_reset_snapshot;

void undo_reset()
{
    ResetSnapshot &s = g_reset_snapshot;
    if (!s.state || s.kind == ResetKind::None)
        return;
    AppState &st = *s.state;
    switch (s.kind)
    {
    case ResetKind::Likes:
        st.liked_tracks = s.likes;
        save_liked_tracks(st.liked_tracks);
        break;
    case ResetKind::Albums:
        st.saved_albums = s.albums;
        save_saved_albums(st.saved_albums);
        break;
    case ResetKind::Artists:
        st.saved_artists = s.artists;
        save_saved_artists(st.saved_artists);
        break;
    case ResetKind::Playlists:
        st.playlists = s.playlists;
        st.selected_playlist_index = st.playlists.empty() ? -1 : 0;
        for (const auto &pl : st.playlists)
            save_playlist(pl.name, pl.tracks);
        break;
    default:
        break;
    }
    st.repaint_requested = true;
    s = ResetSnapshot{};
    ui_notify(UiNotifyType::Info, "Restored", "The reset was undone.", 4.0f);
}

void begin_reset(AppState &state, ResetKind kind)
{
    g_reset_snapshot = ResetSnapshot{};
    g_reset_snapshot.kind = kind;
    g_reset_snapshot.state = &state;
}

} // namespace

// ----------------------------------------------------------------------
// Settings persistence
// ----------------------------------------------------------------------
static std::string settings_file_path()
{
    return p100_config_path("settings.json");
}

// Minimal JSON string escaping/unescaping for the free-form notes field
// (the rest of the settings are scalars, so this is the only string that
// can contain quotes, backslashes or newlines).
static std::string json_escape(const std::string &s)
{
    return jsonutil::escape(s);
}



// Canonical list of individually-scalable UI elements. Keys are stable and
// persisted verbatim as "ui_elem_<key>".
const std::vector<UiElementDef> &ui_element_defs()
{
    static const std::vector<UiElementDef> defs = {
        {"list_thumb", "Track thumbnails"},
        {"list_row", "Track row spacing"},
        {"bottom_art", "Bottom album cover"},
        {"bottom_controls", "Bottom transport controls"},
        {"bottom_seekvol", "Seek & volume bars"},
        {"lyrics_art", "Lyrics panel cover"},
    };
    return defs;
}

static float elem_value(const Settings &s, const char *key)
{
    auto it = s.ui_elements.find(key);
    return it != s.ui_elements.end() ? it->second : 1.0f;
}

void save_settings(const Settings &settings)
{
    const std::string path = settings_file_path();
    // The rest of the persistence stack creates the config dir lazily; do the
    // same here so a settings change made before the first periodic save
    // (e.g. on a fresh install) isn't silently dropped.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (!out)
        return;
    out << "{"
        << "\"crossfade_duration\":" << settings.crossfade_duration << ","
        << "\"skip_crossfade_enabled\":" << (settings.skip_crossfade_enabled ? "true" : "false") << ","
        << "\"skip_crossfade_duration\":" << settings.skip_crossfade_duration << ","
        << "\"skip_crossfade_scope\":" << std::max(0, std::min(2, settings.skip_crossfade_scope)) << ","
        << "\"volume_normalization\":" << (settings.volume_normalization ? "true" : "false") << ","
        << "\"show_notifications\":" << (settings.show_notifications ? "true" : "false") << ","
        << "\"endless_radio\":" << (settings.endless_radio ? "true" : "false") << ","
        << "\"ytdl_browser\":\"" << settings.ytdl_browser << "\","
        << "\"auth_mode\":\"" << (settings.auth_mode == "oauth" ? "oauth" : "browser") << "\","
        << "\"lyrics_offset\":" << settings.lyrics_offset << ","
        << "\"lyrics_panel_open\":" << (settings.lyrics_panel_open ? "true" : "false") << ","
        << "\"lyrics_panel_width\":" << settings.lyrics_panel_width << ","
        << "\"ui_scale\":" << settings.ui_scale << ","
        << "\"ui_text_scale\":" << settings.ui_text_scale << ","
        << "\"ui_widget_scale\":" << settings.ui_widget_scale << ","
        << "\"ui_layout_scale\":" << settings.ui_layout_scale << ","
        << "\"animations\":" << std::max(0, std::min(3, settings.animations)) << ","
        << "\"shuffle_enabled\":" << (settings.shuffle_enabled ? "true" : "false") << ","
        << "\"repeat_mode\":" << std::max(0, std::min(2, settings.repeat_mode)) << ","
        << "\"day_mode\":" << (settings.day_mode ? "true" : "false") << ","
        << "\"accent_color\":\"" << json_escape(settings.accent_color) << "\","
        << "\"album_backdrop\":" << (settings.album_backdrop ? "true" : "false") << ","
        << "\"album_backdrop_strength\":" << settings.album_backdrop_strength << ","
        << "\"hero_backdrop\":" << settings.hero_backdrop << ","
        << "\"hero_backdrop_strength\":" << settings.hero_backdrop_strength << ","
        << "\"backdrop_shader\":" << (settings.backdrop_shader ? "true" : "false") << ","
        << "\"warm_start\":" << (settings.warm_start ? "true" : "false") << ","
        << "\"warm_start_mode\":" << settings.warm_start_mode << ","
        << "\"warm_close_hides\":" << (settings.warm_close_hides ? "true" : "false") << ","
        << "\"diagnostics_history\":" << (settings.diagnostics_history ? "true" : "false") << ","
        << "\"safe_mode\":" << (settings.safe_mode ? "true" : "false") << ","
        << "\"low_power\":" << (settings.low_power ? "true" : "false") << ","
        << "\"low_power_on_battery\":" << (settings.low_power_on_battery ? "true" : "false") << ","
        << "\"glow_separators\":" << (settings.glow_separators ? "true" : "false") << ","
        << "\"glow_separator_strength\":" << settings.glow_separator_strength << ","
        << "\"backdrop_drift\":" << (settings.backdrop_drift ? "true" : "false") << ","
        << "\"download_cap_gb\":" << settings.download_cap_gb
        << ",\"warm_stream_resolver\":" << (settings.warm_stream_resolver ? "true" : "false")
        << ",\"playback_speed\":" << settings.playback_speed
        << ",\"eq_enabled\":" << (settings.eq_enabled ? "true" : "false")
        << ",\"eq_layout\":" << std::max(0, std::min(4, settings.eq_layout))
        << ",\"eq_preset\":" << std::max(0, std::min(5, settings.eq_preset))
        << ",\"eq_freq\":[";
    for (size_t i = 0; i < settings.eq_bands.size(); ++i)
        out << (i ? "," : "") << settings.eq_bands[i].freq;
    out << "],\"eq_gain\":[";
    for (size_t i = 0; i < settings.eq_bands.size(); ++i)
        out << (i ? "," : "") << settings.eq_bands[i].gain;
    out << "],\"eq_q\":[";
    for (size_t i = 0; i < settings.eq_bands.size(); ++i)
        out << (i ? "," : "") << settings.eq_bands[i].q;
    out << "]"
        << ",\"discord_client_id\":\"" << settings.discord_client_id << "\""
        << ",\"server_sync\":" << (settings.server_sync ? "true" : "false")
        << ",\"notes\":\"" << json_escape(settings.notes) << "\""
        << ",\"party\":{"
        << "\"name\":\"" << json_escape(settings.party.name) << "\""
        << ",\"port\":" << std::max(1024, std::min(65535, settings.party.port))
        << ",\"interface_ip\":\"" << json_escape(settings.party.interface_ip) << "\""
        << ",\"require_approval\":" << (settings.party.require_approval ? "true" : "false")
        << ",\"max_per_guest\":" << std::max(0, std::min(20, settings.party.max_per_guest))
        << ",\"cooldown_sec\":" << std::max(0, std::min(60, settings.party.cooldown_sec))
        << ",\"max_video_minutes\":" << std::max(0, std::min(60, settings.party.max_video_minutes))
        << ",\"max_queue\":" << std::max(0, std::min(300, settings.party.max_queue))
        << ",\"allow_search\":" << (settings.party.allow_search ? "true" : "false")
        << ",\"allow_duplicates\":" << (settings.party.allow_duplicates ? "true" : "false")
        << ",\"show_names\":" << (settings.party.show_names ? "true" : "false")
        << "}";
    for (const auto &def : ui_element_defs())
        out << ",\"ui_elem_" << def.key << "\":" << elem_value(settings, def.key);
    out << "}";
    out.close();
}

Settings load_settings()
{
    Settings s; // defaults
    std::ifstream in(settings_file_path());
    if (!in.good())
        return s; // file missing -> return defaults

    // Read entire file
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (json.empty())
        return s; // empty file -> return defaults

    try
    {
        auto get_str = [&](const std::string &key)
        { return jsonutil::get_string(json, key); };
        auto get_float = [&](const std::string &key)
        { return static_cast<float>(jsonutil::get_double(json, key)); };
        auto get_bool = [&](const std::string &key)
        { return jsonutil::get_bool(json, key); };

        s.crossfade_duration = get_float("crossfade_duration");
        if (s.crossfade_duration < 0)
            s.crossfade_duration = 0;
        // Absent keys keep the struct defaults (off, 2 s, Next/Prev only).
        if (json.find("\"skip_crossfade_enabled\"") != std::string::npos)
            s.skip_crossfade_enabled = get_bool("skip_crossfade_enabled");
        if (json.find("\"skip_crossfade_duration\"") != std::string::npos)
        {
            s.skip_crossfade_duration = get_float("skip_crossfade_duration");
            if (s.skip_crossfade_duration < 0.5f)
                s.skip_crossfade_duration = 0.5f;
            if (s.skip_crossfade_duration > 5.0f)
                s.skip_crossfade_duration = 5.0f;
        }
        if (json.find("\"skip_crossfade_scope\"") != std::string::npos)
        {
            const int scope = (int)std::lround(get_float("skip_crossfade_scope"));
            s.skip_crossfade_scope = std::max(0, std::min(2, scope));
        }
        s.volume_normalization = get_bool("volume_normalization");
        s.show_notifications = get_bool("show_notifications");
        if (json.find("\"endless_radio\"") != std::string::npos)
            s.endless_radio = get_bool("endless_radio");
        if (json.find("\"download_cap_gb\"") != std::string::npos)
        {
            s.download_cap_gb = get_float("download_cap_gb");
            if (s.download_cap_gb < 0.0f)
                s.download_cap_gb = 0.0f; // 0 = unlimited
        }
        // Default is ON; only a written "false" turns it off.
        if (json.find("\"warm_stream_resolver\"") != std::string::npos)
            s.warm_stream_resolver = get_bool("warm_stream_resolver");
        if (json.find("\"playback_speed\"") != std::string::npos)
            s.playback_speed = get_float("playback_speed");
        if (s.playback_speed < 0.5f)
            s.playback_speed = 0.5f;
        if (s.playback_speed > 2.0f)
            s.playback_speed = 2.0f;
        if (json.find("\"eq_enabled\"") != std::string::npos)
            s.eq_enabled = get_bool("eq_enabled");
        if (json.find("\"eq_layout\"") != std::string::npos)
            s.eq_layout = std::max(0, std::min(4, (int)std::lround(get_float("eq_layout"))));
        if (json.find("\"eq_preset\"") != std::string::npos)
            s.eq_preset = std::max(0, std::min(5, (int)std::lround(get_float("eq_preset"))));
        {
            auto parse_float_array = [&](const char *key, std::vector<float> &out) {
                const std::string needle = std::string("\"") + key + "\":";
                size_t at = json.find(needle);
                if (at == std::string::npos)
                    return false;
                size_t arr = json.find('[', at);
                if (arr == std::string::npos)
                    return false;
                size_t pos = arr + 1;
                while (pos < json.size() && out.size() < 64)
                {
                    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ','))
                        ++pos;
                    if (pos >= json.size() || json[pos] == ']')
                        break;
                    char *endp = nullptr;
                    const float v = std::strtof(json.c_str() + pos, &endp);
                    if (endp == json.c_str() + pos)
                        break;
                    out.push_back(v);
                    pos = (size_t)(endp - json.c_str());
                }
                return true;
            };

            std::vector<float> freqs, gains, qs;
            const bool have_freq = parse_float_array("eq_freq", freqs);
            parse_float_array("eq_gain", gains);
            parse_float_array("eq_q", qs);
            if (have_freq)
            {
                s.eq_bands.clear();
                for (size_t i = 0; i < freqs.size(); ++i)
                {
                    EqBand b;
                    b.freq = std::max(20.0f, std::min(20000.0f, freqs[i]));
                    b.gain = i < gains.size()
                                 ? std::max(-15.0f, std::min(15.0f, gains[i]))
                                 : 0.0f;
                    b.q = i < qs.size() ? std::max(0.1f, std::min(10.0f, qs[i])) : 1.0f;
                    s.eq_bands.push_back(b);
                }
            }
            else if (json.find("\"eq_gains\"") != std::string::npos)
            {
                // Legacy five fixed gains: map them onto the default centres.
                std::vector<float> legacy;
                if (parse_float_array("eq_gains", legacy))
                {
                    for (size_t i = 0; i < s.eq_bands.size() && i < legacy.size(); ++i)
                        s.eq_bands[i].gain = std::max(-15.0f, std::min(15.0f, legacy[i]));
                }
            }
            std::sort(s.eq_bands.begin(), s.eq_bands.end(),
                      [](const EqBand &a, const EqBand &b) { return a.freq < b.freq; });
            if (s.eq_bands.size() > 32)
                s.eq_bands.resize(32);
            if (s.eq_bands.empty())
                s.eq_bands = Settings().eq_bands;
        }
        s.discord_client_id = get_str("discord_client_id");
        if (json.find("\"server_sync\"") != std::string::npos)
            s.server_sync = get_bool("server_sync");
        s.ytdl_browser = get_str("ytdl_browser");
        if (s.ytdl_browser.empty())
            s.ytdl_browser = "firefox";
        s.auth_mode = get_str("auth_mode");
        if (s.auth_mode != "oauth")
            s.auth_mode = "browser";
        // Absent key -> 0.0, which is exactly the "no shift" default.
        s.lyrics_offset = get_float("lyrics_offset");
        if (s.lyrics_offset > 5.0f)
            s.lyrics_offset = 5.0f;
        if (s.lyrics_offset < -5.0f)
            s.lyrics_offset = -5.0f;
        // Keep the struct default (open) when the key is absent from an
        // older settings file -- get_bool would report false for a missing key.
        if (json.find("\"lyrics_panel_open\"") != std::string::npos)
            s.lyrics_panel_open = get_bool("lyrics_panel_open");
        if (json.find("\"lyrics_panel_width\"") != std::string::npos)
        {
            const float w = get_float("lyrics_panel_width");
            if (w > 0.0f)
                s.lyrics_panel_width = w;
        }
        // ui_scale: keep the struct default (1.0) when the key is absent
        // from an older settings file, and clamp to the slider's range.
        if (json.find("\"ui_scale\"") != std::string::npos)
        {
            const float sc = get_float("ui_scale");
            if (sc > 0.0f)
                s.ui_scale = sc;
        }
        if (s.ui_scale < 0.75f)
            s.ui_scale = 0.75f;
        if (s.ui_scale > 2.0f)
            s.ui_scale = 2.0f;
        // Per-aspect multipliers (default 1.0 when absent).
        auto load_scale = [&](const char *key, float &dst)
        {
            if (json.find(std::string("\"") + key + "\"") != std::string::npos)
            {
                const float v = get_float(key);
                if (v > 0.0f)
                    dst = v;
            }
            if (dst < 0.5f)
                dst = 0.5f;
            if (dst > 2.0f)
                dst = 2.0f;
        };
        load_scale("ui_text_scale", s.ui_text_scale);
        load_scale("ui_widget_scale", s.ui_widget_scale);
        load_scale("ui_layout_scale", s.ui_layout_scale);
        if (json.find("\"animations\"") != std::string::npos)
        {
            int level = (int)std::lround(get_float("animations"));
            s.animations = std::max(0, std::min(3, level));
        }
        if (json.find("\"shuffle_enabled\"") != std::string::npos)
            s.shuffle_enabled = get_bool("shuffle_enabled");
        if (json.find("\"repeat_mode\"") != std::string::npos)
            s.repeat_mode = std::max(0, std::min(2, (int)std::lround(get_float("repeat_mode"))));
        if (json.find("\"day_mode\"") != std::string::npos)
            s.day_mode = get_bool("day_mode");
        // Empty/absent means automatic (system or daily colour). Ignore a
        // malformed value instead of leaving the accent stuck on black.
        s.accent_color = get_str("accent_color");
        if (!s.accent_color.empty())
        {
            ImVec4 parsed;
            if (!accent_parse_hex(s.accent_color.c_str(), parsed))
                s.accent_color.clear();
        }
        if (json.find("\"album_backdrop\"") != std::string::npos)
            s.album_backdrop = get_bool("album_backdrop");
        if (json.find("\"hero_backdrop\"") != std::string::npos)
            s.hero_backdrop = std::max(0, std::min(2, (int)std::lround(get_float("hero_backdrop"))));
        if (json.find("\"hero_backdrop_strength\"") != std::string::npos)
            s.hero_backdrop_strength =
                std::max(0.10f, std::min(1.0f, get_float("hero_backdrop_strength")));
        if (json.find("\"album_backdrop_strength\"") != std::string::npos)
        {
            const float v = get_float("album_backdrop_strength");
            if (v > 0.0f)
                s.album_backdrop_strength = std::max(0.10f, std::min(1.0f, v));
        }
        if (json.find("\"backdrop_shader\"") != std::string::npos)
            s.backdrop_shader = get_bool("backdrop_shader");
        if (json.find("\"warm_start\"") != std::string::npos)
            s.warm_start = get_bool("warm_start");
        if (json.find("\"warm_start_mode\"") != std::string::npos)
            s.warm_start_mode =
                std::max(0, std::min(2, (int)std::lround(get_float("warm_start_mode"))));
        if (json.find("\"warm_close_hides\"") != std::string::npos)
            s.warm_close_hides = get_bool("warm_close_hides");
        if (json.find("\"diagnostics_history\"") != std::string::npos)
            s.diagnostics_history = get_bool("diagnostics_history");
        if (json.find("\"safe_mode\"") != std::string::npos)
            s.safe_mode = get_bool("safe_mode");
        if (json.find("\"low_power\"") != std::string::npos)
            s.low_power = get_bool("low_power");
        if (json.find("\"low_power_on_battery\"") != std::string::npos)
            s.low_power_on_battery = get_bool("low_power_on_battery");
        if (json.find("\"glow_separators\"") != std::string::npos)
            s.glow_separators = get_bool("glow_separators");
        if (json.find("\"glow_separator_strength\"") != std::string::npos)
        {
            const float v = get_float("glow_separator_strength");
            if (v > 0.0f)
                s.glow_separator_strength = std::max(0.10f, std::min(1.50f, v));
        }
        if (json.find("\"backdrop_drift\"") != std::string::npos)
            s.backdrop_drift = get_bool("backdrop_drift");
        // Per-element multipliers (default 1.0 when absent).
        for (const auto &def : ui_element_defs())
        {
            const std::string key = std::string("ui_elem_") + def.key;
            float v = 1.0f;
            if (json.find("\"" + key + "\"") != std::string::npos)
            {
                const float parsed = get_float(key);
                if (parsed > 0.0f)
                    v = parsed;
            }
            if (v < 0.25f)
                v = 0.25f;
            if (v > 3.0f)
                v = 3.0f;
            s.ui_elements[def.key] = v;
        }
        // Free-form notes (multiline, JSON-escaped).
        if (json.find("\"notes\"") != std::string::npos)
            s.notes = jsonutil::get_string(json, "notes");

        // Party settings are stored as a nested object. Only settings live
        // here: the party token, guest ids and guest names are never written
        // to disk (and the party never auto-starts on launch).
        const size_t party_pos = json.find("\"party\"");
        if (party_pos != std::string::npos)
        {
            const size_t obj = json.find('{', party_pos);
            const size_t end = obj == std::string::npos ? std::string::npos
                                                        : json.find('}', obj);
            if (obj != std::string::npos && end != std::string::npos && end > obj)
            {
                const std::string pj = json.substr(obj, end - obj + 1);
                auto pget_str = [&](const std::string &key)
                { return jsonutil::get_string(pj, key); };
                auto pget_num = [&](const std::string &key)
                { return jsonutil::get_int(pj, key); };
                auto pget_bool = [&](const std::string &key)
                { return jsonutil::get_bool(pj, key); };

                std::string name = pget_str("name");
                if (!name.empty())
                    s.party.name = party_util::clean_utf8(name, 60);

                if (pj.find("\"port\"") != std::string::npos)
                    s.party.port = std::max(1024, std::min(65535, pget_num("port")));
                std::string iface = pget_str("interface_ip");
                bool ipv4_like = !iface.empty() && iface.size() <= 15;
                for (char c : iface)
                    if (!(std::isdigit((unsigned char)c) || c == '.'))
                        ipv4_like = false;
                s.party.interface_ip = ipv4_like ? iface : "";

                if (pj.find("\"require_approval\"") != std::string::npos)
                    s.party.require_approval = pget_bool("require_approval");
                if (pj.find("\"max_per_guest\"") != std::string::npos)
                    s.party.max_per_guest = std::max(0, std::min(20, pget_num("max_per_guest")));
                if (pj.find("\"cooldown_sec\"") != std::string::npos)
                    s.party.cooldown_sec = std::max(0, std::min(60, pget_num("cooldown_sec")));
                if (pj.find("\"max_video_minutes\"") != std::string::npos)
                    s.party.max_video_minutes =
                        std::max(0, std::min(60, pget_num("max_video_minutes")));
                if (pj.find("\"max_queue\"") != std::string::npos)
                    s.party.max_queue = std::max(0, std::min(300, pget_num("max_queue")));
                if (pj.find("\"allow_search\"") != std::string::npos)
                    s.party.allow_search = pget_bool("allow_search");
                if (pj.find("\"allow_duplicates\"") != std::string::npos)
                    s.party.allow_duplicates = pget_bool("allow_duplicates");
                if (pj.find("\"show_names\"") != std::string::npos)
                    s.party.show_names = pget_bool("show_names");
            }
        }
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "Failed to parse settings: %s. Using defaults.\n", e.what());
        return Settings(); // return defaults on error
    }
    return s;
}

// ----------------------------------------------------------------------
// Cache repair
// ----------------------------------------------------------------------
// Scan the downloaded songs and re-fetch anything missing from the local
// caches: a track with no album cover in the thumbnail cache gets its cover
// downloaded, and one with no cached lyrics at all gets a lyrics fetch.
// Network-bound, so it runs on a detached thread; the panel polls the
// progress atomics below.
static std::atomic<bool> s_repair_running{false};
static std::atomic<bool> s_repair_finished{false};
static std::atomic<int> s_repair_done{0};
static std::atomic<int> s_repair_total{0};
static std::atomic<int> s_repair_covers{0};
static std::atomic<int> s_repair_lyrics{0};

static void repair_cache_worker(AppState *state, std::vector<Track> tracks)
{
    int covers = 0;
    int lyrics = 0;
    const int total = (int)tracks.size();
    s_repair_total = total;
    for (int i = 0; i < total; ++i)
    {
        const std::string id = tracks[i].id;
        if (id.empty())
        {
            s_repair_done = i + 1;
            continue;
        }

        // Album cover: same URL the UI falls back to when meta is missing it.
        std::string thumb = tracks[i].thumbnail_url;
        if (thumb.empty())
            thumb = "https://img.youtube.com/vi/" + id + "/hqdefault.jpg";
        if (!thumbnail_is_cached(thumb))
        {
            if (!get_cached_thumbnail_path(thumb).empty())
                ++covers;
        }

        // Lyrics: only fetch when nothing (not even a "none" marker) is cached.
        if (!std::filesystem::exists(lyrics_cache_path(id)))
        {
            Lyrics lx;
            if (fetch_lyrics(id, lx))
            {
                save_lyrics_cache(id, lx);
                ++lyrics;
            }
        }

        s_repair_covers = covers;
        s_repair_lyrics = lyrics;
        s_repair_done = i + 1;
        if (state)
            state->repaint_requested = true;
    }
    s_repair_running = false;
    s_repair_finished = true;
    if (state)
        state->repaint_requested = true;
}

static void settings_help(const char *text)
{
    ui_push_small();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextColored(ui_c().text_muted, "%s", text);
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

static void settings_gap()
{
    ImGui::Dummy(ImVec2(0.0f, ui_m().space_4));
}

// ----------------------------------------------------------------------
// Settings category rail
// ----------------------------------------------------------------------
enum SettingsCategory
{
    SC_APPEARANCE = 0,
    SC_PLAYBACK,
    SC_PARTY,
    SC_LYRICS,
    SC_YOUTUBE,
    SC_SHORTCUTS,
    SC_LIBRARY,
    SC_POWER,
    SC_WARMSTART,
    SC_DATA,
    SC_DIAGNOSTICS,
    SC_NOTES,
    SC_DANGER,
    SC_COUNT
};

static int s_settings_category = SC_APPEARANCE;
static std::string s_diag_report;
static bool s_diag_open = false;
static data_transfer::Sections s_xfer_sections;
static std::string s_xfer_export_path;
static std::string s_xfer_import_path;
static std::string s_xfer_pick_dir;
static bool s_xfer_pick_open = false;
static bool s_xfer_confirm_open = false;

static ImU32 settings_lerp_col(ImU32 a, ImU32 b, float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    const int ar = (a >> 0) & 0xFF, ag = (a >> 8) & 0xFF, ab = (a >> 16) & 0xFF;
    const int br = (b >> 0) & 0xFF, bg = (b >> 8) & 0xFF, bb = (b >> 16) & 0xFF;
    return IM_COL32((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t),
                    (int)(ab + (bb - ab) * t),
                    (int)(((a >> 24) & 0xFF) + (((b >> 24) & 0xFF) - ((a >> 24) & 0xFF)) * t));
}

static ImU32 settings_with_alpha(ImU32 col, float alpha)
{
    const int a = (int)(((col >> 24) & 0xFF) * std::max(0.0f, std::min(1.0f, alpha)));
    return (col & 0x00FFFFFF) | ((ImU32)a << 24);
}

struct SettingsNavEntry
{
    int category;
    UiIcon icon;
    const char *label;
    const char *group;
};

static const SettingsNavEntry kSettingsNav[] = {
    {SC_APPEARANCE, UiIcon::Equalizer, "Appearance", "PERSONALISE"},
    {SC_PLAYBACK, UiIcon::Play, "Playback", nullptr},
    {SC_LYRICS, UiIcon::Lyrics, "Lyrics", nullptr},
    {SC_PARTY, UiIcon::Party, "Party", "CONNECT"},
    {SC_YOUTUBE, UiIcon::Playlist, "YouTube Music", nullptr},
    {SC_LIBRARY, UiIcon::Download, "Library", "SYSTEM"},
    {SC_SHORTCUTS, UiIcon::More, "Shortcuts", nullptr},
    {SC_POWER, UiIcon::Moon, "Power", nullptr},
    {SC_WARMSTART, UiIcon::Repeat, "Warm start", nullptr},
    {SC_DATA, UiIcon::Download, "Data transfer", nullptr},
    {SC_DIAGNOSTICS, UiIcon::Stats, "Diagnostics", nullptr},
    {SC_NOTES, UiIcon::Settings, "Notes", nullptr},
    {SC_DANGER, UiIcon::Close, "Danger Zone", nullptr},
};

// Vertical settings navigation. Lives in its own child window so it stays
// put while the content column scrolls independently.
static int settings_category_rail()
{
    const UiMetrics &m = ui_m();
    const float h = std::max(34.0f * m.layout_scale, m.control_h * 0.92f);
    const float fs = 16.5f * m.text_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();

    for (const auto &entry : kSettingsNav)
    {
        if (entry.group)
        {
            ImGui::Dummy(ImVec2(0.0f, m.space_2));
            ui_push_small();
            ImGui::TextColored(ImVec4(ui_c().text_dim.x, ui_c().text_dim.y, ui_c().text_dim.z, 0.72f),
                               "%s", entry.group);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, m.space_1));
        }
        ImGui::PushID(entry.category);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##cat", ImVec2(avail, h));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = s_settings_category == entry.category;
        if (ImGui::IsItemClicked())
        {
            s_settings_category = entry.category;
            ImGui::SetScrollY(0.0f);
        }
        static float active_t[SC_COUNT] = {0};
        static float hover_t[SC_COUNT] = {0};
        active_t[entry.category] = ui_anim_approach(active_t[entry.category], active ? 1.0f : 0.0f, 18.0f);
        hover_t[entry.category] = ui_anim_approach(hover_t[entry.category], hovered ? 1.0f : 0.0f, 20.0f);
        const float at = active_t[entry.category];
        const float ht = hover_t[entry.category];
        if (std::fabs(at - (active ? 1.0f : 0.0f)) > 0.002f ||
            std::fabs(ht - (hovered ? 1.0f : 0.0f)) > 0.002f)
            ui_anim_request();

        const ImVec2 p1(pos.x + avail, pos.y + h);
        if (at > 0.01f)
            dl->AddRectFilled(pos, p1, ui_accent_u32(0.11f * at + 0.035f * ht), m.radius_md);
        else if (ht > 0.01f)
            dl->AddRectFilled(pos, p1, ui_overlay_u32(0.045f * ht), m.radius_md);
        if (at > 0.01f)
            dl->AddRectFilled(ImVec2(pos.x, pos.y + h * 0.24f),
                              ImVec2(pos.x + 2.5f * m.layout_scale, pos.y + h * 0.76f),
                              ui_accent_u32(0.85f * at), 1.5f);

        const ImU32 icon_col = active ? ui_accent_text_u32()
                                      : ui_with_alpha(ui_text_muted_u32(), 0.85f);
        ui_draw_icon(dl, entry.icon,
                     ImVec2(pos.x + m.space_3 + 9.0f * m.layout_scale, pos.y + h * 0.5f),
                     18.0f * m.layout_scale, icon_col, 1.55f);
        dl->AddText(g_ui_fonts.medium, fs,
                    ImVec2(pos.x + m.space_3 + 30.0f * m.layout_scale,
                           pos.y + (h - fs * 1.2f) * 0.5f),
                    active ? ui_text_u32() : ui_with_alpha(ui_text_dim_u32(), 0.92f),
                    entry.label);
        ImGui::Dummy(ImVec2(0.0f, 1.0f));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0.0f, m.space_3));
    return s_settings_category;
}

// ----------------------------------------------------------------------
// Settings content primitives
// ----------------------------------------------------------------------
// A rounded surface that sizes itself to its contents. Drawing happens in a
// second ImDrawList channel so the card sits behind every widget without
// needing a fixed height up front.
class SettingsCard
{
public:
    SettingsCard(const char *title, const char *subtitle, UiIcon icon)
    {
        const UiMetrics &m = ui_m();
        m_dl = ImGui::GetWindowDrawList();
        m_dl->ChannelsSplit(2);
        m_dl->ChannelsSetCurrent(1);
        m_start = ImGui::GetCursorScreenPos();
        m_width = ImGui::GetContentRegionAvail().x;
        ImGui::Indent(m.space_4);
        ImGui::Dummy(ImVec2(0.0f, m.space_4));

        const float icon_box = 36.0f * m.layout_scale;
        const ImVec2 ip = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(icon_box, icon_box));
        m_dl->AddRectFilled(ip, ImVec2(ip.x + icon_box, ip.y + icon_box),
                            ui_accent_u32(0.12f), m.radius_md);
        m_dl->AddRect(ip, ImVec2(ip.x + icon_box, ip.y + icon_box),
                      ui_accent_u32(0.22f), m.radius_md, 0, 1.0f);
        ui_draw_icon(m_dl, icon, ImVec2(ip.x + icon_box * 0.5f, ip.y + icon_box * 0.5f),
                     icon_box * 0.52f, ui_accent_text_u32(), 1.6f);
        ImGui::SameLine(0.0f, m.space_3);
        ImGui::BeginGroup();
        ui_push_semibold(22.0f);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        if (subtitle && *subtitle)
        {
            ui_push_small();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextColored(ui_c().text_muted, "%s", subtitle);
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0.0f, m.space_3));
    }

    ~SettingsCard() { close(); }

    void close()
    {
        if (!m_dl)
            return;
        const UiMetrics &m = ui_m();
        ImGui::Dummy(ImVec2(0.0f, m.space_3));
        const ImVec2 end = ImGui::GetCursorScreenPos();
        ImGui::Unindent(m.space_4);
        m_dl->ChannelsSetCurrent(0);
        const ImVec2 p0(m_start.x, m_start.y);
        const ImVec2 p1(m_start.x + m_width, end.y + m.space_3);
        m_dl->AddRectFilled(p0, p1, ui_overlay_u32(0.028f), m.radius_lg);
        m_dl->AddRect(p0, p1, ui_with_alpha(ui_border_u32(), 0.70f), m.radius_lg, 0, 1.0f);
        m_dl->ChannelsMerge();
        m_dl = nullptr;
    }

private:
    ImDrawList *m_dl = nullptr;
    ImVec2 m_start{};
    float m_width = 0.0f;
};

static void settings_group_label(const char *text)
{
    const UiMetrics &m = ui_m();
    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    ui_push_semibold(16.0f);
    ImGui::TextColored(ImVec4(ui_c().text_muted.x, ui_c().text_muted.y, ui_c().text_muted.z, 0.95f),
                       "%s", text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, m.space_1));
}

static void settings_row_label(const char *label, const char *value, const char *help)
{
    const UiMetrics &m = ui_m();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float title_fs = 18.0f * m.text_scale;
    const float val_fs = 15.5f * m.text_scale;
    float val_w = 0.0f;
    if (value)
        val_w = ImGui::CalcTextSize(value).x;
    ImGui::Dummy(ImVec2(0.0f, title_fs * 1.3f));
    ImGui::GetWindowDrawList()->AddText(g_ui_fonts.medium, title_fs,
                                        ImVec2(pos.x, pos.y + 1.5f * m.layout_scale),
                                        ui_text_u32(), label);
    if (value)
        ImGui::GetWindowDrawList()->AddText(g_ui_fonts.medium, val_fs,
                                            ImVec2(pos.x + avail - val_w,
                                                   pos.y + 3.5f * m.layout_scale),
                                            ui_accent_text_u32(), value);
    if (help)
        settings_help(help);
}

static bool settings_slider_row(const char *id, const char *label, float *value,
                                float lo, float hi, const char *value_fmt,
                                const char *help,
                                const std::function<std::string(float)> &value_text = {})
{
    const UiMetrics &m = ui_m();
    char buf[96];
    if (value_text)
        std::snprintf(buf, sizeof(buf), "%s", value_text(*value).c_str());
    else
        std::snprintf(buf, sizeof(buf), value_fmt, (double)*value);
    settings_row_label(label, buf, help);

    ImGui::PushID(id);
    const float track_h = 28.0f * m.layout_scale;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(60.0f, ImGui::GetContentRegionAvail().x);
    ImGui::InvisibleButton("##slider", ImVec2(w, track_h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (active)
    {
        const float t = std::max(0.0f, std::min(1.0f,
                                                 (ImGui::GetIO().MousePos.x - p.x) / w));
        const float nv = lo + t * (hi - lo);
        if (std::fabs(nv - *value) > 1e-6f)
            *value = nv;
    }

    static std::unordered_map<ImGuiID, float> hover_map;
    float &hover_t = hover_map[ImGui::GetItemID()];
    hover_t = ui_anim_approach(hover_t, (hovered || active) ? 1.0f : 0.0f, 20.0f);
    const float t = std::max(0.0f, std::min(1.0f, (*value - lo) / (hi - lo)));
    const float cy = p.y + track_h * 0.5f;
    const float knob_r = (4.8f + 1.2f * hover_t) * m.layout_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 t0(p.x, cy - 1.6f * m.layout_scale);
    const ImVec2 t1(p.x + w, cy + 1.6f * m.layout_scale);
    dl->AddRectFilled(t0, t1, ui_overlay_u32(0.075f), 2.0f);
    const ImVec2 f1(p.x + w * t, cy + 1.6f * m.layout_scale);
    if (t > 0.001f)
    {
        dl->AddRectFilledMultiColor(t0, f1,
                                    ui_accent_u32(0.55f), ui_accent_u32(0.95f),
                                    ui_accent_u32(0.95f), ui_accent_u32(0.55f));
        dl->AddCircleFilled(ImVec2(p.x + w * t, cy), knob_r * 3.4f,
                            ui_accent_u32(0.10f + 0.06f * hover_t), 24);
        dl->AddCircleFilled(ImVec2(p.x + w * t, cy), knob_r,
                            ui_text_u32(), 18);
        dl->AddCircle(ImVec2(p.x + w * t, cy), knob_r + 1.2f,
                      ui_accent_u32(0.75f), 18, 1.4f);
    }
    static std::unordered_map<ImGuiID, bool> active_map;
    bool &was_active = active_map[ImGui::GetItemID()];
    const bool released = was_active && !active;
    was_active = active;
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    return released;
}

static bool settings_slider_int_row(const char *id, const char *label, int *value,
                                    int lo, int hi, const char *value_fmt,
                                    const char *help,
                                    const std::function<std::string(int)> &value_text = {})
{
    const UiMetrics &m = ui_m();
    char buf[96];
    if (value_text)
        std::snprintf(buf, sizeof(buf), "%s", value_text(*value).c_str());
    else
        std::snprintf(buf, sizeof(buf), value_fmt, *value);
    settings_row_label(label, buf, help);

    ImGui::PushID(id);
    const float track_h = 28.0f * m.layout_scale;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = std::max(60.0f, ImGui::GetContentRegionAvail().x);
    ImGui::InvisibleButton("##slider", ImVec2(w, track_h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    if (active)
    {
        const float t = std::max(0.0f, std::min(1.0f,
                                                 (ImGui::GetIO().MousePos.x - p.x) / w));
        const int nv = (int)std::lround(lo + t * (hi - lo));
        if (nv != *value)
            *value = nv;
    }
    static std::unordered_map<ImGuiID, float> hover_map;
    float &hover_t = hover_map[ImGui::GetItemID()];
    hover_t = ui_anim_approach(hover_t, (hovered || active) ? 1.0f : 0.0f, 20.0f);
    const float t = hi > lo ? std::max(0.0f, std::min(1.0f,
                                                     (float)(*value - lo) / (float)(hi - lo)))
                            : 0.0f;
    const float cy = p.y + track_h * 0.5f;
    const float knob_r = (4.8f + 1.2f * hover_t) * m.layout_scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 t0(p.x, cy - 1.6f * m.layout_scale);
    const ImVec2 t1(p.x + w, cy + 1.6f * m.layout_scale);
    dl->AddRectFilled(t0, t1, ui_overlay_u32(0.075f), 2.0f);
    if (t > 0.001f)
    {
        const ImVec2 f1(p.x + w * t, cy + 1.6f * m.layout_scale);
        dl->AddRectFilledMultiColor(t0, f1,
                                    ui_accent_u32(0.55f), ui_accent_u32(0.95f),
                                    ui_accent_u32(0.95f), ui_accent_u32(0.55f));
        dl->AddCircleFilled(ImVec2(p.x + w * t, cy), knob_r * 3.4f,
                            ui_accent_u32(0.10f + 0.06f * hover_t), 24);
        dl->AddCircleFilled(ImVec2(p.x + w * t, cy), knob_r, ui_text_u32(), 18);
        dl->AddCircle(ImVec2(p.x + w * t, cy), knob_r + 1.2f, ui_accent_u32(0.75f), 18, 1.4f);
    }
    static std::unordered_map<ImGuiID, bool> active_map;
    bool &was_active = active_map[ImGui::GetItemID()];
    const bool released = was_active && !active;
    was_active = active;
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    return released;
}

static bool settings_toggle_row(const char *id, const char *label, bool *value, const char *help)
{
    const UiMetrics &m = ui_m();
    const float avail = std::max(80.0f, ImGui::GetContentRegionAvail().x);
    const float switch_w = 44.0f * m.layout_scale;
    const float switch_h = 24.0f * m.layout_scale;
    const float pad = m.space_2;
    const float title_fs = 18.0f * m.text_scale;
    const float wrap_w = std::max(60.0f, avail - switch_w - pad * 3.0f);
    const float help_h = help ? ImGui::CalcTextSize(help, nullptr, false, wrap_w).y : 0.0f;
    const float row_h = pad * 2.0f + title_fs * 1.3f + (help ? m.space_1 + help_h : 0.0f);

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##toggle", ImVec2(avail, row_h));
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;
    if (ImGui::IsItemClicked())
    {
        *value = !*value;
        changed = true;
    }
    static std::unordered_map<ImGuiID, float> hover_map;
    static std::unordered_map<ImGuiID, float> on_map;
    float &hover_t = hover_map[ImGui::GetItemID()];
    float &on_t = on_map[ImGui::GetItemID()];
    hover_t = ui_anim_approach(hover_t, hovered ? 1.0f : 0.0f, 20.0f);
    on_t = ui_anim_approach(on_t, *value ? 1.0f : 0.0f, 22.0f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (hover_t > 0.01f)
        dl->AddRectFilled(pos, ImVec2(pos.x + avail, pos.y + row_h),
                          ui_overlay_u32(0.030f * hover_t), m.radius_md);
    dl->AddText(g_ui_fonts.medium, title_fs,
                ImVec2(pos.x + pad, pos.y + pad), ui_text_u32(), label);
    if (help)
    {
        ImGui::PushTextWrapPos(pos.x + wrap_w);
        ui_push_small();
        dl->AddText(g_ui_fonts.body, 14.5f * m.text_scale,
                    ImVec2(pos.x + pad, pos.y + pad + title_fs * 1.3f + m.space_1),
                    ui_text_muted_u32(), help, nullptr, wrap_w);
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
    }
    const ImVec2 sp(pos.x + avail - switch_w - pad,
                    pos.y + pad + (title_fs * 1.3f - switch_h) * 0.5f);
    const ImVec2 sp1(sp.x + switch_w, sp.y + switch_h);
    dl->AddRectFilled(sp, sp1,
                      settings_lerp_col(ui_overlay_u32(0.12f), ui_accent_u32(0.92f), on_t),
                      switch_h * 0.5f);
    dl->AddRect(sp, sp1, ui_with_alpha(ui_border_u32(), 0.8f), switch_h * 0.5f, 0, 1.0f);
    const float knob_x = sp.x + switch_h * 0.5f + on_t * (switch_w - switch_h);
    dl->AddCircleFilled(ImVec2(knob_x, sp.y + switch_h * 0.5f),
                        (switch_h * 0.5f - 3.0f * m.layout_scale),
                        on_t > 0.5f ? ui_col(ui_c().on_accent) : ui_text_u32(), 20);
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, m.space_1));
    return changed;
}

static bool settings_button_row(const char *label, const char *desc, const char *button,
                                bool primary = false, float width = 190.0f)
{
    const UiMetrics &m = ui_m();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float bw = width * m.layout_scale;
    const bool stacked = avail < 520.0f * m.layout_scale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float title_fs = 18.0f * m.text_scale;
    const float text_w = stacked ? avail - m.space_2 * 2.0f
                                 : std::max(80.0f, avail - bw - m.space_4);
    const float desc_h = desc ? ImGui::CalcTextSize(desc, nullptr, false, text_w).y : 0.0f;
    const float text_h = m.space_2 * 2.0f + title_fs * 1.3f + (desc ? m.space_1 + desc_h : 0.0f);
    const float row_h = text_h + (stacked ? m.button_h + m.space_2 : 0.0f);
    ImGui::Dummy(ImVec2(0.0f, row_h));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddText(g_ui_fonts.medium, title_fs, ImVec2(pos.x + m.space_2, pos.y + m.space_2),
                ui_text_u32(), label);
    if (desc)
        dl->AddText(g_ui_fonts.body, 14.5f * m.text_scale,
                    ImVec2(pos.x + m.space_2, pos.y + m.space_2 + title_fs * 1.3f + m.space_1),
                    ui_text_muted_u32(), desc, nullptr, text_w);
    if (stacked)
        ImGui::SetCursorScreenPos(ImVec2(pos.x + m.space_2, pos.y + text_h));
    else
        ImGui::SetCursorScreenPos(ImVec2(pos.x + avail - bw,
                                         pos.y + (row_h - m.button_h) * 0.5f));
    const bool clicked = primary ? ui_primary_button(button, ImVec2(stacked ? text_w : bw, m.button_h))
                                 : ui_ghost_button(button, ImVec2(stacked ? text_w : bw, m.button_h));
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + row_h));
    ImGui::Dummy(ImVec2(0.0f, 1.0f));
    return clicked;
}

static void settings_preview_card(const UiMetrics &m, float width)
{
    const float h = std::max(112.0f * m.layout_scale, m.control_h * 2.35f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + h);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float r = m.radius_lg;
    dl->AddRectFilled(p0, p1, ui_overlay_u32(0.035f), r);
    dl->AddRect(p0, p1, ui_with_alpha(ui_border_u32(), 0.85f), r, 0, 1.0f);
    // A faint accent wash in the corner so the card feels part of the app.
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + width * 0.55f, p1.y),
                                ui_accent_u32(0.055f), ui_accent_u32(0.0f),
                                ui_accent_u32(0.0f), ui_accent_u32(0.0f));
    ImGui::Dummy(ImVec2(width, h));

    const float pad = m.space_4;
    const float art = std::min(62.0f * m.layout_scale, h - pad * 2.0f);
    const ImVec2 a0(p0.x + pad, p0.y + (h - art) * 0.5f);
    const ImVec2 a1(a0.x + art, a0.y + art);
    dl->AddRectFilled(a0, a1, ui_accent_u32(0.20f), m.radius_md);
    dl->AddRectFilledMultiColor(a0, a1, ui_accent_u32(0.55f), ui_accent_u32(0.18f),
                                ui_accent_u32(0.30f), ui_accent_u32(0.08f));
    dl->AddRect(a0, a1, ui_accent_u32(0.45f), m.radius_md, 0, 1.0f);
    ui_draw_icon(dl, UiIcon::Play, ImVec2((a0.x + a1.x) * 0.5f, (a0.y + a1.y) * 0.5f),
                 22.0f * m.layout_scale, ui_text_u32(), 1.8f);

    const float text_x = a1.x + m.space_4;
    const float text_right = p1.x - pad;
    const float title_fs = 20.0f * m.text_scale;
    const float sub_fs = 14.0f * m.text_scale;
    const std::string title = "Live preview";
    const std::string sub = "UI scale " +
        std::to_string((int)std::lround(m.layout_scale * 100.0f)) + "% · " +
        (ui_animations_enabled() ? "motion on" : "motion off");
    dl->AddText(g_ui_fonts.semibold, title_fs, ImVec2(text_x, p0.y + pad),
                ui_text_u32(), title.c_str());
    dl->AddText(g_ui_fonts.body, sub_fs, ImVec2(text_x, p0.y + pad + title_fs + 2.0f * m.layout_scale),
                ui_text_dim_u32(), sub.c_str());

    // Three mini rows + an animated five-bar equalizer.
    const float rows_y = p0.y + pad + title_fs + sub_fs + m.space_3;
    const float row_gap = std::max(9.0f * m.layout_scale, 4.0f);
    const float line_w = std::max(24.0f, (text_right - text_x) * 0.46f);
    for (int i = 0; i < 3; ++i)
    {
        const float yy = rows_y + (float)i * row_gap;
        dl->AddRectFilled(ImVec2(text_x, yy), ImVec2(text_x + line_w, yy + 4.0f * m.layout_scale),
                          ui_with_alpha(ui_text_muted_u32(), 0.55f), 2.0f);
        const float tw = line_w * (0.42f + 0.11f * (float)i);
        dl->AddRectFilled(ImVec2(text_x + line_w + m.space_2, yy),
                          ImVec2(text_x + line_w + m.space_2 + tw, yy + 4.0f * m.layout_scale),
                          ui_with_alpha(ui_text_muted_u32(), 0.30f), 2.0f);
    }
    // Static bars: this card used to animate its equalizer forever, which
    // pinned the whole app at 60 FPS whenever Settings was open.
    const float eq_x = text_right - 52.0f * m.layout_scale;
    const float eq_base = p1.y - pad;
    for (int i = 0; i < 5; ++i)
    {
        const float bh = (7.0f + 13.0f * (0.5f + 0.5f * std::sin((float)i * 0.9f))) *
                         m.layout_scale;
        dl->AddRectFilled(ImVec2(eq_x + i * 9.0f * m.layout_scale, eq_base - bh),
                          ImVec2(eq_x + i * 9.0f * m.layout_scale + 4.0f * m.layout_scale, eq_base),
                          ui_accent_u32(0.65f), 2.0f);
    }
}

// ----------------------------------------------------------------------
// Equalizer layouts / presets
// ----------------------------------------------------------------------
static const float kEq5Hz[] = {60.0f, 230.0f, 910.0f, 3600.0f, 14000.0f};
static const float kEq10Hz[] = {31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
                                1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};
static const float kEq15Hz[] = {25.0f, 40.0f, 63.0f, 100.0f, 160.0f, 250.0f,
                                400.0f, 630.0f, 1000.0f, 1600.0f, 2500.0f,
                                4000.0f, 6300.0f, 10000.0f, 16000.0f};
static const float kEq31Hz[] = {
    20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f,
    125.0f, 160.0f, 200.0f, 250.0f, 315.0f, 400.0f, 500.0f, 630.0f,
    800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f, 2500.0f, 3150.0f, 4000.0f,
    5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f, 16000.0f, 20000.0f};

static const float *eq_layout_freqs(int layout, int &count)
{
    switch (layout)
    {
    case 0: count = 5; return kEq5Hz;
    case 1: count = 10; return kEq10Hz;
    case 2: count = 15; return kEq15Hz;
    case 3: count = 31; return kEq31Hz;
    default: count = 0; return nullptr;
    }
}

static float eq_layout_q(int layout)
{
    switch (layout)
    {
    case 0: return 1.0f;
    case 1: return 1.414f;
    case 2: return 2.0f;
    case 3: return 4.318f;
    default: return 1.0f;
    }
}

static float eq_interp_gain(const std::vector<EqBand> &bands, float hz)
{
    if (bands.empty())
        return 0.0f;
    std::vector<EqBand> sorted = bands;
    std::sort(sorted.begin(), sorted.end(),
              [](const EqBand &a, const EqBand &b) { return a.freq < b.freq; });
    const float lf = std::log10(std::max(20.0f, hz));
    if (lf <= std::log10(sorted.front().freq))
        return sorted.front().gain;
    if (lf >= std::log10(sorted.back().freq))
        return sorted.back().gain;
    for (size_t i = 1; i < sorted.size(); ++i)
    {
        const float l0 = std::log10(sorted[i - 1].freq);
        const float l1 = std::log10(sorted[i].freq);
        if (lf <= l1)
        {
            const float t = (lf - l0) / std::max(0.0001f, l1 - l0);
            return sorted[i - 1].gain + (sorted[i].gain - sorted[i - 1].gain) * t;
        }
    }
    return 0.0f;
}

static void eq_rebuild_layout(Settings &st, int layout)
{
    st.eq_layout = std::max(0, std::min(4, layout));
    if (st.eq_layout == 4)
        return; // custom keeps its bands
    int count = 0;
    const float *freqs = eq_layout_freqs(st.eq_layout, count);
    const float q = eq_layout_q(st.eq_layout);
    std::vector<EqBand> old = st.eq_bands;
    st.eq_bands.clear();
    for (int i = 0; i < count; ++i)
    {
        EqBand b;
        b.freq = freqs[i];
        b.gain = eq_interp_gain(old, freqs[i]);
        b.q = q;
        st.eq_bands.push_back(b);
    }
}

static float eq_preset_gain(int preset, float hz)
{
    struct Point { float hz; float db; };
    static const Point flat[] = {{20, 0}, {20000, 0}};
    static const Point bass[] = {{20, 6}, {120, 6}, {400, 0}, {2000, 0}, {20000, 1}};
    static const Point vocal[] = {{20, -3}, {150, -2}, {600, 0}, {1500, 4}, {4000, 4}, {8000, 0}, {20000, -2}};
    static const Point rock[] = {{20, 3}, {80, 5}, {200, 2}, {500, -2}, {1000, -1}, {3000, 3}, {6000, 3}, {20000, 1}};
    static const Point electronic[] = {{20, 5}, {80, 5}, {250, 1}, {800, -1}, {1500, -2}, {3000, 1}, {6000, 4}, {12000, 4}, {20000, 2}};
    const Point *pts = flat;
    size_t n = sizeof(flat) / sizeof(flat[0]);
    switch (preset)
    {
    case 1: pts = bass; n = sizeof(bass) / sizeof(bass[0]); break;
    case 2: pts = vocal; n = sizeof(vocal) / sizeof(vocal[0]); break;
    case 3: pts = rock; n = sizeof(rock) / sizeof(rock[0]); break;
    case 4: pts = electronic; n = sizeof(electronic) / sizeof(electronic[0]); break;
    default: break;
    }
    const float lf = std::log10(std::max(20.0f, hz));
    if (lf <= std::log10(pts[0].hz))
        return pts[0].db;
    for (size_t i = 1; i < n; ++i)
    {
        if (lf <= std::log10(pts[i].hz))
        {
            const float l0 = std::log10(pts[i - 1].hz);
            const float l1 = std::log10(pts[i].hz);
            const float t = (lf - l0) / std::max(0.0001f, l1 - l0);
            return pts[i - 1].db + (pts[i].db - pts[i - 1].db) * t;
        }
    }
    return pts[n - 1].db;
}

static void eq_apply_preset(Settings &st, int preset)
{
    st.eq_preset = std::max(0, std::min(5, preset));
    if (st.eq_preset == 5)
        return; // Custom: keep the curve
    for (EqBand &b : st.eq_bands)
        b.gain = std::max(-15.0f, std::min(15.0f, eq_preset_gain(st.eq_preset, b.freq)));
}

static std::string eq_freq_label(float hz)
{
    char buf[24];
    if (hz >= 1000.0f)
        std::snprintf(buf, sizeof(buf), "%.1fk", hz / 1000.0f);
    else
        std::snprintf(buf, sizeof(buf), "%.0f", hz);
    return buf;
}

static void eq_add_band(Settings &st)
{
    if (st.eq_bands.size() >= 32)
        return;
    float freq = 1000.0f;
    if (st.eq_bands.empty())
    {
        st.eq_bands.push_back({freq, 0.0f, 1.0f});
        return;
    }
    std::vector<EqBand> sorted = st.eq_bands;
    std::sort(sorted.begin(), sorted.end(),
              [](const EqBand &a, const EqBand &b) { return a.freq < b.freq; });
    float best_lo = 20.0f, best_hi = sorted.front().freq;
    float best_gap = std::log10(best_hi) - std::log10(best_lo);
    for (size_t i = 1; i < sorted.size(); ++i)
    {
        const float gap = std::log10(sorted[i].freq) - std::log10(sorted[i - 1].freq);
        if (gap > best_gap)
        {
            best_gap = gap;
            best_lo = sorted[i - 1].freq;
            best_hi = sorted[i].freq;
        }
    }
    if (std::log10(20000.0f) - std::log10(sorted.back().freq) > best_gap)
    {
        best_lo = sorted.back().freq;
        best_hi = 20000.0f;
    }
    freq = std::sqrt(best_lo * best_hi);
    float gain = eq_interp_gain(st.eq_bands, freq);
    float q = eq_layout_q(st.eq_layout == 4 ? 0 : st.eq_layout);
    st.eq_bands.push_back({freq, gain, q});
    std::sort(st.eq_bands.begin(), st.eq_bands.end(),
              [](const EqBand &a, const EqBand &b) { return a.freq < b.freq; });
}

void draw_settings_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    const float panel_avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float nav_w = std::min(210.0f * m.layout_scale,
                                 std::max(160.0f * m.layout_scale, panel_avail * 0.26f));
    const float content_w = std::min(840.0f * m.layout_scale,
                                     std::max(240.0f, panel_avail - nav_w - m.space_5));
    const float field_w = std::min(520.0f * m.layout_scale,
                                   std::max(180.0f * m.layout_scale, content_w));
    bool save = false;

    if (state.settings_focus_party)
        s_settings_category = SC_PARTY;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##settings_nav", ImVec2(nav_w, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    settings_category_rail();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, m.space_5);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##settings_content", ImVec2(content_w, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    smooth_scroll_child();
    ImGui::PopStyleColor();

    if (s_settings_category == SC_APPEARANCE)
    {
        {
            SettingsCard card("Interface", "Scale the whole app, or fine-tune one part at a time.",
                              UiIcon::Equalizer);
            settings_preview_card(m, ImGui::GetContentRegionAvail().x);

            if (settings_slider_row("##ui_scale", "UI scale", &state.settings.ui_scale,
                                    0.75f, 2.0f, "%.2fx",
                                    "Scales text, widgets and layout together. Most people only need this one."))
                save = true;
            if (settings_button_row("Reset scale", "Return the whole interface to 100%.",
                                    "Reset to 1.00x", false, 165.0f))
            {
                state.settings.ui_scale = 1.0f;
                save = true;
            }

            settings_group_label("Fine-tune");
            if (settings_slider_row("##ui_text_scale", "Text size", &state.settings.ui_text_scale,
                                    0.5f, 2.0f, "%.2fx",
                                    "Only changes type. Useful when the layout is right but labels are too small."))
                save = true;
            if (settings_slider_row("##ui_widget_scale", "Widgets & spacing",
                                    &state.settings.ui_widget_scale, 0.5f, 2.0f, "%.2fx",
                                    "Paddings, control heights, scrollbars and gaps between widgets."))
                save = true;
            if (settings_slider_row("##ui_layout_scale", "Layout & sections",
                                    &state.settings.ui_layout_scale, 0.5f, 2.0f, "%.2fx",
                                    "Section heights, artwork and the big blocks. Text stays where you put it."))
                save = true;

            static bool show_elements = false;
            if (settings_toggle_row("##show_elements", "Individual elements", &show_elements,
                                    "Override one part of the player without moving everything else. Leave at 1.00x unless something feels off."))
                save = true;
            if (show_elements)
            {
                ImGui::Dummy(ImVec2(0.0f, m.space_1));
                for (const auto &def : ui_element_defs())
                {
                    auto it = state.settings.ui_elements.find(def.key);
                    if (it == state.settings.ui_elements.end())
                        it = state.settings.ui_elements.emplace(def.key, 1.0f).first;
                    ImGui::PushID(def.key);
                    if (settings_slider_row("##v", def.label, &it->second, 0.25f, 3.0f,
                                            "%.2fx", nullptr))
                        save = true;
                    ImGui::PopID();
                }
                if (settings_button_row("Reset element sizes", "Put every individual element back to 100%.",
                                        "Reset elements", false, 170.0f))
                {
                    for (const auto &def : ui_element_defs())
                        state.settings.ui_elements[def.key] = 1.0f;
                    save = true;
                }
            }
        }

        {
            SettingsCard card("Colour & motion", "Make the room feel like yours.",
                              UiIcon::Sun);

            // Accent swatch row: a live circular preview plus eight presets.
            settings_row_label("Accent colour",
                               state.settings.accent_color.empty() ? "Automatic"
                                                                  : state.settings.accent_color.c_str(),
                               "Used for highlights, progress bars and accents. Automatic follows your desktop accent or a fresh daily colour.");
            {
                const ImVec2 row = ImGui::GetCursorScreenPos();
                const float sw = 26.0f * m.layout_scale;
                const float gap = m.space_2;
                ImDrawList *dl = ImGui::GetWindowDrawList();
                static const ImU32 presets[] = {
                    IM_COL32(29, 185, 84, 255), IM_COL32(62, 198, 255, 255),
                    IM_COL32(91, 140, 255, 255), IM_COL32(169, 123, 255, 255),
                    IM_COL32(255, 122, 200, 255), IM_COL32(255, 93, 115, 255),
                    IM_COL32(255, 159, 67, 255), IM_COL32(247, 209, 84, 255)};
                float x = row.x;
                for (int i = 0; i < 8; ++i)
                {
                    ImGui::SetCursorScreenPos(ImVec2(x, row.y));
                    ImGui::PushID(i);
                    ImGui::InvisibleButton("##preset", ImVec2(sw, sw));
                    const bool hov = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked())
                    {
                        ImVec4 c = ImGui::ColorConvertU32ToFloat4(presets[i]);
                        state.settings.accent_color = accent_to_hex(c);
                        update_accent(state.settings);
                        ui_theme_refresh_accent();
                        save = true;
                    }
                    const ImVec2 c(x + sw * 0.5f, row.y + sw * 0.5f);
                    if (hov)
                        dl->AddCircleFilled(c, sw * 0.62f, settings_with_alpha(presets[i], 0.22f), 24);
                    dl->AddCircleFilled(c, sw * 0.40f, presets[i], 24);
                    dl->AddCircle(c, sw * 0.40f, IM_COL32(255, 255, 255, 80), 24, 1.0f);
                    ImGui::PopID();
                    x += sw + gap;
                }
                // Current / automatic swatch.
                ImGui::SetCursorScreenPos(ImVec2(x + m.space_2, row.y));
                ImGui::InvisibleButton("##current_accent", ImVec2(sw, sw));
                if (ImGui::IsItemClicked())
                    ImGui::OpenPopup("##accent_popup");
                const ImVec2 c(x + m.space_2 + sw * 0.5f, row.y + sw * 0.5f);
                dl->AddCircleFilled(c, sw * 0.52f, IM_COL32(0, 0, 0, 90), 24);
                dl->AddCircleFilled(c, sw * 0.42f, ui_accent_u32(1.0f), 24);
                dl->AddCircle(c, sw * 0.42f, IM_COL32(255, 255, 255, 150), 24, 1.2f);
                dl->AddCircle(c, sw * 0.56f, ui_accent_u32(0.45f), 24, 1.0f);
                ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + sw + m.space_2));
                if (ui_ghost_button("Automatic", ImVec2(130.0f * m.layout_scale, m.control_h)))
                {
                    state.settings.accent_color.clear();
                    update_accent(state.settings);
                    ui_theme_refresh_accent();
                    save = true;
                }
                ImGui::SameLine(0.0f, m.space_2);
                if (ui_ghost_button("Custom...", ImVec2(130.0f * m.layout_scale, m.control_h)))
                    ImGui::OpenPopup("##accent_popup");
                if (ImGui::BeginPopup("##accent_popup"))
                {
                    ImVec4 accent = g_accent;
                    if (ImGui::ColorPicker4("##accent_picker", &accent.x,
                                            ImGuiColorEditFlags_NoAlpha))
                    {
                        state.settings.accent_color = accent_to_hex(accent);
                        update_accent(state.settings);
                        ui_theme_refresh_accent();
                    }
                    if (ImGui::IsWindowAppearing())
                        ImGui::SetNextWindowPos(ImGui::GetMousePos());
                    ImGui::EndPopup();
                }
                else if (ImGui::IsItemDeactivated())
                {
                    if (!state.settings.accent_color.empty())
                        save = true;
                }
                ImGui::Dummy(ImVec2(0.0f, m.space_2));
            }

            // Animations as a segmented control.
            static const char *animations[] = {"Off", "Subtle", "Standard", "Playful"};
            settings_row_label("Motion", animations[std::max(0, std::min(3, state.settings.animations))],
                               "Motion for track changes, list reveals and scrolling. Standard is the default.");
            {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float gap = m.space_2;
                const float bh = 34.0f * m.layout_scale;
                const float bw = (ImGui::GetContentRegionAvail().x - gap * 3.0f) / 4.0f;
                ImDrawList *dl = ImGui::GetWindowDrawList();
                for (int i = 0; i < 4; ++i)
                {
                    ImGui::SetCursorScreenPos(ImVec2(pos.x + i * (bw + gap), pos.y));
                    ImGui::PushID(i);
                    ImGui::InvisibleButton("##anim", ImVec2(bw, bh));
                    const bool hov = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked())
                    {
                        state.settings.animations = i;
                        save = true;
                    }
                    const bool on = state.settings.animations == i;
                    const ImVec2 c0(pos.x + i * (bw + gap), pos.y);
                    const ImVec2 c1(c0.x + bw, c0.y + bh);
                    dl->AddRectFilled(c0, c1, on ? ui_accent_u32(0.20f)
                                                 : ui_overlay_u32(hov ? 0.075f : 0.035f),
                                      m.radius_md);
                    dl->AddRect(c0, c1, on ? ui_accent_u32(0.45f)
                                           : ui_with_alpha(ui_border_u32(), 0.6f),
                                m.radius_md, 0, 1.0f);
                    const ImVec2 ts = g_ui_fonts.medium->CalcTextSizeA(15.5f * m.text_scale,
                                                                       FLT_MAX, 0, animations[i]);
                    dl->AddText(g_ui_fonts.medium, 15.5f * m.text_scale,
                                ImVec2(c0.x + (bw - ts.x) * 0.5f, c0.y + (bh - ts.y) * 0.5f),
                                on ? ui_text_u32() : ui_text_dim_u32(), animations[i]);
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + bh + m.space_2));
            }

            if (settings_toggle_row("##album_backdrop", "Album colour backdrop",
                                    &state.settings.album_backdrop,
                                    "Tint the room with the dominant colour of the playing cover. It crossfades on track changes and stays dim in night mode."))
                save = true;
            if (state.settings.album_backdrop)
            {
                if (settings_slider_row("##album_backdrop_strength", "Backdrop glow",
                                        &state.settings.album_backdrop_strength, 0.10f, 1.0f,
                                        "%.2f", nullptr))
                    save = true;
                if (settings_toggle_row("##backdrop_drift", "Drifting glow",
                                        &state.settings.backdrop_drift,
                                        "Slowly move the glow to a different corner so the backdrop never looks pinned in place."))
                    save = true;
                if (settings_toggle_row("##backdrop_shader", "Smooth backdrop shader",
                                        &state.settings.backdrop_shader,
                                        "One full-screen shader pass with subtle dithering. Removes the colour steps OLED panels expose."))
                    save = true;
            }
            {
                const char *hero_modes[] = {"Off", "Transparent blur", "Cover blur"};
                int hero_mode = std::max(0, std::min(2, state.settings.hero_backdrop));
                settings_row_label("Hero background", hero_modes[hero_mode],
                                   "Transparent blur frosts the page backdrop; Cover blur uses the cover art, cover-fitted (never stretched). The blur is generated once per cover by a shader and cached, so it costs nothing per frame.");
                ImGui::SetNextItemWidth(field_w);
                if (ImGui::Combo("##hero_backdrop", &hero_mode, hero_modes, 3))
                {
                    state.settings.hero_backdrop = hero_mode;
                    save = true;
                }
                if (state.settings.hero_backdrop != 0 &&
                    settings_slider_row("##hero_backdrop_strength", "Hero blur opacity",
                                        &state.settings.hero_backdrop_strength, 0.10f, 1.0f,
                                        "%.2f", nullptr))
                    save = true;
            }
            if (settings_toggle_row("##glow_separators", "Glowing separators",
                                    &state.settings.glow_separators,
                                    "A soft album-coloured light streak under section headers, plus faint tinted dividers."))
                save = true;
            if (state.settings.glow_separators)
            {
                if (settings_slider_row("##glow_separator_strength", "Separator glow",
                                        &state.settings.glow_separator_strength, 0.10f, 1.50f,
                                        "%.2f", nullptr))
                    save = true;
            }
        }
    }
    if (s_settings_category == SC_PLAYBACK)
    {
        SettingsCard card("Playback", "Crossfade, speed and audio.", UiIcon::Play);

        settings_group_label("Crossfade");
        if (settings_slider_row("##crossfade", "End-of-track crossfade",
                                &state.settings.crossfade_duration, 0.0f, 20.0f, "%.1f s",
                                "Blend the end of one track into the beginning of the next.",
                                [](float v) { return v < 0.05f ? std::string("Off")
                                                                     : std::to_string(v).substr(0, 4) + " s"; }))
            save = true;
        if (settings_toggle_row("##skip_fade", "Crossfade manual skips",
                                &state.settings.skip_crossfade_enabled,
                                "Blend into the target track when you press Next or Previous, if it is already downloaded."))
            save = true;
        if (state.settings.skip_crossfade_enabled)
        {
            if (settings_slider_row("##skip_crossfade", "Skip crossfade",
                                    &state.settings.skip_crossfade_duration, 0.5f, 5.0f, "%.1f s",
                                    "Shorter than the end-of-track blend. Uncached tracks switch instantly."))
                save = true;
            static const char *skip_scopes[] = {
                "Next / Previous only",
                "Next / Previous + track clicks",
                "All manual track changes"};
            settings_row_label("Applies to",
                               skip_scopes[std::max(0, std::min(2, state.settings.skip_crossfade_scope))],
                               nullptr);
            ImGui::SetNextItemWidth(field_w);
            if (ImGui::Combo("##skip_crossfade_scope", &state.settings.skip_crossfade_scope,
                             skip_scopes, 3))
                save = true;
            ImGui::Dummy(ImVec2(0.0f, m.space_2));
        }

        settings_group_label("Sound");
        if (settings_slider_row("##playback_speed", "Playback speed",
                                &state.settings.playback_speed, 0.5f, 2.0f, "%.2fx",
                                "Applied live to playback. The bottom bar also has quick presets."))
            save = true;
        if (settings_toggle_row("##vol_norm", "Volume normalization",
                                &state.settings.volume_normalization,
                                "Even out loudness between tracks with EBU R128 loudnorm. Takes effect immediately."))
            save = true;
        if (settings_toggle_row("##eq_enabled", "Equalizer",
                                &state.settings.eq_enabled,
                                "Applied immediately through the player's audio filter chain. Runs in low power mode too; more bands use a little more CPU."))
        {
            save = true;
            if (state.settings.eq_enabled && power::low_power())
                ui_notify(UiNotifyType::Info, "Equalizer",
                          "Low power mode is on; the equalizer still applies.");
        }
        if (state.settings.eq_enabled)
        {
            static int s_eq_selected = 0;
            const char *layouts[] = {"5 bands", "10 bands", "15 bands", "31 bands", "Custom"};
            int layout = std::max(0, std::min(4, state.settings.eq_layout));
            settings_row_label("Bands", layouts[layout],
                               "Preset layouts set the centre frequencies; Custom lets you edit every band.");
            ImGui::SetNextItemWidth(field_w);
            if (ImGui::Combo("##eq_layout", &layout, layouts, 5))
            {
                eq_rebuild_layout(state.settings, layout);
                if (state.settings.eq_preset != 5)
                    eq_apply_preset(state.settings, state.settings.eq_preset);
                s_eq_selected = 0;
                save = true;
            }

            const char *presets[] = {"Flat", "Bass", "Vocal", "Rock", "Electronic", "Custom"};
            int preset = std::max(0, std::min(5, state.settings.eq_preset));
            settings_row_label("Preset", presets[preset],
                               "Quick gain curves. Moving a fader switches this to Custom.");
            ImGui::SetNextItemWidth(field_w);
            if (ImGui::Combo("##eq_preset", &preset, presets, 6))
            {
                eq_apply_preset(state.settings, preset);
                save = true;
            }

            const float fader_col = 46.0f * m.layout_scale;
            const float fader_h = 170.0f * m.layout_scale;
            const float strip_h = fader_h + 52.0f * m.layout_scale;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            if (ImGui::BeginChild("##eq_bands", ImVec2(0, strip_h), false,
                                  ImGuiWindowFlags_HorizontalScrollbar))
            {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                for (int i = 0; i < (int)state.settings.eq_bands.size(); ++i)
                {
                    ImGui::PushID(i);
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##fader", ImVec2(fader_col, fader_h),
                                           ImGuiButtonFlags_MouseButtonLeft);
                    const bool active = ImGui::IsItemActive();
                    const bool hovered = ImGui::IsItemHovered();
                    if (active)
                    {
                        const float t = std::clamp(
                            (p.y + fader_h - ImGui::GetIO().MousePos.y) / fader_h, 0.0f, 1.0f);
                        state.settings.eq_bands[i].gain = -15.0f + t * 30.0f;
                        state.settings.eq_preset = 5;
                    }
                    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        state.settings.eq_bands[i].gain = 0.0f;
                        state.settings.eq_preset = 5;
                        save = true;
                    }
                    if (ImGui::IsItemDeactivated())
                        save = true;
                    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        s_eq_selected = i;

                    const float cx = p.x + fader_col * 0.5f;
                    const float top = p.y + 24.0f * m.layout_scale;
                    const float bot = p.y + fader_h - 20.0f * m.layout_scale;
                    const float t = (state.settings.eq_bands[i].gain + 15.0f) / 30.0f;
                    const float ky = bot - t * (bot - top);
                    dl->AddRectFilled(ImVec2(cx - 2.0f, top), ImVec2(cx + 2.0f, bot),
                                      ui_overlay_u32(0.10f), 2.0f);
                    dl->AddRectFilled(ImVec2(cx - 2.0f, ky), ImVec2(cx + 2.0f, bot),
                                      ui_accent_u32(0.70f), 2.0f);
                    dl->AddCircleFilled(ImVec2(cx, ky), 5.0f * m.layout_scale,
                                        ui_text_u32(), 16);

                    char val[16];
                    std::snprintf(val, sizeof(val), "%+.1f", state.settings.eq_bands[i].gain);
                    const ImVec2 vs = ImGui::CalcTextSize(val);
                    dl->AddText(ImVec2(cx - vs.x * 0.5f, top - 17.0f * m.layout_scale),
                                ui_text_muted_u32(), val);
                    const std::string label = eq_freq_label(state.settings.eq_bands[i].freq);
                    const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
                    dl->AddText(ImVec2(cx - ls.x * 0.5f, bot + 6.0f * m.layout_scale),
                                ui_text_dim_u32(), label.c_str());
                    if (hovered)
                        ImGui::SetTooltip("%.1f Hz  %+.1f dB  Q %.2f", state.settings.eq_bands[i].freq,
                                          state.settings.eq_bands[i].gain, state.settings.eq_bands[i].q);
                    if (state.settings.eq_layout == 4 && s_eq_selected == i)
                        dl->AddRect(ImVec2(p.x + 2.0f, top - 4.0f),
                                    ImVec2(p.x + fader_col - 2.0f, bot + 4.0f),
                                    ui_accent_u32(0.45f), m.radius_sm, 0, 1.0f);
                    ImGui::PopID();
                    ImGui::SameLine(0.0f, m.space_1);
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();

            if (state.settings.eq_layout == 4)
            {
                settings_group_label("Selected band (custom)");
                s_eq_selected = std::max(0, std::min((int)state.settings.eq_bands.size() - 1,
                                                     s_eq_selected));
                if (state.settings.eq_bands.empty())
                {
                    if (ui_ghost_button("Add band", ImVec2(160.0f * m.layout_scale, m.control_h)))
                    {
                        eq_add_band(state.settings);
                        save = true;
                    }
                }
                else
                {
                    EqBand &band = state.settings.eq_bands[s_eq_selected];
                    ImGui::SetNextItemWidth(field_w);
                    float freq = band.freq;
                    if (ImGui::InputFloat("Frequency (Hz)##eq_f", &freq, 0.0f, 0.0f, "%.1f"))
                    {
                        band.freq = std::max(20.0f, std::min(20000.0f, freq));
                        state.settings.eq_preset = 5;
                        save = true;
                    }
                    ImGui::SetNextItemWidth(field_w);
                    float q = band.q;
                    if (ImGui::InputFloat("Q##eq_q", &q, 0.0f, 0.0f, "%.2f"))
                    {
                        band.q = std::max(0.1f, std::min(10.0f, q));
                        save = true;
                    }
                    if (ui_ghost_button("Remove band", ImVec2(160.0f * m.layout_scale, m.control_h)))
                    {
                        state.settings.eq_bands.erase(state.settings.eq_bands.begin() + s_eq_selected);
                        s_eq_selected = std::max(0, s_eq_selected - 1);
                        state.settings.eq_preset = 5;
                        save = true;
                    }
                    ImGui::SameLine(0.0f, m.space_2);
                    if (ui_ghost_button("Add band", ImVec2(160.0f * m.layout_scale, m.control_h)))
                    {
                        eq_add_band(state.settings);
                        state.settings.eq_preset = 5;
                        save = true;
                    }
                }
            }
            if (settings_button_row("Reset equalizer", "Put every band back to 0 dB.",
                                    "Reset bands", false, 160.0f))
            {
                for (EqBand &b : state.settings.eq_bands)
                    b.gain = 0.0f;
                state.settings.eq_preset = 0;
                save = true;
            }
            settings_help("The equalizer also runs in low power mode. Approximate CPU while playing: 5 bands <1%, 10 bands ~1%, 31 bands ~4% of one core.");
        }

        settings_group_label("Connection");
        if (settings_toggle_row("##server_sync", "Sync to YouTube Music",
                                &state.settings.server_sync,
                                "Mirror likes and playlist edits to your signed-in account. Each playlist gets a private server-side copy that follows renames, adds and removals."))
        {
            writeback_set_enabled(state.settings.server_sync);
            save = true;
        }
        settings_row_label("Discord Rich Presence",
                           state.settings.discord_client_id.empty() ? "Off" : "Configured",
                           "Paste an application client id from discord.com/developers. Restart crossfade after changing it.");
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputTextWithHint("##discord_id", "Application client id",
                                 &state.settings.discord_client_id);
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        if (settings_toggle_row("##notifications", "Desktop notifications",
                                &state.settings.show_notifications,
                                "Show a toast when the track changes."))
            save = true;
        if (settings_toggle_row("##endless", "Endless music",
                                &state.settings.endless_radio,
                                "When the list and queue run out, keep playing a radio seeded by the last song."))
            save = true;
    }
    if (s_settings_category == SC_PARTY)
    {
        SettingsCard card("Party", "Let guests add songs from their phones.", UiIcon::Party);
        const float party_section_y = ImGui::GetCursorPosY();
        settings_help("Start a party from the Party tab. Guests scan its QR code with a phone camera, paste a YouTube or YouTube Music link, and their songs join Up Next. Nothing leaves your local network - there is no account, no cloud, and no telemetry.");

        PartySettings &ps = state.settings.party;
        if (party_running())
            ImGui::TextColored(ui_c().accent_text,
                               "A party is running. Changes here apply the next time you start one.");

        settings_group_label("Room");
        settings_row_label("Party name", nullptr,
                           "Shown on the host screen and at the top of the guest page.");
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputText("##party_name", &ps.name);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            ps.name = party_util::clean_utf8(ps.name, 60);
            if (ps.name.empty())
                ps.name = "crossfade party";
            save = true;
        }

        std::vector<PartyInterfaceInfo> ifaces = party_interfaces();
        std::vector<std::string> labels;
        labels.push_back("Automatic (best network)");
        int current = 0;
        for (int i = 0; i < (int)ifaces.size(); ++i)
        {
            std::string label = ifaces[i].label;
            if (ifaces[i].is_preferred)
                label += "  (recommended)";
            if (ifaces[i].is_secondary)
                label += "  (alternate address)";
            if (!ifaces[i].is_private)
                label += "  (public address)";
            labels.push_back(label);
            if (!ps.interface_ip.empty() && ifaces[i].ip == ps.interface_ip)
                current = (int)labels.size() - 1;
        }
        if (!ps.interface_ip.empty() && current == 0)
        {
            labels.push_back("Saved: " + ps.interface_ip);
            current = (int)labels.size() - 1;
        }
        std::vector<const char *> ptrs;
        ptrs.reserve(labels.size());
        for (const std::string &l : labels)
            ptrs.push_back(l.c_str());
        settings_row_label("Network", nullptr,
                           "Pick the Wi-Fi or hotspot address phones can reach. Automatic skips Docker and VPN adapters and prefers the address your network routes.");
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::Combo("##party_iface", &current, ptrs.data(), (int)ptrs.size()))
        {
            if (current == 0)
                ps.interface_ip.clear();
            else if (current - 1 < (int)ifaces.size())
                ps.interface_ip = ifaces[current - 1].ip;
            else
                ps.interface_ip.clear();
            save = true;
        }
        if (!ifaces.empty())
            settings_help(("Automatic will use " + ifaces.front().ip + ".").c_str());

        settings_row_label("Guest page port", nullptr,
                           "If the port is busy, start the party again on another one.");
        ImGui::SetNextItemWidth(160.0f * m.layout_scale);
        ImGui::InputInt("##party_port", &ps.port, 0, 0);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            ps.port = std::max(1024, std::min(65535, ps.port));
            save = true;
        }
        settings_help("Hotspots and public Wi-Fi often isolate clients. If a phone cannot open the link, make this laptop the hotspot, use a normal router, or disable client isolation. On Android 16+ Chrome may also need Nearby devices permission (or try Firefox).");

        settings_group_label("Who can add songs");
        if (settings_toggle_row("##party_approval", "Host approves every request",
                                &ps.require_approval,
                                "New songs wait in the Party tab until you approve them. When off, they join Up Next immediately."))
            save = true;
        if (settings_slider_int_row("##party_max_guest", "Per guest",
                                    &ps.max_per_guest, 0, 20, "%d songs", nullptr,
                                    [](int v) { return v == 0 ? std::string("Unlimited")
                                                              : std::to_string(v) + " songs"; }))
            save = true;
        if (settings_slider_int_row("##party_cooldown", "Cooldown",
                                    &ps.cooldown_sec, 0, 60, "%d s", nullptr,
                                    [](int v) { return v == 0 ? std::string("Off")
                                                              : std::to_string(v) + " s between adds"; }))
            save = true;
        if (settings_slider_int_row("##party_max_minutes", "Max video length",
                                    &ps.max_video_minutes, 0, 60, "%d min", nullptr,
                                    [](int v) { return v == 0 ? std::string("Unlimited")
                                                              : std::to_string(v) + " min"; }))
            save = true;
        if (settings_slider_int_row("##party_max_queue", "Queue cap",
                                    &ps.max_queue, 0, 300, "%d songs", nullptr,
                                    [](int v) { return v == 0 ? std::string("Unlimited")
                                                              : std::to_string(v) + " songs"; }))
            save = true;
        settings_help("Limits apply per guest. Queue cap counts guest requests waiting in Up Next; your own playlist does not use it.");

        settings_group_label("Guest experience");
        if (settings_toggle_row("##party_search", "Let guests search YouTube",
                                &ps.allow_search,
                                "When off, the guest page becomes a paste-only field. Search uses the same local backend as the app."))
            save = true;
        if (settings_toggle_row("##party_dupes", "Allow duplicate songs",
                                &ps.allow_duplicates, nullptr))
            save = true;
        if (settings_toggle_row("##party_names", "Show guest names next to songs",
                                &ps.show_names, nullptr))
            save = true;

        if (state.settings_focus_party)
        {
            ImGui::SetScrollY(std::max(0.0f, party_section_y - m.space_4));
            state.settings_focus_party = false;
        }
    }
    if (s_settings_category == SC_LYRICS)
    {
        SettingsCard card("Lyrics", "Timing and display.", UiIcon::Lyrics);
        if (settings_slider_row("##lyrics_offset", "Timing offset",
                                &state.settings.lyrics_offset, -5.0f, 5.0f, "%+.2f s",
                                "Positive shows lyrics later, negative earlier. Takes effect immediately."))
            save = true;
    }
    if (s_settings_category == SC_YOUTUBE)
    {
        SettingsCard card("YouTube Music", "Account, cookies and stream resolving.", UiIcon::Playlist);

    const char *browsers[] = {"firefox", "chromium", "chrome", "brave", "edge", "librewolf"};
    int current_browser = 0;
    for (int i = 0; i < 6; ++i)
    {
        if (state.settings.ytdl_browser == browsers[i])
        {
            current_browser = i;
            break;
        }
    }
    settings_row_label("Cookie browser",
                       browsers[current_browser],
                       "Browser yt-dlp reads cookies from for private or age-restricted tracks.");
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::Combo("##browser", &current_browser, browsers, 6))
    {
        state.settings.ytdl_browser = browsers[current_browser];
        save = true;
    }
    settings_gap();
    const char *auth_modes[] = {"Browser cookies (browser.json)", "OAuth (browseroauth.json)"};
    int auth_mode = state.settings.auth_mode == "oauth" ? 1 : 0;
    settings_row_label("Sign-in method",
                       auth_modes[auth_mode],
                       "OAuth opens a browser once and saves a token. Browser mode uses the cookies your normal browser already has. Restart crossfade after changing this.");
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::Combo("##auth_mode", &auth_mode, auth_modes, 2))
    {
        state.settings.auth_mode = auth_mode == 1 ? "oauth" : "browser";
        set_auth_mode(state.settings.auth_mode);
        save = true;
    }
    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    if (ui_primary_button("Sign in with browser", ImVec2(210.0f * m.layout_scale, m.button_h)))
    {
        const std::string script = p100_share_dir() + "/scripts/crossfade-login-helper.py";
        AppState *sp = &state;
        tasks::spawn([sp, script]() {
            const std::string cmd = "python3 \"" + script + "\" --google";
            const int rc = std::system(cmd.c_str());
            if (rc == 0)
            {
                sp->auth_refresh_requested = true;
                ui_notify(UiNotifyType::Success, "Signed in",
                          "The YouTube Music session was updated.", 4.0f);
            }
            else
            {
                ui_notify(UiNotifyType::Error, "Sign-in failed",
                          "The browser login did not complete. See the terminal for details.", 7.0f);
            }
            sp->repaint_requested = true;
        });
    }
    ImGui::SameLine(0.0f, m.space_2);
    if (ui_ghost_button("Import Firefox cookies", ImVec2(210.0f * m.layout_scale, m.button_h)))
    {
        const std::string script = p100_share_dir() + "/scripts/crossfade-login-helper.py";
        AppState *sp = &state;
        tasks::spawn([sp, script]() {
            const std::string cmd = "python3 \"" + script + "\" --import-firefox";
            const int rc = std::system(cmd.c_str());
            if (rc == 0)
            {
                sp->auth_refresh_requested = true;
                ui_notify(UiNotifyType::Success, "Firefox session imported",
                          "The YouTube Music session was updated.", 4.0f);
            }
            else
            {
                ui_notify(UiNotifyType::Error, "Import failed",
                          "No signed-in Firefox YouTube Music session was found.", 7.0f);
            }
            sp->repaint_requested = true;
        });
    }
    settings_help("Sign in without copying request headers. The popup needs WebKitGTK; without it the helper falls back to your Firefox session. No OAuth client keys are involved.");
    if (state.settings.auth_mode == "oauth" && !std::filesystem::exists(backend_auth_file_path()))
        ImGui::TextColored(ui_c().accent_text, "No OAuth token yet - use the login command below.");

    settings_gap();
    if (settings_toggle_row("##warm_resolver", "Fast stream resolve (experimental)",
                            &state.settings.warm_stream_resolver,
                            "Resolve stream links inside the backend instead of spawning yt-dlp each time. Takes effect after a restart; turn it off if playback misbehaves."))
    {
        set_warm_stream_resolver(state.settings.warm_stream_resolver);
        save = true;
    }

    // Read-only snapshot: taking the error here would steal it from the
    // fetch workers and silently suppress their toasts. Workers clear the
    // error themselves on their next successful fetch.
    BackendError backend_err = backend_error_snapshot();
    if (backend_err)
        state.backend_status = backend_err;
    if (state.backend_status)
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.48f, 0.48f, 1.0f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextWrapped("%s", backend_error_user_message(state.backend_status).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        ImGui::TextWrapped("Detail: %s", state.backend_status.message.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
            settings_help("If this is an auth problem, refresh the auth file below, then restart crossfade.");
    }

    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    state.backend_running = backend_running();
    state.backend_auth_file = backend_auth_file_path();
    if (state.backend_running)
        ImGui::TextColored(ui_c().accent_text, "Backend: running");
    else
        ImGui::TextColored(ImVec4(0.95f, 0.48f, 0.48f, 1.0f), "Backend: not connected");

    ImGui::TextColored(ui_c().text_muted, "Auth file");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
    ImGui::TextUnformatted(state.backend_auth_file.c_str());
    ImGui::PopTextWrapPos();
    if (ui_ghost_button("Copy auth path", ImVec2(150.0f * m.layout_scale, m.control_h)))
        ImGui::SetClipboardText(state.backend_auth_file.c_str());
    ImGui::SameLine();
    if (ui_ghost_button("Copy folder", ImVec2(130.0f * m.layout_scale, m.control_h)))
        ImGui::SetClipboardText(
            std::filesystem::path(state.backend_auth_file).parent_path().string().c_str());

    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    ImGui::TextColored(ui_c().text_muted, "Re-authenticate");
    const std::string reauth = backend_reauth_command();
    char reauth_buf[1024];
    std::snprintf(reauth_buf, sizeof(reauth_buf), "%s", reauth.c_str());
    ImGui::SetNextItemWidth(content_w);
    ImGui::InputText("##reauth_cmd", reauth_buf, sizeof(reauth_buf), ImGuiInputTextFlags_ReadOnly);
    if (ui_ghost_button("Copy re-auth command", ImVec2(200.0f * m.layout_scale, m.control_h)))
        ImGui::SetClipboardText(reauth.c_str());
    settings_help("Paste this into a terminal, complete the browser login, then restart crossfade.");
    if (ImGui::TreeNode("Backend details"))
    {
        ImGui::TextWrapped("Python: %s", backend_python_path().c_str());
        ImGui::TextWrapped("Script: %s", backend_script_path().c_str());
        ImGui::TreePop();
    }

    }
    if (s_settings_category == SC_SHORTCUTS)
    {
        SettingsCard card("Shortcuts", "Every action, one keystroke.", UiIcon::More);
    ImGui::TextDisabled("Space  Play / pause          Left / Right  Seek 5s");
    ImGui::TextDisabled("Shift + arrows  Seek 20s    Ctrl + Left / Right  Previous / next");
    ImGui::TextDisabled("Up / Down  Volume            / or Ctrl+F  Focus search");
    ImGui::TextDisabled("L  Toggle lyrics            S  Toggle sidebar");
    ImGui::TextDisabled("Ctrl + ,  Open settings");
    ImGui::TextDisabled("Esc  Back / clear search      ?  Shortcut help");

    }
    if (s_settings_category == SC_LIBRARY)
    {
        SettingsCard card("Library", "Caches, downloads and local data.", UiIcon::Download);

    if (s_repair_running)
    {
        ImGui::Text("Repairing %d / %d  ·  %d covers  ·  %d lyrics",
                    s_repair_done.load(), s_repair_total.load(),
                    s_repair_covers.load(), s_repair_lyrics.load());
    }
    else
    {
        if (ui_secondary_button("Repair cached songs", ImVec2(200.0f * m.layout_scale, m.button_h)))
        {
            std::vector<Track> snapshot = state.downloaded_tracks;
            if (snapshot.empty())
                snapshot = get_cached_tracks();
            if (!snapshot.empty() && !s_repair_running.exchange(true))
            {
                s_repair_finished = false;
                s_repair_done = 0;
                s_repair_total = (int)snapshot.size();
                s_repair_covers = 0;
                s_repair_lyrics = 0;
                tasks::spawn([&state, snapshot = std::move(snapshot)]() mutable {
                    repair_cache_worker(&state, std::move(snapshot));
                });
            }
        }
        settings_help("Re-download missing album covers and lyrics for downloaded songs.");
        if (s_repair_finished)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_c().accent_text);
            ImGui::Text("Last repair: %d covers, %d lyrics fetched.", s_repair_covers.load(), s_repair_lyrics.load());
            ImGui::PopStyleColor();
        }
    }

    settings_gap();
    ui_section_header("Download Cache");

    // Disk usage: recompute at most every 5 s while the panel is open.
    static double s_usage_bytes = 0.0;
    static double s_usage_at = -100.0;
    const double now_s = ImGui::GetTime();
    if (now_s - s_usage_at > 5.0)
    {
        s_usage_bytes = audio_cache_usage_bytes();
        s_usage_at = now_s;
    }
    char usage_buf[64];
    if (s_usage_bytes >= 1024.0 * 1024.0 * 1024.0)
        snprintf(usage_buf, sizeof(usage_buf), "%.2f GB on disk",
                 s_usage_bytes / (1024.0 * 1024.0 * 1024.0));
    else
        snprintf(usage_buf, sizeof(usage_buf), "%.1f MB on disk",
                 s_usage_bytes / (1024.0 * 1024.0));
    ImGui::TextDisabled("%s", usage_buf);
    ImGui::SameLine();
    if (ui_ghost_button("Clear cache", ImVec2(130.0f * m.layout_scale, m.control_h)))
    {
        audio_cache_clear(state.current_track_id);
        s_usage_at = -100.0; // refresh the usage readout next frame
    }
    settings_help("Removes all downloaded audio (except the song currently playing). Songs re-download on next play.");

    // The slider drives a local value, so mirror it into settings while the
    // user drags; only persist on release.
    float cap_gb = state.settings.download_cap_gb;
    const bool cap_released = settings_slider_row(
        "##dl_cap", "Download cache cap", &cap_gb, 0.0f, 20.0f, "%.0f GB",
        "Keep at most this much downloaded audio. At zero the cache is unlimited; when a cap is reached, the least-listened songs are removed first.",
        [](float v) { return v < 0.5f ? std::string("Unlimited")
                                            : std::to_string((int)std::lround(v)) + " GB"; });
    const float snapped = cap_gb < 0.5f ? 0.0f : std::floor(cap_gb + 0.5f);
    if (std::fabs(snapped - state.settings.download_cap_gb) > 0.01f)
    {
        state.settings.download_cap_gb = snapped;
        set_audio_cache_cap((double)snapped * 1024.0 * 1024.0 * 1024.0);
    }
    if (cap_released)
        save = true;
    if (state.settings.download_cap_gb < 0.5f)
        settings_help("Currently unlimited.");
    else
        settings_help(("Currently " + std::to_string((int)state.settings.download_cap_gb) + " GB. Songs re-download if you raise the cap.").c_str());

    }
    if (s_settings_category == SC_POWER)
    {
        SettingsCard card("Power", "Spend less CPU, GPU and network while music plays.", UiIcon::Moon);

        if (settings_toggle_row("##low_power", "Low power mode",
                                &state.settings.low_power,
                                "Caps the UI at a few frames per second, turns off motion and the ambient backdrop, and stops speculative stream warming and background downloads. Playback is unaffected."))
        {
            save = true;
            if (state.settings.low_power && state.settings.eq_enabled)
                ui_notify(UiNotifyType::Info, "Low power mode",
                          "The equalizer stays active; low power does not disable it.");
        }
        if (settings_toggle_row("##low_power_battery", "Enable on battery",
                                &state.settings.low_power_on_battery,
                                "Turn low power mode on automatically when the system reports it is running on battery."))
            save = true;

        const char *state_label = power::low_power()
                                      ? (power::on_battery() ? "On (battery)" : "On")
                                      : "Off";
        settings_row_label("Currently", state_label,
                           power::env_forced()
                               ? "P100_LOW_POWER is set, so low power mode is forced on."
                               : nullptr);
    }
    if (s_settings_category == SC_WARMSTART)
    {
        SettingsCard card("Warm start", "Keep a background instance so relaunching is instant.",
                          UiIcon::Repeat);

        if (settings_toggle_row("##warm_start", "Warm start",
                                &state.settings.warm_start,
                                "Reuse one background instance instead of booting a new process. Start it at login with `crossfade --warm`; running `crossfade` later just shows the existing window."))
            save = true;

        if (state.settings.warm_start)
        {
            const char *modes[] = {"Everything (UI + all backends)",
                                   "Main backend only",
                                   "UI only (backends start when shown)"};
            int mode = std::max(0, std::min(2, state.settings.warm_start_mode));
            settings_row_label("Keep warm", modes[mode],
                               "What stays resident while the window is hidden. Everything uses the most memory; UI only parks the Python backends.");
            ImGui::SetNextItemWidth(field_w);
            if (ImGui::Combo("##warm_mode", &mode, modes, 3))
            {
                state.settings.warm_start_mode = mode;
                save = true;
            }
            if (settings_toggle_row("##warm_close", "Closing the window keeps running",
                                    &state.settings.warm_close_hides,
                                    "Hide instead of quitting when the window is closed. Use `crossfade --quit` to stop the background instance."))
                save = true;
            settings_help("xinitrc: `crossfade --warm &`. Later: `crossfade` shows it, `crossfade --toggle` hides/shows, `crossfade --quit` exits.");
        }
    }
    if (s_settings_category == SC_DATA)
    {
        SettingsCard card("Data transfer", "One ZIP for backup or moving to another machine.",
                          UiIcon::Download);
        settings_help("Plain ZIP files; works between Linux, Windows and macOS. The library is snapshotted consistently before export.");

        if (s_xfer_export_path.empty())
            s_xfer_export_path = data_transfer::default_export_path();
        if (s_xfer_import_path.empty())
            s_xfer_import_path = data_transfer::default_import_path();

        settings_group_label("Include");
        settings_toggle_row("##xf_settings", "Settings & session",
                            &s_xfer_sections.settings,
                            "settings.json, session.json and the ImGui layout.");
        settings_toggle_row("##xf_auth", "Sign-in & cookies",
                            &s_xfer_sections.auth,
                            "browser.json, browseroauth.json and the Spotify cookie. Cookies may have expired by the time you restore.");
        settings_toggle_row("##xf_library", "Library",
                            &s_xfer_sections.library,
                            "SQLite library, likes, history, playlists, stats, queue and recommendations cache.");
        settings_toggle_row("##xf_album", "Album cache", &s_xfer_sections.album_cache,
                            "Cached album pages for instant/offline reopening.");
        settings_toggle_row("##xf_artist", "Artist cache", &s_xfer_sections.artist_cache,
                            "Cached artist pages for instant/offline reopening.");
        settings_toggle_row("##xf_lyrics", "Lyrics", &s_xfer_sections.lyrics,
                            "Downloaded per-track lyrics.");
        settings_toggle_row("##xf_thumbs", "Thumbnail cache", &s_xfer_sections.thumbs,
                            "Cover artwork cache (can be several hundred MB).");
        settings_toggle_row("##xf_audio", "Downloaded audio", &s_xfer_sections.audio,
                            "Full audio cache. This can make the ZIP many GB; it is stored uncompressed for speed.");
        settings_toggle_row("##xf_logs", "Logs & crash reports", &s_xfer_sections.logs,
                            "backend.log, crash logs and saved diagnostics bundles.");

        settings_group_label("Export");
        settings_row_label("Destination ZIP", "", "The archive is created on a background worker; the UI stays responsive.");
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputText("##xf_export_path", &s_xfer_export_path);
        if (ui_ghost_button("Export ZIP", ImVec2(160.0f * m.layout_scale, m.control_h)))
        {
            std::string err;
            if (!data_transfer::export_async(s_xfer_export_path, s_xfer_sections, err))
                ui_notify(UiNotifyType::Error, "Export", err, 7.0f);
        }
        if (data_transfer::export_running())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Exporting...");
        }
        settings_help("Default location is the crossfade config folder. Copy the ZIP anywhere; it is portable.");

        settings_group_label("Import");
        settings_row_label("Backup ZIP", "", "The selected sections in the archive replace the current ones. A safety copy of replaced data is kept next to the config folder.");
        ImGui::SetNextItemWidth(field_w);
        ImGui::InputText("##xf_import_path", &s_xfer_import_path);
        if (ui_ghost_button("Browse...", ImVec2(120.0f * m.layout_scale, m.control_h)))
        {
            s_xfer_pick_dir = p100_config_dir();
            s_xfer_pick_open = true;
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Import and close", ImVec2(190.0f * m.layout_scale, m.control_h)))
            s_xfer_confirm_open = true;
        settings_help("Import runs after crossfade closes, so open files can be replaced safely. Relaunch when it finishes; the next launch reports the result.");
    }

    if (s_xfer_pick_open)
    {
        ImGui::OpenPopup("Choose backup ZIP");
        s_xfer_pick_open = false;
    }
    if (ImGui::BeginPopupModal("Choose backup ZIP", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::SetNextItemWidth(700.0f * m.layout_scale);
        ImGui::InputText("##xf_pick_dir", &s_xfer_pick_dir);
        std::vector<std::filesystem::directory_entry> dirs, zips;
        std::error_code ec;
        for (std::filesystem::directory_iterator it(s_xfer_pick_dir, ec), end;
             !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.')
                continue;
            std::error_code tc;
            if (it->is_directory(tc))
                dirs.push_back(*it);
            else if (it->path().extension() == ".zip")
                zips.push_back(*it);
        }
        auto by_name = [](const auto &a, const auto &b) {
            return a.path().filename().string() < b.path().filename().string();
        };
        std::sort(dirs.begin(), dirs.end(), by_name);
        std::sort(zips.begin(), zips.end(), by_name);
        ImGui::BeginChild("##xf_pick_list", ImVec2(700.0f * m.layout_scale,
                                                   320.0f * m.layout_scale),
                          true);
        if (ui_ghost_button("..", ImVec2(60.0f * m.layout_scale, m.control_h)))
        {
            const std::filesystem::path parent = std::filesystem::path(s_xfer_pick_dir).parent_path();
            if (!parent.empty())
                s_xfer_pick_dir = parent.string();
        }
        for (const auto &d : dirs)
        {
            const std::string name = d.path().filename().string();
            if (ui_ghost_button(("[dir] " + name).c_str(),
                                ImVec2(660.0f * m.layout_scale, m.control_h)))
                s_xfer_pick_dir = d.path().string();
        }
        for (const auto &z : zips)
        {
            const std::string name = z.path().filename().string();
            if (ui_ghost_button(name.c_str(), ImVec2(660.0f * m.layout_scale, m.control_h)))
            {
                s_xfer_import_path = z.path().string();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();
        if (ui_ghost_button("Close", ImVec2(120.0f * m.layout_scale, m.control_h)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (s_xfer_confirm_open)
    {
        ImGui::OpenPopup("Import backup");
        s_xfer_confirm_open = false;
    }
    if (ImGui::BeginPopupModal("Import backup", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushTextWrapPos(520.0f * m.layout_scale);
        ImGui::TextWrapped(
            "The selected sections in this ZIP replace the current data. "
            "crossfade will close, then the import runs in the background and "
            "keeps a safety copy of replaced files next to the config folder. "
            "Relaunch crossfade when it finishes.");
        ImGui::PopTextWrapPos();
        ImGui::Text("Archive: %s", s_xfer_import_path.c_str());
        if (ui_ghost_button("Import and close", ImVec2(190.0f * m.layout_scale, m.control_h)))
        {
            std::string err;
            if (data_transfer::start_import(s_xfer_import_path, err))
            {
                state.status_text = "Import pending; closing...";
                state.quit_requested = true;
                ImGui::CloseCurrentPopup();
            }
            else
            {
                ui_notify(UiNotifyType::Error, "Import", err, 7.0f);
            }
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Cancel", ImVec2(120.0f * m.layout_scale, m.control_h)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (s_settings_category == SC_DIAGNOSTICS)
    {
        SettingsCard card("Diagnostics", "Local-only troubleshooting.", UiIcon::Stats);
    settings_help("Local-only troubleshooting. Nothing here is uploaded automatically.");
    {
        // SQLite status runs several COUNT queries; refresh it at most twice
        // a second instead of once per rendered frame.
        static std::string s_library_status;
        static double s_library_status_at = -1.0e9;
        const double now = ImGui::GetTime();
        if (now - s_library_status_at > 0.5)
        {
            s_library_status = library::status_text();
            s_library_status_at = now;
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
        ImGui::TextUnformatted(s_library_status.c_str());
        ImGui::PopTextWrapPos();
        if (!library::backup_dir().empty())
            settings_help(("Legacy JSON backup: " + library::backup_dir()).c_str());

        if (settings_toggle_row("##diag_history", "Detailed diagnostics history",
                                &state.settings.diagnostics_history,
                                "Keep recent log lines and background-task timings for the report. Passive: a small in-memory ring plus one record per task, no polling or per-frame work."))
        {
            diagnostics::set_history_enabled(state.settings.diagnostics_history);
            tasks::set_history_enabled(state.settings.diagnostics_history);
            save = true;
        }

        const float bw = std::min(176.0f * m.layout_scale,
                                  (content_w - m.space_2 * 2.0f) / 3.0f);
        if (ui_ghost_button("Full report", ImVec2(bw, m.control_h)))
        {
            s_diag_report = diagnostics::bundle();
            s_diag_open = true;
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Save report", ImVec2(bw, m.control_h)))
        {
            std::string path, err;
            if (diagnostics::save_bundle(path, err))
                ui_notify(UiNotifyType::Success, "Diagnostics saved", path, 6.0f);
            else
                ui_notify(UiNotifyType::Error, "Could not save diagnostics", err, 7.0f);
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Copy report", ImVec2(bw, m.control_h)))
        {
            ImGui::SetClipboardText(diagnostics::bundle().c_str());
            ui_notify(UiNotifyType::Success, "Diagnostics copied",
                      "The full bundle is on the clipboard.", 4.0f);
        }
        if (ui_ghost_button("Check library", ImVec2(bw, m.control_h)))
        {
            std::string result;
            const bool ok = library::integrity_check(result);
            ui_notify(ok ? UiNotifyType::Success : UiNotifyType::Error,
                      ok ? "Library is healthy" : "Library check failed",
                      result.empty() ? "No details returned." : result, ok ? 4.0f : 8.0f);
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Back up library", ImVec2(bw, m.control_h)))
        {
            char stamp[32];
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
            const std::string path =
                p100_config_path(std::string("library-backup-") + stamp + ".db");
            std::string err;
            if (library::backup_to(path, err))
                ui_notify(UiNotifyType::Success, "Library backed up", path, 5.0f);
            else
                ui_notify(UiNotifyType::Error, "Backup failed", err, 7.0f);
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Open config folder", ImVec2(bw, m.control_h)))
            diagnostics::open_folder(p100_config_dir());

        if (settings_toggle_row("##safe_mode", "Safe mode on next launch",
                                &state.settings.safe_mode,
                                "Disables the backdrop shader, auto-update checks and optional background integrations. Use it if a new build crashes or renders badly."))
            save = true;

        if (ImGui::TreeNode("Recent diagnostics"))
        {
            const std::string log = diagnostics::recent_log();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
            ImGui::TextUnformatted(log.empty() ? "No log lines yet." : log.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TreePop();
        }
    }

    }

    if (s_diag_open)
    {
        ImGui::OpenPopup("Full diagnostics##diag");
        s_diag_open = false;
    }
    if (ImGui::BeginPopupModal("Full diagnostics##diag", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        const float w = std::min(1000.0f * m.layout_scale,
                                 ImGui::GetIO().DisplaySize.x - 80.0f * m.layout_scale);
        ImGui::InputTextMultiline("##diag_text", &s_diag_report,
                                  ImVec2(w, 520.0f * m.layout_scale),
                                  ImGuiInputTextFlags_ReadOnly);
        const float bw2 = 150.0f * m.layout_scale;
        if (ui_ghost_button("Copy", ImVec2(bw2, m.control_h)))
        {
            ImGui::SetClipboardText(s_diag_report.c_str());
            ui_notify(UiNotifyType::Success, "Diagnostics copied",
                      "The full report is on the clipboard.", 4.0f);
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Save as file", ImVec2(bw2, m.control_h)))
        {
            std::string path, err;
            if (diagnostics::save_bundle(path, err))
                ui_notify(UiNotifyType::Success, "Diagnostics saved", path, 6.0f);
            else
                ui_notify(UiNotifyType::Error, "Could not save diagnostics", err, 7.0f);
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Close", ImVec2(bw2, m.control_h)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (s_settings_category == SC_NOTES)
    {
        SettingsCard card("Notes", "Anything you want to remember.", UiIcon::Settings);
    ImGui::InputTextMultiline("##notes", &state.settings.notes,
                              ImVec2(content_w, ImGui::GetTextLineHeight() * 6),
                              ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_WordWrap);
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    settings_help("Kept in settings.json and saved when you leave the box.");

    }
    if (s_settings_category == SC_DANGER)
    {
        SettingsCard card("Danger Zone", "These actions remove local data. Most offer an Undo toast right after they run.", UiIcon::Close);

    const float danger_w = std::max(220.0f * m.layout_scale,
                                    ImGui::CalcTextSize("Clear download cache").x +
                                        ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f);
    auto danger_row = [&](const char *label, const char *desc) -> bool
    {
        const bool clicked = ui_secondary_button(label, ImVec2(danger_w, m.button_h));
        ImGui::SameLine();
        settings_help(desc);
        ImGui::Dummy(ImVec2(0.0f, m.space_1));
        return clicked;
    };

    if (danger_row("Clear download cache", "Delete every downloaded song from disk."))
    {
        std::string dir = playlists_dir() + "/../audio_cache";
        cancel_pending_metadata(state.current_track_id);
        with_metadata_writes_blocked([&]()
                                     {
            std::filesystem::remove_all(dir);
            state.downloaded_tracks.clear();
            state.downloaded_loaded = false;
            load_downloaded_tracks(state); });
    }
    if (danger_row("Reset liked songs", "Remove all liked songs."))
    {
        begin_reset(state, ResetKind::Likes);
        g_reset_snapshot.likes = state.liked_tracks;
        state.liked_tracks.clear();
        save_liked_tracks(state.liked_tracks);
        ui_notify(UiNotifyType::Warning, "Liked songs reset", "All liked songs were removed.",
                  6.0f, "Undo", &undo_reset);
    }
    if (danger_row("Reset saved albums", "Remove all saved albums."))
    {
        begin_reset(state, ResetKind::Albums);
        g_reset_snapshot.albums = state.saved_albums;
        state.saved_albums.clear();
        save_saved_albums(state.saved_albums);
        ui_notify(UiNotifyType::Warning, "Saved albums reset", "All saved albums were removed.",
                  6.0f, "Undo", &undo_reset);
    }
    if (danger_row("Reset saved artists", "Remove all saved artists."))
    {
        begin_reset(state, ResetKind::Artists);
        g_reset_snapshot.artists = state.saved_artists;
        state.saved_artists.clear();
        save_saved_artists(state.saved_artists);
        ui_notify(UiNotifyType::Warning, "Saved artists reset", "All saved artists were removed.",
                  6.0f, "Undo", &undo_reset);
    }
    if (danger_row("Reset playlists", "Delete all playlists."))
    {
        begin_reset(state, ResetKind::Playlists);
        g_reset_snapshot.playlists = state.playlists;
        const auto names = get_playlist_names();
        for (const auto &name : names)
            delete_playlist(name);
        // Also clear the fallback JSON files if the DB is unavailable.
        std::string dir = playlists_dir();
        if (std::filesystem::exists(dir))
        {
            for (const auto &entry : std::filesystem::directory_iterator(dir))
                std::filesystem::remove(entry.path());
        }
        state.playlists.clear();
        state.selected_playlist_index = -1;
        ui_notify(UiNotifyType::Warning, "Playlists reset", "All playlists were removed.",
                  6.0f, "Undo", &undo_reset);
    }
    ImGui::Dummy(ImVec2(0.0f, m.space_5));
    }

    ImGui::EndChild();

    if (save)
        save_settings(state.settings);
}
