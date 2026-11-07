#include "ui.h"
#include "library.h"
#include "paths.h"
#include "persist_util.h"
#include "writeback.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

using persist::extract_int_field;
using persist::extract_str_field;
using persist::json_escape;
using persist::track_to_json;

std::string liked_file_path()
{
    return p100_config_path("liked.json");
}

void save_liked_tracks(const std::vector<Track> &tracks)
{
    if (library::available())
    {
        library::save_liked(tracks);
        return;
    }

    std::ofstream out(liked_file_path());
    if (!out)
        return;
    out << "[";
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        track_to_json(out, tracks[i]);
        if (i != tracks.size() - 1)
            out << ",";
    }
    out << "]";
    out.close();
}

std::vector<Track> load_liked_tracks()
{
    if (library::available())
    {
        return library::load_liked();
    }

    std::ifstream in(liked_file_path());
    if (!in.good())
        return {};
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    return parse_track_array(json);
}

std::string history_file_path()
{
    return p100_config_path("history.json");
}

void save_playback_history(const std::vector<Track> &tracks)
{
    if (library::available())
    {
        library::save_history(tracks);
        return;
    }

    std::ofstream out(history_file_path());
    if (!out)
        return;
    out << "[";
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        track_to_json(out, tracks[i]);
        if (i != tracks.size() - 1)
            out << ",";
    }
    out << "]";
    out.close();
}

std::vector<Track> load_playback_history()
{
    if (library::available())
    {
        return library::load_history();
    }

    std::ifstream in(history_file_path());
    if (!in.good())
        return {};
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    auto tracks = parse_track_array(json);
    // Dedupe: keep only the most recent entry per track (list is
    // most-recent-first), so replays/repeats collapse instead of stacking
    // identical rows in the History tab.
    {
        std::unordered_set<std::string> seen;
        std::vector<Track> unique;
        unique.reserve(tracks.size());
        for (auto &t : tracks)
        {
            if (t.id.empty() || !seen.insert(t.id).second)
                continue;
            unique.push_back(std::move(t));
        }
        tracks = std::move(unique);
    }
    // Cap at 200 on load as a safety measure
    if (tracks.size() > 200)
        tracks.resize(200);
    return tracks;
}

std::string queue_file_path()
{
    return p100_config_path("queue.json");
}

void save_play_queue(const std::vector<Track> &tracks)
{
    if (library::available())
    {
        library::save_queue(tracks);
        return;
    }

    const std::string path = queue_file_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (!out)
        return;
    out << "[";
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (i)
            out << ",";
        track_to_json(out, tracks[i]);
    }
    out << "]";
}

std::vector<Track> load_play_queue()
{
    if (library::available())
    {
        return library::load_queue();
    }

    std::ifstream in(queue_file_path());
    if (!in.good())
        return {};
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parse_track_array(json);
}

std::string search_recent_file_path()
{
    return p100_config_path("search_recent.txt");
}

void save_search_recent(const std::vector<std::string> &queries)
{
    const std::string path = search_recent_file_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (!out)
        return;
    int n = 0;
    for (const auto &q : queries)
    {
        if (q.empty() || n++ >= 10)
            break;
        std::string line = q;
        std::replace(line.begin(), line.end(), '\n', ' ');
        std::replace(line.begin(), line.end(), '\r', ' ');
        out << line << "\n";
    }
}

std::vector<std::string> load_search_recent()
{
    std::ifstream in(search_recent_file_path());
    if (!in.good())
        return {};
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line) && out.size() < 10)
    {
        if (!line.empty())
            out.push_back(line);
    }
    return out;
}

bool is_track_liked(const AppState &state, const std::string &id)
{
    for (const auto &t : state.liked_tracks)
    {
        if (t.id == id)
            return true;
    }
    return false;
}

