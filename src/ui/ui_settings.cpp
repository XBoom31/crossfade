#include "ui.h"
#include "json_util.h"
#include "tasks.h"
#include <cstdlib>
#include "party_tunnel.h"
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
#include "tunables.h"
#include "shortcuts.h"
#include "data_transfer.h"
#include "power.h"
#include "updater.h"
#include "misc/cpp/imgui_stdlib.h"
#include <fstream>
#include <filesystem>
#include <cctype>
#include <cstring>
#include <map>
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
#include <chrono>

#ifdef P100_WINDOWS
#include "win_process.h"
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif


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
        {"list_title", "Song title size"},
        {"list_meta", "Artist & detail size"},
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
        << "\"video_quality\":" << std::max(0, std::min(2, settings.video_quality)) << ","
        << "\"volume_normalization\":" << (settings.volume_normalization ? "true" : "false") << ","
        << "\"show_notifications\":" << (settings.show_notifications ? "true" : "false") << ","
        << "\"endless_radio\":" << (settings.endless_radio ? "true" : "false") << ","
        << "\"ytdl_browser\":\"" << settings.ytdl_browser << "\","
        << "\"auth_mode\":\"" << (settings.auth_mode == "oauth" ? "oauth" : "browser") << "\","
        << "\"lyrics_offset\":" << settings.lyrics_offset << ","
        << "\"lyrics_effect_style\":"
        << std::max(0, std::min(2, settings.lyrics_effect_style)) << ","
        << "\"lyrics_panel_open\":" << (settings.lyrics_panel_open ? "true" : "false") << ","
        << "\"lyrics_panel_width\":" << settings.lyrics_panel_width << ","
        << "\"ui_scale\":" << settings.ui_scale << ","
        << "\"ui_text_scale\":" << settings.ui_text_scale << ","
        << "\"ui_widget_scale\":" << settings.ui_widget_scale << ","
        << "\"ui_layout_scale\":" << settings.ui_layout_scale << ","
        << "\"animations\":" << std::max(0, std::min(3, settings.animations)) << ","
        << "\"max_refresh_hz\":" << settings.max_refresh_hz << ","
        << "\"shuffle_enabled\":" << (settings.shuffle_enabled ? "true" : "false") << ","
        << "\"repeat_mode\":" << std::max(0, std::min(2, settings.repeat_mode)) << ","
        << "\"day_mode\":" << (settings.day_mode ? "true" : "false") << ","
        << "\"accent_color\":\"" << json_escape(settings.accent_color) << "\","
        << "\"album_backdrop\":" << (settings.album_backdrop ? "true" : "false") << ","
        << "\"album_backdrop_strength\":" << settings.album_backdrop_strength << ","
        << "\"edge_fade_height\":" << settings.edge_fade_height << ","
        << "\"hero_backdrop\":" << settings.hero_backdrop << ","
        << "\"hero_backdrop_strength\":" << settings.hero_backdrop_strength << ","
        << "\"backdrop_shader\":" << (settings.backdrop_shader ? "true" : "false") << ","
        << "\"warm_start\":" << (settings.warm_start ? "true" : "false") << ","
        << "\"warm_start_mode\":" << settings.warm_start_mode << ","
        << "\"warm_close_hides\":" << (settings.warm_close_hides ? "true" : "false") << ","
        << "\"diagnostics_history\":" << (settings.diagnostics_history ? "true" : "false") << ","
        << "\"safe_mode\":" << (settings.safe_mode ? "true" : "false") << ","
        << "\"check_for_updates\":" << (settings.check_for_updates ? "true" : "false") << ","
        << "\"low_power\":" << (settings.low_power ? "true" : "false") << ","
        << "\"low_power_on_battery\":" << (settings.low_power_on_battery ? "true" : "false") << ","
        << "\"low_memory_mode\":" << (settings.low_memory_mode ? "true" : "false") << ","
        << "\"glow_separators\":" << (settings.glow_separators ? "true" : "false") << ","
        << "\"glow_separator_strength\":" << settings.glow_separator_strength << ","
        << "\"backdrop_drift\":" << (settings.backdrop_drift ? "true" : "false") << ","
        << "\"download_cap_gb\":" << settings.download_cap_gb
        << ",\"warm_stream_resolver\":" << (settings.warm_stream_resolver ? "true" : "false")
        << ",\"aggressive_prefetch\":" << (settings.aggressive_prefetch ? "true" : "false")
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
        << ",\"rq_enabled\":" << (settings.room_eq_enabled ? "true" : "false")
        << ",\"rq_preamp\":" << settings.room_eq_preamp
        << ",\"rq_bass\":" << settings.room_eq_bass_db
        << ",\"rq_treble\":" << settings.room_eq_treble_db
        << ",\"rq_max_bands\":" << std::max(6, std::min(32, settings.room_eq_max_bands))
        << ",\"rq_mic\":\"" << json_escape(settings.room_eq_mic) << "\""
        << ",\"rq_target2\":\"" << json_escape(settings.room_eq_target) << "\""
        << ",\"rq_strength\":" << settings.room_eq_strength
        << ",\"rq_label\":\"" << json_escape(settings.room_eq_label) << "\""
        << ",\"rq_residual\":" << settings.room_eq_residual_db
        << ",\"rq_room_active\":\"" << json_escape(settings.room_eq_active_id) << "\""
        << ",\"rq_auto_room\":" << (settings.room_eq_auto_room ? "true" : "false") << ",\"rq_freq\":[";
    for (size_t i = 0; i < settings.room_eq_bands.size(); ++i)
        out << (i ? "," : "") << settings.room_eq_bands[i].freq;
    out << "],\"rq_gain\":[";
    for (size_t i = 0; i < settings.room_eq_bands.size(); ++i)
        out << (i ? "," : "") << settings.room_eq_bands[i].gain;
    out << "],\"rq_q\":[";
    for (size_t i = 0; i < settings.room_eq_bands.size(); ++i)
        out << (i ? "," : "") << settings.room_eq_bands[i].q;
    out << "]";
    {
        auto write_floats = [&out](const char *key, const std::vector<float> &v) {
            out << ",\"" << key << "\":[";
            for (size_t i = 0; i < v.size(); ++i)
                out << (i ? "," : "") << v[i];
            out << "]";
        };
        const RoomEqCurve &c = settings.room_eq_curve;
        write_floats("rq_curve_hz", c.hz);
        write_floats("rq_curve_measured", c.measured_db);
        write_floats("rq_curve_target", c.target_db);
        write_floats("rq_curve_corrected", c.corrected_db);
        out << ",\"rq_curve_valid\":[";
        for (size_t i = 0; i < c.valid.size(); ++i)
            out << (i ? "," : "") << (c.valid[i] ? 1 : 0);
        out << "]"
            << ",\"rq_curve_mic\":\"" << json_escape(c.mic) << "\""
            << ",\"rq_curve_before\":" << c.residual_before_db
            << ",\"rq_curve_after\":" << c.residual_after_db
            << ",\"rq_curve_lo\":" << c.correct_lo_hz
            << ",\"rq_curve_hi\":" << c.correct_hi_hz
            << ",\"rq_curve_content_hi\":" << c.content_hi_hz
            << ",\"rq_curve_max_bands\":" << c.max_bands
            << ",\"rq_curve_rate\":" << c.sample_rate;
    }
    out << ",\"discord_client_id\":\"" << settings.discord_client_id << "\""
        << ",\"server_sync\":" << (settings.server_sync ? "true" : "false")
        << ",\"notes\":\"" << json_escape(settings.notes) << "\""
        << ",\"party\":{"
        << "\"name\":\"" << json_escape(settings.party.name) << "\""
        << ",\"port\":" << std::max(1024, std::min(65535, settings.party.port))
        << ",\"interface_ip\":\"" << json_escape(settings.party.interface_ip) << "\""
        << ",\"reach\":\"" << (settings.party.public_reach() ? "public" : "lan") << "\""
        << ",\"tunnel_path\":\"" << json_escape(settings.party.tunnel_path) << "\""
        << ",\"require_approval\":" << (settings.party.require_approval ? "true" : "false")
        << ",\"max_per_guest\":" << std::max(0, std::min(20, settings.party.max_per_guest))
        << ",\"cooldown_sec\":" << std::max(0, std::min(60, settings.party.cooldown_sec))
        << ",\"max_video_minutes\":" << std::max(0, std::min(60, settings.party.max_video_minutes))
        << ",\"max_queue\":" << std::max(0, std::min(300, settings.party.max_queue))
        << ",\"allow_search\":" << (settings.party.allow_search ? "true" : "false")
        << ",\"allow_duplicates\":" << (settings.party.allow_duplicates ? "true" : "false")
        << ",\"show_names\":" << (settings.party.show_names ? "true" : "false")
        << ",\"allow_controls\":" << (settings.party.allow_controls ? "true" : "false")
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
        if (json.find("\"video_quality\"") != std::string::npos)
        {
            const int q = (int)std::lround(get_float("video_quality"));
            s.video_quality = std::max(0, std::min(2, q));
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
        if (json.find("\"aggressive_prefetch\"") != std::string::npos)
            s.aggressive_prefetch = get_bool("aggressive_prefetch");
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
            auto parse_float_array = [&](const char *key, std::vector<float> &out,
                                         size_t cap = 64) {
                const std::string needle = std::string("\"") + key + "\":";
                size_t at = json.find(needle);
                if (at == std::string::npos)
                    return false;
                size_t arr = json.find('[', at);
                if (arr == std::string::npos)
                    return false;
                size_t pos = arr + 1;
                while (pos < json.size() && out.size() < cap)
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

            // Room correction: same array encoding as the equalizer.
            std::vector<float> rf, rg, rq;
            const bool have_rf = parse_float_array("rq_freq", rf);
            parse_float_array("rq_gain", rg);
            parse_float_array("rq_q", rq);
            s.room_eq_bands.clear();
            if (have_rf)
            {
                for (size_t i = 0; i < rf.size(); ++i)
                {
                    EqBand b;
                    b.freq = std::max(20.0f, std::min(20000.0f, rf[i]));
                    b.gain = i < rg.size() ? std::max(-15.0f, std::min(15.0f, rg[i])) : 0.0f;
                    b.q = i < rq.size() ? std::max(0.1f, std::min(10.0f, rq[i])) : 1.0f;
                    s.room_eq_bands.push_back(b);
                }
            }
            std::sort(s.room_eq_bands.begin(), s.room_eq_bands.end(),
                      [](const EqBand &a, const EqBand &b) { return a.freq < b.freq; });
            if (s.room_eq_bands.size() > 32)
                s.room_eq_bands.resize(32);

            // Curves behind the applied correction, used for the Room EQ
            // response graph after a restart or room switch.
            RoomEqCurve &c = s.room_eq_curve;
            parse_float_array("rq_curve_hz", c.hz, 512);
            parse_float_array("rq_curve_measured", c.measured_db, 512);
            parse_float_array("rq_curve_target", c.target_db, 512);
            parse_float_array("rq_curve_corrected", c.corrected_db, 512);
            std::vector<float> cv;
            if (parse_float_array("rq_curve_valid", cv, 512))
            {
                c.valid.reserve(cv.size());
                for (float v : cv)
                    c.valid.push_back(v > 0.5f ? 1 : 0);
            }
            c.mic = get_str("rq_curve_mic");
            c.residual_before_db = get_float("rq_curve_before");
            c.residual_after_db = get_float("rq_curve_after");
            c.correct_lo_hz = get_float("rq_curve_lo");
            c.correct_hi_hz = get_float("rq_curve_hi");
            c.content_hi_hz = get_float("rq_curve_content_hi");
            c.max_bands = (int)std::lround(get_float("rq_curve_max_bands"));
            c.sample_rate = (int)std::lround(get_float("rq_curve_rate"));
            if (c.sample_rate <= 0)
                c.sample_rate = 48000;
            if (c.empty() || c.valid.size() != c.hz.size())
                c.valid.assign(c.hz.size(), 1);
        }
        if (json.find("\"rq_enabled\"") != std::string::npos)
            s.room_eq_enabled = get_bool("rq_enabled");
        s.room_eq_preamp = std::max(-24.0f, std::min(0.0f, get_float("rq_preamp")));
        s.room_eq_bass_db = std::max(-6.0f, std::min(6.0f, get_float("rq_bass")));
        s.room_eq_treble_db = std::max(-6.0f, std::min(6.0f, get_float("rq_treble")));
        s.room_eq_max_bands = std::max(6, std::min(32, (int)std::lround(get_float("rq_max_bands"))));
        if (s.room_eq_max_bands != 20 && s.room_eq_max_bands != 32)
            s.room_eq_max_bands = 12;
        s.room_eq_mic = get_str("rq_mic");
        if (s.room_eq_mic == "galaxy_s24") // profile removed
            s.room_eq_mic = "phone_generic";
        // Renamed from "rq_target": pre-release builds defaulted to a flat
        // target that gutted the bass, and the old value is deliberately
        // ignored so those installs start from the safer default.
        s.room_eq_target = get_str("rq_target2");
        if (s.room_eq_target != "warm" && s.room_eq_target != "dynamic" &&
            s.room_eq_target != "flat")
            s.room_eq_target = "balanced";
        s.room_eq_strength = get_float("rq_strength");
        if (s.room_eq_strength <= 0.05f)
            s.room_eq_strength = 1.0f;
        s.room_eq_strength = std::max(0.3f, std::min(1.5f, s.room_eq_strength));
        s.room_eq_label = get_str("rq_label");
        s.room_eq_residual_db = get_float("rq_residual");
        s.room_eq_active_id = get_str("rq_room_active");
        if (json.find("\"rq_auto_room\"") != std::string::npos)
            s.room_eq_auto_room = get_bool("rq_auto_room");
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
        s.lyrics_effect_style =
            std::max(0, std::min(2, (int)std::lround(get_float("lyrics_effect_style"))));
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
            // Default 60; 0 means "follow the display". Absent keys keep 60.
            if (json.find("\"max_refresh_hz\"") != std::string::npos)
                s.max_refresh_hz = (int)std::lround(get_float("max_refresh_hz"));
            if (s.max_refresh_hz != 0)
                s.max_refresh_hz = std::max(30, std::min(240, s.max_refresh_hz));
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
        if (json.find("\"edge_fade_height\"") != std::string::npos)
            s.edge_fade_height = std::max(0.0f, std::min(80.0f, get_float("edge_fade_height")));
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
        // Default is ON; only a written "false" turns it off.
        if (json.find("\"check_for_updates\"") != std::string::npos)
            s.check_for_updates = get_bool("check_for_updates");
        if (json.find("\"low_power\"") != std::string::npos)
            s.low_power = get_bool("low_power");
        if (json.find("\"low_power_on_battery\"") != std::string::npos)
            s.low_power_on_battery = get_bool("low_power_on_battery");
        if (json.find("\"low_memory_mode\"") != std::string::npos)
            s.low_memory_mode = get_bool("low_memory_mode");
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

                std::string reach = pget_str("reach");
                s.party.reach = reach == "lan" ? "lan" : "public";
                s.party.tunnel_path = pget_str("tunnel_path");

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
                if (pj.find("\"allow_controls\"") != std::string::npos)
                    s.party.allow_controls = pget_bool("allow_controls");
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
    SC_ROOM_EQ,
    SC_PARTY,
    SC_LYRICS,
    SC_YOUTUBE,
    SC_SHORTCUTS,
    SC_LIBRARY,
    SC_POWER,
    SC_WARMSTART,
    SC_DATA,
    SC_DIAGNOSTICS,
    SC_UPDATES,
    SC_TUNABLES,
    SC_NATIVE_BACKEND,
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

// ----------------------------------------------------------------------
// Sign-in helper plumbing
//
// The helper runs as a detached child so a webview window can stay open for
// minutes without blocking the settings panel or joining on shutdown. It
// writes <config>/login.log and <config>/login-result.txt; the UI tails both.
// ----------------------------------------------------------------------
static bool s_auth_paste_open = false;
static std::string s_auth_headers;
static bool s_auth_probe_started = false;
static bool s_auth_import_fallback = false;
// The helper's exit code only says it wrote a file. The success toast waits
// for the restarted backend, whose ready handshake is the authoritative
// "YouTube accepted this session" answer.
static bool s_auth_verify_pending = false;
static std::chrono::steady_clock::time_point s_auth_started{};

static std::string auth_helper_script()
{
    return p100_share_dir() + "/scripts/crossfade-login-helper.py";
}

static std::string auth_quote(const std::string &value)
{
#ifdef P100_WINDOWS
    std::string out = "\"";
    for (char c : value)
        out += (c == '"') ? "\\\"" : std::string(1, c);
    return out + "\"";
#else
    std::string out = "'";
    for (char c : value)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out + "'";
#endif
}

static std::string auth_join(const std::vector<std::string> &args)
{
    std::string cmd;
    for (const auto &arg : args)
    {
        if (!cmd.empty())
            cmd += ' ';
        cmd += auth_quote(arg);
    }
    return cmd;
}

static std::string auth_read_tail(const std::string &path, size_t max_bytes)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (data.size() > max_bytes)
        data.erase(0, data.size() - max_bytes);
    return data;
}

