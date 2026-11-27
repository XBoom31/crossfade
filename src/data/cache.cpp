#include "tasks.h"
#include "ui.h"
#include "library.h"
#include "paths.h"
#include "persist_util.h"
#include "ytdlp.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <vector>

using persist::extract_int_field;
using persist::extract_str_field;
using persist::json_escape;
using jsonutil::write_track;

static std::mutex g_album_cache_mutex;
static std::mutex g_artist_cache_mutex;
static std::mutex g_podcast_cache_mutex;

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
    if (!parse_lyrics_json(json, out))
        return false;
    // Version 3 caches predate the Python backend's translation pass and can
    // still be from a provider that never supplied word timings. Refetch once
    // so the immersive view can pick up translations. The caller keeps this
    // payload as a fallback if the refetch fails.
    return out.version >= 4;
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
        out << "{\"version\":4,\"none\":true}";
        out.close();
        return;
    }
    out << "{\"version\":4,\"synced\":" << (lyrics.synced ? "true" : "false")
        << ",\"source\":\"" << json_escape(lyrics.source) << "\",\"language\":\""
        << json_escape(lyrics.language) << "\",\"lines\":[";
    const size_t count = std::max(lyrics.lines.size(), lyrics.rich.size());
    for (size_t i = 0; i < count; ++i)
    {
        if (i != 0)
            out << ",";
        if (i < lyrics.rich.size())
        {
            const RichLyricLine &l = lyrics.rich[i];
            out << "{\"t\":" << (l.start_ms / 1000);
            if (l.end_ms > 0)
                out << ",\"end\":" << (l.end_ms / 1000);
            out << ",\"text\":\"" << json_escape(l.text) << "\"";
            if (!l.translation.empty())
                out << ",\"translation\":\"" << json_escape(l.translation) << "\"";
            if (!l.romanization.empty())
                out << ",\"roman\":\"" << json_escape(l.romanization) << "\"";
            if (!l.words.empty())
            {
                out << ",\"words\":[";
                for (size_t w = 0; w < l.words.size(); ++w)
                {
                    if (w)
                        out << ",";
                    out << "{\"s\":" << l.words[w].start_ms << ",\"e\":"
                        << l.words[w].end_ms << ",\"text\":\""
                        << json_escape(l.words[w].text) << "\"}";
                }
                out << "]";
            }
            out << "}";
        }
        else
        {
            int t = i < lyrics.times.size() ? lyrics.times[i] : 0;
            out << "{\"t\":" << t << ",\"text\":\"" << json_escape(lyrics.lines[i])
                << "\"}";
        }
    }
    out << "]}";
    out.close();
}

void load_downloaded_tracks(AppState &state)
{
    state.downloaded_tracks = get_cached_tracks();
    {
        auto videos = get_cached_videos();
        state.downloaded_tracks.insert(state.downloaded_tracks.end(), videos.begin(), videos.end());
    }
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
    delete_cached_video(id);
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
        if (i != 0)
            out << ",";
        write_track(out, tracks[i]);
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
    tasks::spawn("album-prefetch", [browse_id, path]() {
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
            return;
        Album album;
        if (fetch_album(browse_id, album))
            save_album_cache(album);
    });
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
    tasks::spawn("artist-prefetch", [browse_id, path]() {
        std::error_code ec;
        if (std::filesystem::exists(path, ec))
            return;
        ArtistInfo artist;
        if (fetch_artist(browse_id, artist))
            save_artist_cache(artist);
    });
}

// ----------------------------------------------------------------------
// Podcast show cache
//
// The show page used to hit the backend on every open, so an offline launch
// (or the moment before Wi-Fi comes up) showed an empty episode list -- even
// for episodes already downloaded. Mirroring the album/artist caches keeps
// the header and episode list on disk.
// ----------------------------------------------------------------------
std::string podcast_cache_dir()
{
    return p100_config_path("podcast_cache");
}

std::string podcast_cache_path(const std::string &browse_id)
{
    return podcast_cache_dir() + "/" + sanitize_filename(browse_id) + ".json";
}

