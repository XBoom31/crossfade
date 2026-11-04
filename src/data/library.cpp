#include "library.h"

#include "paths.h"
#include "sqlite3.h"
#include "ytdlp.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>

namespace library
{
namespace
{
sqlite3 *g_db = nullptr;
bool g_ready = false;
bool g_imported = false;
std::string g_error;
std::string g_backup_dir;
std::recursive_mutex g_mu;

const char *kTrackCols =
    "t.video_id,t.title,t.artist,t.artist_id,t.album,t.album_id,t.track_number,"
    "t.thumbnail_url,t.duration_sec,t.url,t.requested_by,t.guest_id,t.from_party,"
    "t.party_round";

bool exec_locked(const char *sql)
{
    char *err = nullptr;
    if (sqlite3_exec(g_db, sql, nullptr, nullptr, &err) != SQLITE_OK)
    {
        if (err)
        {
            g_error = err;
            sqlite3_free(err);
        }
        return false;
    }
    return true;
}

std::string text_col(sqlite3_stmt *st, int col)
{
    const unsigned char *s = sqlite3_column_text(st, col);
    return s ? std::string(reinterpret_cast<const char *>(s)) : std::string();
}

Track read_track(sqlite3_stmt *st, int base = 0)
{
    Track t;
    t.id = text_col(st, base + 0);
    t.title = text_col(st, base + 1);
    t.artist = text_col(st, base + 2);
    t.artist_id = text_col(st, base + 3);
    t.album = text_col(st, base + 4);
    t.album_id = text_col(st, base + 5);
    t.track_number = sqlite3_column_int(st, base + 6);
    t.thumbnail_url = text_col(st, base + 7);
    t.duration_sec = sqlite3_column_int(st, base + 8);
    t.url = text_col(st, base + 9);
    t.requested_by = text_col(st, base + 10);
    t.guest_id = text_col(st, base + 11);
    t.from_party = sqlite3_column_int(st, base + 12) != 0;
    t.party_round = sqlite3_column_int(st, base + 13);
    return t;
}

bool begin_tx()
{
    return exec_locked("BEGIN IMMEDIATE");
}
bool commit_tx()
{
    return exec_locked("COMMIT");
}

bool upsert_track(const Track &t)
{
    if (!g_db || t.id.empty())
        return false;
    sqlite3_stmt *st = nullptr;
    const char *sql =
        "INSERT INTO tracks(video_id,title,artist,artist_id,album,album_id,track_number,"
        "thumbnail_url,duration_sec,url,requested_by,guest_id,from_party,party_round,updated_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,strftime('%s','now'))"
        " ON CONFLICT(video_id) DO UPDATE SET title=excluded.title,artist=excluded.artist,"
        "artist_id=excluded.artist_id,album=excluded.album,album_id=excluded.album_id,"
        "track_number=excluded.track_number,thumbnail_url=excluded.thumbnail_url,"
        "duration_sec=excluded.duration_sec,url=excluded.url,requested_by=excluded.requested_by,"
        "guest_id=excluded.guest_id,from_party=excluded.from_party,"
        "party_round=excluded.party_round,updated_at=excluded.updated_at";
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, t.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, t.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, t.artist.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, t.artist_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, t.album.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, t.album_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, t.track_number);
    sqlite3_bind_text(st, 8, t.thumbnail_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 9, t.duration_sec);
    sqlite3_bind_text(st, 10, t.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 11, t.requested_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 12, t.guest_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 13, t.from_party ? 1 : 0);
    sqlite3_bind_int(st, 14, t.party_round);
    const bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        return false;