static int auth_capture(const std::vector<std::string> &args, std::string &out)
{
    out.clear();
#ifdef P100_WINDOWS
    return winproc::system_hidden_capture(auth_join(args), out);
#else
    const std::string cmd = auth_join(args) + " 2>&1";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return 127;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0)
        out.append(buf, n);
    const int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static bool auth_spawn(const std::vector<std::string> &args, const std::string &log_path)
{
#ifdef P100_WINDOWS
    return winproc::spawn_argv_hidden(args);
#else
    const pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        setsid();
        const int fd = open(log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
        if (fd >= 0)
        {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &arg : args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return true;
#endif
}

static void auth_start(AppState &state, std::vector<std::string> args)
{
    if (state.auth_login_running.load())
        return;
    const std::string script = auth_helper_script();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(script, ec))
    {
        ui_notify(UiNotifyType::Error, "Sign-in helper missing",
                  "The login helper was not found next to the app files. "
                  "Reinstall crossfade.", 8.0f);
        return;
    }

    const std::string result = p100_config_path("login-result.txt");
    const std::string log = p100_config_path("login.log");
    std::filesystem::remove(result, ec);
    std::filesystem::remove(log, ec);

    std::vector<std::string> full;
    full.push_back(backend_python_path());
    full.push_back(script);
    for (auto &arg : args)
        full.push_back(std::move(arg));
    full.push_back("--result-file");
    full.push_back(result);

    state.auth_login_exit = -1;
    state.auth_login_log.clear();
    state.auth_login_running.store(true);
    // A fresh run is never a continuation of the browser fallback unless the
    // caller says so right after spawning (the code-3 branch does).
    s_auth_import_fallback = false;
    s_auth_verify_pending = false;
    s_auth_started = std::chrono::steady_clock::now();
    state.repaint_requested = true;
    if (!auth_spawn(full, log))
    {
        state.auth_login_running.store(false);
        std::filesystem::remove(p100_config_path("login-headers.txt"), ec);
        ui_notify(UiNotifyType::Error, "Could not start sign-in",
                  "The login helper could not be launched.", 7.0f);
    }
}

static std::string auth_probe_snapshot(AppState &state)
{
    std::lock_guard<std::mutex> lock(state.auth_probe_mutex);
    return state.auth_probe_output;
}

static bool auth_write_headers(const std::string &path, const std::string &text)
{
#ifdef P100_WINDOWS
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << text;
    return out.good();
#else
    const int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0)
        return false;
    const ssize_t n = write(fd, text.data(), text.size());
    close(fd);
    return n == (ssize_t)text.size();
#endif
}

