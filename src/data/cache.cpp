#include "ui.h"
#include "library.h"
#include "paths.h"
#include "persist_util.h"
#include "ytdlp.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>

using persist::extract_int_field;
using persist::extract_str_field;
using persist::json_escape;

static std::mutex g_album_cache_mutex;
static std::mutex g_artist_cache_mutex;

std::string lyrics_dir()
{
    return p100_config_path("lyrics");
}

std::string lyrics_cache_path(const std::string &video_id)
{
    return lyrics_dir() + "/" + sanitize_filename(video_id) + ".json";
}

bool load_lyrics_cache(const std::string &video_id, Lyrics &out)
{
    if (video_id.empty())
        return false;
    std::ifstream in(lyrics_cache_path(video_id));
    if (!in.good())
        return false;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    return parse_lyrics_json(json, out);
}

void save_lyrics_cache(const std::string &video_id, const Lyrics &lyrics)
{
    if (video_id.empty())
        return;
    std::string dir = lyrics_dir();
    mkdir(dir.c_str(), 0755);

    std::ofstream out(lyrics_cache_path(video_id));
    if (!out)
        return;
    if (lyrics.none)
    {
        out << "{\"none\":true}";
        out.close();
        return;
    }
    out << "{\"synced\":" << (lyrics.synced ? "true" : "false")
        << ",\"source\":\"" << json_escape(lyrics.source) << "\",\"lines\":[";
    for (size_t i = 0; i < lyrics.lines.size(); ++i)
    {
        if (i != 0)
            out << ",";
        int t = i < lyrics.times.size() ? lyrics.times[i] : 0;
        out << "{\"t\":" << t << ",\"text\":\"" << json_escape(lyrics.lines[i]) << "\"}";
    }
    out << "]}";
    out.close();
}

void load_downloaded_tracks(AppState &state)
{
    state.downloaded_tracks = get_cached_tracks();
    if (library::available())
        library::save_downloads(state.downloaded_tracks);
    state.downloaded_loaded = true;
}

void delete_downloaded_track(AppState &state, std::string id)
{
    if (id.empty())
        return;

    // Block a finalize thread that's still fetching metadata for this track:
    // it would otherwise write a .meta.json (and via the EOF rename even
    // re-create the audio) right after this delete.
    cancel_pending_metadata(id);

    // Remove the cached audio file and its metadata sidecar from disk.
    std::string path = find_cached_audio_path(id);
    if (!path.empty())
    {
        std::filesystem::remove(path);
        std::filesystem::remove(path + ".meta.json");
    }
    // Also drop cached lyrics so a future re-download starts clean.
    std::filesystem::remove(lyrics_cache_path(id));

    // Drop it from the visible download lists.
    auto erase_id = [&](std::vector<Track> &v)
    {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Track &t) { return t.id == id; }),
                v.end());
    };
    erase_id(state.downloaded_tracks);
    if (state.local_search_tab == Tab::Downloads)
        erase_id(state.local_search_results);
}

std::string album_cache_dir()
{
    return p100_config_path("album_cache");
}

std::string album_cache_path(const std::string &browse_id)
{
    return album_cache_dir() + "/" + sanitize_filename(browse_id) + ".json";
}

static void write_album_tracks(std::ostream &out, const std::vector<Track> &tracks)
{
    out << "[";
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto &t = tracks[i];
        if (i != 0)
            out << ",";
        out << "{\"id\":\"" << json_escape(t.id)
            << "\",\"title\":\"" << json_escape(t.title)
            << "\",\"artist\":\"" << json_escape(t.artist)
            << "\",\"artist_id\":\"" << json_escape(t.artist_id)
            << "\",\"album\":\"" << json_escape(t.album)
            << "\",\"album_id\":\"" << json_escape(t.album_id)
            << "\",\"track_number\":" << t.track_number
            << ",\"thumbnail_url\":\"" << json_escape(t.thumbnail_url)
            << "\",\"duration_sec\":" << t.duration_sec
            << ",\"url\":\"" << json_escape(t.url) << "\"}";
    }
    out << "]";
}

void save_album_cache(const Album &album)
{
    if (album.browse_id.empty())
        return;
    std::lock_guard<std::mutex> lock(g_album_cache_mutex);
    std::string dir = album_cache_dir();
    mkdir(dir.c_str(), 0755);

    std::string path = album_cache_path(album.browse_id);
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp);
    if (!out)
        return;
    out << "{\"browse_id\":\"" << json_escape(album.browse_id)
        << "\",\"title\":\"" << json_escape(album.title)
        << "\",\"artist\":\"" << json_escape(album.artist)
        << "\",\"year\":\"" << json_escape(album.year)
        << "\",\"duration\":\"" << json_escape(album.duration)
        << "\",\"description\":\"" << json_escape(album.description)
        << "\",\"thumbnail_url\":\"" << json_escape(album.thumbnail_url)
        << "\",\"audio_playlist_id\":\"" << json_escape(album.audio_playlist_id)
        << "\",\"track_count\":" << album.track_count
        << ",\"tracks\":";
    write_album_tracks(out, album.tracks);
    out << "}";
    out.close();

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
        std::filesystem::remove(tmp, ec);
}

