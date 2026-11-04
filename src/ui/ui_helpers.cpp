#include <unordered_set>
#include "ui.h"
#include "ui_backdrop.h"
#include "imgui_internal.h"
#include "ytdlp.h"
#include "mpvplayer.h"
#include "paths.h"
#include "writeback.h"
#include "library.h"
#include "ui_notify.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <map>
#include <thread>
#include <sys/stat.h>
#ifndef P100_WINDOWS
#include <unistd.h>
#endif

// ----------------------------------------------------------------------
// Liked songs persistence
// ----------------------------------------------------------------------
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

// ----------------------------------------------------------------------
// Playback history persistence
// ----------------------------------------------------------------------
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

// ----------------------------------------------------------------------
// Playlist persistence
// ----------------------------------------------------------------------
std::string playlists_dir()
{
    return p100_config_path("playlists");
}

void ensure_playlists_dir()
{
    std::string dir = playlists_dir();
    mkdir(dir.c_str(), 0755);
}

std::string sanitize_filename(const std::string &name)
{
    std::string safe = name;
    for (char &c : safe)
    {
        if (!isalnum(c) && c != ' ' && c != '_' && c != '-')
            c = '_';
    }
    return safe;
}

std::string playlist_path(const std::string &name)
{
    return playlists_dir() + "/" + sanitize_filename(name) + ".json";
}

static std::string json_escape(const std::string &s);