    sqlite3_stmt *fts = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM tracks_fts WHERE video_id=?", -1, &fts, nullptr) ==
        SQLITE_OK)
    {
        sqlite3_bind_text(fts, 1, t.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(fts);
        sqlite3_finalize(fts);
    }
    if (sqlite3_prepare_v2(g_db,
                           "INSERT INTO tracks_fts(video_id,title,artist,album) VALUES(?,?,?,?)",
                           -1, &fts, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text(fts, 1, t.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(fts, 2, t.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(fts, 3, t.artist.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(fts, 4, t.album.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(fts);
        sqlite3_finalize(fts);
    }
    return true;
}

bool ensure_playlist_row(const std::string &name, sqlite3_int64 &id)
{
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db, "INSERT INTO playlists(name) VALUES(?) "
                                 "ON CONFLICT(name) DO NOTHING",
                           -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    if (sqlite3_prepare_v2(g_db, "SELECT id FROM playlists WHERE name=?", -1, &st, nullptr) !=
        SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok)
        id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return ok;
}

std::string now_stamp()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

bool copy_legacy_backup(std::string &error)
{
    namespace fs = std::filesystem;
    const fs::path config(p100_config_dir());
    const fs::path backup = config / ("library-backup-" + now_stamp());
    std::error_code ec;
    fs::create_directories(backup, ec);
    if (ec)
    {
        error = "Could not create backup directory: " + ec.message();
        return false;
    }
    const char *files[] = {"liked.json", "history.json", "queue.json",
                           "play_counts.json", "play_log.json", "albums.json",
                           "artists.json"};
    for (const char *name : files)
    {
        const fs::path src = config / name;
        if (!fs::exists(src, ec))
            continue;
        fs::copy_file(src, backup / name, fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = std::string("Could not back up ") + name + ": " + ec.message();
            return false;
        }
    }
    const fs::path playlists = config / "playlists";
    if (fs::exists(playlists, ec) && fs::is_directory(playlists, ec))
    {
        fs::copy(playlists, backup / "playlists",
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec)
        {
            error = "Could not back up playlists: " + ec.message();
            return false;
        }
    }
    g_backup_dir = backup.string();
    return true;
}

bool verify_count(const char *sql, size_t expected, const char *what, std::string &error)
{
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) != SQLITE_OK)
        return false;
    size_t got = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
        got = (size_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (got != expected)
    {
        error = std::string("Migration verification failed for ") + what + ": expected " +
                std::to_string(expected) + ", got " + std::to_string(got);
        return false;
    }
    return true;
}

std::string read_whole_file(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

std::string extract_str_field(const std::string &json, const std::string &key)
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

int extract_int_field(const std::string &json, const std::string &key)
{
    size_t pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos)
        return 0;
    pos += key.size() + 3;
    while (pos < json.size() && std::isspace((unsigned char)json[pos]))
        ++pos;
    size_t end = json.find_first_of(",}", pos);
    try
    {
        return std::stoi(json.substr(pos, end == std::string::npos ? std::string::npos
                                                                   : end - pos));
    }
    catch (...)
    {
        return 0;
    }
}

std::vector<Track> legacy_tracks_file(const std::filesystem::path &path)
{
    return parse_track_array(read_whole_file(path));
}

std::unordered_map<std::string, int> legacy_counts_file(const std::filesystem::path &path)
{
    std::unordered_map<std::string, int> counts;
    const std::string json = read_whole_file(path);
    size_t pos = 0;
    int guard = 0;
    while ((pos = json.find("\"id\":", pos)) != std::string::npos && guard++ < 100000)
    {
        const size_t id_start = json.find('"', pos + 5);
        if (id_start == std::string::npos)
            break;
        const size_t id_end = json.find('"', id_start + 1);
        if (id_end == std::string::npos)
            break;
        const size_t plays_pos = json.find("\"plays\":", id_end);
        if (plays_pos == std::string::npos)
            break;
        const size_t plays_start = plays_pos + 8;
        const size_t plays_end = json.find_first_of(",}", plays_start);
        try
        {
            counts[json.substr(id_start + 1, id_end - id_start - 1)] =
                std::stoi(json.substr(plays_start, plays_end == std::string::npos
                                                      ? std::string::npos
                                                      : plays_end - plays_start));
        }
        catch (...)
        {
        }
        pos = id_end + 1;
    }
    return counts;
}

std::vector<PlayEvent> legacy_log_file(const std::filesystem::path &path)
{
    std::vector<PlayEvent> events;
    const std::string json = read_whole_file(path);
    size_t pos = 0;
    int guard = 0;
    while ((pos = json.find("\"id\":\"", pos)) != std::string::npos && guard++ < 100000)
    {
        size_t obj_end = json.find("},", pos);
        if (obj_end == std::string::npos)
            obj_end = json.rfind("}");
        if (obj_end == std::string::npos || obj_end <= pos)
            break;
        const std::string obj = json.substr(pos, obj_end - pos + 1);
        PlayEvent e;
        e.id = extract_str_field(obj, "id");
        e.title = extract_str_field(obj, "title");
        e.artist = extract_str_field(obj, "artist");
        e.thumbnail_url = extract_str_field(obj, "thumb");
        e.ts = extract_int_field(obj, "ts");
        if (!e.id.empty())
            events.push_back(std::move(e));
        pos = obj_end + 1;
    }
    return events;
}

std::vector<SavedAlbum> legacy_albums_file(const std::filesystem::path &path)
{
    std::vector<SavedAlbum> albums;
    const std::string json = read_whole_file(path);
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

std::vector<SavedArtist> legacy_artists_file(const std::filesystem::path &path)
{
    std::vector<SavedArtist> artists;
    const std::string json = read_whole_file(path);
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

bool legacy_import(std::string &error)
{
    namespace fs = std::filesystem;
    const fs::path config(p100_config_dir());
    const auto liked = legacy_tracks_file(config / "liked.json");
    const auto history = legacy_tracks_file(config / "history.json");
    const auto queue = legacy_tracks_file(config / "queue.json");
    const auto counts = legacy_counts_file(config / "play_counts.json");
    const auto log = legacy_log_file(config / "play_log.json");
    const auto albums = legacy_albums_file(config / "albums.json");
    const auto artists = legacy_artists_file(config / "artists.json");
    std::vector<std::string> names;
    std::vector<std::vector<Track>> playlists;
    const fs::path playlist_dir = config / "playlists";
    std::error_code ec;
    if (fs::exists(playlist_dir, ec) && fs::is_directory(playlist_dir, ec))
    {
        for (const auto &entry : fs::directory_iterator(playlist_dir, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            if (entry.path().stem().extension() == ".meta")
                continue;
            const std::string name = entry.path().stem().string();
            names.push_back(name);
            playlists.push_back(legacy_tracks_file(entry.path()));
        }
    }

    if (!copy_legacy_backup(error))
        return false;

    if (!exec_locked("DELETE FROM liked; DELETE FROM history; DELETE FROM queue;"
                     "DELETE FROM playlists; DELETE FROM playlist_items;"
                     "DELETE FROM play_counts; DELETE FROM play_log;"
                     "DELETE FROM saved_albums; DELETE FROM saved_artists;"
                     "DELETE FROM tracks; DELETE FROM tracks_fts;"))
        return false;

    // Each save function is its own transaction; the marker is only written
    // after all of them verify, so a failure simply retries next launch.
    library::save_liked(liked);
    library::save_history(history);
    library::save_queue(queue);
    for (size_t i = 0; i < names.size(); ++i)
        library::save_playlist(names[i], playlists[i]);
    library::save_play_counts(counts);
    library::save_play_log(log);
    library::save_saved_albums(albums);
    library::save_saved_artists(artists);

    if (!verify_count("SELECT COUNT(*) FROM liked", liked.size(), "likes", error) ||
        !verify_count("SELECT COUNT(*) FROM history", history.size(), "history", error) ||
        !verify_count("SELECT COUNT(*) FROM queue", queue.size(), "queue", error) ||
        !verify_count("SELECT COUNT(*) FROM playlists", names.size(), "playlists", error) ||
        !verify_count("SELECT COUNT(*) FROM play_counts", counts.size(), "play counts", error) ||
        !verify_count("SELECT COUNT(*) FROM play_log", log.size(), "play log", error) ||
        !verify_count("SELECT COUNT(*) FROM saved_albums", albums.size(), "saved albums", error) ||
        !verify_count("SELECT COUNT(*) FROM saved_artists", artists.size(), "saved artists", error))
        return false;

    return exec_locked("INSERT INTO meta(k,v) VALUES('legacy_imported','1') "
                       "ON CONFLICT(k) DO UPDATE SET v='1'");
}

bool schema_setup()
{
    return exec_locked(
        "PRAGMA journal_mode=WAL;"
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS tracks("
        " video_id TEXT PRIMARY KEY, title TEXT NOT NULL DEFAULT '', artist TEXT,"
        " artist_id TEXT, album TEXT, album_id TEXT, track_number INTEGER DEFAULT 0,"
        " thumbnail_url TEXT, duration_sec INTEGER DEFAULT 0, url TEXT,"
        " requested_by TEXT, guest_id TEXT, from_party INTEGER DEFAULT 0,"
        " party_round INTEGER DEFAULT 0, updated_at INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS liked(video_id TEXT PRIMARY KEY, position INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS history(video_id TEXT PRIMARY KEY, position INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS queue(position INTEGER PRIMARY KEY, video_id TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS playlists(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT UNIQUE NOT NULL);"
        "CREATE TABLE IF NOT EXISTS playlist_items(playlist_id INTEGER NOT NULL,"
        " position INTEGER NOT NULL, video_id TEXT NOT NULL,"
        " PRIMARY KEY(playlist_id,position));"
        "CREATE TABLE IF NOT EXISTS play_counts(video_id TEXT PRIMARY KEY, count INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS play_log(seq INTEGER PRIMARY KEY AUTOINCREMENT,"
        " video_id TEXT, title TEXT, artist TEXT, thumb TEXT, ts INTEGER);"
        "CREATE TABLE IF NOT EXISTS saved_albums(browse_id TEXT PRIMARY KEY, position INTEGER,"
        " title TEXT, artist TEXT, year TEXT, duration TEXT, thumbnail_url TEXT,"
        " track_count INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS saved_artists(browse_id TEXT PRIMARY KEY, position INTEGER,"
        " name TEXT, thumbnail_url TEXT, subscribers TEXT, monthly_listeners TEXT);"
        "CREATE TABLE IF NOT EXISTS downloads(video_id TEXT PRIMARY KEY,"
        " position INTEGER NOT NULL);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5("
        " video_id UNINDEXED, title, artist, album);"
        "PRAGMA user_version=1;");
}

std::string fts_query(const std::string &query)
{
    std::string out;
    std::string tok;
    auto flush = [&]() {
        if (tok.empty())
            return;
        if (!out.empty())
            out += ' ';
        out += tok;
        out += '*';
        tok.clear();
    };
    for (unsigned char c : query)
    {
        if (std::isalnum(c) || c >= 0x80)
            tok.push_back((char)c);
        else
            flush();
    }
    flush();
    return out;
}

} // namespace

bool init(std::string &error)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    error.clear();
    if (g_ready)
        return true;
    p100_ensure_config_dir();
    const std::string path = db_path();
    if (sqlite3_open_v2(path.c_str(), &g_db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK)
    {
        error = g_db ? sqlite3_errmsg(g_db) : "Could not open library database";
        if (g_db)
        {
            sqlite3_close(g_db);
            g_db = nullptr;
        }
        return false;
    }
    sqlite3_busy_timeout(g_db, 3000);
    if (!schema_setup())
    {
        error = g_error.empty() ? "Could not create library schema" : g_error;
        sqlite3_close(g_db);
        g_db = nullptr;
        return false;
    }

    bool imported = false;
    {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db, "SELECT v FROM meta WHERE k='legacy_imported'", -1, &st,
                               nullptr) == SQLITE_OK)
        {
            imported = sqlite3_step(st) == SQLITE_ROW;
            sqlite3_finalize(st);
        }
    }
    if (!imported)
    {
        if (!legacy_import(error))
        {
            if (error.empty())
                error = "Library migration failed";
            sqlite3_close(g_db);
            g_db = nullptr;
            g_ready = false;
            return false;
        }
        g_imported = true;
    }
    g_ready = true;
    // Keep the library index complete even if the Downloads tab is never
    // opened this session. Audio files themselves stay in audio_cache/.
    save_downloads(get_cached_tracks());
    return true;
}

bool available()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    return g_ready;
}

std::string db_path()
{
    return p100_config_path("library.db");
}

std::string backup_dir()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    return g_backup_dir;
}

std::string status_text()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_ready)
        return g_error.empty() ? "Library database not available" : g_error;
    auto count = [](const char *sql) -> long long {
        sqlite3_stmt *st = nullptr;
        long long n = 0;
        if (sqlite3_prepare_v2(g_db, sql, -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW)
                n = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        return n;
    };
    std::ostringstream out;
    out << "SQLite library ready\n"
        << "Tracks: " << count("SELECT COUNT(*) FROM tracks") << "\n"
        << "Downloads: " << count("SELECT COUNT(*) FROM downloads") << "\n"
        << "Playlists: " << count("SELECT COUNT(*) FROM playlists") << "\n"
        << "Likes: " << count("SELECT COUNT(*) FROM liked") << "\n"
        << "History: " << count("SELECT COUNT(*) FROM history") << "\n"
        << "Play count rows: " << count("SELECT COUNT(*) FROM play_counts") << "\n"
        << "Play log rows: " << count("SELECT COUNT(*) FROM play_log") << "\n"
        << "DB: " << db_path();
    if (!g_backup_dir.empty())
        out << "\nLegacy backup: " << g_backup_dir;
    return out.str();
}

void save_liked(const std::vector<Track> &tracks)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db)
        return;
    if (!begin_tx())
        return;
    exec_locked("DELETE FROM liked");
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (!upsert_track(tracks[i]))
            continue;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO liked(video_id,position) VALUES(?,?)",
                               -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, tracks[i].id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, (int)i);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    commit_tx();
}