void toggle_like(AppState &state, const Track &track)
{
    bool liked;
    auto it = std::find_if(state.liked_tracks.begin(), state.liked_tracks.end(),
                           [&](const Track &t)
                           { return t.id == track.id; });
    if (it != state.liked_tracks.end())
    {
        state.liked_tracks.erase(it);
        liked = false;
    }
    else
    {
        state.liked_tracks.push_back(track);
        liked = true;
    }
    save_liked_tracks(state.liked_tracks);
    // Mirror to the signed-in YouTube Music account (async, ordered by the
    // write-back worker; failures surface as toasts).
    wb_like(track.id, liked);
}

std::string play_counts_file_path()
{
    return p100_config_path("play_counts.json");
}

std::unordered_map<std::string, int> load_play_counts()
{
    if (library::available())
    {
        return library::load_play_counts();
    }

    std::unordered_map<std::string, int> counts;
    std::ifstream in(play_counts_file_path());
    if (!in.good())
        return counts;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t pos = 0;
    int guard = 0;
    while ((pos = json.find("\"id\":", pos)) != std::string::npos && guard < 100000)
    {
        size_t id_start = json.find('"', pos + 5);
        if (id_start == std::string::npos)
            break;
        size_t id_end = json.find('"', id_start + 1);
        if (id_end == std::string::npos)
            break;
        std::string id = json.substr(id_start + 1, id_end - id_start - 1);

        size_t plays_pos = json.find("\"plays\":", id_end);
        if (plays_pos == std::string::npos)
            break;
        plays_pos += 8;
        size_t plays_end = json.find_first_of(",}", plays_pos);
        std::string val = json.substr(plays_pos, plays_end == std::string::npos ? std::string::npos : plays_end - plays_pos);
        try
        {
            counts[id] = std::stoi(val);
        }
        catch (...)
        {
        }
        pos = id_end + 1;
        ++guard;
    }
    return counts;
}

void save_play_counts(const std::unordered_map<std::string, int> &counts)
{
    if (library::available())
    {
        library::save_play_counts(counts);
        return;
    }

    std::string path = play_counts_file_path();
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);
    std::ofstream out(path);
    if (!out || counts.empty())
    {
        // An empty file is fine; skip writing if no plays recorded yet.
        if (out && counts.empty())
            out << "[]";
        return;
    }
    out << "[";
    bool first = true;
    for (const auto &kv : counts)
    {
        if (kv.second <= 0)
            continue;
        if (!first)
            out << ",";
        first = false;
        out << "{\"id\":\"" << kv.first << "\",\"plays\":" << kv.second << "}";
    }
    out << "]";
    out.close();
}

int play_count_for(const AppState &state, const std::string &id)
{
    auto it = state.play_counts.find(id);
    return it == state.play_counts.end() ? 0 : it->second;
}

std::string play_log_file_path()
{
    return p100_config_path("play_log.json");
}

void save_play_log(const std::vector<PlayEvent> &events)
{
    if (library::available())
    {
        library::save_play_log(events);
        return;
    }

    std::string path = play_log_file_path();
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);
    std::ofstream out(path);
    if (!out)
        return;
    out << "[";
    size_t begin = events.size() > (size_t)kPlayLogCap ? events.size() - kPlayLogCap : 0;
    for (size_t i = begin; i < events.size(); ++i)
    {
        if (i != begin)
            out << ",";
        out << "{\"id\":\"" << json_escape(events[i].id)
            << "\",\"title\":\"" << json_escape(events[i].title)
            << "\",\"artist\":\"" << json_escape(events[i].artist)
            << "\",\"thumb\":\"" << json_escape(events[i].thumbnail_url)
            << "\",\"ts\":" << events[i].ts << "}";
    }
    out << "]";
    out.close();
}

