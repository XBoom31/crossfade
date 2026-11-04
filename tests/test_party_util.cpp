#include "party_util.h"

#include <cstdio>
#include <string>
#include <unordered_set>

namespace
{
int g_failures = 0;

void check(bool cond, const char *what)
{
    if (!cond)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
} // namespace

int main()
{
    {
        const unsigned char bytes[] = {0xfb, 0xff, 0xbf};
        check(party_util::base64url(bytes, sizeof(bytes)) == "-_-_", "base64url");
    }

    {
        std::unordered_set<std::string> seen;
        for (int i = 0; i < 200; ++i)
        {
            const std::string t = party_util::random_token();
            check(t.size() == 22, "token length");
            check(seen.insert(t).second, "token uniqueness");
        }
    }

    check(party_util::constant_time_equal("abc", "abc"), "constant_time_equal equal");
    check(!party_util::constant_time_equal("abc", "abd"), "constant_time_equal different");
    check(!party_util::constant_time_equal("abc", "abcd"), "constant_time_equal length");

    check(party_util::clean_utf8("hello", 3) == "hel", "codepoint cap");
    check(party_util::clean_utf8(std::string("a\x01z"), 0) == "a z", "control to space");
    check(party_util::clean_utf8("\xff") == "\xEF\xBF\xBD", "invalid utf8 replaced");
    check(party_util::clean_utf8("Привет", 3) == "При", "utf8 cap by codepoint");

    // clean_utf8() folds control characters (including newlines) to spaces,
    // so json_escape() only has to handle quotes and backslashes here.
    check(party_util::json_escape("a\"b\\c\nd") == "a\\\"b\\\\c d", "json escape");
    check(party_util::html_escape("<b a='x'>&\"") == "&lt;b a=&#39;x&#39;&gt;&amp;&quot;",
          "html escape");

    check(party_util::safe_accent("#a1B2c3") == "#a1B2c3", "safe accent valid");
    check(party_util::safe_accent("red") == "#6db2d0", "safe accent invalid");
    check(party_util::safe_accent("#12345g") == "#6db2d0", "safe accent bad hex");

    {
        party_util::TokenBucket b(1.0, 2.0, 100.0);
        check(b.take(100.0), "bucket first");
        check(b.take(100.0), "bucket second");
        check(!b.take(100.0), "bucket empty");
        check(b.retry_after(100.0) > 0.9 && b.retry_after(100.0) < 1.1, "bucket retry");
        check(b.take(101.0), "bucket refill");
    }

    if (g_failures == 0)
        std::printf("party_util: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