static void auth_probe_start(AppState &state)
{
    if (state.auth_probe_running.load())
        return;
    state.auth_probe_running.store(true);
    AppState *sp = &state;
    tasks::spawn("auth-probe", [sp]() {
        std::string out;
        const int rc =
            auth_capture({backend_python_path(), auth_helper_script(), "--probe"}, out);
        {
            std::lock_guard<std::mutex> lock(sp->auth_probe_mutex);
            sp->auth_probe_output =
                rc == 0 ? out
                        : (std::string("Sign-in detection failed (helper exit ") +
                           std::to_string(rc) + ")");
        }
        sp->auth_probe_running.store(false);
        sp->repaint_requested = true;
    });
}

static std::string auth_probe_string(const std::string &json, size_t from,
                                     const char *key)
{
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t at = json.find(needle, from);
    if (at == std::string::npos)
        return {};
    std::string out;
    for (size_t i = at + needle.size(); i < json.size(); ++i)
    {
        const char c = json[i];
        if (c == '\\' && i + 1 < json.size())
        {
            out += json[++i];
            continue;
        }
        if (c == '"')
            break;
        out += c;
    }
    return out;
}

static bool auth_probe_available(const std::string &json, const char *id)
{
    const std::string needle = std::string("\"id\":\"") + id + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos)
        return false;
    const size_t available = json.find("\"available\":", at);
    return available != std::string::npos && available + 16 <= json.size() &&
           json.compare(available + 12, 4, "true") == 0;
}

static std::string auth_probe_engine(const std::string &json, const char *id,
                                     const char *name)
{
    const std::string needle = std::string("\"id\":\"") + id + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos)
        return std::string(name) + ": not reported";
    if (!auth_probe_available(json, id))
    {
        // The helper names the missing piece (WebKitGTK not installed, no
        // PyGObject, ...) so the panel explains why there is no window.
        const std::string reason = auth_probe_string(json, at, "reason");
        return std::string(name) + ": not available" +
               (reason.empty() ? std::string() : " — " + reason);
    }
    const std::string label = auth_probe_string(json, at, "label");
    return std::string(name) + ": " + (label.empty() ? "available" : label);
}

