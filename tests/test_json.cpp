#include "ytdlp.h"

#include <cstdio>
#include <string>

static int g_failures = 0;

static void check(bool cond, const char *what)
{
    if (!cond)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main()
{
    {
        const std::string json =
            "[{\"id\":\"a1\",\"title\":\"A \\\"quoted\\\" song\",\"artist\":\"Art\\\\ist\","
            "\"album\":\"Alb\",\"duration_sec\":215,\"track_number\":3},"
            "{\"id\":\"b2\",\"title\":\"Ünïcode\",\"artist\":\"Ärtist\"}]";
        auto tracks = parse_track_array(json);
        check(tracks.size() == 2, "two tracks");
        check(tracks[0].id == "a1", "id");
        check(tracks[0].title == "A \"quoted\" song", "escaped quote");
        check(tracks[0].artist == "Art\\ist", "escaped backslash");
        check(tracks[0].duration_sec == 215, "duration");
        check(tracks[0].track_number == 3, "track number");
        check(tracks[1].id == "b2", "second id");
        check(tracks[1].title == "Ünïcode", "unicode preserved");
    }

    {
        const std::string json =
            "[{\"id\":\"a\",\"title\":\"A\",\"meta\":{\"nested\":[1,2,3]}},"
            "{\"id\":\"b\",\"title\":\"B\"}]";
        auto tracks = parse_track_array(json);
        check(tracks.size() == 2, "nested object array");
        check(tracks[0].id == "a" && tracks[1].id == "b", "nested ids");
    }

    {
        auto tracks = parse_track_array("[{\"id\":\"a\",\"title\":\"A\"");
        check(tracks.size() <= 1, "truncated array does not crash");
    }

    {
        auto tracks = parse_track_array("[{\"videoId\":\"v1\",\"title\":\"V\"}]");
        check(tracks.size() == 1 && tracks[0].id == "v1", "videoId fallback");
    }

    {
        auto tracks = parse_track_array("{\"id\":\"a\",\"title\":\"A\"}");
        check(tracks.size() == 1, "object input still parses");
    }

    {
        auto tracks = parse_track_array("[{\"id\":\"\",\"title\":\"No id\"},"
                                        "{\"id\":\"x\",\"title\":\"\"}]");
        check(tracks.empty(), "empty id/title skipped");
    }

    {
        Lyrics lx;
        const std::string json =
            "{\"ok\":true,\"synced\":true,\"source\":\"LRCLIB\",\"lines\":["
            "{\"t\":1000,\"text\":\"  Hello   world  \"},"
            "{\"t\":2500,\"text\":\"Привет\"}]}";
        check(parse_lyrics_json(json, lx), "lyrics parse");
        check(lx.ok && lx.synced, "lyrics flags");
        check(lx.source == "LRCLIB", "lyrics source");
        check(lx.lines.size() == 2, "lyrics line count");
        check(lx.times.size() == 2 && lx.times[0] == 1000, "lyrics timestamps");
        check(lx.lines[0] == "  Hello   world  ", "lyrics text preserved");
        check(lx.lines[0].find("Hello") != std::string::npos, "lyrics text present");
        check(lx.lines[1] == "Привет", "lyrics unicode");
    }

    {
        Lyrics lx;
        check(parse_lyrics_json("{\"none\":true}", lx), "lyrics none parses");
        check(lx.none, "lyrics none flag");
        check(lx.lines.empty(), "lyrics none has no lines");
    }

    {
        Lyrics lx;
        check(!parse_lyrics_json("{\"error\":\"boom\"}", lx), "lyrics error rejected");
        check(!parse_lyrics_json("", lx), "empty lyrics rejected");
    }

    {
        Lyrics lx;
        check(parse_lyrics_json("{\"synced\":false,\"lines\":[{\"t\":0,\"text\":\"plain\"}]}",
                                lx),
              "plain lyrics parse");
        check(!lx.synced && lx.lines.size() == 1 && lx.lines[0] == "plain",
              "plain lyrics fields");
    }

    {
        // Deeply nested input is bounded and cannot make the scanner run away.
        std::string deep = "{\"id\":\"x\",\"title\":\"X\",\"a\":";
        for (int i = 0; i < 300; ++i)
            deep += "{\"a\":";
        deep += "1";
        auto tracks = parse_track_array(deep);
        check(tracks.empty(), "deep nesting bounded");
    }

    {
        // Oversized backend replies are rejected before scanning.
        std::string huge(16u * 1024u * 1024u + 1u, 'x');
        check(parse_track_array(huge).empty(), "oversized input rejected");
        Lyrics lx;
        check(!parse_lyrics_json(huge, lx), "oversized lyrics rejected");
    }

    {
        Lyrics lx;
        check(parse_lyrics_json(
                  "{\"synced\":true,\"lines\":[{\"t\":1,\"text\":\"A：B\"}]}", lx),
              "full-width lyrics parse");
        check(lx.lines.size() == 1 && lx.lines[0] == "A: B",
              "full-width punctuation normalized");
    }

    if (g_failures == 0)
        std::printf("json: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
