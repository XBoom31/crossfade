#include "types.h"

#include <cstdio>

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
    Settings s;
    check(s.crossfade_duration == 5.0f, "default crossfade");
    check(!s.skip_crossfade_enabled, "default skip crossfade");
    check(s.repeat_mode == 1, "default repeat all");
    check(!s.shuffle_enabled, "default shuffle off");
    check(s.ui_scale == 1.0f && s.ui_text_scale == 1.0f, "default ui scale");
    check(s.animations == 2, "default animations");
    check(!s.safe_mode, "default safe mode");
    check(!s.low_power, "default low power off");
    check(s.low_power_on_battery, "default low power on battery");
    check(s.backdrop_shader, "default shader");
    check(s.warm_stream_resolver, "default warm resolver");
    check(s.playback_speed == 1.0f, "default speed");
    check(!s.eq_enabled, "default eq");
    check(s.ytdl_browser == "firefox", "default browser");
    check(s.auth_mode == "browser", "default auth");

    Settings a, b;
    check(a == b, "defaults compare equal");
    b.crossfade_duration = 1.0f;
    check(!(a == b), "crossfade change compares unequal");
    b = a;
    b.safe_mode = true;
    check(!(a == b), "safe mode change compares unequal");
    b = a;
    b.low_power = true;
    check(!(a == b), "low power change compares unequal");
    b = a;
    b.low_power_on_battery = false;
    check(!(a == b), "low power battery change compares unequal");
    b = a;
    b.eq_enabled = true;
    check(!(a == b), "eq change compares unequal");
    b = a;
    b.ui_elements["bottom_art"] = 1.2f;
    check(!(a == b), "ui element change compares unequal");
    b = a;
    b.party.allow_duplicates = !b.party.allow_duplicates;
    check(!(a == b), "party setting change compares unequal");

    if (g_failures == 0)
        std::printf("settings: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
