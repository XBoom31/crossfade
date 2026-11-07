#include "party_link.h"
#include "test_util.h"
using testutil::check;

#include <cstdio>
#include <string>

void ok(const std::string &input, const std::string &id)
{
    party_link::Parsed p = party_link::parse(input);
    if (p.result != party_link::Result::Ok || p.video_id != id)
    {
        std::fprintf(stderr, "FAIL: expected %s -> %s (got result %d, id '%s')\n",
                     input.c_str(), id.c_str(), (int)p.result, p.video_id.c_str());
        ++testutil::failures;
    }
}

void bad(const std::string &input, party_link::Result expected)
{
    party_link::Parsed p = party_link::parse(input);
    if (p.result != expected)
    {
        std::fprintf(stderr, "FAIL: expected result %d for '%s' (got %d)\n",
                     (int)expected, input.c_str(), (int)p.result);
        ++testutil::failures;
    }
}

int main()
{
    ok("dQw4w9WgXcQ", "dQw4w9WgXcQ");
    ok("https://www.youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ");
    ok("https://youtube.com/watch?v=dQw4w9WgXcQ&t=42s", "dQw4w9WgXcQ");
    ok("https://m.youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ");
    ok("https://music.youtube.com/watch?v=dQw4w9WgXcQ", "dQw4w9WgXcQ");
    ok("https://youtu.be/dQw4w9WgXcQ", "dQw4w9WgXcQ");
    ok("https://youtu.be/dQw4w9WgXcQ?si=tracking", "dQw4w9WgXcQ");
    ok("youtu.be/dQw4w9WgXcQ", "dQw4w9WgXcQ");
    ok("https://www.youtube.com/shorts/dQw4w9WgXcQ", "dQw4w9WgXcQ");
    ok("https://www.youtube.com/embed/dQw4w9WgXcQ", "dQw4w9WgXcQ");
    ok("check this: https://youtu.be/dQw4w9WgXcQ, thanks", "dQw4w9WgXcQ");
    ok("  https://www.youtube.com/watch?v=dQw4w9WgXcQ  ", "dQw4w9WgXcQ");

    check(party_link::valid_video_id("dQw4w9WgXcQ"), "valid id");
    check(!party_link::valid_video_id("short"), "short id");
    check(!party_link::valid_video_id("dQw4w9WgXc!"), "punctuation id");
    check(!party_link::valid_video_id(""), "empty id");

    bad("", party_link::Result::Empty);
    bad("   ", party_link::Result::Empty);
    bad("https://example.com/watch?v=dQw4w9WgXcQ", party_link::Result::NotYoutube);
    bad("hello world", party_link::Result::NotYoutube);
    bad("https://www.youtube.com/playlist?list=PL123", party_link::Result::PlaylistOnly);
    bad("https://www.youtube.com/@somechannel", party_link::Result::Channel);
    bad("https://www.youtube.com/channel/UC123", party_link::Result::Channel);
    bad("https://www.youtube.com/watch?v=too-short", party_link::Result::InvalidId);
    bad(std::string(5000, 'a'), party_link::Result::TooLong);

    if (testutil::failures == 0)
        std::printf("party_link: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
