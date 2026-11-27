#include "ytdlp.h"
#include "test_util.h"
using testutil::write_file;
using testutil::check;
#include "paths.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

static std::unordered_map<std::string, int> g_rank;

static int rank_fn(const std::string &id)
{
    auto it = g_rank.find(id);
    return it == g_rank.end() ? 0 : it->second;
}

int main()
{
    const fs::path root = fs::temp_directory_path() / "p100-cache-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "audio_cache", ec);
#ifdef _WIN32
    _putenv_s("P100_CONFIG_DIR", root.string().c_str());
#else
    setenv("P100_CONFIG_DIR", root.string().c_str(), 1);
#endif

    const fs::path cache = root / "audio_cache";
    write_file(cache / "low.webm", std::string(200, 'a'));
    write_file(cache / "mid.opus", std::string(200, 'b'));
    write_file(cache / "high.m4a", std::string(200, 'c'));

    const double total = audio_cache_usage_bytes();
    check(total >= 600.0, "cache usage sees files");

    g_rank = {{"low", 0}, {"mid", 1}, {"high", 2}};
    set_audio_cache_rank(rank_fn);
    set_audio_cache_cap(total - 200.0); // exactly one file must go
    audio_cache_enforce_cap_now();
    check(!fs::exists(cache / "low.webm"), "least-listened evicted first");
    check(fs::exists(cache / "mid.opus"), "mid kept");
    check(fs::exists(cache / "high.m4a"), "most-played kept");
    check(audio_cache_usage_bytes() <= total - 200.0 + 0.1, "usage under cap");

    // cap = 0 means unlimited: nothing is evicted.
    set_audio_cache_cap(0.0);
    audio_cache_enforce_cap_now();
    check(fs::exists(cache / "mid.opus") && fs::exists(cache / "high.m4a"),
          "cap zero is unlimited");

    // The currently playing id is never evicted, even with a tiny cap.
    write_file(cache / "other.webm", std::string(200, 'd'));
    set_audio_cache_cap(1.0);
    audio_cache_enforce_cap_now("high");
    check(fs::exists(cache / "high.m4a"), "current track kept");
    check(!fs::exists(cache / "mid.opus"), "tiny cap evicts the rest");
    check(!fs::exists(cache / "other.webm"), "tiny cap evicts the rest 2");

    // Metadata parsing from a sidecar.
    write_file(cache / "meta.webm", "audio");
    write_file(cache / "meta.webm.meta.json",
               "{\"id\":\"meta\",\"title\":\"Meta song\",\"artist\":\"Meta artist\","
               "\"artist_id\":\"A1\",\"album\":\"Meta album\",\"album_id\":\"B1\","
               "\"track_number\":4,\"thumbnail_url\":\"\",\"duration_sec\":321}");
    auto tracks = get_cached_tracks();
    bool found_meta = false;
    for (const auto &t : tracks)
        if (t.id == "meta")
        {
            found_meta = true;
            check(t.title == "Meta song", "sidecar title");
            check(t.artist == "Meta artist", "sidecar artist");
            check(t.album == "Meta album", "sidecar album");
            check(t.duration_sec == 321, "sidecar duration");
        }
    check(found_meta, "sidecar track listed");

    // Sidecars written from a truncated yt-dlp capture have no thumbnail or
    // artist. The standard YouTube grid thumbnail is derived from the id so
    // the Downloads list can still show a cover.
    write_file(cache / "empty.webm", "audio");
    write_file(cache / "empty.webm.meta.json",
               "{\"id\":\"empty\",\"title\":\"Title only\",\"artist\":\"\","
               "\"thumbnail_url\":\"\",\"duration_sec\":100}");
    tracks = get_cached_tracks();
    bool found_empty = false;
    for (const auto &t : tracks)
        if (t.id == "empty")
        {
            found_empty = true;
            check(t.artist == "Unknown", "empty sidecar artist");
            check(t.thumbnail_url == "https://img.youtube.com/vi/empty/hqdefault.jpg",
                  "derived sidecar thumbnail");
            check(t.title == "Title only", "sidecar title kept");
        }
    check(found_empty, "incomplete sidecar listed");

    // Orphan sidecars are swept.
    write_file(cache / "orphan.webm.meta.json", "{\"id\":\"orphan\",\"title\":\"x\"}");
    (void)get_cached_tracks();
    check(!fs::exists(cache / "orphan.webm.meta.json"), "orphan sidecar swept");

    // Artwork validation: a sidecar is identity-scoped, but its URL must also
    // carry the id. A URL copied from another row must never become the cover.
    write_file(cache / "cover.webm.meta.json",
               "{\"thumbnail_url\":\"https://i.ytimg.com/vi/cover/maxresdefault.jpg\"}");
    write_file(cache / "stolen.webm.meta.json",
               "{\"thumbnail_url\":\"https://yt3.googleusercontent.com/deftones\"}");
    check(authoritative_thumbnail_url("cover", "https://wrong") ==
              "https://i.ytimg.com/vi/cover/maxresdefault.jpg",
          "id-matching sidecar artwork trusted");
    check(authoritative_thumbnail_url("stolen", "https://wrong") ==
              "https://img.youtube.com/vi/stolen/hqdefault.jpg",
          "foreign sidecar artwork rejected");
    check(authoritative_thumbnail_url("missing", "https://wrong") ==
              "https://img.youtube.com/vi/missing/hqdefault.jpg",
          "missing sidecar falls back to id-derived art");
    check(authoritative_thumbnail_url("local:abc", "file:///cover.jpg") ==
              "file:///cover.jpg",
          "local media keeps its explicit artwork");

    // Clear keeps the current file and its sidecar.
    audio_cache_clear("meta");
    check(fs::exists(cache / "meta.webm"), "clear keeps current audio");
    check(fs::exists(cache / "meta.webm.meta.json"), "clear keeps current sidecar");
    check(!fs::exists(cache / "high.m4a"), "clear removes the rest");

    set_audio_cache_rank(nullptr);
    fs::remove_all(root, ec);
    if (testutil::failures == 0)
        std::printf("cache: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
