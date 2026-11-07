#include "session.h"
#include "mpvplayer.h"
#include "paths.h"
#include "json_util.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

// ----------------------------------------------------------------------
// Session persistence
// ----------------------------------------------------------------------
static std::string session_file_path()
{
    return p100_config_path("session.json");
}

const char *playlist_source_name(PlaylistSource source)
{
    switch (source)
    {
    case PlaylistSource::HOME: return "home";
    case PlaylistSource::SEARCH: return "search";
    case PlaylistSource::DOWNLOADS: return "downloads";
    case PlaylistSource::RADIO: return "radio";
    case PlaylistSource::LIKED: return "liked";
    case PlaylistSource::PLAYLIST: return "playlist";
    case PlaylistSource::ALBUM: return "album";
    case PlaylistSource::ARTIST: return "artist";
    case PlaylistSource::PODCAST: return "podcast";
    case PlaylistSource::HISTORY: return "history";
    case PlaylistSource::STATS: return "stats";
    case PlaylistSource::SMART: return "smart";
    case PlaylistSource::YOUTUBE_PLAYLIST: return "youtube_playlist";
    }
    return "home";
}

PlaylistSource playlist_source_from_name(const std::string &name)
{
    for (PlaylistSource s : {PlaylistSource::HOME, PlaylistSource::SEARCH,
                             PlaylistSource::DOWNLOADS, PlaylistSource::RADIO,
                             PlaylistSource::LIKED, PlaylistSource::PLAYLIST,
                             PlaylistSource::ALBUM, PlaylistSource::ARTIST,
                             PlaylistSource::PODCAST, PlaylistSource::HISTORY,
                             PlaylistSource::STATS, PlaylistSource::SMART,
                             PlaylistSource::YOUTUBE_PLAYLIST})
    {
        if (name == playlist_source_name(s))
            return s;
    }
    return PlaylistSource::HOME;
}

std::string session_json(const AppState &state, double position, int volume)
{
    auto esc = [](const std::string &s) { return jsonutil::escape(s); };

    std::ostringstream out;
    out << "{"
        << "\"track_id\":\"" << esc(state.current_track_id) << "\","
        << "\"track_title\":\"" << esc(state.current_title) << "\","
        << "\"track_artist\":\"" << esc(state.current_artist) << "\","
        << "\"track_thumbnail\":\"" << esc(state.current_thumbnail_url) << "\","
        << "\"track_url\":\"" << esc(state.current_track_url) << "\","
        << "\"track_album_id\":\"" << esc(state.current_album_id) << "\","
        << "\"track_artist_id\":\"" << esc(state.current_artist_id) << "\","
        << "\"source\":\"";
    out << playlist_source_name(state.current_playlist_source);
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
        << "\"position\":" << position << ","
        << "\"volume\":" << volume << ","
        << "\"playing\":" << (state.playing ? "true" : "false") << ","
        << "\"search_query\":\"" << esc(state.search_buffer) << "\""
        << "}";
    return out.str();
}

void save_session(const AppState &state, const MpvPlayer &player, int volume_override)
{
    std::string path = session_file_path();
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);

    const int volume = volume_override >= 0
                           ? volume_override
                           : (int)player.get_property_double("volume");
    std::ofstream out(path);
    if (!out)
        return;
    out << session_json(state, player.get_property_double("time-pos"), volume);
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

    auto get_str = [&](const std::string &key)
    { return jsonutil::get_string(json, key); };

    auto get_int = [&](const std::string &key)
    { return jsonutil::get_int(json, key, -1); };

    auto get_double = [&](const std::string &key)
    { return jsonutil::get_double(json, key, 0.0); };

    auto get_bool = [&](const std::string &key)
    { return jsonutil::get_bool(json, key, false); };

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