std::vector<PlayEvent> load_play_log()
{
    if (library::available())
    {
        return library::load_play_log();
    }

    std::vector<PlayEvent> events;
    std::ifstream in(play_log_file_path());
    if (!in.good())
        return events;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t pos = 0;
    int guard = 0;
    while ((pos = json.find("\"id\":\"", pos)) != std::string::npos && guard < 100000)
    {
        // Bound the current event object so field lookups stay O(n) overall.
        size_t obj_end = json.find("},", pos);
        if (obj_end == std::string::npos)
            obj_end = json.rfind("}");
        if (obj_end == std::string::npos || obj_end <= pos)
            break;
        std::string obj = json.substr(pos, obj_end - pos + 1);

        PlayEvent ev;
        ev.id = extract_str_field(obj, "id");
        ev.title = extract_str_field(obj, "title");
        ev.artist = extract_str_field(obj, "artist");
        ev.thumbnail_url = extract_str_field(obj, "thumb");

        size_t ts_key = obj.find("\"ts\":");
        if (ts_key != std::string::npos)
        {
            size_t ts_start = ts_key + 5;
            size_t ts_end = obj.find_first_of(",}]", ts_start);
            std::string ts_str = obj.substr(ts_start, ts_end == std::string::npos
                                                         ? std::string::npos
                                                         : ts_end - ts_start);
            try
            {
                ev.ts = std::stoll(ts_str);
            }
            catch (...)
            {
            }
        }

        if (!ev.id.empty())
            events.push_back(ev);
        pos = obj_end + 2;
        ++guard;
    }

    // Ring-cap on load too (file may be hand-edited or from a newer version)
    if (events.size() > (size_t)kPlayLogCap)
        events.erase(events.begin(), events.end() - kPlayLogCap);
    return events;
}

std::string saved_albums_file_path()
{
    return p100_config_path("albums.json");
}

void save_saved_albums(const std::vector<SavedAlbum> &albums)
{
    if (library::available())
    {
        library::save_saved_albums(albums);
        return;
    }

    std::string path = saved_albums_file_path();
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);

    std::ofstream out(path);
    if (!out)
        return;
    out << "[";
    for (size_t i = 0; i < albums.size(); ++i)
    {
        const auto &a = albums[i];
        if (i != 0)
            out << ",";
        out << "{\"browse_id\":\"" << json_escape(a.browse_id)
            << "\",\"title\":\"" << json_escape(a.title)
            << "\",\"artist\":\"" << json_escape(a.artist)
            << "\",\"year\":\"" << json_escape(a.year)
            << "\",\"duration\":\"" << json_escape(a.duration)
            << "\",\"thumbnail_url\":\"" << json_escape(a.thumbnail_url)
            << "\",\"track_count\":" << a.track_count << "}";
    }
    out << "]";
    out.close();
}

std::vector<SavedAlbum> load_saved_albums()
{
    if (library::available())
    {
        return library::load_saved_albums();
    }

    std::vector<SavedAlbum> albums;
    std::ifstream in(saved_albums_file_path());
    if (!in.good())
        return albums;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t pos = 0;
    while ((pos = json.find("\"browse_id\":\"", pos)) != std::string::npos)
    {
        size_t next = json.find("\"browse_id\":\"", pos + 1);
        std::string obj = next == std::string::npos ? json.substr(pos)
                                                    : json.substr(pos, next - pos);

        SavedAlbum a;
        a.browse_id = extract_str_field(obj, "browse_id");
        a.title = extract_str_field(obj, "title");
        a.artist = extract_str_field(obj, "artist");
        a.year = extract_str_field(obj, "year");
        a.duration = extract_str_field(obj, "duration");
        a.thumbnail_url = extract_str_field(obj, "thumbnail_url");
        a.track_count = extract_int_field(obj, "track_count");
        if (!a.browse_id.empty())
            albums.push_back(std::move(a));

        if (next == std::string::npos)
            break;
        pos = next;
    }
    return albums;
}

bool is_album_saved(const AppState &state, const std::string &browse_id)
{
    if (browse_id.empty())
        return false;
    for (const auto &a : state.saved_albums)
    {
        if (a.browse_id == browse_id)
            return true;
    }
    return false;
}