std::vector<Track> load_liked()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<Track> out;
    if (!g_db)
        return out;
    sqlite3_stmt *st = nullptr;
    const std::string sql = std::string("SELECT ") + kTrackCols +
                            " FROM liked l JOIN tracks t ON t.video_id=l.video_id "
                            "ORDER BY l.position";
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(read_track(st));
    sqlite3_finalize(st);
    return out;
}

void save_history(const std::vector<Track> &tracks)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || !begin_tx())
        return;
    exec_locked("DELETE FROM history");
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (!upsert_track(tracks[i]))
            continue;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO history(video_id,position) VALUES(?,?)",
                               -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, tracks[i].id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, (int)i);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    commit_tx();
}

std::vector<Track> load_history()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<Track> out;
    if (!g_db)
        return out;
    sqlite3_stmt *st = nullptr;
    const std::string sql = std::string("SELECT ") + kTrackCols +
                            " FROM history h JOIN tracks t ON t.video_id=h.video_id "
                            "ORDER BY h.position";
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(read_track(st));
    sqlite3_finalize(st);
    return out;
}

void save_queue(const std::vector<Track> &tracks)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || !begin_tx())
        return;
    exec_locked("DELETE FROM queue");
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (!upsert_track(tracks[i]))
            continue;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db, "INSERT INTO queue(position,video_id) VALUES(?,?)", -1, &st,
                               nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int(st, 1, (int)i);
            sqlite3_bind_text(st, 2, tracks[i].id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    commit_tx();
}

