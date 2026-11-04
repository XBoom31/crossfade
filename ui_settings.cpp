#include "ui.h"
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
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += (char)c;
        }
    }
    return out;
}

static void json_append_utf8(std::string &out, unsigned int cp)
{
    if (cp < 0x80)
        out += (char)cp;
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::string json_unescape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '\\' || i + 1 >= s.size())
        {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        switch (c)
        {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'u':
        {
            if (i + 4 >= s.size())
                break;
            unsigned int cp = 0;
            bool ok = true;
            for (int k = 1; k <= 4; ++k)
            {
                const int v = hex_val(s[i + k]);
                if (v < 0) { ok = false; break; }
                cp = (cp << 4) | (unsigned int)v;
            }
            if (!ok)
                break;
            i += 4;
            // Surrogate pair -> combine into a single code point.
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < s.size() &&
                s[i + 1] == '\\' && s[i + 2] == 'u')
            {
                unsigned int lo = 0;
                bool lok = true;
                for (int k = 3; k <= 6; ++k)
                {
                    const int v = hex_val(s[i + k]);
                    if (v < 0) { lok = false; break; }
                    lo = (lo << 4) | (unsigned int)v;
                }
                if (lok && lo >= 0xDC00 && lo <= 0xDFFF)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            json_append_utf8(out, cp);
            break;
        }
        default: out += c; break;
        }
    }
    return out;
}