void ui_auth_poll(AppState &state)
{
    // A successful helper run replaced the auth file and asked the main loop
    // to restart the backend. Only the restarted backend knows whether YouTube
    // accepted the session, so the toast waits one frame for that handshake:
    // an import that produced a signed-out file must never say "Signed in".
    if (s_auth_verify_pending && !state.auth_login_running.load() &&
        !state.auth_refresh_requested)
    {
        s_auth_verify_pending = false;
        if (backend_is_guest())
        {
            s_auth_paste_open = true;
            ui_notify(UiNotifyType::Warning, "Still in guest mode",
                      "YouTube Music did not accept that session. Sign in at "
                      "music.youtube.com in your browser, then import it again.",
                      9.0f);
        }
        else
        {
            ui_notify(UiNotifyType::Success, "Signed in",
                      "The YouTube Music session was updated.", 4.0f);
        }
        state.repaint_requested = true;
    }

    if (!state.auth_login_running.load())
        return;

    const std::string log = p100_config_path("login.log");
    state.auth_login_log = auth_read_tail(log, 8000);

    const std::string result = p100_config_path("login-result.txt");
    std::error_code ec;
    if (!std::filesystem::exists(result, ec))
    {
        // A helper that crashed before installing its atexit handler would
        // otherwise leave the panel "running" forever.
        if (s_auth_started.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() - s_auth_started >
                std::chrono::minutes(20))
        {
            state.auth_login_running.store(false);
            state.repaint_requested = true;
            s_auth_import_fallback = false;
            s_auth_paste_open = true;
            ui_notify(UiNotifyType::Error, "Sign-in timed out",
                      "The helper did not report a result. Use the paste panel.", 8.0f);
        }
        return;
    }

    int code = -1;
    {
        std::ifstream in(result);
        in >> code;
    }
    std::filesystem::remove(result, ec);
    state.auth_login_exit = code;
    state.auth_login_running.store(false);
    state.repaint_requested = true;

    if (code == 0)
    {
        s_auth_import_fallback = false;
        state.auth_refresh_requested = true;
        s_auth_headers.clear();
        s_auth_paste_open = false;
        // The toast is raised by the pending check above once the restarted
        // backend reports whether the session is real.
        s_auth_verify_pending = true;
    }
    else if (code == 4)
    {
        s_auth_import_fallback = false;
        ui_notify(UiNotifyType::Info, "Sign-in canceled",
                  "No session was changed.", 4.0f);
    }
    else if (code == 3)
    {
        // No embedded webview on this system (Fedora, Debian and Arch do not
        // ship WebKitGTK by default, and Qt WebEngine needs Python bindings).
        // Try a signed-in browser session first: when one verifies, sign in
        // without opening anything. The browser is only opened when there is
        // nothing to import.
        s_auth_paste_open = false;
        auth_start(state, {"--import-cookies", "auto"});
        s_auth_import_fallback = true;
    }
    else if (code == 5 && s_auth_import_fallback)
    {
        // No browser session to import. Open YouTube Music so the user can
        // sign in there; the toast points at the button that finishes the job.
        s_auth_import_fallback = false;
        s_auth_paste_open = false;
        if (!ui_open_external_url("https://music.youtube.com/"))
            ui_notify(UiNotifyType::Warning, "Could not open a browser",
                      "Open music.youtube.com manually, then press \"Import session "
                      "from this browser\" in Settings.", 9.0f);
        else
            ui_notify(UiNotifyType::Info, "Sign in in your browser",
                      "No signed-in browser session was found, so YouTube Music was "
                      "opened. Log in there, then press \"Import session from this "
                      "browser\" (set the right browser under Cookie browser first).",
                      9.0f);
    }
    else if (code == 6)
    {
        s_auth_import_fallback = false;
        s_auth_paste_open = true;
        ui_notify(UiNotifyType::Warning, "Google blocked the sign-in window",
                  "Use the paste panel below with your normal browser.", 9.0f);
    }
    else
    {
        s_auth_import_fallback = false;
        s_auth_paste_open = true;
        ui_notify(UiNotifyType::Error, "Sign-in failed",
                  "See the details in Settings -> YouTube Music.", 8.0f);
    }
}

static const char *update_state_label(updater::State state)
{
    switch (state)
    {
    case updater::State::Idle:
        return "Not checked yet";
    case updater::State::Checking:
        return "Checking...";
    case updater::State::Available:
        return "Update available";
    case updater::State::UpToDate:
        return "Up to date";
    case updater::State::Downloading:
        return "Downloading...";
    case updater::State::Ready:
        return "Ready to install";
    case updater::State::Updating:
        return "Updating...";
    case updater::State::Failed:
        return "Update check failed";
    }
    return "Unknown";
}

static std::string update_time_label(long long epoch)
{
    if (epoch <= 0)
        return "Never";
    const std::time_t t = (std::time_t)epoch;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

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
    {SC_ROOM_EQ, UiIcon::Qr, "Room correction", nullptr},
    {SC_LYRICS, UiIcon::Lyrics, "Lyrics", nullptr},
    {SC_PARTY, UiIcon::Party, "Party", "CONNECT"},
    {SC_YOUTUBE, UiIcon::Playlist, "YouTube Music", nullptr},
    {SC_LIBRARY, UiIcon::Download, "Library", "SYSTEM"},
    {SC_SHORTCUTS, UiIcon::More, "Shortcuts", nullptr},
    {SC_POWER, UiIcon::Moon, "Power", nullptr},
    {SC_WARMSTART, UiIcon::Repeat, "Warm start", nullptr},
    {SC_DATA, UiIcon::Download, "Data transfer", nullptr},
    {SC_DIAGNOSTICS, UiIcon::Stats, "Diagnostics", nullptr},
    {SC_UPDATES, UiIcon::Refresh, "Updates", nullptr},
    {SC_TUNABLES, UiIcon::Settings, "Tunables", "DEVELOPER"},
    {SC_NATIVE_BACKEND, UiIcon::Stats, "Native backend", nullptr},
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

static std::string tune_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return text;
}

