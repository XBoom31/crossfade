#include "ui.h"
#include "library.h"
#include "paths.h"
#include "persist_util.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>

std::string sanitize_filename(const std::string &name)
{
    return persist::sanitize_filename(name);
}

using persist::json_escape;

namespace
{
std::string xml_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c; break;
        }
    }
    return out;
}
} // namespace

std::string playlist_m3u(const Playlist &pl)
{
    std::ostringstream out;
    out << "#EXTM3U\n";
    for (const Track &t : pl.tracks)
    {
        out << "#EXTINF:" << t.duration_sec << "," << t.artist << " - " << t.title
            << "\n"
            << t.url << "\n";
    }
    return out.str();
}

std::string playlist_xspf(const Playlist &pl)
{
    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\">\n"
        << "  <title>" << xml_escape(pl.name) << "</title>\n"
        << "  <trackList>\n";
    for (const Track &t : pl.tracks)
    {
        out << "    <track>\n"
            << "      <location>" << xml_escape(t.url) << "</location>\n"
            << "      <title>" << xml_escape(t.title) << "</title>\n"
            << "      <creator>" << xml_escape(t.artist) << "</creator>\n"
            << "      <album>" << xml_escape(t.album) << "</album>\n"
            << "      <duration>" << t.duration_sec * 1000 << "</duration>\n"
            << "    </track>\n";
    }
    out << "  </trackList>\n</playlist>\n";
    return out.str();
}

std::string playlists_dir()
{
    return p100_config_path("playlists");
}

void ensure_playlists_dir()
{
    std::string dir = playlists_dir();
    mkdir(dir.c_str(), 0755);
}

std::string playlist_path(const std::string &name)
{
    return playlists_dir() + "/" + sanitize_filename(name) + ".json";
}

void save_playlist(const std::string &name, const std::vector<Track> &tracks)
{
    if (library::available())
    {
        library::save_playlist(name, tracks);
        return;
    }

    ensure_playlists_dir();
    std::string path = playlist_path(name);
    std::ofstream out(path);
    if (!out)
        return;
    out << "[";
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        const auto &t = tracks[i];
        out << "{\"id\":\"" << t.id << "\","
            << "\"title\":\"" << t.title << "\","
            << "\"artist\":\"" << t.artist << "\","
            << "\"artist_id\":\"" << t.artist_id << "\","
            << "\"album\":\"" << t.album << "\","
            << "\"album_id\":\"" << t.album_id << "\","
            << "\"track_number\":" << t.track_number << ","
            << "\"thumbnail_url\":\"" << t.thumbnail_url << "\","
            << "\"duration_sec\":" << t.duration_sec << ","
            << "\"url\":\"" << t.url << "\"}";
        if (i != tracks.size() - 1)
            out << ",";
    }
    out << "]";
    out.close();
}

std::vector<Track> load_playlist_tracks(const std::string &name)
{
    if (library::available())
    {
        return library::load_playlist_tracks(name);
    }

    std::string path = playlist_path(name);
    std::ifstream in(path);
    if (!in.good())
        return {};
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    return parse_track_array(json);
}

std::vector<std::string> get_playlist_names()
{
    if (library::available())
    {
        return library::playlist_names();
    }

    std::vector<std::string> names;
    std::string dir = playlists_dir();
    if (!std::filesystem::exists(dir))
        return names;
    for (const auto &entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        // <name>.meta.json is the server-link sidecar, not a playlist.
        if (entry.path().stem().extension() == ".meta")
            continue;
        names.push_back(entry.path().stem().string());
    }
    return names;
}

void delete_playlist(const std::string &name)
{
    if (library::available())
    {
        library::delete_playlist(name);
        return;
    }

    std::string path = playlist_path(name);
    std::filesystem::remove(path);
}

void rename_playlist(const std::string &old_name, const std::string &new_name)
{
    std::string old_path = playlist_path(old_name);
    std::string new_path = playlist_path(new_name);
    std::filesystem::rename(old_path, new_path);
}