void save_podcast_cache(const PodcastDetails &details)
{
    const std::string browse_id = details.show.browse_id;
    if (browse_id.empty())
        return;
    std::lock_guard<std::mutex> lock(g_podcast_cache_mutex);
    std::string dir = podcast_cache_dir();
    mkdir(dir.c_str(), 0755);

    std::string path = podcast_cache_path(browse_id);
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp);
    if (!out)
        return;
    out << "{\"browse_id\":\"" << json_escape(browse_id)
        << "\",\"title\":\"" << json_escape(details.show.title)
        << "\",\"author\":\"" << json_escape(details.show.author)
        << "\",\"thumbnail_url\":\"" << json_escape(details.show.thumbnail_url)
        << "\",\"description\":\"" << json_escape(details.show.description)
        << "\",\"episodes\":[";
    for (size_t i = 0; i < details.episodes.size(); ++i)
    {
        const PodcastEpisode &e = details.episodes[i];
        if (i != 0)
            out << ",";
        // video_id first: the loader walks episode objects by that key.
        out << "{\"video_id\":\"" << json_escape(e.video_id)
            << "\",\"title\":\"" << json_escape(e.title)
            << "\",\"author\":\"" << json_escape(e.author)
            << "\",\"thumbnail_url\":\"" << json_escape(e.thumbnail_url)
            << "\",\"date\":\"" << json_escape(e.date)
            << "\",\"description\":\"" << json_escape(e.description)
            << "\",\"duration_sec\":" << e.duration_sec << "}";
    }
    out << "]}";
    out.close();

    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec)
        std::filesystem::remove(tmp, ec);
}

bool load_podcast_cache(const std::string &browse_id, PodcastDetails &out)
{
    if (browse_id.empty())
        return false;

    std::ifstream in(podcast_cache_path(browse_id));
    if (!in.good())
        return false;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (json.empty())
        return false;

    out = PodcastDetails{};
    out.show.browse_id = extract_str_field(json, "browse_id");
    if (out.show.browse_id.empty())
        out.show.browse_id = browse_id;
    out.show.title = extract_str_field(json, "title");
    out.show.author = extract_str_field(json, "author");
    out.show.thumbnail_url = extract_str_field(json, "thumbnail_url");
    out.show.description = extract_str_field(json, "description");

    const size_t eps_key = json.find("\"episodes\":");
    if (eps_key != std::string::npos)
    {
        size_t pos = json.find("\"video_id\":\"", eps_key);
        while (pos != std::string::npos)
        {
            const size_t next = json.find("\"video_id\":\"", pos + 1);
            const std::string obj =
                next == std::string::npos ? json.substr(pos) : json.substr(pos, next - pos);
            PodcastEpisode e;
            e.video_id = extract_str_field(obj, "video_id");
            e.title = extract_str_field(obj, "title");
            e.author = extract_str_field(obj, "author");
            e.thumbnail_url = extract_str_field(obj, "thumbnail_url");
            e.date = extract_str_field(obj, "date");
            e.description = extract_str_field(obj, "description");
            e.duration_sec = extract_int_field(obj, "duration_sec");
            if (!e.video_id.empty())
                out.episodes.push_back(std::move(e));
            if (next == std::string::npos)
                break;
            pos = next;
        }
    }
    return !out.show.title.empty() || !out.episodes.empty();
}

