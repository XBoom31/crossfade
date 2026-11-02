#include "ui_backdrop.h"
#include "ui.h"
#include "ui_theme.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace
{

struct BackdropState
{
    bool init = false;
    ImVec4 target = ImVec4(0.43f, 0.70f, 0.81f, 1.0f);
    ImVec4 color = target;      // displayed tint (eased)
    ImVec4 from_color = target; // start of a crossfade-synced transition
    float visibility = 0.0f;    // raw 0..1 fade
    float strength = 0.65f;     // eased copy of the setting
    float pulse = 0.0f;         // short brightness lift on track change
    float transition_time = 1.0f;
    float transition_duration = 0.0f;
    bool timed_transition = false;
    // Slow orientation drift: the wash and blooms turn a quarter/half turn
    // every so often so the glow never looks pinned in one place.
    float spin = 0.0f;
    float spin_from = 0.0f;
    float spin_to = 0.0f;
    float spin_t = 1.0f;
    float spin_duration = 7.0f;
    float spin_wait = 40.0f;
    bool track_pending = false; // waiting for the new cover's colour
    std::string last_track_id;
};

BackdropState g_backdrop;

// Tiny xorshift PRNG for the orientation jitter. Keeps the drift irregular
// without pulling in <random> for one call.
uint32_t next_rand()
{
    static uint32_t s = 0x9e3779b9u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float smootherstep01(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float color_distance(const ImVec4 &a, const ImVec4 &b)
{
    return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
}

ImVec4 ease_color(const ImVec4 &current, const ImVec4 &target, float speed)
{
    return ImVec4(ui_anim_approach(current.x, target.x, speed),
                  ui_anim_approach(current.y, target.y, speed),
                  ui_anim_approach(current.z, target.z, speed), 1.0f);
}

// Lift dark covers toward a usable glow. Brightness is raised while the
// channel ratios (i.e. the hue) stay intact.
ImVec4 glow_tint(const ImVec4 &c)
{
    ImVec4 out = c;
    out.w = 1.0f;
    const float peak = std::max({out.x, out.y, out.z});
    if (peak > 0.001f && peak < 0.76f)
    {
        const float k = 0.76f / peak;
        out.x = std::min(1.0f, out.x * k);
        out.y = std::min(1.0f, out.y * k);
        out.z = std::min(1.0f, out.z * k);
    }
    return out;
}

ImVec4 mix_white(const ImVec4 &c, float t)
{
    return ImVec4(c.x + (1.0f - c.x) * t,
                  c.y + (1.0f - c.y) * t,
                  c.z + (1.0f - c.z) * t, 1.0f);
}

ImVec4 with_alpha(const ImVec4 &c, float alpha)
{
    return ImVec4(c.x, c.y, c.z, clampf(alpha, 0.0f, 1.0f));
}

ImVec4 lerp4(const ImVec4 &a, const ImVec4 &b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

// Straight-alpha "source over destination" for an opaque destination.
ImVec4 blend_over(const ImVec4 &dst, const ImVec4 &src)
{
    const float a = clampf(src.w, 0.0f, 1.0f);
    return ImVec4(src.x * a + dst.x * (1.0f - a),
                  src.y * a + dst.y * (1.0f - a),
                  src.z * a + dst.z * (1.0f - a), 1.0f);
}

// One ambient bloom. Positions are derived from the slow orientation spin so
// the drawing and the sampling path always agree.
struct GlowSpot
{
    ImVec2 center;
    ImVec2 radius;
    ImVec4 tint;
    float peak = 0.0f;
};

// Every visual parameter of the backdrop, derived from the current animation
// state. Shared by the drawing and the sampling path so the list edge fades
// always match what is actually on screen.
struct BackdropParams
{
    bool light = false;
    float intensity = 0.0f;
    float wash = 0.0f;
    ImVec4 tint;
    ImVec4 tint_soft;
    // Per-corner wash weights, ordered top-left, top-right, bottom-right,
    // bottom-left. Rotated by the current orientation.
    float wash_c[4] = {1.10f, 0.52f, 0.015f, 0.10f};
    GlowSpot spots[4];
    int spot_count = 0;
};

// The un-rotated wash pattern. A quarter-turn of the orientation shifts this
// array by one slot, so the bright corner walks around the screen.
constexpr float k_wash_base[4] = {1.10f, 0.52f, 0.015f, 0.10f};

BackdropParams backdrop_params()
{
    BackdropParams p;
    const float theme_light = ui_theme_progress(); // 0 = night, 1 = day
    p.light = theme_light > 0.5f;

    // Night mode deliberately runs at a fraction of the day-mode intensity
    // so the pure-black OLED base is preserved. The fade-in itself is done by
    // the panel opacity (ui_backdrop_window_bg), which keeps the colour at
    // full strength and makes the crossfade linear and exact.
    const float theme_scale = p.light ? 1.0f : 0.58f;
    p.intensity = g_backdrop.strength * theme_scale *
                  (1.0f + 0.38f * g_backdrop.pulse);

    p.tint = glow_tint(g_backdrop.color);
    p.tint = mix_white(p.tint, 0.06f * g_backdrop.pulse);
    p.tint_soft = mix_white(p.tint, p.light ? 0.16f : 0.08f);
    p.wash = 0.22f * p.intensity;

    const float spin = g_backdrop.spin;
    const float kPi = 3.14159265358979f;

    // Wash: blend between two neighbouring quarter-turn patterns so the light
    // sweeps around instead of snapping corner to corner. The patterns all sum
    // to the same total, so overall brightness never changes.
    const float turns = spin / (kPi * 0.5f);
    const int s0 = (int)std::floor(turns);
    const float f = turns - (float)s0;
    for (int k = 0; k < 4; ++k)
    {
        const float a = k_wash_base[((k - s0) % 4 + 4) % 4];
        const float b = k_wash_base[((k - s0 - 1) % 4 + 4) % 4];
        p.wash_c[k] = a + (b - a) * f;
    }

    // Blooms orbit the screen centre together with the wash. At spin == 0 the
    // anchors match the original hand-tuned layout (album art bottom-left,
    // header top-right, a soft centre lift).
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float w = std::max(1.0f, display.x);
    const float h = std::max(1.0f, display.y);
    auto place = [&](float base_deg, float rx, float ry, const ImVec2 &radius,
                     const ImVec4 &tint, float peak) {
        GlowSpot &s = p.spots[p.spot_count++];
        const float a = spin + base_deg * (kPi / 180.0f);
        s.center = ImVec2(w * (0.5f + std::cos(a) * rx),
                          h * (0.5f + std::sin(a) * ry));
        s.radius = radius;
        s.tint = tint;
        s.peak = peak;
    };
    place(127.8f, 0.31f, 0.40f, ImVec2(w * 0.44f, h * 0.46f), p.tint, 0.18f * p.intensity);
    place(-51.9f, 0.36f, 0.46f, ImVec2(w * 0.50f, h * 0.56f), p.tint_soft, 0.14f * p.intensity);
    place(-36.9f, 0.08f, 0.06f, ImVec2(w * 0.56f, h * 0.60f), p.tint, 0.07f * p.intensity);
    if (p.light)
        place(-115.5f, 0.20f, 0.42f, ImVec2(w * 0.36f, h * 0.32f),
              mix_white(p.tint, 0.55f), 0.05f * p.intensity);

    return p;
}

ImVec4 wash_color_at(const BackdropParams &p, float u, float v)
{
    const ImVec4 tl = with_alpha(p.tint, p.wash * p.wash_c[0]);
    const ImVec4 tr = with_alpha(p.tint_soft, p.wash * p.wash_c[1]);
    const ImVec4 br = with_alpha(p.tint, p.wash * p.wash_c[2]);
    const ImVec4 bl = with_alpha(p.tint, p.wash * p.wash_c[3]);
    const ImVec4 top = lerp4(tl, tr, u);
    const ImVec4 bottom = lerp4(bl, br, u);
    return lerp4(top, bottom, v);
}

// Cumulative alpha contributed by soft_ellipse() at a point. Mirrors the ring
// construction exactly (same step count, same minimum 1/255 alpha).
float ellipse_coverage(const ImVec2 &p, const ImVec2 &center, const ImVec2 &radius, float peak_alpha)
{
    if (peak_alpha <= 0.001f || radius.x <= 0.0f || radius.y <= 0.0f)
        return 0.0f;

    const float nx = (p.x - center.x) / radius.x;
    const float ny = (p.y - center.y) / radius.y;
    const float dist = std::sqrt(nx * nx + ny * ny);
    if (dist >= 1.0f)
        return 0.0f;

    constexpr int kSteps = 22;
    const float alpha = std::max(peak_alpha / (float)kSteps, 1.0f / 255.0f);
    const float t_max = std::pow(std::min(1.0f, (1.0f - dist) / 0.94f), 1.0f / 0.72f);
    const int layers = std::max(0, std::min(kSteps, (int)std::floor(t_max * (kSteps - 1) + 1.0f)));
    if (layers <= 0)
        return 0.0f;
    return 1.0f - std::pow(1.0f - alpha, (float)layers);
}

ImU32 argb(const ImVec4 &c, float alpha)
{
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, clampf(alpha, 0.0f, 1.0f)));
}

// Soft elliptical falloff. Drawing progressively smaller filled ellipses with
// a small constant alpha approximates a blurred radial gradient without a
// shader. Layer alpha is quantised to 8-bit but the resulting steps are below
// what the eye can resolve on an OLED panel.
void soft_ellipse(ImDrawList *dl, const ImVec2 &center, const ImVec2 &radius,
                  const ImVec4 &color, float peak_alpha)
{
    if (peak_alpha <= 0.001f || radius.x <= 0.0f || radius.y <= 0.0f)
        return;

    const int steps = 22;
    const float alpha = std::max(peak_alpha / (float)steps, 1.0f / 255.0f);
    for (int i = 0; i < steps; ++i)
    {
        // Outer rings first, shrinking toward the centre. The 0.72 exponent
        // biases the falloff outward so the glow stays soft at the edges.
        const float t = (float)i / (float)(steps - 1);
        const float shrink = 1.0f - 0.94f * std::pow(t, 0.72f);
        dl->AddEllipseFilled(center, ImVec2(radius.x * shrink, radius.y * shrink),
                             argb(color, alpha), 0.0f, 48);
    }
}

} // namespace

float ui_backdrop_visibility()
{
    const float v = clampf(g_backdrop.visibility, 0.0f, 1.0f);
    return v * v * (3.0f - 2.0f * v); // smoothstep for a gentle fade-in
}

ImVec4 ui_backdrop_window_bg()
{
    ImVec4 bg = ui_c().bg;
    bg.w *= 1.0f - ui_backdrop_visibility();
    return bg;
}

void ui_backdrop_update(const AppState &state)
{
    // Initialise from the app accent so the first fade-in has a sane colour
    // even before any cover has finished loading.
    if (!g_backdrop.init)
    {
        g_backdrop.init = true;
        g_backdrop.target = g_accent;
        g_backdrop.color = g_accent;
        g_backdrop.last_track_id = state.current_track_id;
    }

    // Follow the setting with a smooth ramp instead of a hard cut.
    g_backdrop.strength = ui_anim_approach(
        g_backdrop.strength, clampf(state.settings.album_backdrop_strength, 0.10f, 1.0f), 9.0f);

    float dt = ImGui::GetIO().DeltaTime;
    if (!(dt > 0.0f) || dt > 0.1f)
        dt = 1.0f / 60.0f;

    const float want_visible = state.settings.album_backdrop ? 1.0f : 0.0f;
    g_backdrop.visibility = ui_anim_approach(g_backdrop.visibility, want_visible, 3.2f);

    // A track change arms a "breath" pulse. The colour itself can only be
    // sampled once the cover has decoded, so the pulse is held until then --
    // that way the flourish lands together with the colour transition.
    if (state.current_track_id != g_backdrop.last_track_id)
    {
        g_backdrop.last_track_id = state.current_track_id;
        g_backdrop.track_pending = true;
    }

    bool have_target = false;
    ImVec4 desired = g_backdrop.target;

    // Paused / stopped: rest on the app accent instead of the cover colour,
    // so play/pause breathes the backdrop between the two like the rest of
    // the UI does. (No track loaded at all already lands on the accent.)
    if (!state.playing)
    {
        desired = g_accent;
        have_target = true;
    }
    else if (state.current_thumbnail_url.empty())
    {
        // Nothing loaded: use the app accent (portal / daily colour).
        desired = g_accent;
        have_target = true;
    }
    else
    {
        const TextureColor cover = texture_dominant_color(state.current_thumbnail_url);
        if (cover.ready)
        {
            // Greyscale covers fall back to the accent rather than producing
            // a washed-out grey haze.
            desired = cover.valid ? ImVec4(cover.r, cover.g, cover.b, 1.0f) : g_accent;
            have_target = true;
        }
        // Not decoded yet: keep the previous tint. It will crossfade as soon
        // as the colour is available, which avoids a flash through the accent.
    }

    if (have_target)
    {
        if (color_distance(desired, g_backdrop.target) > 0.006f)
        {
            g_backdrop.target = desired;

            // When an audio crossfade is actually in flight, stretch the
            // colour wash over the same window (using the remaining time, so
            // it lands with the audio rather than running past it). With no
            // crossfade we keep the quick ambient ease.
            const float progress = clampf(state.crossfade_progress, 0.0f, 1.0f);
            const bool sync = ui_animations_enabled() && state.crossfade_active &&
                              state.crossfade_seconds > 0.15f && progress < 0.999f;
            if (sync)
            {
                g_backdrop.from_color = g_backdrop.color;
                g_backdrop.transition_duration =
                    std::max(0.15f, state.crossfade_seconds * (1.0f - progress));
                g_backdrop.transition_time = 0.0f;
                g_backdrop.timed_transition = true;
            }
            else
            {
                g_backdrop.timed_transition = false;
            }
        }

        // Fire the change flourish as soon as we have a usable target for the
        // new track (the cover may have needed a moment to decode).
        if (g_backdrop.track_pending)
        {
            g_backdrop.pulse = 1.0f;
            g_backdrop.track_pending = false;
        }
    }

    const ImVec4 previous = g_backdrop.color;
    if (g_backdrop.timed_transition)
    {
        const float duration = std::max(0.05f, g_backdrop.transition_duration);
        g_backdrop.transition_time = std::min(1.0f, g_backdrop.transition_time + dt / duration);
        g_backdrop.color = lerp4(g_backdrop.from_color, g_backdrop.target,
                                 smootherstep01(g_backdrop.transition_time));
        if (g_backdrop.transition_time >= 1.0f)
            g_backdrop.timed_transition = false;
        ui_anim_request();
    }
    else
    {
        g_backdrop.color = ease_color(g_backdrop.color, g_backdrop.target, 2.35f);
    }

    if (g_backdrop.pulse > 0.0f)
    {
        g_backdrop.pulse = std::max(0.0f, g_backdrop.pulse - dt / 0.85f);
        ui_anim_request();
    }

    // Orientation drift. Every so often the whole light arrangement (wash
    // corner + bloom anchors) turns a quarter or half turn, eased over several
    // seconds. Only runs while the wash is actually on screen.
    if (state.settings.backdrop_drift && ui_animations_enabled() &&
        g_backdrop.visibility > 0.02f)
    {
        if (g_backdrop.spin_t >= 1.0f)
        {
            g_backdrop.spin_wait -= dt;
            if (g_backdrop.spin_wait <= 0.0f)
            {
                const uint32_t r = next_rand();
                const int quarters = 1 + (int)(r & 1u); // 90 or 180 degrees
                g_backdrop.spin_from = g_backdrop.spin;
                g_backdrop.spin_to = g_backdrop.spin + (float)quarters * 1.5707963f;
                g_backdrop.spin_t = 0.0f;
                g_backdrop.spin_duration = 6.0f + (float)((r >> 8) % 300u) / 100.0f;
                g_backdrop.spin_wait = 45.0f + (float)((r >> 16) % 4500u) / 100.0f;
            }
        }
        if (g_backdrop.spin_t < 1.0f)
        {
            g_backdrop.spin_t =
                std::min(1.0f, g_backdrop.spin_t + dt / std::max(0.5f, g_backdrop.spin_duration));
            g_backdrop.spin = g_backdrop.spin_from +
                              (g_backdrop.spin_to - g_backdrop.spin_from) *
                                  smootherstep01(g_backdrop.spin_t);
            ui_anim_request();
        }
    }

    // Keep the frame loop awake only while something is actually moving.
    if (g_backdrop.timed_transition ||
        color_distance(previous, g_backdrop.color) > 0.0006f ||
        std::fabs(g_backdrop.visibility - want_visible) > 0.002f ||
        std::fabs(g_backdrop.strength - state.settings.album_backdrop_strength) > 0.004f)
    {
        ui_anim_request();
    }
}

void ui_backdrop_draw()
{
    const float visibility = ui_backdrop_visibility();
    if (visibility <= 0.001f)
        return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float w = display.x;
    const float h = display.y;
    if (w <= 1.0f || h <= 1.0f)
        return;

    const BackdropParams p = backdrop_params();
    if (p.intensity <= 0.0005f)
        return;

    ImDrawList *dl = ImGui::GetBackgroundDrawList();

    // Broad diagonal wash. The bright corner walks around the screen as the
    // orientation drifts; four-corner interpolation keeps it perfectly smooth
    // on low-end GPUs.
    dl->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(w, h),
                                argb(p.tint, p.wash * p.wash_c[0]),
                                argb(p.tint_soft, p.wash * p.wash_c[1]),
                                argb(p.tint, p.wash * p.wash_c[2]),
                                argb(p.tint, p.wash * p.wash_c[3]));

    // Ambient blooms. They orbit with the wash so the glow feels connected to
    // the artwork rather than a flat screen tint.
    for (int i = 0; i < p.spot_count; ++i)
        soft_ellipse(dl, p.spots[i].center, p.spots[i].radius, p.spots[i].tint,
                     p.spots[i].peak);
}