std::vector<Track> load_queue()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<Track> out;
    if (!g_db)
        return out;
    sqlite3_stmt *st = nullptr;
    const std::string sql = std::string("SELECT ") + kTrackCols +
                            " FROM queue q JOIN tracks t ON t.video_id=q.video_id "
                            "ORDER BY q.position";
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(read_track(st));
    sqlite3_finalize(st);
    return out;
}

void save_playlist(const std::string &name, const std::vector<Track> &tracks)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || name.empty() || !begin_tx())
        return;
    sqlite3_int64 id = -1;
    if (ensure_playlist_row(name, id))
    {
        sqlite3_stmt *del = nullptr;
        if (sqlite3_prepare_v2(g_db, "DELETE FROM playlist_items WHERE playlist_id=?", -1, &del,
                               nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(del, 1, id);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            if (!upsert_track(tracks[i]))
                continue;
            sqlite3_stmt *st = nullptr;
            if (sqlite3_prepare_v2(g_db,
                                   "INSERT INTO playlist_items(playlist_id,position,video_id)"
                                   " VALUES(?,?,?)",
                                   -1, &st, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_int64(st, 1, id);
                sqlite3_bind_int(st, 2, (int)i);
                sqlite3_bind_text(st, 3, tracks[i].id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }
    }
    commit_tx();
}

std::vector<Track> load_playlist_tracks(const std::string &name)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<Track> out;
    if (!g_db || name.empty())
        return out;
    sqlite3_stmt *st = nullptr;
    const std::string sql = std::string("SELECT ") + kTrackCols +
                            " FROM playlist_items i JOIN playlists p ON p.id=i.playlist_id"
                            " JOIN tracks t ON t.video_id=i.video_id"
                            " WHERE p.name=? ORDER BY i.position";
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(read_track(st));
    sqlite3_finalize(st);
    return out;
}

std::vector<std::string> playlist_names()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<std::string> out;
    if (!g_db)
        return out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT name FROM playlists ORDER BY id", -1, &st, nullptr) !=
        SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(text_col(st, 0));
    sqlite3_finalize(st);
    return out;
}

void delete_playlist(const std::string &name)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || !begin_tx())
        return;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db, "DELETE FROM playlist_items WHERE playlist_id="
                                 "(SELECT id FROM playlists WHERE name=?)",
                           -1, &st, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(g_db, "DELETE FROM playlists WHERE name=?", -1, &st, nullptr) ==
        SQLITE_OK)
    {
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    commit_tx();
}