// ----------------------------------------------------------------------
// Downloaded-track metadata repair
//
// Sidecars written while run_capture() truncated yt-dlp's -j JSON at 64 KB
// carry a title but no artist/thumbnail. Rebuild them from local listening
// data first (play log, likes, history, queue, playlists), derive missing
// covers from the video id, and optionally ask the backend for the rest.
// The worker never touches UI-owned vectors: it publishes an id -> Track map
// that the main loop applies to play_log.
// ----------------------------------------------------------------------
namespace
{
std::atomic<bool> g_metadata_repair_running{false};
std::atomic<int> g_metadata_repair_done{0};
std::atomic<int> g_metadata_repair_total{0};
std::atomic<int> g_metadata_repair_updated{0};

bool missing_artist(const std::string &artist)
{
    return artist.empty() || artist == "Unknown" || artist == "Unknown Artist";
}

bool local_media_id(const std::string &id)
{
    return id.rfind("local:", 0) == 0;
}

void merge_meta(std::unordered_map<std::string, Track> &map, const Track &t)
{
    if (t.id.empty() || local_media_id(t.id))
        return;
    Track &m = map[t.id];
    m.id = t.id;
    if ((m.title.empty() || m.title == "Unknown Track") && !t.title.empty() &&
        t.title != "Unknown Track")
        m.title = t.title;
    if (missing_artist(m.artist) && !missing_artist(t.artist))
        m.artist = t.artist;
    if (m.thumbnail_url.empty() && !t.thumbnail_url.empty())
        m.thumbnail_url = t.thumbnail_url;
    if (m.artist_id.empty())
        m.artist_id = t.artist_id;
    if (m.album.empty())
        m.album = t.album;
    if (m.album_id.empty())
        m.album_id = t.album_id;
    if (m.duration_sec == 0)
        m.duration_sec = t.duration_sec;
}

void metadata_repair_worker(AppState *state, std::vector<PlayEvent> play_log,
                            std::vector<Track> liked, std::vector<Track> history,
                            std::vector<Track> queue, std::vector<Playlist> playlists,
                            bool fetch_missing)
{
    std::unordered_map<std::string, Track> meta;
    std::vector<std::string> unknown_ids;
    for (const PlayEvent &ev : play_log)
    {
        Track t;
        t.id = ev.id;
        t.title = ev.title;
        t.artist = ev.artist;
        t.thumbnail_url = ev.thumbnail_url;
        merge_meta(meta, t);
        if (!ev.id.empty() && !local_media_id(ev.id) && missing_artist(ev.artist))
            unknown_ids.push_back(ev.id);
    }
    std::sort(unknown_ids.begin(), unknown_ids.end());
    unknown_ids.erase(std::unique(unknown_ids.begin(), unknown_ids.end()), unknown_ids.end());
    for (const Track &t : liked)
        merge_meta(meta, t);
    for (const Track &t : history)
        merge_meta(meta, t);
    for (const Track &t : queue)
        merge_meta(meta, t);
    for (const Playlist &p : playlists)
        for (const Track &t : p.tracks)
            merge_meta(meta, t);

    std::vector<Track> downloads = get_cached_tracks();
    int done = 0;
    const int total = (int)downloads.size() + (int)unknown_ids.size();
    g_metadata_repair_total = total;
    int updated = 0;
    int failures = 0;
    bool network = fetch_missing;

    for (Track &t : downloads)
    {
        Track r = t;
        auto it = meta.find(t.id);
        if (it != meta.end())
        {
            const Track &m = it->second;
            if (missing_artist(r.artist) && !missing_artist(m.artist))
                r.artist = m.artist;
            if ((r.title.empty() || r.title == "Unknown Track") && !m.title.empty())
                r.title = m.title;
            if (r.thumbnail_url.empty() && !m.thumbnail_url.empty())
                r.thumbnail_url = m.thumbnail_url;
            if (r.artist_id.empty())
                r.artist_id = m.artist_id;
            if (r.album.empty())
                r.album = m.album;
            if (r.album_id.empty())
                r.album_id = m.album_id;
            if (r.duration_sec == 0)
                r.duration_sec = m.duration_sec;
        }
        if (r.thumbnail_url.empty() && !local_media_id(t.id) && !t.id.empty())
            r.thumbnail_url = "https://img.youtube.com/vi/" + t.id + "/hqdefault.jpg";

        if (network && missing_artist(r.artist) && !local_media_id(t.id) && !t.id.empty())
        {
            Track fetched;
            std::string error;
            if (fetch_track_meta(t.id, fetched, error))
            {
                if (!fetched.title.empty() &&
                    (r.title.empty() || r.title == "Unknown Track"))
                    r.title = fetched.title;
                if (!fetched.artist.empty())
                    r.artist = fetched.artist;
                if (!fetched.thumbnail_url.empty())
                    r.thumbnail_url = fetched.thumbnail_url;
                if (fetched.duration_sec > 0 && r.duration_sec == 0)
                    r.duration_sec = fetched.duration_sec;
                failures = 0;
            }
            else if (++failures >= 3)
            {
                // Backend/network is unavailable. Keep the local repairs and
                // leave the rest for the next run instead of hammering it.
                network = false;
            }
        }

        const bool changed = r.title != t.title || r.artist != t.artist ||
                             r.artist_id != t.artist_id || r.album != t.album ||
                             r.album_id != t.album_id ||
                             r.thumbnail_url != t.thumbnail_url ||
                             r.duration_sec != t.duration_sec;
        if (changed)
        {
            const std::string path = find_cached_audio_path(t.id);
            if (!path.empty())
            {
                save_cached_track_metadata(r, path);
                ++updated;
            }
        }
        meta[t.id] = r;
        g_metadata_repair_done = ++done;
        if (state)
            state->repaint_requested = true;
    }

    // Play-log rows can reference songs that are no longer downloaded (or
    // never were). Resolve those too so the Top artist card is not skewed by
    // a handful of "Unknown" entries. No sidecar is written for them.
    if (network)
    {
        for (const std::string &id : unknown_ids)
        {
            g_metadata_repair_done = ++done;
            auto it = meta.find(id);
            if (it != meta.end() && !missing_artist(it->second.artist))
                continue;
            Track fetched;
            std::string error;
            if (fetch_track_meta(id, fetched, error))
            {
                Track &m = meta[id];
                m.id = id;
                if (!fetched.title.empty())
                    m.title = fetched.title;
                if (!fetched.artist.empty())
                    m.artist = fetched.artist;
                if (!fetched.thumbnail_url.empty())
                    m.thumbnail_url = fetched.thumbnail_url;
                if (fetched.duration_sec > 0)
                    m.duration_sec = fetched.duration_sec;
                failures = 0;
            }
            else if (++failures >= 3)
            {
                network = false;
                break;
            }
        }
    }

    g_metadata_repair_done = total;
    auto result =
        std::make_shared<const std::unordered_map<std::string, Track>>(std::move(meta));
    if (state)
    {
        std::atomic_store(&state->repaired_meta, result);
        state->metadata_repair_pending = true;
        state->repaint_requested = true;
    }
    g_downloads_dirty = true;
    g_metadata_repair_updated = updated;
    g_metadata_repair_running = false;
}
} // namespace