void save_album(AppState &state, const SavedAlbum &album)
{
    if (album.browse_id.empty())
        return;
    auto it = std::find_if(state.saved_albums.begin(), state.saved_albums.end(),
                           [&](const SavedAlbum &a)
                           { return a.browse_id == album.browse_id; });
    if (it != state.saved_albums.end())
        *it = album;
    else
        state.saved_albums.push_back(album);
    save_saved_albums(state.saved_albums);
    // Populate the full album cache in the background so the first click on
    // this saved album doesn't have to show a loading state.
    prefetch_album_cache(album.browse_id);
}

void remove_saved_album(AppState &state, const std::string &browse_id)
{
    auto it = std::find_if(state.saved_albums.begin(), state.saved_albums.end(),
                           [&](const SavedAlbum &a)
                           { return a.browse_id == browse_id; });
    if (it == state.saved_albums.end())
        return;
    state.saved_albums.erase(it);
    save_saved_albums(state.saved_albums);
}

std::string saved_artists_file_path()
{
    return p100_config_path("artists.json");
}

void save_saved_artists(const std::vector<SavedArtist> &artists)
{
    if (library::available())
    {
        library::save_saved_artists(artists);
        return;
    }

    std::string path = saved_artists_file_path();
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.c_str(), 0755);

    std::ofstream out(path);
    if (!out)
        return;
    out << "[";
    for (size_t i = 0; i < artists.size(); ++i)
    {
        const auto &a = artists[i];
        if (i != 0)
            out << ",";
        out << "{\"browse_id\":\"" << json_escape(a.browse_id)
            << "\",\"name\":\"" << json_escape(a.name)
            << "\",\"thumbnail_url\":\"" << json_escape(a.thumbnail_url)
            << "\",\"subscribers\":\"" << json_escape(a.subscribers)
            << "\",\"monthly_listeners\":\"" << json_escape(a.monthly_listeners)
            << "\"}";
    }
    out << "]";
    out.close();
}

std::vector<SavedArtist> load_saved_artists()
{
    if (library::available())
    {
        return library::load_saved_artists();
    }

    std::vector<SavedArtist> artists;
    std::ifstream in(saved_artists_file_path());
    if (!in.good())
        return artists;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    size_t pos = 0;
    while ((pos = json.find("\"browse_id\":\"", pos)) != std::string::npos)
    {
        size_t next = json.find("\"browse_id\":\"", pos + 1);
        std::string obj = next == std::string::npos ? json.substr(pos)
                                                    : json.substr(pos, next - pos);

        SavedArtist a;
        a.browse_id = extract_str_field(obj, "browse_id");
        a.name = extract_str_field(obj, "name");
        a.thumbnail_url = extract_str_field(obj, "thumbnail_url");
        a.subscribers = extract_str_field(obj, "subscribers");
        a.monthly_listeners = extract_str_field(obj, "monthly_listeners");
        if (!a.browse_id.empty())
            artists.push_back(std::move(a));

        if (next == std::string::npos)
            break;
        pos = next;
    }
    return artists;
}

bool is_artist_saved(const AppState &state, const std::string &browse_id)
{
    if (browse_id.empty())
        return false;
    for (const auto &a : state.saved_artists)
    {
        if (a.browse_id == browse_id)
            return true;
    }
    return false;
}

void save_artist(AppState &state, const SavedArtist &artist)
{
    if (artist.browse_id.empty())
        return;
    auto it = std::find_if(state.saved_artists.begin(), state.saved_artists.end(),
                           [&](const SavedArtist &a)
                           { return a.browse_id == artist.browse_id; });
    if (it != state.saved_artists.end())
        *it = artist;
    else
        state.saved_artists.push_back(artist);
    save_saved_artists(state.saved_artists);
    prefetch_artist_cache(artist.browse_id);
}

void remove_saved_artist(AppState &state, const std::string &browse_id)
{
    auto it = std::find_if(state.saved_artists.begin(), state.saved_artists.end(),
                           [&](const SavedArtist &a)
                           { return a.browse_id == browse_id; });
    if (it == state.saved_artists.end())
        return;
    state.saved_artists.erase(it);
    save_saved_artists(state.saved_artists);
}