ImVec4 ui_backdrop_tint()
{
    return glow_tint(g_backdrop.color);
}

ImVec4 ui_backdrop_sample(const ImVec2 &screen_pos)
{
    ImVec4 base = ui_c().bg;
    base.w = 1.0f;
    ImVec4 color = base;

    const float visibility = ui_backdrop_visibility();
    if (visibility <= 0.001f)
        return color;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 1.0f || display.y <= 1.0f)
        return color;

    const BackdropParams p = backdrop_params();
    const float w = display.x;
    const float h = display.y;

    // Wash (bilinear corner interpolation, same as AddRectFilledMultiColor).
    const float u = clampf(screen_pos.x / w, 0.0f, 1.0f);
    const float v = clampf(screen_pos.y / h, 0.0f, 1.0f);
    color = blend_over(color, wash_color_at(p, u, v));

    // Blooms, in the same order they are drawn.
    for (int i = 0; i < p.spot_count; ++i)
    {
        const float a = ellipse_coverage(screen_pos, p.spots[i].center,
                                         p.spots[i].radius, p.spots[i].peak);
        if (a > 0.0f)
            color = blend_over(color, with_alpha(p.spots[i].tint, a));
    }

    // The panels fade their opacity in, so the perceived colour is the
    // backdrop composited at `visibility` over the base surface.
    return lerp4(base, color, visibility);
}