void save_play_counts(const std::unordered_map<std::string, int> &counts)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || !begin_tx())
        return;
    exec_locked("DELETE FROM play_counts");
    for (const auto &kv : counts)
    {
        if (kv.second <= 0)
            continue;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db, "INSERT INTO play_counts(video_id,count) VALUES(?,?)", -1,
                               &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, kv.first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, kv.second);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    commit_tx();
}

std::unordered_map<std::string, int> load_play_counts()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::unordered_map<std::string, int> out;
    if (!g_db)
        return out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db, "SELECT video_id,count FROM play_counts", -1, &st, nullptr) !=
        SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
        out[text_col(st, 0)] = sqlite3_column_int(st, 1);
    sqlite3_finalize(st);
    return out;
}

void save_play_log(const std::vector<PlayEvent> &events)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || !begin_tx())
        return;
    exec_locked("DELETE FROM play_log");
    for (const auto &e : events)
    {
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                               "INSERT INTO play_log(video_id,title,artist,thumb,ts)"
                               " VALUES(?,?,?,?,?)",
                               -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, e.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, e.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, e.artist.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, e.thumbnail_url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 5, (sqlite3_int64)e.ts);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    commit_tx();
}

std::vector<PlayEvent> load_play_log()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<PlayEvent> out;
    if (!g_db)
        return out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db,
                           "SELECT video_id,title,artist,thumb,ts FROM play_log ORDER BY seq",
                           -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        PlayEvent e;
        e.id = text_col(st, 0);
        e.title = text_col(st, 1);
        e.artist = text_col(st, 2);
        e.thumbnail_url = text_col(st, 3);
        e.ts = (long long)sqlite3_column_int64(st, 4);
        out.push_back(std::move(e));
    }
    sqlite3_finalize(st);
    return out;
}

