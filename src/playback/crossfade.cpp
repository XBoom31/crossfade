#include "crossfade.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace crossfade
{
ManualAction plan_manual(bool manual, bool pending, bool active, double elapsed,
                         double seconds)
{
    if (!manual)
        return ManualAction::None;
    if (!pending && !active)
        return ManualAction::Cancel;
    if (active && elapsed >= seconds)
        return ManualAction::Finish;
    if (!active && elapsed >= 1.0)
        return ManualAction::StartFailed;
    return ManualAction::None;
}

bool should_start_natural(const NaturalInputs &in)
{
    if (in.crossfade_duration <= 0.0)
        return false;
    if (in.already_fading)
        return false;
    if (!in.playing || in.loading)
        return false;
    if (in.repeat_one)
        return false;
    if (in.track_duration <= 0.0)
        return false;

    const double cf = std::min(in.crossfade_duration, in.track_duration * 0.5);
    if (cf <= 0.0)
        return false;
    if (in.position <= 0.0 || in.track_duration - in.position > cf)
        return false;
    return in.target_cached;
}

Gains gains(double progress)
{
    const double t = std::clamp(progress, 0.0, 1.0);
    // Humans hear loudness roughly as amplitude^0.6 (Stevens' law), so an
    // equal-gain ramp sounds like it is over after ~1/3 of its length. Raising
    // the incoming gain to t^(1/0.6) makes its perceived level rise linearly
    // across the whole fade. The outgoing gain is the constant-power
    // complement (in^2 + out^2 == 1), so the two tracks' powers sum to one and
    // the overall level doesn't sag in the middle.
    Gains g;
    g.incoming = std::pow(t, 1.0 / 0.6);
    g.outgoing = std::sqrt(std::max(0.0, 1.0 - g.incoming * g.incoming));
    return g;
}

std::string audio_filter(const Settings &s)
{
    std::string af;
    if (s.volume_normalization)
        af = "loudnorm=I=-16:TP=-1.5:LRA=11";
    if (s.eq_enabled)
    {
        for (const EqBand &b : s.eq_bands)
        {
            if (std::fabs(b.gain) < 0.05f || b.freq < 20.0f || b.freq > 20000.0f)
                continue; // flat bands cost nothing
            char band[80];
            std::snprintf(band, sizeof(band), "%sequalizer=f=%.1f:t=q:w=%.3f:g=%.2f",
                          af.empty() ? "" : ",", (double)b.freq, (double)b.q,
                          (double)b.gain);
            af += band;
        }
    }
    return af;
}
} // namespace crossfade
