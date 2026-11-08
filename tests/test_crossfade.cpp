#include "crossfade.h"
#include "test_util.h"
using testutil::check;

#include <cmath>
#include <cstdio>
#include <string>

static bool close_to(double a, double b, double eps = 1e-6)
{
    return std::fabs(a - b) <= eps;
}

int main()
{
    using crossfade::ManualAction;
    using crossfade::NaturalInputs;

    check(crossfade::plan_manual(false, false, false, 0.0, 5.0) == ManualAction::None,
          "manual off");
    check(crossfade::plan_manual(true, false, false, 0.0, 5.0) == ManualAction::Cancel,
          "manual cancel");
    check(crossfade::plan_manual(true, true, false, 0.5, 5.0) == ManualAction::None,
          "manual pending");
    check(crossfade::plan_manual(true, false, true, 4.9, 5.0) == ManualAction::None,
          "manual active before end");
    check(crossfade::plan_manual(true, false, true, 5.0, 5.0) == ManualAction::Finish,
          "manual finish");
    check(crossfade::plan_manual(true, true, false, 1.0, 5.0) == ManualAction::StartFailed,
          "manual start failed");
    check(crossfade::plan_manual(true, true, false, 0.99, 5.0) == ManualAction::None,
          "manual start grace");

    NaturalInputs n;
    n.crossfade_duration = 6.0;
    n.track_duration = 30.0;
    n.position = 26.0;
    n.playing = true;
    n.target_cached = true;
    check(crossfade::should_start_natural(n), "natural normal");

    n.crossfade_duration = 0.0;
    check(!crossfade::should_start_natural(n), "natural disabled");
    n.crossfade_duration = 6.0;

    n.already_fading = true;
    check(!crossfade::should_start_natural(n), "natural already fading");
    n.already_fading = false;

    n.playing = false;
    check(!crossfade::should_start_natural(n), "natural not playing");
    n.playing = true;

    n.loading = true;
    check(!crossfade::should_start_natural(n), "natural loading");
    n.loading = false;

    n.repeat_one = true;
    check(!crossfade::should_start_natural(n), "natural repeat one");
    n.repeat_one = false;

    n.track_duration = 0.0;
    check(!crossfade::should_start_natural(n), "natural unknown duration");
    n.track_duration = 30.0;

    n.position = 0.0;
    check(!crossfade::should_start_natural(n), "natural no position");
    n.position = 20.0;
    check(!crossfade::should_start_natural(n), "natural too early");
    n.position = 26.0;

    n.target_cached = false;
    check(!crossfade::should_start_natural(n), "natural target uncached");
    n.target_cached = true;

    // Fades are capped to half the track, so a long setting still starts on
    // a short track near its end.
    n.crossfade_duration = 20.0;
    n.track_duration = 10.0;
    n.position = 5.1;
    check(crossfade::should_start_natural(n), "natural half-track cap");

    const auto g0 = crossfade::gains(0.0);
    check(close_to(g0.incoming, 0.0) && close_to(g0.outgoing, 1.0), "gain start");
    const auto g1 = crossfade::gains(1.0);
    check(close_to(g1.incoming, 1.0) && close_to(g1.outgoing, 0.0), "gain end");
    const auto gmid = crossfade::gains(0.5);
    check(gmid.incoming > 0.0 && gmid.incoming < 1.0, "gain midpoint in range");
    check(close_to(gmid.incoming * gmid.incoming + gmid.outgoing * gmid.outgoing, 1.0),
          "gain constant power");
    const auto gclamp = crossfade::gains(2.0);
    check(close_to(gclamp.incoming, 1.0) && close_to(gclamp.outgoing, 0.0),
          "gain clamps above one");

    Settings s;
    check(crossfade::audio_filter(s).empty(), "filter off");
    s.volume_normalization = true;
    check(crossfade::audio_filter(s) == "loudnorm=I=-16:TP=-1.5:LRA=11",
          "filter loudnorm");
    s.volume_normalization = false;

    // Flat bands are skipped entirely: an enabled-but-flat EQ costs nothing.
    s.eq_enabled = true;
    check(crossfade::audio_filter(s).empty(), "flat eq skipped");
    s.eq_bands[0].gain = 3.0f;
    s.eq_bands[4].gain = -2.0f;
    const std::string eq = crossfade::audio_filter(s);
    check(eq.find("equalizer=f=60.0") != std::string::npos, "filter eq low");
    check(eq.find("equalizer=f=14000.0") != std::string::npos, "filter eq high");
    check(eq.find("g=3.00") != std::string::npos, "filter eq gain");
    check(eq.find("equalizer=") != std::string::npos, "filter eq present");

    s.eq_bands.clear();
    for (int i = 0; i < 31; ++i)
        s.eq_bands.push_back({20.0f * std::pow(10.0f, i / 10.0f), 3.0f, 4.318f});
    const std::string eq31 = crossfade::audio_filter(s);
    size_t count = 0, at = 0;
    while ((at = eq31.find("equalizer=", at)) != std::string::npos)
    {
        ++count;
        at += 10;
    }
    check(count == 31, "filter eq 31 bands");

    s.volume_normalization = true;
    const std::string both = crossfade::audio_filter(s);
    check(both.find("loudnorm") == 0, "filter normalization first");
    check(both.find(",equalizer=f=20.0") != std::string::npos, "filter eq after comma");

    if (testutil::failures == 0)
        std::printf("crossfade: all checks passed\n");
    return testutil::failures == 0 ? 0 : 1;
}