void save_saved_albums(const std::vector<SavedAlbum> &albums)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || !begin_tx())
        return;
    exec_locked("DELETE FROM saved_albums");
    for (size_t i = 0; i < albums.size(); ++i)
    {
        const auto &a = albums[i];
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                               "INSERT OR REPLACE INTO saved_albums(browse_id,position,title,"
                               "artist,year,duration,thumbnail_url,track_count)"
                               " VALUES(?,?,?,?,?,?,?,?)",
                               -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, a.browse_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, (int)i);
            sqlite3_bind_text(st, 3, a.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, a.artist.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 5, a.year.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 6, a.duration.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 7, a.thumbnail_url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 8, a.track_count);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    commit_tx();
}

std::vector<SavedAlbum> load_saved_albums()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<SavedAlbum> out;
    if (!g_db)
        return out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db,
                           "SELECT browse_id,title,artist,year,duration,thumbnail_url,"
                           "track_count FROM saved_albums ORDER BY position",
                           -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        SavedAlbum a;
        a.browse_id = text_col(st, 0);
        a.title = text_col(st, 1);
        a.artist = text_col(st, 2);
        a.year = text_col(st, 3);
        a.duration = text_col(st, 4);
        a.thumbnail_url = text_col(st, 5);
        a.track_count = sqlite3_column_int(st, 6);
        out.push_back(std::move(a));
    }
    sqlite3_finalize(st);
    return out;
}

void save_saved_artists(const std::vector<SavedArtist> &artists)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || !begin_tx())
        return;
    exec_locked("DELETE FROM saved_artists");
    for (size_t i = 0; i < artists.size(); ++i)
    {
        const auto &a = artists[i];
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                               "INSERT OR REPLACE INTO saved_artists(browse_id,position,name,"
                               "thumbnail_url,subscribers,monthly_listeners)"
                               " VALUES(?,?,?,?,?,?)",
                               -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, a.browse_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, (int)i);
            sqlite3_bind_text(st, 3, a.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, a.thumbnail_url.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 5, a.subscribers.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 6, a.monthly_listeners.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    commit_tx();
}

