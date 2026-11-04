#pragma once

#include "ui.h"

#include <atomic>
#include <chrono>
#include <string>

// Crossfade state and pure decisions. The mpv handoff itself stays in the app
// loop for now: moving it would require threading a large set of playback
// callbacks through this module, which is a behavior-risk refactor rather
// than the testable extraction this file is for.
namespace crossfade
{
struct State
{
    // pending: the helper player was asked to start the incoming track.
    // active: the helper actually started, so the outgoing "end" must promote
    // it instead of advancing normally.
    std::atomic<bool> pending{false};
    std::atomic<bool> active{false};
    // manual: the fade was started by a click/Next rather than by the
    // end-of-track watcher, so it finishes on a timer instead of EOF.
    std::atomic<bool> manual{false};

    float applied_duration = -1.0f;
    Track track;
    int index = -1;
    std::chrono::steady_clock::time_point start;
    float user_volume = 100.0f;
    double seconds = 0.0;

    void clear_track()
    {
        track = Track{};
        index = -1;
    }

    void reset_flags()
    {
        pending = false;
        active = false;
        manual = false;
    }
};

enum class ManualAction
{
    None,
    Finish,
    StartFailed,
    Cancel
};

// Decide what to do with a manual (timer-driven) fade. Mirrors the branches
// that used to live inline in maybe_start_crossfade().
ManualAction plan_manual(bool manual, bool pending, bool active, double elapsed,
                         double seconds);

struct NaturalInputs
{
    double crossfade_duration = 0.0;
    double track_duration = 0.0;
    double position = 0.0;
    bool playing = false;
    bool loading = false;
    bool already_fading = false;
    bool repeat_one = false;
    bool target_cached = false;
};

// True when the end-of-track watcher should start a natural crossfade.
bool should_start_natural(const NaturalInputs &in);

struct Gains
{
    double incoming = 0.0;
    double outgoing = 1.0;
};

// Perceptually even constant-power ramp. `progress` is 0..1.
Gains gains(double progress);

// mpv audio filter chain for normalization + the five-band equalizer.
std::string audio_filter(const Settings &settings);
} // namespace crossfade