bool load_album_cache(const std::string &browse_id, Album &out)
{
    if (browse_id.empty())
        return false;

    std::ifstream in(album_cache_path(browse_id));
    if (!in.good())
        return false;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (json.empty())
        return false;

    out = Album{};
    out.browse_id = extract_str_field(json, "browse_id");
    if (out.browse_id.empty())
        out.browse_id = browse_id;
    out.title = extract_str_field(json, "title");
    out.artist = extract_str_field(json, "artist");
    out.year = extract_str_field(json, "year");
    out.duration = extract_str_field(json, "duration");
    out.description = extract_str_field(json, "description");
    out.thumbnail_url = extract_str_field(json, "thumbnail_url");
    out.audio_playlist_id = extract_str_field(json, "audio_playlist_id");
    out.track_count = extract_int_field(json, "track_count");
    out.tracks = parse_track_array(json);
    if (out.track_count == 0)
        out.track_count = (int)out.tracks.size();
    return !out.title.empty() || !out.tracks.empty();
}

void prefetch_album_cache(const std::string &browse_id)
{
    if (browse_id.empty())
        return;
    std::string path = album_cache_path(browse_id);
    std::thread([browse_id, path]() {
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
            return;
        Album album;
        if (fetch_album(browse_id, album))
            save_album_cache(album);
    }).detach();
}

std::string artist_cache_dir()
{
    return p100_config_path("artist_cache");
}

std::string artist_cache_path(const std::string &browse_id)
{
    return artist_cache_dir() + "/" + sanitize_filename(browse_id) + ".json";
}

void save_artist_cache(const ArtistInfo &artist)
{
    if (artist.browse_id.empty())
        return;
    std::lock_guard<std::mutex> lock(g_artist_cache_mutex);
    std::string dir = artist_cache_dir();
    mkdir(dir.c_str(), 0755);

    std::string path = artist_cache_path(artist.browse_id);
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp);
    if (!out)
        return;
    out << "{\"browse_id\":\"" << json_escape(artist.browse_id)
        << "\",\"name\":\"" << json_escape(artist.name)
        << "\",\"description\":\"" << json_escape(artist.description)
        << "\",\"subscribers\":\"" << json_escape(artist.subscribers)
        << "\",\"monthly_listeners\":\"" << json_escape(artist.monthly_listeners)
        << "\",\"thumbnail_url\":\"" << json_escape(artist.thumbnail_url)
        << "\",\"songs_browse_id\":\"" << json_escape(artist.songs_browse_id)
        << "\",\"songs\":";
    write_album_tracks(out, artist.songs);
    out << ",\"albums\":[";
    for (size_t i = 0; i < artist.albums.size(); ++i)
    {
        const auto &a = artist.albums[i];
        if (i != 0)
            out << ",";
        out << "{\"browse_id\":\"" << json_escape(a.browse_id)
            << "\",\"title\":\"" << json_escape(a.title)
            << "\",\"year\":\"" << json_escape(a.year)
            << "\",\"thumbnail_url\":\"" << json_escape(a.thumbnail_url)
            << "\",\"type\":\"" << json_escape(a.type) << "\"}";
    }
    out << "]}";
    out.close();

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
        std::filesystem::remove(tmp, ec);
}

bool load_artist_cache(const std::string &browse_id, ArtistInfo &out)
{
    if (browse_id.empty())
        return false;

    std::ifstream in(artist_cache_path(browse_id));
    if (!in.good())
        return false;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (json.empty())
        return false;

    out = ArtistInfo{};
    out.browse_id = extract_str_field(json, "browse_id");
    if (out.browse_id.empty())
        out.browse_id = browse_id;
    out.name = extract_str_field(json, "name");
    out.description = extract_str_field(json, "description");
    out.subscribers = extract_str_field(json, "subscribers");
    out.monthly_listeners = extract_str_field(json, "monthly_listeners");
    out.thumbnail_url = extract_str_field(json, "thumbnail_url");
    out.songs_browse_id = extract_str_field(json, "songs_browse_id");
    out.songs = parse_track_array(json);

    size_t albums_key = json.find("\"albums\":");
    if (albums_key != std::string::npos)
    {
        size_t pos = albums_key;
        while ((pos = json.find("\"browse_id\":\"", pos)) != std::string::npos)
        {
            size_t next = json.find("\"browse_id\":\"", pos + 1);
            std::string obj = next == std::string::npos ? json.substr(pos)
                                                        : json.substr(pos, next - pos);
            ArtistAlbum a;
            a.browse_id = extract_str_field(obj, "browse_id");
            a.title = extract_str_field(obj, "title");
            a.year = extract_str_field(obj, "year");
            a.thumbnail_url = extract_str_field(obj, "thumbnail_url");
            a.type = extract_str_field(obj, "type");
            if (!a.browse_id.empty())
                out.albums.push_back(std::move(a));
            if (next == std::string::npos)
                break;
            pos = next;
        }
    }
    return !out.name.empty() || !out.songs.empty();
}

void prefetch_artist_cache(const std::string &browse_id)
{
    if (browse_id.empty())
        return;
    std::string path = artist_cache_path(browse_id);
    std::thread([browse_id, path]() {
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
            return;
        ArtistInfo artist;
        if (fetch_artist(browse_id, artist))
            save_artist_cache(artist);
    }).detach();
}