void start_download_metadata_repair(AppState &state, bool fetch_missing)
{
    if (g_metadata_repair_running.exchange(true))
        return;
    g_metadata_repair_done = 0;
    g_metadata_repair_total = 0;
    g_metadata_repair_updated = 0;

    // Snapshot UI-owned containers on the main thread.
    std::vector<PlayEvent> play_log = state.play_log;
    std::vector<Track> liked = state.liked_tracks;
    std::vector<Track> history = state.playback_history;
    std::vector<Track> queue = state.play_queue;
    std::vector<Playlist> playlists = state.playlists;
    AppState *sp = &state;
    tasks::spawn("download-metadata-repair",
                 [sp, play_log = std::move(play_log), liked = std::move(liked),
                  history = std::move(history), queue = std::move(queue),
                  playlists = std::move(playlists), fetch_missing]() mutable {
                     metadata_repair_worker(sp, std::move(play_log), std::move(liked),
                                            std::move(history), std::move(queue),
                                            std::move(playlists), fetch_missing);
                 });
}

bool download_metadata_repair_running()
{
    return g_metadata_repair_running.load();
}

void download_metadata_repair_progress(int &done, int &total, int &updated)
{
    done = g_metadata_repair_done.load();
    total = g_metadata_repair_total.load();
    updated = g_metadata_repair_updated.load();
}
