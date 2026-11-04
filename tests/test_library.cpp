#include "library.h"
#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;
static void check(bool cond, const char *what)
{
    if (!cond)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

static void write_file(const fs::path &path, const char *text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "p100-library-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("P100_CONFIG_DIR", root.string().c_str());
#else
    setenv("P100_CONFIG_DIR", root.string().c_str(), 1);
#endif

    const char *track_a =
        "{\"id\":\"aaaaaaaaaaa\",\"title\":\"Liked A\",\"artist\":\"Artist A\","
        "\"artist_id\":\"ARTA\",\"album\":\"Album A\",\"album_id\":\"ALBA\","
        "\"track_number\":1,\"thumbnail_url\":\"\",\"duration_sec\":180,"
        "\"url\":\"https://www.youtube.com/watch?v=aaaaaaaaaaa\"}";
    const char *track_b =
        "{\"id\":\"bbbbbbbbbbb\",\"title\":\"Liked B\",\"artist\":\"Artist B\","
        "\"artist_id\":\"ARTB\",\"album\":\"Album B\",\"album_id\":\"ALBB\","
        "\"track_number\":2,\"thumbnail_url\":\"\",\"duration_sec\":200,"
        "\"url\":\"https://www.youtube.com/watch?v=bbbbbbbbbbb\"}";
    const char *track_c =
        "{\"id\":\"ccccccccccc\",\"title\":\"History C\",\"artist\":\"Artist C\","
        "\"artist_id\":\"ARTC\",\"album\":\"Album C\",\"album_id\":\"ALBC\","
        "\"track_number\":1,\"thumbnail_url\":\"\",\"duration_sec\":210,"
        "\"url\":\"https://www.youtube.com/watch?v=ccccccccccc\"}";

    write_file(root / "liked.json", (std::string("[") + track_a + "," + track_b + "]").c_str());
    write_file(root / "queue.json", (std::string("[") + track_c + "]").c_str());
    write_file(root / "history.json", (std::string("[") + track_a + "," + track_c + "]").c_str());
    write_file(root / "playlists" / "Test_List.json",
               (std::string("[") + track_a + "," + track_b + "]").c_str());
    write_file(root / "play_counts.json",
               "[{\"id\":\"aaaaaaaaaaa\",\"plays\":5},{\"id\":\"bbbbbbbbbbb\",\"plays\":2}]");
    write_file(root / "play_log.json",
               "[{\"id\":\"aaaaaaaaaaa\",\"title\":\"Liked A\",\"artist\":\"Artist A\","
               "\"thumb\":\"\",\"ts\":1700000000}]");
    write_file(root / "albums.json",
               "[{\"browse_id\":\"ALBA\",\"title\":\"Album A\",\"artist\":\"Artist A\","
               "\"year\":\"2020\",\"duration\":\"3 min\",\"thumbnail_url\":\"\","
               "\"track_count\":1}]");
    write_file(root / "artists.json",
               "[{\"browse_id\":\"ARTA\",\"name\":\"Artist A\",\"thumbnail_url\":\"\","
               "\"subscribers\":\"1K\",\"monthly_listeners\":\"2K\"}]");
    // A downloaded song lives on disk; its sidecar metadata should still be
    // indexed in the library so it shows up in diagnostics and search.
    write_file(root / "audio_cache" / "ddddddddddd.webm", "test-audio");
    write_file(root / "audio_cache" / "ddddddddddd.webm.meta.json",
               "{\"id\":\"ddddddddddd\",\"title\":\"Download D\",\"artist\":\"Artist D\","
               "\"artist_id\":\"ARTD\",\"album\":\"Album D\",\"album_id\":\"ALBD\","
               "\"track_number\":3,\"thumbnail_url\":\"\",\"duration_sec\":220}");

    std::string error;
    check(library::init(error), ("library init: " + error).c_str());
    check(library::available(), "library available");
    check(library::load_liked().size() == 2, "liked migrated");
    check(library::load_history().size() == 2, "history migrated");
    check(library::load_queue().size() == 1, "queue migrated");
    check(library::playlist_names().size() == 1, "playlist names migrated");
    check(library::load_playlist_tracks("Test_List").size() == 2, "playlist items migrated");
    check(library::load_play_counts().size() == 2, "play counts migrated");
    check(library::load_play_log().size() == 1, "play log migrated");
    check(library::load_saved_albums().size() == 1, "albums migrated");
    check(library::load_saved_artists().size() == 1, "artists migrated");
    check(library::search_tracks("Liked").size() == 2, "FTS search");
    check(library::search_tracks("").empty(), "empty search");
    check(library::search_tracks("Download").size() == 1, "downloaded track searchable");
    check(library::download_count() == 1, "download metadata indexed");
    check(library::playlist_names()[0] == "Test_List", "playlist name");

    std::string integrity;
    check(library::integrity_check(integrity), ("integrity: " + integrity).c_str());
    const fs::path backup = root / "backup.db";
    check(library::backup_to(backup.string(), error), ("backup: " + error).c_str());
    check(fs::exists(backup) && fs::file_size(backup) > 0, "backup file exists");

    library::shutdown_for_tests();
    check(library::init(error), ("second init: " + error).c_str());
    check(library::load_liked().size() == 2, "second init keeps data");
    check(library::load_playlist_tracks("Test_List").size() == 2, "second init keeps playlist");
    check(library::download_count() == 1, "second init keeps downloads indexed");

    // A path that cannot be opened as a database must fail cleanly so the
    // caller can fall back to legacy JSON instead of losing the library.
    library::shutdown_for_tests();
    const fs::path bad_root = fs::temp_directory_path() / "p100-library-test-bad";
    fs::remove_all(bad_root, ec);
    fs::create_directories(bad_root / "library.db", ec);
#ifdef _WIN32
    _putenv_s("P100_CONFIG_DIR", bad_root.string().c_str());
#else
    setenv("P100_CONFIG_DIR", bad_root.string().c_str(), 1);
#endif
    error.clear();
    check(!library::init(error), "broken DB path fails");
    check(!library::available(), "failed init stays unavailable");
    library::shutdown_for_tests();
    fs::remove_all(bad_root, ec);

    fs::remove_all(root, ec);
    if (g_failures == 0)
        std::printf("library: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