std::vector<SavedArtist> load_saved_artists()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<SavedArtist> out;
    if (!g_db)
        return out;
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db,
                           "SELECT browse_id,name,thumbnail_url,subscribers,monthly_listeners"
                           " FROM saved_artists ORDER BY position",
                           -1, &st, nullptr) != SQLITE_OK)
        return out;
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        SavedArtist a;
        a.browse_id = text_col(st, 0);
        a.name = text_col(st, 1);
        a.thumbnail_url = text_col(st, 2);
        a.subscribers = text_col(st, 3);
        a.monthly_listeners = text_col(st, 4);
        out.push_back(std::move(a));
    }
    sqlite3_finalize(st);
    return out;
}

void save_downloads(const std::vector<Track> &tracks)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db || !begin_tx())
        return;
    exec_locked("DELETE FROM downloads");
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        if (!upsert_track(tracks[i]))
            continue;
        sqlite3_stmt *st = nullptr;
        if (sqlite3_prepare_v2(g_db,
                               "INSERT OR REPLACE INTO downloads(video_id,position) VALUES(?,?)",
                               -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(st, 1, tracks[i].id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, (int)i);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
    // Drop metadata for tracks that are no longer referenced anywhere. This
    // keeps deleted downloads out of Library search without touching the
    // audio files themselves.
    exec_locked("DELETE FROM tracks WHERE video_id NOT IN ("
                " SELECT video_id FROM liked UNION SELECT video_id FROM history"
                " UNION SELECT video_id FROM queue"
                " UNION SELECT video_id FROM playlist_items"
                " UNION SELECT video_id FROM downloads);"
                "DELETE FROM tracks_fts WHERE video_id NOT IN"
                " (SELECT video_id FROM tracks);");
    commit_tx();
}

long long download_count()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (!g_db)
        return 0;
    sqlite3_stmt *st = nullptr;
    long long n = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM downloads", -1, &st, nullptr) ==
        SQLITE_OK)
    {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

std::vector<Track> search_tracks(const std::string &query, int limit)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    std::vector<Track> out;
    if (!g_db || query.empty() || limit <= 0)
        return out;
    const std::string fts = fts_query(query);
    if (fts.empty())
        return out;
    sqlite3_stmt *st = nullptr;
    const std::string sql = std::string("SELECT ") + kTrackCols +
                            " FROM tracks_fts f JOIN tracks t ON t.video_id=f.video_id"
                            " WHERE tracks_fts MATCH ? ORDER BY rank LIMIT ?";
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_text(st, 1, fts.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back(read_track(st));
    sqlite3_finalize(st);
    return out;
}

bool backup_to(const std::string &path, std::string &error)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    error.clear();
    if (!g_db)
    {
        error = "Library database is not open";
        return false;
    }
    std::string esc;
    esc.reserve(path.size() + 8);
    for (char c : path)
    {
        if (c == '\'')
            esc += "''";
        else
            esc += c;
    }
    const std::string sql = "VACUUM INTO '" + esc + "'";
    if (!exec_locked(sql.c_str()))
    {
        error = g_error.empty() ? "Backup failed" : g_error;
        return false;
    }
    return true;
}

bool integrity_check(std::string &result)
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    result.clear();
    if (!g_db)
    {
        result = "Library database is not open";
        return false;
    }
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(g_db, "PRAGMA integrity_check", -1, &st, nullptr) != SQLITE_OK)
    {
        result = g_error.empty() ? "Integrity check failed" : g_error;
        return false;
    }
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        result = text_col(st, 0);
        ok = result == "ok";
    }
    sqlite3_finalize(st);
    return ok;
}

void shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(g_mu);
    if (g_db)
    {
        sqlite3_close(g_db);
        g_db = nullptr;
    }
    g_ready = false;
    g_imported = false;
    g_error.clear();
    g_backup_dir.clear();
}

void shutdown_for_tests()
{
    shutdown();
}

} // namespace library
