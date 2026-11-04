#include "ui.h"
#include "mpvplayer.h"
#include "paths.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>

// ----------------------------------------------------------------------
// Session persistence
// ----------------------------------------------------------------------
static std::string session_file_path()
{
    return p100_config_path("session.json");
}

void save_session(const AppState &state, const MpvPlayer &player, int volume_override)
{
    std::string path = session_file_path();
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);

    auto esc = [](const std::string &s) -> std::string
    {
        std::string r;
        r.reserve(s.size() + 8);
        for (char c : s)
        {
            if (c == '"' || c == '\\') r += '\\';
            r += c;
        }
        return r;
    };

    std::ofstream out(path);
    if (!out)
        return;

    out << "{"
        << "\"track_id\":\"" << esc(state.current_track_id) << "\","
        << "\"track_title\":\"" << esc(state.current_title) << "\","
        << "\"track_artist\":\"" << esc(state.current_artist) << "\","
        << "\"track_thumbnail\":\"" << esc(state.current_thumbnail_url) << "\","
        << "\"track_url\":\"" << esc(state.current_track_url) << "\","
        << "\"track_album_id\":\"" << esc(state.current_album_id) << "\","
        << "\"track_artist_id\":\"" << esc(state.current_artist_id) << "\","
        << "\"source\":\"";
    switch (state.current_playlist_source)
    {
    case PlaylistSource::HOME:      out << "home"; break;
    case PlaylistSource::SEARCH:    out << "search"; break;
    case PlaylistSource::DOWNLOADS: out << "downloads"; break;
    case PlaylistSource::RADIO:     out << "radio"; break;
    case PlaylistSource::LIKED:     out << "liked"; break;
    case PlaylistSource::PLAYLIST:  out << "playlist"; break;
    case PlaylistSource::ALBUM:     out << "album"; break;
    case PlaylistSource::ARTIST:    out << "artist"; break;
    default:                        out << "home"; break;
    }
    out << "\","
        << "\"playlist_name\":\"" << esc(
            state.current_playlist_source == PlaylistSource::PLAYLIST &&
            state.selected_playlist_index >= 0 &&
            state.selected_playlist_index < (int)state.playlists.size()
                ? state.playlists[state.selected_playlist_index].name
                : "") << "\","
        << "\"album_browse_id\":\"" << esc(state.album_browse_id) << "\","
        << "\"album_title\":\"" << esc(state.album_title) << "\","
        << "\"album_artist\":\"" << esc(state.album_artist) << "\","
        << "\"artist_browse_id\":\"" << esc(state.artist_browse_id) << "\","
        << "\"artist_name\":\"" << esc(state.artist_name) << "\","
        << "\"index\":" << state.current_track_index << ","
        << "\"position\":" << player.get_property_double("time-pos") << ","
        << "\"volume\":" << (volume_override >= 0
                                 ? volume_override
                                 : (int)player.get_property_double("volume")) << ","
        << "\"playing\":" << (state.playing ? "true" : "false") << ","
        << "\"search_query\":\"" << esc(state.search_buffer) << "\""
        << "}";
    out.close();
}


void load_session(AppState &state)
{
    std::ifstream in(session_file_path());
    if (!in.good())
        return;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    if (json.empty())
        return;

    auto get_str = [&](const std::string &key) -> std::string
    {
        std::string search = "\"" + key + "\":\"";
        size_t start = json.find(search);
        if (start == std::string::npos)
            return "";
        start += search.size(); // past the opening quote of the value

        std::string out;
        out.reserve(256);
        for (size_t i = start; i < json.size(); ++i)
        {
            char c = json[i];
            if (c == '\\' && i + 1 < json.size())
            {
                out += json[i + 1];
                i++;
            }
            else if (c == '"')
                break;
            else
                out += c;
        }
        return out;
    };

    auto get_int = [&](const std::string &key) -> int
    {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return -1;
        pos += search.size();
        size_t end = json.find_first_of(",}", pos);
        if (end == std::string::npos)
            return -1;
        std::string val = json.substr(pos, end - pos);
        val.erase(0, val.find_first_not_of(" \t\n\r"));
        val.erase(val.find_last_not_of(" \t\n\r") + 1);
        if (val.empty())
            return -1;
        try { return std::stoi(val); }
        catch (...) { return -1; }
    };

    auto get_double = [&](const std::string &key) -> double
    {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return 0.0;
        pos += search.size();
        size_t end = json.find_first_of(",}", pos);
        if (end == std::string::npos)
            return 0.0;
        std::string val = json.substr(pos, end - pos);
        val.erase(0, val.find_first_not_of(" \t\n\r"));
        val.erase(val.find_last_not_of(" \t\n\r") + 1);
        if (val.empty())
            return 0.0;
        try { return std::stod(val); }
        catch (...) { return 0.0; }
    };

    auto get_bool = [&](const std::string &key) -> bool
    {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos)
            return false;
        pos += search.size();
        if (json.compare(pos, 4, "true") == 0)
            return true;
        if (json.compare(pos, 5, "false") == 0)
            return false;
        return false;
    };

    SessionState &restore = state.session_restore;
    restore.track_id = get_str("track_id");
    if (restore.track_id.empty())
        return;

    restore.track_title      = get_str("track_title");
    restore.track_artist     = get_str("track_artist");
    restore.track_thumbnail  = get_str("track_thumbnail");
    restore.track_url        = get_str("track_url");
    restore.track_album_id   = get_str("track_album_id");
    restore.track_artist_id  = get_str("track_artist_id");
    restore.source           = get_str("source");
    restore.playlist_name    = get_str("playlist_name");
    restore.index            = get_int("index");
    restore.position         = get_double("position");
    restore.volume           = get_int("volume");
    restore.playing          = get_bool("playing");
    restore.search_query     = get_str("search_query");

    // Album restore hints
    state.album_browse_id = get_str("album_browse_id");
    state.album_title = get_str("album_title");
    state.album_artist = get_str("album_artist");
    state.artist_browse_id = get_str("artist_browse_id");
    state.artist_name = get_str("artist_name");

    if (restore.source == "album" && !state.album_browse_id.empty())
    {
        Album album;
        if (load_album_cache(state.album_browse_id, album))
            state.album_tracks = std::move(album.tracks);
    }
    if (restore.source == "artist" && !state.artist_browse_id.empty())
    {
        ArtistInfo artist;
        if (load_artist_cache(state.artist_browse_id, artist))
        {
            state.artist_name = artist.name;
            state.artist_thumbnail_url = artist.thumbnail_url;
            state.artist_songs = std::move(artist.songs);
        }
    }

    state.session_restore_pending = true;
    fprintf(stderr, "Session restored: track=%s pos=%.1f vol=%d playing=%d\n",
            restore.track_id.c_str(), restore.position, restore.volume, (int)restore.playing);
}