// ----------------------------------------------------------------------
// Settings -> Tunables
// Every hardcoded developer value, searchable. Overrides are stored in
// <config>/tunables.json; the Python backend reads the same file at startup.
// ----------------------------------------------------------------------
static void draw_tunables_panel()
{
    const UiMetrics &m = ui_m();
    static char s_search[128] = "";
    bool changed = false;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(m.space_3, m.space_2));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##tune_search",
                                 "Search tunables by name, key, group or description...",
                                 s_search, sizeof(s_search)))
        changed = true;
    ImGui::PopStyleVar();

    std::string query = tune_lower(std::string(s_search));
    const int overridden = tunables_changed_count();
    const int total = (int)tunable_defs().size();
    ImGui::TextColored(ui_c().text_muted, "%d of %d values overridden", overridden, total);
    if (overridden > 0)
    {
        ImGui::SameLine(0.0f, m.space_4);
        if (ui_ghost_button("Reset all", ImVec2(130.0f * m.layout_scale, m.control_h)))
        {
            tunables_reset_all();
            changed = true;
        }
    }
    ImGui::Dummy(ImVec2(0.0f, m.space_2));

    const char *group = nullptr;
    for (const TunableDef &d : tunable_defs())
    {
        if (!query.empty())
        {
            std::string hay = std::string(d.key) + " " + d.label + " " + d.group + " " + d.desc;
            hay = tune_lower(std::move(hay));
            if (hay.find(query) == std::string::npos)
                continue;
        }
        if (!group || std::strcmp(group, d.group) != 0)
        {
            group = d.group;
            settings_group_label(group);
        }

        ImGui::PushID(d.key);
        ImGui::TextUnformatted(d.label);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n\n%s\n\nDefault: %g%s", d.key, d.desc, (double)d.def,
                              d.restart ? "\nTakes effect after a restart." : "");

        ImGui::Indent(m.space_3);
        const float editor_w = 260.0f * m.layout_scale;
        ImGui::SetNextItemWidth(editor_w);
        if (d.kind == TunableKind::Bool)
        {
            bool v = tunable_bool(d.key);
            if (ImGui::Checkbox("##tune_value", &v))
            {
                tunables_set(d.key, v ? 1.0f : 0.0f);
                changed = true;
            }
        }
        else if (d.kind == TunableKind::Int)
        {
            int v = tunable_int(d.key);
            if (ImGui::SliderInt("##tune_value", &v, (int)d.min, (int)d.max))
            {
                tunables_set(d.key, (float)v);
                changed = true;
            }
        }
        else
        {
            float v = tunable(d.key);
            if (ImGui::SliderFloat("##tune_value", &v, d.min, d.max, "%.3g"))
            {
                tunables_set(d.key, v);
                changed = true;
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", d.key);
        if (d.restart)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("restart");
        }
        if (tunables_is_set(d.key))
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset"))
            {
                tunables_reset(d.key);
                changed = true;
            }
        }
        ImGui::Unindent(m.space_3);
        ImGui::PopID();
    }

    // Write once the drag/slider interaction settles instead of on every frame
    // the value moves (values still apply live through tunable()).
    static bool s_pending_save = false;
    if (changed)
    {
        s_pending_save = true;
        ui_anim_request();
    }
    if (s_pending_save && !ImGui::IsAnyItemActive())
    {
        tunables_save_if_dirty();
        s_pending_save = false;
    }
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
    if (state.settings_focus_auth)
        s_settings_category = SC_YOUTUBE;

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

            // Display refresh: caps the fast frame budget while the UI moves.
            static const int refresh_hz[] = {60, 120, 144, 240, 0};
            static const char *refresh_names[] = {"60", "120", "144", "240", "Auto"};
            const int cur_hz = state.settings.max_refresh_hz;
            const char *refresh_value = nullptr;
            for (int i = 0; i < 5; ++i)
            {
                if (refresh_hz[i] == cur_hz)
                    refresh_value = refresh_names[i];
            }
            settings_row_label("Refresh rate",
                               refresh_value ? refresh_value : "Auto",
                               "How smoothly the interface moves on your screen. Higher "
                               "rates are smoother on 120/144/240 Hz monitors but use more "
                               "CPU while animating; playback and idle stay frugal. Auto "
                               "follows the monitor.");
            {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const float gap = m.space_2;
                const float bh = 34.0f * m.layout_scale;
                const float bw = (ImGui::GetContentRegionAvail().x - gap * 4.0f) / 5.0f;
                ImDrawList *dl = ImGui::GetWindowDrawList();
                for (int i = 0; i < 5; ++i)
                {
                    ImGui::SetCursorScreenPos(ImVec2(pos.x + i * (bw + gap), pos.y));
                    ImGui::PushID(i);
                    ImGui::InvisibleButton("##refresh", ImVec2(bw, bh));
                    const bool hov = ImGui::IsItemHovered();
                    if (ImGui::IsItemClicked())
                    {
                        state.settings.max_refresh_hz = refresh_hz[i];
                        save = true;
                    }
                    const bool on = refresh_hz[i] == state.settings.max_refresh_hz;
                    const ImVec2 c0(pos.x + i * (bw + gap), pos.y);
                    const ImVec2 c1(c0.x + bw, c0.y + bh);
                    dl->AddRectFilled(c0, c1, on ? ui_accent_u32(0.20f)
                                                 : ui_overlay_u32(hov ? 0.075f : 0.035f),
                                      m.radius_md);
                    dl->AddRect(c0, c1, on ? ui_accent_u32(0.45f)
                                           : ui_with_alpha(ui_border_u32(), 0.6f),
                                m.radius_md, 0, 1.0f);
                    const ImVec2 ts = g_ui_fonts.medium->CalcTextSizeA(15.5f * m.text_scale,
                                                                       FLT_MAX, 0, refresh_names[i]);
                    dl->AddText(g_ui_fonts.medium, 15.5f * m.text_scale,
                                ImVec2(c0.x + (bw - ts.x) * 0.5f, c0.y + (bh - ts.y) * 0.5f),
                                on ? ui_text_u32() : ui_text_dim_u32(), refresh_names[i]);
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
            if (settings_slider_row("##edge_fade_height", "List edge fade",
                                    &state.settings.edge_fade_height, 0.0f, 80.0f, "%.0f",
                                    "Height of the soft gradient that fades track lists and lyrics into the background at the top and bottom. 0 turns it off."))
                save = true;
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
    if (s_settings_category == SC_ROOM_EQ)
    {
        draw_room_eq_panel(state);
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

        settings_group_label("Video");
        {
            static const char *quality_items[] = {"720p", "1080p", "Fit window"};
            const int q = std::max(0, std::min(2, state.settings.video_quality));
            settings_row_label("Music video quality", quality_items[q],
                               "Resolution cap for the video page.");
            ImGui::SetNextItemWidth(field_w);
            if (ImGui::Combo("##video_quality", &state.settings.video_quality, quality_items, 3))
                save = true;
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
            writeback_set_enabled(state.settings.server_sync && !backend_is_guest());
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
        settings_help("Start a party from the Party tab. Guests scan its QR code with a phone camera, paste a YouTube or YouTube Music link, and their songs join Up Next. There is no account and no telemetry.");

        PartySettings &ps = state.settings.party;
        if (party_running())
            ImGui::TextColored(ui_c().accent_text,
                               "A party is running. Changes here apply the next time you start one, except guest playback controls, which apply immediately.");

        settings_group_label("Guest access");
        int reach = ps.public_reach() ? 0 : 1;
        const char *reach_modes[] = {"Public link (Cloudflare)", "Same Wi-Fi only"};
        settings_row_label("Reach", reach_modes[reach],
                           "Public links work from any network through a temporary Cloudflare tunnel. Same Wi-Fi is the fastest and keeps every request local.");
        ImGui::SetNextItemWidth(field_w);
        if (ImGui::Combo("##party_reach", &reach, reach_modes, 2))
        {
            ps.reach = reach == 0 ? "public" : "lan";
            save = true;
        }

        if (ps.public_reach())
        {
            std::string cf_path;
            const bool cf_found = party_tunnel::available(cf_path, ps.tunnel_path);
            if (cf_found)
            {
                settings_help(("cloudflared found: " + cf_path).c_str());
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.48f, 0.48f, 1.0f));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
                ImGui::TextWrapped(
                    "cloudflared is not installed, so public parties cannot start. Install it with %s (or download it from Cloudflare) and restart crossfade, or switch Reach to Same Wi-Fi only.",
#ifdef P100_WINDOWS
                    "winget install --id Cloudflare.cloudflared");
#elif defined(__APPLE__)
                    "brew install cloudflared");
#else
                    "your package manager (for example pacman -S cloudflared)");
#endif
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            settings_row_label("cloudflared path", nullptr,
                               "Leave empty to search PATH. Set this if the binary is installed somewhere custom.");
            ImGui::SetNextItemWidth(field_w);
            ImGui::InputText("##party_tunnel_path", &ps.tunnel_path);
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                while (!ps.tunnel_path.empty() &&
                       std::isspace((unsigned char)ps.tunnel_path.front()))
                    ps.tunnel_path.erase(ps.tunnel_path.begin());
                while (!ps.tunnel_path.empty() &&
                       std::isspace((unsigned char)ps.tunnel_path.back()))
                    ps.tunnel_path.pop_back();
                save = true;
            }
            settings_help("The public link is a random *.trycloudflare.com address that changes every time the party starts or the network changes. Cloudflare relays the guest page, so the link and its requests pass through Cloudflare; anyone with the link can add songs. Approval mode is recommended.");
        }
        else
        {
            settings_help("The link only works on this network. Hotspots and public Wi-Fi often isolate clients, so a phone may not reach this laptop even on the same Wi-Fi.");
        }

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
                           "The local address the party server (and the Cloudflare tunnel) use. Automatic skips Docker and VPN adapters and prefers the address your network routes.");
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
        if (ps.public_reach())
            settings_help("The tunnel connects to this local address; guests use the public link, so they do not need to be on this network.");
        else
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
        if (settings_toggle_row("##party_controls", "Let guests control playback",
                                &ps.allow_controls,
                                "Adds play/pause, previous and next buttons to the guest page. Guests can also tap any song in Up next to move it to the top of the queue."))
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

        settings_group_label("Immersive lyrics");
        {
            static const char *effect_styles[] = {"Waves", "Spectrum", "Off"};
            const int style =
                std::max(0, std::min(2, state.settings.lyrics_effect_style));
            settings_row_label("Intro / gap effect", effect_styles[style],
                               "Visual style for the loader before the first word and between instrumental lyric rows. Timing is identical for Waves and Spectrum.");
            ImGui::SetNextItemWidth(field_w);
            if (ImGui::Combo("##lyrics_effect_style",
                             &state.settings.lyrics_effect_style,
                             effect_styles, 3))
                save = true;
        }
    }
    if (s_settings_category == SC_YOUTUBE)
    {
        SettingsCard card("YouTube Music", "Account, cookies and stream resolving.", UiIcon::Playlist);
        const float auth_section_y = ImGui::GetCursorPosY();

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
    settings_row_label("Sign-in method", "Browser cookies",
                       "crossfade opens its own sign-in window when WebKitGTK, Qt WebEngine "
                       "or Windows WebView2 is present. Otherwise it imports your browser "
                       "session, and only opens YouTube Music if there is none to import.");
    if (state.settings.auth_mode == "oauth")
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.72f, 0.35f, 1.0f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
        ImGui::TextWrapped("OAuth is selected in settings.json but is not exposed in the "
                           "interface yet. Switch back to browser cookies to use the sign-in "
                           "panel.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (ui_ghost_button("Switch to browser cookies",
                            ImVec2(230.0f * m.layout_scale, m.control_h)))
        {
            state.settings.auth_mode = "browser";
            set_auth_mode("browser");
            save = true;
            state.auth_refresh_requested = true;
        }
    }

    settings_gap();
    {
        // State the mode the backend is actually in, not what the last helper
        // run claimed. A rejected import leaves the app in guest mode, and the
        // card has to say so.
        const bool guest = backend_is_guest();
        ImGui::PushStyleColor(ImGuiCol_Text, guest ? ImVec4(0.95f, 0.72f, 0.35f, 1.0f)
                                                   : ui_c().accent_text);
        ImGui::TextUnformatted(guest ? "Guest mode — not signed in"
                                     : "Signed in to YouTube Music");
        ImGui::PopStyleColor();
    }
    settings_help(backend_is_guest()
        ? (state.auth_login_exit == 5
               ? "The last session was rejected or has expired. Search, albums, "
                 "artists, radio and podcasts still work; sign in to unlock your "
                 "library."
               : "Search, albums, artists, radio and podcasts work. Sign in to "
                 "unlock personalized recommendations and your library.")
        : "Personalized recommendations and your YouTube Music library are active.");

    if (ui_primary_button("Sign in", ImVec2(170.0f * m.layout_scale, m.button_h)))
        auth_start(state, {"--signin", "auto"});
    ImGui::SameLine(0.0f, m.space_2);
    if (ui_ghost_button("Open YouTube Music##signin_open",
                        ImVec2(210.0f * m.layout_scale, m.button_h)))
    {
        if (!ui_open_external_url("https://music.youtube.com/"))
            ui_notify(UiNotifyType::Warning, "Could not open a browser",
                      "Copy the link and open it manually.", 6.0f);
    }
    ImGui::SameLine(0.0f, m.space_2);
    if (ui_ghost_button("Copy link##signin_copy", ImVec2(120.0f * m.layout_scale, m.button_h)))
        ImGui::SetClipboardText("https://music.youtube.com/");

    if (ui_ghost_button("Import session from this browser",
                        ImVec2(290.0f * m.layout_scale, m.control_h)))
        auth_start(state, {"--import-cookies", state.settings.ytdl_browser});
    settings_help("Import is a one-click snapshot and is verified before replacing the "
                  "current session; if it has expired, nothing changes.");

    // Most Linux desktops do not ship a webview engine, so "Sign in" falls
    // back to the user's browser plus this import. Say so up front instead of
    // letting the button look broken.
    {
        const std::string probe = auth_probe_snapshot(state);
        if (!state.auth_probe_running.load() && !probe.empty() &&
            probe.rfind("Sign-in detection failed", 0) != 0 &&
            !auth_probe_available(probe, "webkit") &&
            !auth_probe_available(probe, "qt") &&
            !auth_probe_available(probe, "webview2"))
            settings_help("No embedded sign-in window is available on this system. "
                          "\"Sign in\" imports an existing browser session and only "
                          "opens YouTube Music when there is none, so no extra packages "
                          "are needed.");
    }

    if (state.auth_login_running.load())
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_1));
        ImGui::TextColored(ui_c().accent_text, "%s",
                           s_auth_import_fallback
                               ? "Signing in... checking your browser session."
                               : "Signing in... close the sign-in window to cancel.");
    }
    if (!state.auth_login_log.empty())
    {
        ImGui::BeginChild("##auth_log", ImVec2(-1.0f, 104.0f * m.layout_scale), true);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(state.auth_login_log.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
    }

    if (ui_ghost_button(s_auth_paste_open ? "Hide paste panel" : "Paste request headers",
                        ImVec2(220.0f * m.layout_scale, m.control_h)))
        s_auth_paste_open = !s_auth_paste_open;

    if (s_auth_paste_open)
    {
        settings_gap();
        settings_help("1. Open music.youtube.com in any browser and sign in. "
                      "2. Press F12, open Network, then refresh the page or click a song. "
                      "3. Right-click a POST request to music.youtube.com and choose "
                      "Copy -> Copy request headers. 4. Paste the whole block below.");
        if (ui_ghost_button("Open YouTube Music##paste_open",
                            ImVec2(200.0f * m.layout_scale, m.control_h)))
        {
            if (!ui_open_external_url("https://music.youtube.com/"))
                ui_notify(UiNotifyType::Warning, "Could not open a browser",
                          "Open https://music.youtube.com/ manually, then copy its "
                          "request headers.", 7.0f);
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Copy link##paste_copy", ImVec2(120.0f * m.layout_scale, m.control_h)))
            ImGui::SetClipboardText("https://music.youtube.com/");
        ImGui::InputTextMultiline(
            "##auth_headers", &s_auth_headers,
            ImVec2(ImGui::GetContentRegionAvail().x, 120.0f * m.layout_scale));
        if (ui_ghost_button("Paste from clipboard",
                            ImVec2(190.0f * m.layout_scale, m.control_h)))
        {
            const char *clip = ImGui::GetClipboardText();
            if (clip && *clip)
                s_auth_headers = clip;
        }
        ImGui::SameLine(0.0f, m.space_2);
        if (ui_ghost_button("Clear", ImVec2(90.0f * m.layout_scale, m.control_h)))
            s_auth_headers.clear();
        ImGui::SameLine(0.0f, m.space_2);
        if (!state.auth_login_running.load() && !s_auth_headers.empty())
        {
            if (ui_primary_button("Connect", ImVec2(130.0f * m.layout_scale, m.control_h)))
            {
                const std::string path = p100_config_path("login-headers.txt");
                if (!auth_write_headers(path, s_auth_headers))
                    ui_notify(UiNotifyType::Error, "Could not save the headers",
                              "Check that the config directory is writable.", 7.0f);
                else
                    auth_start(state, {"--headers", path});
            }
        }
    }

    if (!s_auth_probe_started)
    {
        s_auth_probe_started = true;
        auth_probe_start(state);
    }
    if (ImGui::TreeNode("Sign-in methods detected"))
    {
        const std::string probe = auth_probe_snapshot(state);
        if (state.auth_probe_running.load())
            ImGui::TextUnformatted("Checking available engines...");
        else if (probe.empty())
            ImGui::TextUnformatted("Not checked yet.");
        else if (probe.rfind("Sign-in detection failed", 0) == 0)
            ImGui::TextUnformatted(probe.c_str());
        else
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
            ImGui::TextWrapped("%s",
                               auth_probe_engine(probe, "webkit", "WebKitGTK").c_str());
            ImGui::TextWrapped("%s",
                               auth_probe_engine(probe, "qt", "Qt WebEngine").c_str());
            ImGui::TextWrapped("%s",
                               auth_probe_engine(probe, "webview2", "WebView2").c_str());
            ImGui::PopTextWrapPos();
            const size_t b = probe.find("\"browsers\":[");
            if (b != std::string::npos)
            {
                const size_t s = b + 12;
                const size_t e = probe.find(']', s);
                std::string list = probe.substr(
                    s, e == std::string::npos ? std::string::npos : e - s);
                for (char &c : list)
                    if (c == '"' || c == ',')
                        c = ' ';
                ImGui::TextWrapped("Cookie import: %s",
                                   list.empty() ? "none detected" : list.c_str());
            }
        }
        if (!state.auth_probe_running.load() &&
            ui_ghost_button("Check again", ImVec2(140.0f * m.layout_scale, m.control_h)))
            auth_probe_start(state);
        ImGui::TreePop();
    }

    settings_gap();
    if (settings_toggle_row("##warm_resolver", "Fast stream resolve (experimental)",
                            &state.settings.warm_stream_resolver,
                            "Resolve stream links inside the backend instead of spawning yt-dlp each time. Takes effect after a restart; turn it off if playback misbehaves."))
    {
        set_warm_stream_resolver(state.settings.warm_stream_resolver);
        save = true;
    }

    settings_gap();
    if (settings_toggle_row("##aggressive_prefetch", "Prefetch the world",
                            &state.settings.aggressive_prefetch,
                            "Warm stream URLs for the whole visible list in the background, "
                            "not just the row under the pointer, so clicks and the search "
                            "top result start instantly. Costs bandwidth and can trip "
                            "YouTube rate limits."))
        save = true;

    // Read-only snapshot: taking the error here would steal it from the
    // fetch workers and silently suppress their toasts. Workers clear the
    // error themselves on their next successful fetch.
    BackendError backend_err = backend_error_snapshot();
    if (backend_err)
        state.set_backend_status(backend_err);
    const BackendError status = state.backend_status_snapshot();
    if (status)
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.48f, 0.48f, 1.0f));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextWrapped("%s", backend_error_user_message(status).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        ImGui::TextWrapped("Detail: %s", status.message.c_str());
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

    if (state.settings.auth_mode != "oauth")
    {
        ImGui::Dummy(ImVec2(0.0f, m.space_2));
        ImGui::TextColored(ui_c().text_muted, "Re-authenticate");
        const std::string reauth = backend_reauth_command();
        char reauth_buf[1024];
        std::snprintf(reauth_buf, sizeof(reauth_buf), "%s", reauth.c_str());
        ImGui::SetNextItemWidth(content_w);
        ImGui::InputText("##reauth_cmd", reauth_buf, sizeof(reauth_buf),
                         ImGuiInputTextFlags_ReadOnly);
        if (ui_ghost_button("Copy re-auth command",
                            ImVec2(200.0f * m.layout_scale, m.control_h)))
            ImGui::SetClipboardText(reauth.c_str());
        settings_help("Paste this into a terminal, complete the browser login, then "
                      "restart crossfade.");
    }
    if (ImGui::TreeNode("Backend details"))
    {
        ImGui::TextWrapped("Python: %s", backend_python_path().c_str());
        ImGui::TextWrapped("Script: %s", backend_script_path().c_str());
        ImGui::TreePop();
    }

        if (state.settings_focus_auth)
        {
            ImGui::SetScrollY(std::max(0.0f, auth_section_y - m.space_4));
            state.settings_focus_auth = false;
        }
    }
    if (s_settings_category == SC_SHORTCUTS)
    {
        SettingsCard card("Shortcuts", "Click a binding, then press the new keys. Esc cancels.",
                          UiIcon::More);

        const int custom = shortcuts_custom_count();
        if (custom > 0)
        {
            ImGui::TextColored(ui_c().text_muted, "%d custom binding%s", custom,
                               custom == 1 ? "" : "s");
            ImGui::SameLine(0.0f, m.space_4);
            if (ui_ghost_button("Reset all", ImVec2(130.0f * m.layout_scale, m.control_h)))
            {
                shortcuts_reset_all();
                shortcuts_save_if_dirty();
            }
        }
        if (!state.rebind_shortcut_id.empty())
            ImGui::TextColored(ui_c().accent_text,
                               "Press the new key combination, or Esc to cancel.");
        ImGui::Dummy(ImVec2(0.0f, m.space_2));

        // Highlight bindings that collide with another action: the first match
        // in the dispatch chain wins, so a duplicate is almost always a bug.
        std::map<long long, int> usage;
        for (const ShortcutDef &d : shortcut_defs())
            ++usage[(long long)shortcut_key(d.id) * 8 + (long long)shortcut_mods(d.id)];

        const char *group = nullptr;
        for (const ShortcutDef &d : shortcut_defs())
        {
            if (!group || std::strcmp(group, d.group) != 0)
            {
                group = d.group;
                settings_group_label(group);
            }
            ImGui::PushID(d.id);
            ImGui::TextUnformatted(d.label);

            const long long slot =
                (long long)shortcut_key(d.id) * 8 + (long long)shortcut_mods(d.id);
            const bool duplicate = usage[slot] > 1;
            const bool capturing = state.rebind_shortcut_id == d.id;
            const std::string shown =
                capturing ? "Press keys..." : shortcut_label(d.id);
            if (ui_ghost_button(shown.c_str(), ImVec2(170.0f * m.layout_scale, m.control_h)))
                state.rebind_shortcut_id = d.id;
            if (duplicate)
            {
                ImGui::SameLine();
                ImGui::TextColored(ui_c().accent_text, "duplicate");
            }
            ImGui::SameLine(0.0f, m.space_4);
            ImGui::TextDisabled("default %s",
                                shortcut_binding_label(d.key, d.mods).c_str());
            if (shortcut_is_custom(d.id))
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset"))
                {
                    shortcuts_reset(d.id);
                    shortcuts_save_if_dirty();
                }
            }
            ImGui::PopID();
        }
    }
    if (s_settings_category == SC_LIBRARY)
    {
        SettingsCard card("Library", "Caches, downloads and local data.", UiIcon::Download);

    if (s_repair_running || download_metadata_repair_running())
    {
        if (s_repair_running)
            ImGui::Text("Repairing %d / %d  ·  %d covers  ·  %d lyrics",
                        s_repair_done.load(), s_repair_total.load(),
                        s_repair_covers.load(), s_repair_lyrics.load());
        if (download_metadata_repair_running())
        {
            int done = 0, total = 0, updated = 0;
            download_metadata_repair_progress(done, total, updated);
            ImGui::Text("Repairing download metadata %d / %d  ·  %d updated", done, total,
                        updated);
        }
    }
    else
    {
        if (ui_secondary_button("Repair cached songs", ImVec2(200.0f * m.layout_scale, m.button_h)))
        {
            start_download_metadata_repair(state, true);
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
        settings_help("Re-download missing album covers and lyrics, and repair downloaded artist/cover metadata.");
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

        if (settings_toggle_row("##low_memory_mode", "Low memory mode",
                                &state.settings.low_memory_mode,
                                "Run one concurrent backend process instead of four. Saves a few hundred MB; background warmups and lyrics share the main channel. Takes effect after a restart."))
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
        settings_toggle_row("##xf_podcast", "Podcast cache", &s_xfer_sections.podcast_cache,
                            "Cached podcast shows and episode lists for instant/offline reopening.");
        settings_toggle_row("##xf_lyrics", "Lyrics", &s_xfer_sections.lyrics,
                            "Downloaded per-track lyrics.");
        settings_toggle_row("##xf_thumbs", "Thumbnail cache", &s_xfer_sections.thumbs,
                            "Cover artwork cache (can be several hundred MB).");
        settings_toggle_row("##xf_audio", "Downloaded audio", &s_xfer_sections.audio,
                            "Full audio cache. This can make the ZIP many GB; it is stored uncompressed for speed.");
        settings_toggle_row("##xf_video", "Downloaded videos", &s_xfer_sections.video,
                            "Offline video cache (video_cache/). Videos are large; this can make the ZIP very big.");
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

    if (s_settings_category == SC_UPDATES)
    {
        SettingsCard card("Updates", "Check GitHub for a newer release.", UiIcon::Refresh);
        settings_help("The daily check fetches one small version.txt from GitHub. "
                      "There is no telemetry.");
        const updater::Status us = updater::status();
        const bool safe_mode_now =
            diagnostics::safe_mode_requested() || state.settings.safe_mode;

        settings_row_label("Current version", us.current.empty() ? "Unknown" : us.current.c_str(),
                           nullptr);
        settings_row_label("Latest release",
                           us.latest.empty() ? "Not checked yet" : us.latest.c_str(), nullptr);
        settings_row_label("Install",
                           us.install.empty() ? "Unknown" : us.install.c_str(), nullptr);
        settings_row_label("Status", update_state_label(us.state),
                           us.message.empty() ? nullptr : us.message.c_str());
        const std::string last_checked = update_time_label(us.last_checked);
        settings_row_label("Last checked", last_checked.c_str(), nullptr);

        settings_gap();
        if (settings_toggle_row("##check_updates", "Check for updates automatically",
                                &state.settings.check_for_updates,
                                "Once a day, fetch the release's version.txt and show a "
                                "toast when something newer exists. Manual checks still work "
                                "when this is off. Safe mode pauses automatic checks and "
                                "staged updates for that launch."))
        {
            save = true;
            updater::set_auto_check(state.settings.check_for_updates);
        }

        if (safe_mode_now)
            settings_help("Safe mode is on for this launch, so automatic checks and "
                          "automatic installs are paused. You can still check manually.");

        settings_gap();
        const float bw = std::min(200.0f * m.layout_scale, (content_w - m.space_2) * 0.5f);
        if (us.state == updater::State::Downloading)
        {
            ImGui::TextUnformatted("Downloading and verifying the release...");
        }
        else if (us.state == updater::State::Ready)
        {
            if (ui_primary_button("Restart & update", ImVec2(bw, m.control_h)))
                updater::restart_and_update();
            ImGui::SameLine(0.0f, m.space_2);
            if (ui_ghost_button("Discard download", ImVec2(bw, m.control_h)))
                updater::discard_staged();
        }
        else
        {
            if (ui_ghost_button("Check now", ImVec2(bw, m.control_h)))
                updater::check_async(true);
            ImGui::SameLine(0.0f, m.space_2);
            if (us.can_self_update &&
                (us.state == updater::State::Available || us.state == updater::State::Failed))
            {
                if (ui_primary_button("Update now", ImVec2(bw, m.control_h)))
                    updater::start_update();
            }
            else if (ui_ghost_button("Open release page", ImVec2(bw, m.control_h)))
            {
                updater::open_release_page();
            }
        }

        settings_gap();
        if (us.can_self_update)
            settings_help("Updates download the matching release asset, verify its SHA-256, "
                          "and replace only crossfade's own files. Your settings, library, "
                          "downloads and listening history stay put.");
        else if (us.package_managed)
            settings_help("This copy is managed by your package manager. crossfade never "
                          "rewrites a system-owned install; update it with your package "
                          "manager or from the release page.");
        else
            settings_help("This build runs from a source or build tree. Rebuild and re-run "
                          "./install.sh to update, or use the release page.");
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

    if (s_settings_category == SC_NATIVE_BACKEND)
    {
        draw_native_backend_panel(state);
    }

    if (s_settings_category == SC_TUNABLES)
        draw_tunables_panel();

    ImGui::EndChild();

    if (save)
        save_settings(state.settings);
}
