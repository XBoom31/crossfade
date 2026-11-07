#include "ui.h"
#include "test_util.h"
using testutil::write_file;
using testutil::check;
#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

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

    {
        Track t = make_track("x1", "A&B <song>");
        t.artist = "X<Y";
        t.url = "https://example/watch?v=x1&list=2";
        Playlist pl{"Road & <trip>", {t}};
        const std::string m3u = playlist_m3u(pl);
        check(m3u.rfind("#EXTM3U\n", 0) == 0, "m3u header");
        check(m3u.find("#EXTINF:120,X<Y - A&B <song>\n") != std::string::npos,
              "m3u entry");
        check(m3u.find("https://example/watch?v=x1&list=2\n") != std::string::npos,
              "m3u url");
        const std::string xspf = playlist_xspf(pl);
        check(xspf.find("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") == 0,
              "xspf header");
        check(xspf.find("<title>Road &amp; &lt;trip&gt;</title>") != std::string::npos,
              "xspf escaped playlist title");
        check(xspf.find("<creator>X&lt;Y</creator>") != std::string::npos,
              "xspf escaped creator");
        check(xspf.find("<location>https://example/watch?v=x1&amp;list=2</location>") !=
                  std::string::npos,
              "xspf escaped location");
        check(xspf.find("<duration>120000</duration>") != std::string::npos,
              "xspf duration");
    }

    fs::remove_all(root, ec);
    if (testutil::failures == 0)
        std::printf("playlists: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