static void track_to_json(std::ostream &out, const Track &t)
{
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

void queue_add_next(AppState &state, const Track &track)
{
    if (track.id.empty())
        return;
    Track pinned = track;
    pinned.from_queue_source = false;
    state.play_queue.insert(state.play_queue.begin(), pinned);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_add_end(AppState &state, const Track &track)
{
    if (track.id.empty())
        return;
    Track added = track;
    added.from_queue_source = false;
    state.play_queue.push_back(added);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_remove_at(AppState &state, int index)
{
    if (index < 0 || index >= (int)state.play_queue.size())
        return;
    state.play_queue.erase(state.play_queue.begin() + index);
    state.queue_dirty = true;
    state.repaint_requested = true;
}

void queue_move(AppState &state, int from, int to)
{
    if (from < 0 || from >= (int)state.play_queue.size())
        return;
    to = std::max(0, std::min((int)state.play_queue.size() - 1, to));
    if (from == to)
        return;
    Track t = state.play_queue[from];
    state.play_queue.erase(state.play_queue.begin() + from);
    state.play_queue.insert(state.play_queue.begin() + to, std::move(t));
    state.queue_dirty = true;
    state.repaint_requested = true;
}

static std::vector<Track> g_queue_undo;
static AppState *g_queue_undo_state = nullptr;

static void undo_queue_clear()
{
    if (!g_queue_undo_state || g_queue_undo.empty())
        return;
    g_queue_undo_state->play_queue = g_queue_undo;
    g_queue_undo_state->queue_dirty = true;
    g_queue_undo_state->repaint_requested = true;
    g_queue_undo.clear();
    ui_notify(UiNotifyType::Info, "Queue restored", "Your previous Up Next is back.", 4.0f);
}

void queue_clear(AppState &state)
{
    if (state.play_queue.empty())
        return;
    g_queue_undo = state.play_queue;
    g_queue_undo_state = &state;
    state.play_queue.clear();
    state.queue_dirty = true;
    state.repaint_requested = true;
    ui_notify(UiNotifyType::Info, "Queue cleared", "Up Next is empty.", 6.0f, "Undo",
              &undo_queue_clear);
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

void ui_invalidate_forward(AppState &state)
{
    state.nav_forward_valid = false;
}

void ui_go_back(AppState &state)
{
    // Remember where we are so ui_go_forward() can bring the user back.
    state.nav_forward.tab = state.current_tab;
    state.nav_forward.source = state.current_playlist_source;
    state.nav_forward.playlist_index = state.selected_playlist_index;
    if (state.current_tab == Tab::Album)
        state.nav_forward.album_id = state.album_browse_id;
    else
        state.nav_forward.album_id.clear();
    if (state.current_tab == Tab::Artist)
        state.nav_forward.artist_id = state.artist_browse_id;
    else
        state.nav_forward.artist_id.clear();
    state.nav_forward_valid = true;

    if (state.current_tab == Tab::Album)
    {
        state.current_tab = state.album_return_tab;
        state.current_playlist_source = state.album_return_source;
        if (state.current_tab == Tab::Playlists)
            state.selected_playlist_index = state.album_return_playlist_index;
    }
    else if (state.current_tab == Tab::Artist)
    {
        state.current_tab = state.artist_return_tab;
        state.current_playlist_source = state.artist_return_source;
        if (state.current_tab == Tab::Playlists)
            state.selected_playlist_index = state.artist_return_playlist_index;
    }
    else if (state.current_tab == Tab::Queue)
    {
        state.current_tab = state.pre_queue_tab;
    }
    else if (state.current_tab == Tab::Settings)
    {
        state.current_tab = Tab::Home;
        state.current_playlist_source = PlaylistSource::HOME;
    }
}

void ui_go_forward(AppState &state)
{
    if (!state.nav_forward_valid)
        return;
    const AppState::NavSnapshot fwd = state.nav_forward;
    state.nav_forward_valid = false;

    switch (fwd.tab)
    {
    case Tab::Album:
        // The album detail fields may have been repurposed by a newer view;
        // reopen (cache-backed, so it is instant) when they no longer match.
        if (fwd.album_id.empty() || fwd.album_id != state.album_browse_id)
            open_album_view(state, fwd.album_id);
        else
            state.current_tab = Tab::Album;
        break;
    case Tab::Artist:
        if (fwd.artist_id.empty() || fwd.artist_id != state.artist_browse_id)
            open_artist_view(state, fwd.artist_id);
        else
            state.current_tab = Tab::Artist;
        break;
    case Tab::Queue:
        state.pre_queue_tab = state.current_tab;
        state.current_tab = Tab::Queue;
        break;
    default:
        state.current_tab = fwd.tab;
        break;
    }
    state.current_playlist_source = fwd.source;
    if (fwd.source == PlaylistSource::PLAYLIST)
        state.selected_playlist_index = fwd.playlist_index;
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

// ----------------------------------------------------------------------
// Lyrics cache (one JSON file per track; never touches the network)
// ----------------------------------------------------------------------
static std::string json_escape(const std::string &s);

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

// ----------------------------------------------------------------------
// Shared UI helpers
// ----------------------------------------------------------------------
std::string truncate_text(const std::string &text, float max_width)
{
    if (max_width <= 0.0f || ImGui::CalcTextSize(text.c_str()).x <= max_width)
        return text;

    std::string result = text;
    while (!result.empty() && ImGui::CalcTextSize((result + "...").c_str()).x > max_width)
        result.pop_back();

    if (result.empty())
        return "...";
    return result + "...";
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

void build_smart_view(AppState &state)
{
    // Track metadata comes from the play log (title/artist/thumbnail per
    // play event); play counts only know ids. Newest entry per id wins.
    std::unordered_map<std::string, const PlayEvent *> meta;
    meta.reserve(state.play_log.size());
    for (const PlayEvent &ev : state.play_log)
        meta.emplace(ev.id, &ev); // log is newest-first: first hit is newest

    std::vector<Track> out;
    if (state.smart_mode == 0)
    {
        // On Repeat: most-played tracks, all time, top 50.
        std::vector<std::pair<int, const PlayEvent *>> ranked;
        ranked.reserve(meta.size());
        for (const auto &[id, ev] : meta)
        {
            int plays = 1;
            auto it = state.play_counts.find(id);
            if (it != state.play_counts.end() && it->second > 0)
                plays = it->second;
            ranked.emplace_back(plays, ev);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &[plays, ev] : ranked)
        {
            if ((int)out.size() >= 50)
                break;
            Track t;
            t.id = ev->id;
            t.title = ev->title;
            t.artist = ev->artist;
            t.thumbnail_url = ev->thumbnail_url;
            t.url = "https://www.youtube.com/watch?v=" + ev->id;
            out.push_back(std::move(t));
        }
    }
    else
    {
        // Recently Played: most recent distinct tracks, top 100.
        std::unordered_set<std::string> seen;
        for (const PlayEvent &ev : state.play_log)
        {
            if ((int)out.size() >= 100)
                break;
            if (!seen.insert(ev.id).second)
                continue;
            Track t;
            t.id = ev.id;
            t.title = ev.title;
            t.artist = ev.artist;
            t.thumbnail_url = ev.thumbnail_url;
            t.url = "https://www.youtube.com/watch?v=" + ev.id;
            out.push_back(std::move(t));
        }
    }
    state.smart_view = std::make_shared<const std::vector<Track>>(std::move(out));
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

// ----------------------------------------------------------------------
// Playlist source helper. Returns an immutable snapshot; network-fed
// lists are shared_ptrs swapped atomically by worker threads, and
// main-thread vectors are wrapped with a non-owning alias.
// ----------------------------------------------------------------------
TrackListPtr alias_of(const std::vector<Track> &v)
{
    return TrackListPtr(&v, [](const std::vector<Track> *) {});
}

TrackListPtr get_playlist(const AppState &state)
{
    switch (state.current_playlist_source)
    {
    case PlaylistSource::HOME:
        return std::atomic_load(&state.recommendations);
    case PlaylistSource::SEARCH:
        return std::atomic_load(&state.search_results);
    case PlaylistSource::DOWNLOADS:
        return alias_of(state.downloaded_tracks);
    case PlaylistSource::RADIO:
        return std::atomic_load(&state.radio_results);
    case PlaylistSource::LIKED:
        return alias_of(state.liked_tracks);
    case PlaylistSource::PLAYLIST:
        if (state.selected_playlist_index >= 0 && state.selected_playlist_index < (int)state.playlists.size())
            return alias_of(state.playlists[state.selected_playlist_index].tracks);
        return nullptr;
    case PlaylistSource::ALBUM:
        return alias_of(state.album_tracks);
    case PlaylistSource::ARTIST:
        return alias_of(state.artist_songs);
    case PlaylistSource::HISTORY:
        return alias_of(state.playback_history);
    case PlaylistSource::STATS:
        return alias_of(state.stats.tracks);
    case PlaylistSource::SMART:
        if (state.smart_view)
            return state.smart_view;
        return alias_of(state.playback_history);
    default:
        return nullptr;
    }
}

// ----------------------------------------------------------------------
// Play counts persistence (times each song has been played)
// ----------------------------------------------------------------------
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

// ----------------------------------------------------------------------
// Play event log persistence
// ----------------------------------------------------------------------
std::string play_log_file_path()
{
    return p100_config_path("play_log.json");
}

static std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += c;
        }
    }
    return out;
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

// Minimal extractor for "key":"value" pairs (no array-in-string handling,
// which the caps above prevent from mattering much).
static std::string extract_str_field(const std::string &json, const std::string &key)
{
    size_t pos = json.find("\"" + key + "\":\"");
    if (pos == std::string::npos)
        return "";
    pos += key.size() + 4;
    std::string out;
    bool escaped = false;
    while (pos < json.size())
    {
        char c = json[pos++];
        if (escaped)
        {
            switch (c)
            {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default: out += c;
            }
            escaped = false;
        }
        else if (c == '\\')
            escaped = true;
        else if (c == '"')
            break;
        else
            out += c;
    }
    return out;
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

// ----------------------------------------------------------------------
// Saved albums persistence
// ----------------------------------------------------------------------
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

static int extract_int_field(const std::string &json, const std::string &key)
{
    size_t pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos)
        return 0;
    pos += key.size() + 3;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    size_t end = json.find_first_of(",}", pos);
    std::string val = json.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    try
    {
        return std::stoi(val);
    }
    catch (...)
    {
        return 0;
    }
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

// ----------------------------------------------------------------------
// Album data cache (full header + track list, one JSON file per album)
// ----------------------------------------------------------------------
static std::mutex g_album_cache_mutex;

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

// ----------------------------------------------------------------------
// Saved artists persistence
// ----------------------------------------------------------------------
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

// ----------------------------------------------------------------------
// Artist data cache (header + top songs + albums, one JSON file per artist)
// ----------------------------------------------------------------------
static std::mutex g_artist_cache_mutex;

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

void resolve_current_track_links(AppState &state)
{
    if (state.current_track_id.empty() ||
        (!state.current_album_id.empty() && !state.current_artist_id.empty()))
        return;

    const std::string &id = state.current_track_id;
    auto scan = [&](const std::vector<Track> &tracks) -> bool
    {
        for (const auto &t : tracks)
        {
            if (t.id != id)
                continue;
            if (state.current_album_id.empty())
                state.current_album_id = t.album_id;
            if (state.current_artist_id.empty())
                state.current_artist_id = t.artist_id;
            return !state.current_album_id.empty() && !state.current_artist_id.empty();
        }
        return false;
    };

    auto recommendations = std::atomic_load(&state.recommendations);
    if (recommendations && scan(*recommendations))
        return;
    auto search_results = std::atomic_load(&state.search_results);
    if (search_results && scan(*search_results))
        return;
    auto radio_results = std::atomic_load(&state.radio_results);
    if (radio_results && scan(*radio_results))
        return;
    if (scan(state.downloaded_tracks) || scan(state.liked_tracks) ||
        scan(state.playback_history) || scan(state.album_tracks) ||
        scan(state.artist_songs) || scan(state.local_search_results))
        return;
    for (const auto &pl : state.playlists)
        if (scan(pl.tracks))
            return;
}

int ui_tracks_duration(const std::vector<Track> &tracks)
{
    int total = 0;
    for (const auto &track : tracks)
        total += std::max(0, track.duration_sec);
    return total;
}

std::string ui_format_duration(int seconds)
{
    if (seconds <= 0)
        return "";
    const int h = seconds / 3600;
    const int m = (seconds / 60) % 60;
    const int s = seconds % 60;
    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// ----------------------------------------------------------------------
// Smooth scrolling — wheel/touch input as pixels
// ----------------------------------------------------------------------
// main.cpp turns wheel events and touch drags into pixel deltas. Each frame it
// adds them here; these pixels are applied directly to the hovered child.
// ImGui's native wheel path is disabled on those children so there is exactly
// one scroll path and no target/easing lag.
static float g_scroll_pixel_accum = 0.0f;
static float g_frame_scroll_pixels = 0.0f;
static bool g_frame_scroll_consumed = false;
static float g_scroll_clamp_unapplied = 0.0f;

// Absolute sub-pixel scroll accumulator. ImGui rounds every scroll position
// to whole pixels (ImRound64 in CalcNextScrollFromScrollTargetAndClamp), so a
// per-frame delta fed on top of GetScrollY() loses its fraction whenever it
// is under 0.5px -- slow scrolling would freeze and then step. Accumulating
// the absolute value here and feeding SetScrollY(absolute) makes the rounding
// happen on the cumulative position instead, so the fractions carry over and
// motion stays one-to-one at any speed.
static float g_scroll_applied = 0.0f;
static bool g_scroll_applied_valid = false;
static std::string g_scroll_applied_window;
static int g_scroll_applied_frame = -1;

void add_scroll_pixels(float delta_pixels)
{
    g_scroll_pixel_accum += delta_pixels;
}

// Reported by smooth_scroll_child when the hovered child hit its scroll limit
// and could not apply all requested pixels; consumed by update_wheel_inertia()
// to keep the virtual scroll target inside the reachable range.
void scroll_clamp_feedback(float unapplied)
{
    g_scroll_clamp_unapplied += unapplied;
}

float take_scroll_clamp_feedback()
{
    const float unapplied = g_scroll_clamp_unapplied;
    g_scroll_clamp_unapplied = 0.0f;
    return unapplied;
}

// Called before ImGui::NewFrame() each frame.
void smooth_scroll_begin_frame()
{
    g_frame_scroll_pixels = g_scroll_pixel_accum;
    g_scroll_pixel_accum = 0.0f;
    g_frame_scroll_consumed = false;
}

void smooth_scroll_child(const char *window_name)
{
    (void)window_name;
    if (g_frame_scroll_consumed || g_frame_scroll_pixels == 0.0f)
        return;
    if (!ImGui::IsWindowHovered())
        return;
    g_frame_scroll_consumed = true;

    // Continuous consumption: same child received pixels on the previous
    // consuming frame, so keep accumulating sub-pixel. Any discontinuity --
    // a new gesture, or the hover moving to a different child -- re-bases on
    // the child's actual scroll so external changes (scrollbar drag, tab
    // switch) are honoured.
    const char *name = ImGui::GetCurrentWindow()->Name;
    const int frame = ImGui::GetFrameCount();
    const bool continuous = g_scroll_applied_valid && name &&
                            g_scroll_applied_window == name &&
                            frame == g_scroll_applied_frame + 1;
    g_scroll_applied_window = name ? name : "";
    g_scroll_applied_valid = true;
    g_scroll_applied_frame = frame;

    const float base = continuous ? g_scroll_applied : ImGui::GetScrollY();
    const float max_scroll = ImGui::GetScrollMaxY();
    const float requested = base + g_frame_scroll_pixels;
    const float applied = std::max(0.0f, std::min(requested, max_scroll));
    g_scroll_applied = applied;
    ImGui::SetScrollY(applied);
    if (applied != requested)
        scroll_clamp_feedback(applied - requested);
}

bool is_scroll_animating()
{
    return false;
}

// Lyrics panel animation flag (see ui.h). Set by draw_lyrics_panel while its
// auto-centre scroll or fade-in is in motion; consumed by the main loop to
// hold 60 FPS. Consuming (rather than just reading) means a panel that stops
// drawing -- e.g. the user switched to a non-list tab -- can't leave the loop
// pinned at the fast cadence.
static bool g_lyrics_animating = false;

void set_lyrics_animating(bool active)
{
    g_lyrics_animating = active;
}

bool is_lyrics_animating()
{
    bool active = g_lyrics_animating;
    g_lyrics_animating = false;
    return active;
}

// ----------------------------------------------------------------------
// Motion helpers
// ----------------------------------------------------------------------
int g_ui_animation_level = 2;
static bool g_ui_animating = false;

void ui_anim_request()
{
    g_ui_animating = true;
}

bool is_ui_animating()
{
    bool active = g_ui_animating;
    g_ui_animating = false;
    return active;
}

// Page-epoch: bumped whenever the visible page changes (tab switch, search
// reset). Panel entrance animations use it to decide "replay the intro",
// which used to be inferred from gaps in the ImGui frame counter. That proxy
// breaks now that the frame loop skips renders while idle (a safety frame
// would replay every intro on screen), while a page change always coincides
// with real input, so it is always observed on a live frame.
static uint64_t g_page_epoch = 0;
uint64_t ui_page_epoch()
{
    return g_page_epoch;
}
void ui_bump_page_epoch()
{
    ++g_page_epoch;
}

float ui_anim_ease_out(float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static float anim_factor()
{
    switch (g_ui_animation_level)
    {
    case 1: return 0.70f;
    case 3: return 1.55f;
    default: return 1.0f;
    }
}

float ui_anim_duration(float base_seconds)
{
    return g_ui_animation_level > 0 ? base_seconds * anim_factor() : 0.0f;
}

float ui_anim_delay(float base_seconds)
{
    return g_ui_animation_level > 0 ? base_seconds * anim_factor() : 0.0f;
}

float ui_anim_offset(float base_px)
{
    return g_ui_animation_level > 0 ? base_px * anim_factor() : 0.0f;
}

float ui_anim_approach(float current, float target, float speed)
{
    if (!ui_animations_enabled())
        return target;
    float dt = ImGui::GetIO().DeltaTime;
    if (!(dt > 0.0f) || dt > 0.1f)
        dt = 1.0f / 60.0f;
    const float t = 1.0f - expf(-speed * dt);
    return current + (target - current) * t;
}

namespace
{
struct WidthAnim
{
    bool init = false;
    float from = 0.0f;
    float to = 0.0f;
    float current = 0.0f;
    float last_target = -1.0f;
    double start = 0.0;
};

float anim_width(WidthAnim &st, float target, float base_duration)
{
    const double now = ImGui::GetTime();
    if (!st.init)
    {
        st.init = true;
        st.from = st.to = st.current = target;
        st.last_target = target;
        st.start = now;
        return st.current;
    }
    if (fabsf(target - st.last_target) > 0.01f)
    {
        st.from = st.current;
        st.to = target;
        st.last_target = target;
        st.start = now;
    }
    const float dur = ui_anim_duration(base_duration);
    float t = dur > 0.0f ? (float)((now - st.start) / dur) : 1.0f;
    st.current = st.from + (st.to - st.from) * ui_anim_ease_out(t);
    if (t < 1.0f)
        ui_anim_request();
    return st.current;
}
} // namespace

float ui_anim_sidebar_width(float target_width)
{
    static WidthAnim st;
    return anim_width(st, target_width, 0.26f);
}

float ui_anim_lyrics_width(float target_width)
{
    static WidthAnim st;
    return anim_width(st, target_width, 0.28f);
}

struct ListRevealState
{
    uint64_t token = 0;
    size_t count = 0;
    std::string first;
    std::string last;
    double start = -1.0;
    uint64_t epoch = (uint64_t)-1; // page epoch the list was last drawn under
};

// (last_frame removed: with idle frame-skipping, frame-number gaps no longer
// mean "not drawn recently"; page changes are tracked via ui_page_epoch.)

static std::unordered_map<std::string, ListRevealState> g_list_reveals;

void ui_anim_reset_list_reveals()
{
    g_list_reveals.clear();
}

struct ScrollRevealState
{
    int last_frame = -1;
    uint64_t epoch = (uint64_t)-1;
    double start = -1.0;
    int order = 0;
};

static std::unordered_map<std::string, ScrollRevealState> g_scroll_reveals;
// Per-scope (frame, next order) counter so rows that become visible together
// cascade instead of popping in as a block.
static std::unordered_map<int, std::pair<int, int>> g_scroll_reveal_batches;

double ui_anim_scroll_reveal(int scope, int index, const std::string &id, int *out_order)
{
    if (!ui_animations_enabled())
    {
        if (out_order)
            *out_order = 0;
        return -1.0;
    }

    std::string key = std::to_string(scope);
    key += '#';
    key += id.empty() ? std::to_string(index) : id;

    const int frame = ImGui::GetFrameCount();
    const uint64_t page_epoch = ui_page_epoch();
    ScrollRevealState &st = g_scroll_reveals[key];
    if (st.start < 0.0 || st.epoch != page_epoch)
    {
        st.start = ImGui::GetTime();
        auto &batch = g_scroll_reveal_batches[scope];
        if (batch.first != frame)
        {
            batch.first = frame;
            batch.second = 0;
        }
        st.order = batch.second++;
        ui_anim_request();
    }
    st.epoch = page_epoch;
    st.last_frame = frame;
    if (out_order)
        *out_order = st.order;
    return st.start;
}

double ui_anim_list_reveal(const char *key, uint64_t token, size_t count,
                           const std::string &first_id, const std::string &last_id)
{
    if (!ui_animations_enabled() || count == 0 || !key || !*key)
        return -1.0;

    const uint64_t page_epoch = ui_page_epoch();
    ListRevealState &st = g_list_reveals[key];
    // Restart the reveal whenever the page changed since the list was last
    // drawn (tab/view switch, returning from lyrics-only, etc.) so the
    // slide-down plays every time the list is shown, not just on the very
    // first load.
    if (st.start < 0.0 || st.epoch != page_epoch ||
        st.token != token || st.count != count ||
        st.first != first_id || st.last != last_id)
    {
        st.token = token;
        st.count = count;
        st.first = first_id;
        st.last = last_id;
        st.start = ImGui::GetTime();
    }
    st.epoch = page_epoch;

    const double elapsed = ImGui::GetTime() - st.start;
    const double total = (double)ui_anim_duration(0.22f) +
                         (double)ui_anim_delay(0.018f * 10.0f) + 0.03;
    if (elapsed < total)
        ui_anim_request();
    return st.start;
}

double ui_anim_track_list_reveal(const char *key, const std::vector<Track> &tracks,
                                 uint64_t token)
{
    if (tracks.empty())
        return -1.0;
    return ui_anim_list_reveal(key, token, tracks.size(),
                               tracks.front().id, tracks.back().id);
}

static std::unordered_map<std::string, double> g_texture_reveals;

float ui_anim_texture_alpha(const std::string &url)
{
    if (!ui_animations_enabled() || url.empty())
        return 1.0f;

    const double now = ImGui::GetTime();
    auto it = g_texture_reveals.find(url);
    if (it == g_texture_reveals.end())
    {
        it = g_texture_reveals.emplace(url, now).first;
    }
    const float dur = ui_anim_duration(0.18f);
    float t = dur > 0.0f ? (float)((now - it->second) / dur) : 1.0f;
    if (t < 1.0f)
        ui_anim_request();
    return ui_anim_ease_out(t);
}

float ui_content_edge_alpha(float top_y, float bottom_y)
{
    const float max_scroll = ImGui::GetScrollMaxY();
    if (max_scroll <= 1.0f)
        return 1.0f;

    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    if (size.y <= 1.0f)
        return 1.0f;

    const float scroll = ImGui::GetScrollY();
    const float fade = std::min(ui_px(26.0f), size.y * 0.18f);
    float alpha = 1.0f;
    if (scroll > 1.0f && top_y < pos.y + fade)
        alpha = std::min(alpha, std::clamp((top_y - pos.y) / fade, 0.0f, 1.0f));
    if (scroll < max_scroll - 1.0f && bottom_y > pos.y + size.y - fade)
        alpha = std::min(alpha, std::clamp(((pos.y + size.y) - bottom_y) / fade, 0.0f, 1.0f));
    return alpha;
}

void draw_scroll_edge_fades()
{
    // Rows, media cards and lyric lines now fade their own content with
    // ui_content_edge_alpha(). Painting a background-coloured overlay here
    // could never match the local panel surface beneath it.
}

