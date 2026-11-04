#include "session.h"
#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// session.cpp's save_session() is linked but not called here. Provide the one
// mpv accessor it needs so the test doesn't have to link libmpv.
#include "mpvplayer.h"
double MpvPlayer::get_property_double(const std::string &) const
{
    return 0.0;
}

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static void write_file(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "p100-session-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("P100_CONFIG_DIR", root.string().c_str());
#else
    setenv("P100_CONFIG_DIR", root.string().c_str(), 1);
#endif

    AppState s;
    s.current_track_id = "vid\"1";
    s.current_title = "A \\ B \"quoted\"";
    s.current_artist = "Ünïcode Artist";
    s.current_thumbnail_url = "https://example/thumb.jpg";
    s.current_track_url = "https://example/watch?v=vid1";
    s.current_album_id = "ALB1";
    s.current_artist_id = "ART1";
    s.current_playlist_source = PlaylistSource::PLAYLIST;
    s.current_track_index = 4;
    s.playing = true;
    std::snprintf(s.search_buffer, sizeof(s.search_buffer), "%s", "search query");
    s.playlists.push_back(Playlist{"Road trip", {}});
    s.selected_playlist_index = 0;
    s.album_browse_id = "ALB1";
    s.album_title = "Album";
    s.album_artist = "Artist";
    s.artist_browse_id = "ART1";
    s.artist_name = "Artist";

    const std::string json = session_json(s, 123.25, 70);
    check(json.find("\"track_id\":\"vid\\\"1\"") != std::string::npos, "escape quote");
    check(json.find("\"track_title\":\"A \\\\ B \\\"quoted\\\"\"") != std::string::npos,
          "escape backslash");
    check(json.find("Ünïcode Artist") != std::string::npos, "unicode kept");
    check(json.find("\"position\":123.25") != std::string::npos, "position");
    check(json.find("\"volume\":70") != std::string::npos, "volume override");
    check(json.find("\"playing\":true") != std::string::npos, "playing");
    check(json.find("\"source\":\"playlist\"") != std::string::npos, "source");
    check(json.find("\"playlist_name\":\"Road trip\"") != std::string::npos, "playlist name");

    write_file(root / "session.json", json);
    AppState restored;
    load_session(restored);
    check(restored.session_restore_pending, "load marks pending");
    check(restored.session_restore.track_id == s.current_track_id, "round-trip id");
    check(restored.session_restore.track_title == s.current_title, "round-trip title");
    check(restored.session_restore.track_artist == s.current_artist, "round-trip artist");
    check(restored.session_restore.track_url == s.current_track_url, "round-trip url");
    check(restored.session_restore.track_album_id == "ALB1", "round-trip album id");
    check(restored.session_restore.track_artist_id == "ART1", "round-trip artist id");
    check(restored.session_restore.source == "playlist", "round-trip source");
    check(restored.session_restore.playlist_name == "Road trip", "round-trip playlist");
    check(restored.session_restore.index == 4, "round-trip index");
    check(restored.session_restore.volume == 70, "round-trip volume");
    check(restored.session_restore.position > 123.24 &&
              restored.session_restore.position < 123.26, "round-trip position");
    check(restored.session_restore.playing, "round-trip playing");
    check(restored.session_restore.search_query == "search query", "round-trip search");
    check(restored.album_browse_id == "ALB1", "round-trip album hint");
    check(restored.album_title == "Album", "round-trip album title");
    check(restored.artist_browse_id == "ART1", "round-trip artist hint");
    check(restored.artist_name == "Artist", "round-trip artist name");

    // Missing file is a clean no-op.
    fs::remove(root / "session.json", ec);
    AppState missing;
    load_session(missing);
    check(!missing.session_restore_pending, "missing file no-op");

    // Corrupt/truncated input must not crash or restore a phantom track.
    write_file(root / "session.json", "{\"track_id\":");
    AppState corrupt;
    load_session(corrupt);
    check(!corrupt.session_restore_pending, "corrupt file no-op");

    // An older/minimal file with only the required field still restores.
    write_file(root / "session.json",
               "{\"track_id\":\"old\",\"track_title\":\"Old\",\"position\":5}");
    AppState minimal;
    load_session(minimal);
    check(minimal.session_restore_pending, "minimal file restores");
    check(minimal.session_restore.track_id == "old", "minimal id");
    check(minimal.session_restore.track_title == "Old", "minimal title");
    check(minimal.session_restore.volume == -1, "minimal volume default");

    fs::remove_all(root, ec);
    if (g_failures == 0)
        std::printf("session: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
