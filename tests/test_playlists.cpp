#include "ui.h"
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

static void write_file(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

static Track make_track(const std::string &id, const std::string &title)
{
    Track t;
    t.id = id;
    t.title = title;
    t.artist = "Artist";
    t.url = "https://example/watch?v=" + id;
    t.duration_sec = 120;
    return t;
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "p100-playlists-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("P100_CONFIG_DIR", root.string().c_str());
#else
    setenv("P100_CONFIG_DIR", root.string().c_str(), 1);
#endif

    const std::vector<Track> tracks = {make_track("a1", "A simple title"),
                                       make_track("b2", "Ünïcode")};
    save_playlist("Road trip", tracks);
    save_playlist("empty", {});

    auto names = get_playlist_names();
    check(names.size() == 2, "two playlists");
    check(fs::exists(root / "playlists" / "Road trip.json"), "playlist file");

    auto loaded = load_playlist_tracks("Road trip");
    check(loaded.size() == 2, "round-trip count");
    check(loaded[0].id == "a1", "round-trip id");
    check(loaded[0].title == "A simple title", "round-trip title");
    check(loaded[1].title == "Ünïcode", "round-trip unicode");

    // A server-link sidecar is not a playlist.
    write_file(root / "playlists" / "Road trip.meta.json", "{\"id\":\"x\"}");
    names = get_playlist_names();
    check(names.size() == 2, "meta sidecar ignored");

    rename_playlist("Road trip", "Renamed trip");
    check(!fs::exists(root / "playlists" / "Road trip.json"), "rename removes old");
    check(fs::exists(root / "playlists" / "Renamed trip.json"), "rename creates new");
    check(load_playlist_tracks("Renamed trip").size() == 2, "rename keeps tracks");

    // Separators are sanitized into a flat filename.
    save_playlist("../escape", {make_track("c3", "C")});
    check(fs::exists(root / "playlists" / "___escape.json"), "path traversal sanitized");
    check(!fs::exists(root / "escape.json"), "no file outside playlist dir");

    delete_playlist("empty");
    check(!fs::exists(root / "playlists" / "empty.json"), "delete removes file");
    check(get_playlist_names().size() == 2, "delete updates names");

    write_file(root / "playlists" / "broken.json", "[{\"id\":\"x\"");
    check(load_playlist_tracks("broken").empty(), "malformed playlist is empty");

    fs::remove_all(root, ec);
    if (g_failures == 0)
        std::printf("playlists: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