// Canonical list of individually-scalable UI elements. Keys are stable and
// persisted verbatim as "ui_elem_<key>".
const std::vector<UiElementDef> &ui_element_defs()
{
    static const std::vector<UiElementDef> defs = {
        {"topnav", "Top navigation bar"},
        {"searchbar", "Search bar"},
        {"bottombar", "Bottom bar"},
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
        << "\"backdrop_shader\":" << (settings.backdrop_shader ? "true" : "false") << ","
        << "\"safe_mode\":" << (settings.safe_mode ? "true" : "false") << ","
        << "\"glow_separators\":" << (settings.glow_separators ? "true" : "false") << ","
        << "\"glow_separator_strength\":" << settings.glow_separator_strength << ","
        << "\"backdrop_drift\":" << (settings.backdrop_drift ? "true" : "false") << ","
        << "\"download_cap_gb\":" << settings.download_cap_gb
        << ",\"warm_stream_resolver\":" << (settings.warm_stream_resolver ? "true" : "false")
        << ",\"playback_speed\":" << settings.playback_speed
        << ",\"eq_enabled\":" << (settings.eq_enabled ? "true" : "false")
        << ",\"eq_gains\":["
        << settings.eq_gains[0] << "," << settings.eq_gains[1] << ","
        << settings.eq_gains[2] << "," << settings.eq_gains[3] << ","
        << settings.eq_gains[4] << "]"
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
        auto get_str = [&](const std::string &key) -> std::string
        {
            size_t pos = json.find("\"" + key + "\":\"");
            if (pos == std::string::npos)
                return "";
            pos += key.size() + 4;
            size_t end = json.find("\"", pos);
            if (end == std::string::npos)
                return "";
            return json.substr(pos, end - pos);
        };
        auto get_float = [&](const std::string &key) -> float
        {
            size_t pos = json.find("\"" + key + "\":");
            if (pos == std::string::npos)
                return 0.0f;
            // Skip the opening quote, the key, the closing quote and the
            // colon (= key.size() + 3) so we land on the first value char.
            pos += key.size() + 3;
            std::string num;
            if (pos < json.size() && (json[pos] == '-' || json[pos] == '+'))
                num += json[pos++];
            while (pos < json.size() && (isdigit(json[pos]) || json[pos] == '.'))
            {
                num += json[pos++];
            }
            if (num.empty() || num == "-" || num == "+")
                return 0.0f;
            return std::stof(num);
        };
        auto get_bool = [&](const std::string &key) -> bool
        {
            size_t pos = json.find("\"" + key + "\":");
            if (pos == std::string::npos)
                return false;
            pos += key.size() + 2;
            return json.find("true", pos) < json.find("false", pos);
        };

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
        if (json.find("\"eq_gains\"") != std::string::npos)
        {
            size_t arr = json.find('[', json.find("\"eq_gains\""));
            if (arr != std::string::npos)
            {
                size_t pos = arr + 1;
                for (int i = 0; i < 5; ++i)
                {
                    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ','))
                        ++pos;
                    if (pos >= json.size() || json[pos] == ']')
                        break;
                    char *endp = nullptr;
                    float v = std::strtof(json.c_str() + pos, &endp);
                    if (endp == json.c_str() + pos)
                        break;
                    s.eq_gains[i] = std::max(-12.0f, std::min(12.0f, v));
                    pos = (size_t)(endp - json.c_str());
                }
            }
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
        if (json.find("\"album_backdrop_strength\"") != std::string::npos)
        {
            const float v = get_float("album_backdrop_strength");
            if (v > 0.0f)
                s.album_backdrop_strength = std::max(0.10f, std::min(1.0f, v));
        }
        if (json.find("\"backdrop_shader\"") != std::string::npos)
            s.backdrop_shader = get_bool("backdrop_shader");
        if (json.find("\"safe_mode\"") != std::string::npos)
            s.safe_mode = get_bool("safe_mode");
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
        // Free-form notes (multiline, JSON-escaped). Kept separate from
        // get_str, which stops at the first quote and would truncate on
        // embedded \". Keep the struct default (empty) when the key is
        // absent from an older settings file.
        if (json.find("\"notes\"") != std::string::npos)
        {
            size_t pos = json.find("\"notes\":\"");
            if (pos != std::string::npos)
            {
                pos += 9; // strlen("\"notes\":\"")
                size_t end = pos;
                while (end < json.size())
                {
                    if (json[end] == '\\')
                    {
                        end += 2;
                        continue;
                    }
                    if (json[end] == '"')
                        break;
                    ++end;
                }
                if (end <= json.size())
                    s.notes = json_unescape(json.substr(pos, end - pos));
            }
        }

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
                auto pget_str = [&](const std::string &key) -> std::string
                {
                    size_t pos = pj.find("\"" + key + "\":\"");
                    if (pos == std::string::npos)
                        return "";
                    pos += key.size() + 4;
                    size_t close = pos;
                    while (close < pj.size())
                    {
                        if (pj[close] == '\\')
                        {
                            close += 2;
                            continue;
                        }
                        if (pj[close] == '"')
                            break;
                        ++close;
                    }
                    if (close > pj.size())
                        return "";
                    return json_unescape(pj.substr(pos, close - pos));
                };
                auto pget_num = [&](const std::string &key) -> int
                {
                    size_t pos = pj.find("\"" + key + "\":");
                    if (pos == std::string::npos)
                        return 0;
                    pos += key.size() + 3;
                    char *endp = nullptr;
                    const long v = std::strtol(pj.c_str() + pos, &endp, 10);
                    if (endp == pj.c_str() + pos)
                        return 0;
                    return (int)v;
                };
                auto pget_bool = [&](const std::string &key) -> bool
                {
                    size_t pos = pj.find("\"" + key + "\":");
                    if (pos == std::string::npos)
                        return false;
                    pos += key.size() + 2;
                    const size_t t = pj.find("true", pos);
                    const size_t f = pj.find("false", pos);
                    return t != std::string::npos && (f == std::string::npos || t < f);
                };

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

void draw_settings_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    const float content_w = std::min(780.0f * m.layout_scale, ImGui::GetContentRegionAvail().x);
    const float field_w = std::min(420.0f * m.layout_scale, content_w);
    bool save = false;

    ui_section_header("Appearance", "Type, spacing and scale");

    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderFloat("##ui_scale", &state.settings.ui_scale, 0.75f, 2.0f, "UI scale  %.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    settings_help("Scales text, widgets and layout together. Most people only need this one.");

    // Accent colour: automatic by default. Editing the swatch stores an
    // explicit override, which is the only option on Windows and on Linux
    // desktops that don't expose org.freedesktop.appearance.
    ImGui::TextColored(ui_c().text_muted, "Accent colour");
    if (ImGui::ColorButton("##accent_swatch", g_accent,
                           ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
                           ImVec2(field_w, m.control_h)))
    {
        ImGui::OpenPopup("##accent_picker");
        ImGui::SetNextWindowPos(
            ImVec2(ImGui::GetItemRectMin().x,
                   ImGui::GetItemRectMax().y + ImGui::GetStyle().ItemSpacing.y));
    }
    static bool accent_picker_was_open = false;
    if (ImGui::BeginPopup("##accent_picker"))
    {
        accent_picker_was_open = true;
        ImVec4 accent = g_accent;
        if (ImGui::ColorPicker4("##accent_picker_edit", &accent.x,
                                ImGuiColorEditFlags_NoAlpha))
        {
            state.settings.accent_color = accent_to_hex(accent);
            update_accent(state.settings);
            ui_theme_refresh_accent();
        }
        ImGui::EndPopup();
    }
    else if (accent_picker_was_open)
    {
        accent_picker_was_open = false;
        save = true;
    }
    if (state.settings.accent_color.empty())
    {
        ImGui::TextDisabled("Automatic - system colour or today's colour");
    }
    else
    {
        ImGui::TextDisabled("Custom colour %s", state.settings.accent_color.c_str());
        ImGui::SameLine();
        if (ui_ghost_button("Use automatic", ImVec2(150.0f * m.layout_scale, m.control_h)))
        {
            state.settings.accent_color.clear();
            update_accent(state.settings);
            ui_theme_refresh_accent();
            save = true;
        }
    }
    settings_help("Used for highlights, progress bars and glows. Automatic follows your desktop accent, or a new daily colour when the system doesn't provide one. Pick a colour to use your own everywhere.");

    ImGui::TextColored(ui_c().text_muted, "Animations");
    const char *animations[] = {"Off", "Subtle", "Standard", "Playful"};
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::Combo("##animations", &state.settings.animations, animations, 4))
        save = true;
    settings_help("Motion for track changes, list reveals and scrolling. Standard is the default.");

    if (ImGui::Checkbox("Album colour backdrop", &state.settings.album_backdrop))
        save = true;
    settings_help("Tints the background with the dominant colour of the playing album art, or the app accent when nothing is loaded. The colour crossfades on track changes. Night mode stays dim to preserve true blacks.");
    if (state.settings.album_backdrop)
    {
        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderFloat("##album_backdrop_strength", &state.settings.album_backdrop_strength,
                           0.10f, 1.0f, "Backdrop glow  %.2f");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
        if (ImGui::Checkbox("Drifting glow", &state.settings.backdrop_drift))
            save = true;
        settings_help("Every so often the glow slowly turns to a different corner, so the backdrop never looks pinned in place. Long, gentle transitions.");
        if (ImGui::Checkbox("Smooth backdrop shader", &state.settings.backdrop_shader))
            save = true;
        settings_help("Draws the ambient wash as one full-screen shader pass with subtle dithering, which removes the colour steps OLED panels expose. It replaces the layered ellipse overdraw, so GPU use stays similar or lower.");
    }

    if (ImGui::Checkbox("Glowing separators", &state.settings.glow_separators))
        save = true;
    settings_help("A soft album-coloured light streak under each section header, plus a faint tint on panel edges and dividers. Follows the playing cover even when the full backdrop is off.");
    if (state.settings.glow_separators)
    {
        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderFloat("##glow_separator_strength", &state.settings.glow_separator_strength,
                           0.10f, 1.50f, "Separator glow  %.2f");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
    }

    if (ImGui::TreeNode("Fine-tune scale"))
    {
        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderFloat("##ui_text_scale", &state.settings.ui_text_scale, 0.5f, 2.0f, "Text  %.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderFloat("##ui_widget_scale", &state.settings.ui_widget_scale, 0.5f, 2.0f, "Widgets & spacing  %.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
        ImGui::SetNextItemWidth(field_w);
        ImGui::SliderFloat("##ui_layout_scale", &state.settings.ui_layout_scale, 0.5f, 2.0f, "Layout & sections  %.2fx");
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
        if (ui_ghost_button("Reset fine-tuning", ImVec2(150.0f * m.layout_scale, m.button_h)))
        {
            state.settings.ui_text_scale = 1.0f;
            state.settings.ui_widget_scale = 1.0f;
            state.settings.ui_layout_scale = 1.0f;
            save = true;
        }

        if (ImGui::TreeNode("Individual elements"))
        {
            settings_help("Fine-grained overrides for specific parts of the player. Leave at 1.00x unless one element feels off.");
            for (const auto &def : ui_element_defs())
            {
                auto it = state.settings.ui_elements.find(def.key);
                if (it == state.settings.ui_elements.end())
                    it = state.settings.ui_elements.emplace(def.key, 1.0f).first;
                ImGui::PushID(def.key);
                ImGui::SetNextItemWidth(field_w);
                ImGui::SliderFloat("##v", &it->second, 0.25f, 3.0f, def.label);
                if (ImGui::IsItemDeactivatedAfterEdit())
                    save = true;
                ImGui::PopID();
            }
            if (ui_ghost_button("Reset element sizes", ImVec2(160.0f * m.layout_scale, m.button_h)))
            {
                for (const auto &def : ui_element_defs())
                    state.settings.ui_elements[def.key] = 1.0f;
                save = true;
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    settings_gap();
    ui_section_header("Playback");

    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderFloat("##crossfade", &state.settings.crossfade_duration, 0.0f, 20.0f, "Crossfade  %.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    settings_help("Blend the end of one track into the beginning of the next (up to 20 s).");

    bool skip_fade = state.settings.skip_crossfade_enabled;
    if (ImGui::Checkbox("Crossfade manual skips", &skip_fade))
    {
        state.settings.skip_crossfade_enabled = skip_fade;
        save = true;
    }
    settings_help("When you press Next/Previous, blend into the target track if it is already downloaded.");

    ImGui::BeginDisabled(!state.settings.skip_crossfade_enabled);
    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderFloat("##skip_crossfade", &state.settings.skip_crossfade_duration, 0.5f, 5.0f,
                       "Skip crossfade  %.1f s");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    const char *skip_scopes[] = {
        "Next / Previous only",
        "Next / Previous + track clicks",
        "All manual track changes"};
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::Combo("##skip_crossfade_scope", &state.settings.skip_crossfade_scope,
                     skip_scopes, 3))
        save = true;
    ImGui::EndDisabled();
    settings_help("Skip fades are shorter than the end-of-track crossfade. Tracks that are not cached switch instantly.");

    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderFloat("##playback_speed", &state.settings.playback_speed, 0.5f, 2.0f,
                       "Playback speed  %.2g×");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    settings_help("Rate multiplier applied live to playback (0.5–2×). The bottom bar has quick presets.");

    bool vol_norm = state.settings.volume_normalization;
    if (ImGui::Checkbox("Volume normalization", &vol_norm))
    {
        state.settings.volume_normalization = vol_norm;
        save = true;
    }
    ImGui::SameLine();
    settings_help("Even out loudness between tracks (EBU R128 loudnorm). Takes effect immediately.");

    bool eq_enabled = state.settings.eq_enabled;
    if (ImGui::Checkbox("Equalizer", &eq_enabled))
    {
        state.settings.eq_enabled = eq_enabled;
        save = true;
    }
    ImGui::BeginDisabled(!state.settings.eq_enabled);
    const char *band_labels[5] = {"60 Hz", "230 Hz", "910 Hz", "3.6 kHz", "14 kHz"};
    for (int i = 0; i < 5; ++i)
    {
        ImGui::SetNextItemWidth(field_w);
        char sid[24];
        snprintf(sid, sizeof(sid), "##eq_band_%d", i);
        char label[32];
        snprintf(label, sizeof(label), "%s  %+.1f dB", band_labels[i],
                 (double)state.settings.eq_gains[i]);
        ImGui::SliderFloat(sid, &state.settings.eq_gains[i], -12.0f, 12.0f, label);
        if (ImGui::IsItemDeactivatedAfterEdit())
            save = true;
    }
    if (ui_ghost_button("Reset equalizer", ImVec2(160.0f * m.layout_scale, m.control_h)))
    {
        for (float &g : state.settings.eq_gains)
            g = 0.0f;
        save = true;
    }
    ImGui::EndDisabled();
    settings_help("Five-band equalizer (60 Hz – 14 kHz), applied immediately.");

    bool server_sync = state.settings.server_sync;
    if (ImGui::Checkbox("Sync to YouTube Music", &server_sync))
    {
        state.settings.server_sync = server_sync;
        writeback_set_enabled(server_sync);
        save = true;
    }
    settings_help("Off by default. When on, likes and playlist edits also apply to your signed-in YouTube Music account: each playlist gets a private server-side mirror that keeps in sync (renames, adds, removals, reordering, deletion).");

    ImGui::SetNextItemWidth(field_w);
    ImGui::InputText("##discord_id", &state.settings.discord_client_id);
    if (ImGui::IsItemEdited())
        save = true;
    settings_help("Discord Rich Presence application client id (empty = off). Create an app at discord.com/developers/applications, copy its Application ID, restart crossfade after pasting.");

    bool notif = state.settings.show_notifications;
    if (ImGui::Checkbox("Desktop notifications", &notif))
    {
        state.settings.show_notifications = notif;
        save = true;
    }

    bool endless = state.settings.endless_radio;
    if (ImGui::Checkbox("Endless music", &endless))
    {
        state.settings.endless_radio = endless;
        save = true;
    }
    settings_help("When the list and queue run out, keep playing a radio seeded by the last song.");

    // ------------------------------------------------------------------
    // Party mode. The Party tab is only the start/live surface; everything
    // that can be configured ahead of time lives here so the tab stays a
    // one-click experience.
    // ------------------------------------------------------------------
    const float party_section_y = ImGui::GetCursorPosY();
    settings_gap();
    ui_section_header("Party", "Let guests add songs from their phones");
    settings_help("Start a party from the Party tab. Guests scan its QR code with a phone camera, paste a YouTube or YouTube Music link, and their songs join Up Next. Nothing leaves your local network - there is no account, no cloud, and no telemetry.");

    PartySettings &ps = state.settings.party;
    if (party_running())
    {
        ImGui::TextColored(ui_c().accent_text,
                           "A party is running. Changes here apply the next time you start one.");
    }

    ImGui::TextColored(ui_c().text_muted, "Party name");
    ImGui::SetNextItemWidth(field_w);
    ImGui::InputText("##party_name", &ps.name);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        ps.name = party_util::clean_utf8(ps.name, 60);
        if (ps.name.empty())
            ps.name = "crossfade party";
        save = true;
    }
    settings_help("Shown on the host screen and at the top of the guest page.");

    ImGui::Dummy(ImVec2(0.0f, m.space_3));
    ImGui::TextColored(ui_c().text_muted, "Network");
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
    settings_help("Pick the Wi-Fi or hotspot address phones can reach. Automatic skips virtual adapters like Docker and VPNs and prefers the address your network actually routes.");
    if (!ifaces.empty())
        settings_help(("Automatic will use " + ifaces.front().ip + ".").c_str());

    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    ImGui::TextColored(ui_c().text_muted, "Port");
    ImGui::SetNextItemWidth(160.0f * m.layout_scale);
    ImGui::InputInt("##party_port", &ps.port, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        ps.port = std::max(1024, std::min(65535, ps.port));
        save = true;
    }
    settings_help("The guest page is served on this port. If it is busy, start the party again on another port.");

    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
    ImGui::TextColored(ui_c().text_muted,
                       "Hotspots and public Wi-Fi often isolate clients. If a phone cannot open the link, make this laptop the hotspot, use a normal router, or disable client isolation on the network. On Android 16 and newer, Chrome may also need Nearby devices permission (or try Firefox).");
    ImGui::PopTextWrapPos();

    ImGui::Dummy(ImVec2(0.0f, m.space_3));
    ImGui::TextColored(ui_c().text_muted, "Who can add songs");
    if (ImGui::Checkbox("Host approves every request", &ps.require_approval))
        save = true;
    settings_help("When on, new songs wait in the Party tab until you approve them. When off, they join Up Next immediately.");

    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderInt("##party_max_guest", &ps.max_per_guest, 0, 20,
                     ps.max_per_guest == 0 ? "Per guest  unlimited" : "Per guest  %d songs");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderInt("##party_cooldown", &ps.cooldown_sec, 0, 60,
                     ps.cooldown_sec == 0 ? "Cooldown  off" : "Cooldown  %d s between adds");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderInt("##party_max_minutes", &ps.max_video_minutes, 0, 60,
                     ps.max_video_minutes == 0 ? "Max video length  unlimited"
                                               : "Max video length  %d min");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderInt("##party_max_queue", &ps.max_queue, 0, 300,
                     ps.max_queue == 0 ? "Queue cap  unlimited" : "Queue cap  %d songs");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    settings_help("Limits apply per guest. Queue cap counts guest requests waiting in Up Next; your own playlist does not use it.");

    ImGui::Dummy(ImVec2(0.0f, m.space_2));
    ImGui::TextColored(ui_c().text_muted, "Guest experience");
    if (ImGui::Checkbox("Let guests search YouTube", &ps.allow_search))
        save = true;
    settings_help("Turns the guest search box into a paste-only field when off. Search runs through the same local backend as the app.");
    if (ImGui::Checkbox("Allow duplicate songs", &ps.allow_duplicates))
        save = true;
    if (ImGui::Checkbox("Show guest names next to songs", &ps.show_names))
        save = true;

    if (state.settings_focus_party)
    {
        ImGui::SetScrollY(std::max(0.0f, party_section_y - m.space_4));
        state.settings_focus_party = false;
    }

    settings_gap();
    ui_section_header("Lyrics");
    ImGui::SetNextItemWidth(field_w);
    ImGui::SliderFloat("##lyrics_offset", &state.settings.lyrics_offset, -5.0f, 5.0f, "Timing offset  %+.2f s");
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    settings_help("Positive shows lyrics later, negative earlier. Takes effect immediately.");

    settings_gap();
    ui_section_header("YouTube Music");

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
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::Combo("##browser", &current_browser, browsers, 6))
    {
        state.settings.ytdl_browser = browsers[current_browser];
        save = true;
    }
    settings_help("Browser yt-dlp reads cookies from for private or age-restricted tracks.");

    settings_gap();
    const char *auth_modes[] = {"Browser cookies (browser.json)", "OAuth (browseroauth.json)"};
    int auth_mode = state.settings.auth_mode == "oauth" ? 1 : 0;
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::Combo("##auth_mode", &auth_mode, auth_modes, 2))
    {
        state.settings.auth_mode = auth_mode == 1 ? "oauth" : "browser";
        set_auth_mode(state.settings.auth_mode);
        save = true;
    }
    settings_help("Choose how the YouTube Music backend signs in. OAuth opens a browser once and saves browseroauth.json. Restart crossfade after changing this.");
    if (state.settings.auth_mode == "oauth" && !std::filesystem::exists(backend_auth_file_path()))
        ImGui::TextColored(ui_c().accent_text, "No OAuth token yet - use the login command below.");

    settings_gap();
    if (ImGui::Checkbox("Fast stream resolve (experimental)", &state.settings.warm_stream_resolver))
    {
        set_warm_stream_resolver(state.settings.warm_stream_resolver);
        save = true;
    }
    settings_help("Resolves song stream links inside the backend instead of spawning yt-dlp each time, so uncached songs start faster. Takes effect after restarting p100. Turn this off if playback misbehaves.");

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

    settings_gap();
    ui_section_header("Keyboard Shortcuts");
    ImGui::TextDisabled("Space  Play / pause          Left / Right  Seek 5s");
    ImGui::TextDisabled("Shift + arrows  Seek 20s    Ctrl + Left / Right  Previous / next");
    ImGui::TextDisabled("Up / Down  Volume            / or Ctrl+F  Focus search");
    ImGui::TextDisabled("L  Toggle lyrics            S  Toggle sidebar");
    ImGui::TextDisabled("Esc  Back / clear search      ?  Shortcut help");

    settings_gap();
    ui_section_header("Library Maintenance");

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
                std::thread(repair_cache_worker, &state, std::move(snapshot)).detach();
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

    // Slider from 0 (unlimited, the default) to 20 GB; snaps to whole GB.
    float cap_gb = state.settings.download_cap_gb;
    ImGui::SetNextItemWidth(field_w);
    if (ImGui::SliderFloat("##dl_cap", &cap_gb, 0.0f, 20.0f,
                           cap_gb < 0.5f ? "Unlimited" : "%.0f GB",
                           ImGuiSliderFlags_AlwaysClamp))
    {
        cap_gb = cap_gb < 0.5f ? 0.0f : std::floor(cap_gb + 0.5f);
        if (cap_gb != state.settings.download_cap_gb)
        {
            state.settings.download_cap_gb = cap_gb;
            set_audio_cache_cap((double)cap_gb * 1024.0 * 1024.0 * 1024.0);
            save = true;
        }
    }
    settings_help("Keep at most this much downloaded audio (0 = unlimited). When the cap is reached, the least-listened songs are removed first.");

    settings_gap();
    ui_section_header("Diagnostics");
    settings_help("Local-only troubleshooting. Nothing here is uploaded automatically.");
    {
        const std::string status = library::status_text();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopTextWrapPos();
        if (!library::backup_dir().empty())
            settings_help(("Legacy JSON backup: " + library::backup_dir()).c_str());

        const float bw = 176.0f * m.layout_scale;
        if (ui_ghost_button("Copy diagnostics", ImVec2(bw, m.control_h)))
        {
            ImGui::SetClipboardText(diagnostics::bundle().c_str());
            ui_notify(UiNotifyType::Success, "Diagnostics copied",
                      "Version, paths, library status and recent log lines are on the clipboard.", 4.0f);
        }
        ImGui::SameLine(0.0f, m.space_2);
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

        bool safe = state.settings.safe_mode;
        if (ImGui::Checkbox("Safe mode on next launch", &safe))
        {
            state.settings.safe_mode = safe;
            save = true;
        }
        settings_help("Disables the backdrop shader, auto-update checks and optional background integrations. Use it if a new build crashes or renders badly.");

        if (ImGui::TreeNode("Recent diagnostics"))
        {
            const std::string log = diagnostics::recent_log();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
            ImGui::TextUnformatted(log.empty() ? "No log lines yet." : log.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TreePop();
        }
    }

    settings_gap();
    ui_section_header("Notes");
    ImGui::InputTextMultiline("##notes", &state.settings.notes,
                              ImVec2(content_w, ImGui::GetTextLineHeight() * 6),
                              ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_WordWrap);
    if (ImGui::IsItemDeactivatedAfterEdit())
        save = true;
    settings_help("Kept in settings.json and saved when you leave the box.");

    settings_gap();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.42f, 0.42f, 1.0f));
    ImGui::TextUnformatted("Danger Zone");
    ImGui::PopStyleColor();
    settings_help("These actions remove local data. Most offer an Undo toast right after they run.");

    const float danger_w = std::max(220.0f * m.layout_scale,
                                    ImGui::CalcTextSize("Clear download cache").x +
                                        ImGui::GetStyle().FramePadding.x * 2.0f + 4.0f);
    auto danger_row = [&](const char *label, const char *desc, const char *help) -> bool
    {
        const bool clicked = ui_secondary_button(label, ImVec2(danger_w, m.button_h));
        ImGui::SameLine();
        settings_help(desc);
        (void)help;
        ImGui::Dummy(ImVec2(0.0f, m.space_1));
        return clicked;
    };

    if (danger_row("Clear download cache", "Delete every downloaded song from disk.", nullptr))
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
    if (danger_row("Reset liked songs", "Remove all liked songs.", nullptr))
    {
        begin_reset(state, ResetKind::Likes);
        g_reset_snapshot.likes = state.liked_tracks;
        state.liked_tracks.clear();
        save_liked_tracks(state.liked_tracks);
        ui_notify(UiNotifyType::Warning, "Liked songs reset", "All liked songs were removed.",
                  6.0f, "Undo", &undo_reset);
    }
    if (danger_row("Reset saved albums", "Remove all saved albums.", nullptr))
    {
        begin_reset(state, ResetKind::Albums);
        g_reset_snapshot.albums = state.saved_albums;
        state.saved_albums.clear();
        save_saved_albums(state.saved_albums);
        ui_notify(UiNotifyType::Warning, "Saved albums reset", "All saved albums were removed.",
                  6.0f, "Undo", &undo_reset);
    }
    if (danger_row("Reset saved artists", "Remove all saved artists.", nullptr))
    {
        begin_reset(state, ResetKind::Artists);
        g_reset_snapshot.artists = state.saved_artists;
        state.saved_artists.clear();
        save_saved_artists(state.saved_artists);
        ui_notify(UiNotifyType::Warning, "Saved artists reset", "All saved artists were removed.",
                  6.0f, "Undo", &undo_reset);
    }
    if (danger_row("Reset playlists", "Delete all playlists.", nullptr))
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

    if (save)
        save_settings(state.settings);
}
