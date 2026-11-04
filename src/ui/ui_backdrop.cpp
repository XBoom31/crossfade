#include "ui_backdrop.h"
#include "ui.h"
#include "ui_theme.h"
#include "texture.h"

#include <GL/gl.h>
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    bool crossfade_synced = false;
    // Slow orientation drift: the wash and blooms turn a quarter/half turn
    // every so often so the glow never looks pinned in one place.
    float spin = 0.0f;
    float spin_from = 0.0f;
    float spin_to = 0.0f;
    float spin_t = 1.0f;
    float spin_duration = 7.0f;
    float spin_wait = 40.0f;
    bool track_pending = false; // waiting for the new cover's colour
    bool use_shader = false;
    bool shader_failed = false;
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
    g_backdrop.use_shader = state.settings.backdrop_shader;

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

            // When an audio crossfade is actually in flight, blend the wash
            // through the same 34-66% window as the bottom-bar metadata. That
            // keeps the colour change centered on the visual hand-off instead
            // of running behind it. With no crossfade use the quick ease.
            const bool sync = ui_animations_enabled() && state.crossfade_active &&
                              state.crossfade_seconds > 0.15f;
            if (sync)
            {
                g_backdrop.from_color = g_backdrop.color;
                g_backdrop.crossfade_synced = true;
                g_backdrop.timed_transition = false;
            }
            else
            {
                g_backdrop.crossfade_synced = false;
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
    if (g_backdrop.crossfade_synced && state.crossfade_active)
    {
        const float p = clampf((state.crossfade_progress - 0.34f) / 0.32f, 0.0f, 1.0f);
        g_backdrop.color = lerp4(g_backdrop.from_color, g_backdrop.target,
                                 smootherstep01(p));
        ui_anim_request();
    }
    else
    {
        g_backdrop.crossfade_synced = false;
        if (g_backdrop.timed_transition)
        {
            const float duration = std::max(0.05f, g_backdrop.transition_duration);
            g_backdrop.transition_time =
                std::min(1.0f, g_backdrop.transition_time + dt / duration);
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

    // The shader path paints the same wash in one pass after glClear().
    if (g_backdrop.use_shader && !g_backdrop.shader_failed)
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

// ----------------------------------------------------------------------
// Shader backdrop
//
// The ImGui primitive path stacks a 4-corner wash and dozens of translucent
// ellipses. That is fine on an IPS panel but OLED near-black response makes
// the 8-bit alpha/colour steps visible. This path draws one full-screen
// triangle, evaluates the same wash/blooms in the fragment shader, and adds a
// 1-LSB dither before output. It also removes the heavy ellipse overdraw.
// ----------------------------------------------------------------------
namespace
{
// Windows' GL/gl.h only covers GL 1.1. These are the few GL 2 enums/types the
// shader pass needs; Linux GL headers already define them.
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GLchar
typedef char GLchar;
#endif

// The system GL 1.1 headers don't declare the GL 2/3 entry points we need, and
// pulling in a full loader just for one pass is overkill. Resolve the handful
// of functions through SDL; this works with both WGL and GLX.
typedef GLuint(APIENTRY *PFN_CreateShader)(GLenum);
typedef void(APIENTRY *PFN_ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void(APIENTRY *PFN_CompileShader)(GLuint);
typedef void(APIENTRY *PFN_GetShaderiv)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *PFN_GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void(APIENTRY *PFN_DeleteShader)(GLuint);
typedef GLuint(APIENTRY *PFN_CreateProgram)(void);
typedef void(APIENTRY *PFN_AttachShader)(GLuint, GLuint);
typedef void(APIENTRY *PFN_LinkProgram)(GLuint);
typedef void(APIENTRY *PFN_GetProgramiv)(GLuint, GLenum, GLint *);
typedef void(APIENTRY *PFN_GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void(APIENTRY *PFN_DeleteProgram)(GLuint);
typedef void(APIENTRY *PFN_UseProgram)(GLuint);
typedef GLint(APIENTRY *PFN_GetUniformLocation)(GLuint, const GLchar *);
typedef void(APIENTRY *PFN_Uniform1f)(GLint, GLfloat);
typedef void(APIENTRY *PFN_Uniform2f)(GLint, GLfloat, GLfloat);
typedef void(APIENTRY *PFN_Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY *PFN_Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void(APIENTRY *PFN_Uniform1fv)(GLint, GLsizei, const GLfloat *);
typedef void(APIENTRY *PFN_Uniform2fv)(GLint, GLsizei, const GLfloat *);
typedef void(APIENTRY *PFN_Uniform3fv)(GLint, GLsizei, const GLfloat *);
typedef void(APIENTRY *PFN_GenVertexArrays)(GLsizei, GLuint *);
typedef void(APIENTRY *PFN_BindVertexArray)(GLuint);
typedef void(APIENTRY *PFN_DrawArrays)(GLenum, GLint, GLsizei);

PFN_CreateShader p_glCreateShader = nullptr;
PFN_ShaderSource p_glShaderSource = nullptr;
PFN_CompileShader p_glCompileShader = nullptr;
PFN_GetShaderiv p_glGetShaderiv = nullptr;
PFN_GetShaderInfoLog p_glGetShaderInfoLog = nullptr;
PFN_DeleteShader p_glDeleteShader = nullptr;
PFN_CreateProgram p_glCreateProgram = nullptr;
PFN_AttachShader p_glAttachShader = nullptr;
PFN_LinkProgram p_glLinkProgram = nullptr;
PFN_GetProgramiv p_glGetProgramiv = nullptr;
PFN_GetProgramInfoLog p_glGetProgramInfoLog = nullptr;
PFN_DeleteProgram p_glDeleteProgram = nullptr;
PFN_UseProgram p_glUseProgram = nullptr;
PFN_GetUniformLocation p_glGetUniformLocation = nullptr;
PFN_Uniform1f p_glUniform1f = nullptr;
PFN_Uniform2f p_glUniform2f = nullptr;
PFN_Uniform3f p_glUniform3f = nullptr;
PFN_Uniform4f p_glUniform4f = nullptr;
PFN_Uniform1fv p_glUniform1fv = nullptr;
PFN_Uniform2fv p_glUniform2fv = nullptr;
PFN_Uniform3fv p_glUniform3fv = nullptr;
PFN_GenVertexArrays p_glGenVertexArrays = nullptr;
PFN_BindVertexArray p_glBindVertexArray = nullptr;
PFN_DrawArrays p_glDrawArrays = nullptr;

template <typename T>
bool load_gl_fn(T &fn, const char *name)
{
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    return fn != nullptr;
}

GLuint g_shader_program = 0;
GLuint g_shader_vao = 0;
struct ShaderUniforms
{
    GLint resolution = -1;
    GLint background = -1;
    GLint tint = -1;
    GLint tint_soft = -1;
    GLint wash = -1;
    GLint wash_corners = -1;
    GLint centers = -1;
    GLint radii = -1;
    GLint spot_tints = -1;
    GLint peaks = -1;
} g_shader_uniforms;

GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = p_glCreateShader(type);
    p_glShaderSource(shader, 1, &source, nullptr);
    p_glCompileShader(shader);
    GLint ok = GL_FALSE;
    p_glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {0};
        p_glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "Backdrop shader compile failed: %s\n", log);
        p_glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool init_backdrop_shader()
{
    if (g_shader_program != 0)
        return true;
    if (g_backdrop.shader_failed)
        return false;

    if (!load_gl_fn(p_glCreateShader, "glCreateShader") ||
        !load_gl_fn(p_glShaderSource, "glShaderSource") ||
        !load_gl_fn(p_glCompileShader, "glCompileShader") ||
        !load_gl_fn(p_glGetShaderiv, "glGetShaderiv") ||
        !load_gl_fn(p_glGetShaderInfoLog, "glGetShaderInfoLog") ||
        !load_gl_fn(p_glDeleteShader, "glDeleteShader") ||
        !load_gl_fn(p_glCreateProgram, "glCreateProgram") ||
        !load_gl_fn(p_glAttachShader, "glAttachShader") ||
        !load_gl_fn(p_glLinkProgram, "glLinkProgram") ||
        !load_gl_fn(p_glGetProgramiv, "glGetProgramiv") ||
        !load_gl_fn(p_glGetProgramInfoLog, "glGetProgramInfoLog") ||
        !load_gl_fn(p_glDeleteProgram, "glDeleteProgram") ||
        !load_gl_fn(p_glUseProgram, "glUseProgram") ||
        !load_gl_fn(p_glGetUniformLocation, "glGetUniformLocation") ||
        !load_gl_fn(p_glUniform1f, "glUniform1f") ||
        !load_gl_fn(p_glUniform2f, "glUniform2f") ||
        !load_gl_fn(p_glUniform3f, "glUniform3f") ||
        !load_gl_fn(p_glUniform4f, "glUniform4f") ||
        !load_gl_fn(p_glUniform1fv, "glUniform1fv") ||
        !load_gl_fn(p_glUniform2fv, "glUniform2fv") ||
        !load_gl_fn(p_glUniform3fv, "glUniform3fv") ||
        !load_gl_fn(p_glGenVertexArrays, "glGenVertexArrays") ||
        !load_gl_fn(p_glBindVertexArray, "glBindVertexArray") ||
        !load_gl_fn(p_glDrawArrays, "glDrawArrays"))
    {
        std::fprintf(stderr, "Backdrop shader requires GL 3 entry points\n");
        g_backdrop.shader_failed = true;
        return false;
    }

    static const char *kVertex = R"GLSL(#version 130
out vec2 v_uv;
void main()
{
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0,
                  gl_VertexID == 2 ? 3.0 : -1.0);
    v_uv = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

    static const char *kFragment = R"GLSL(#version 130
in vec2 v_uv;
out vec4 frag_color;

uniform vec2 u_resolution;
uniform vec3 u_background;
uniform vec3 u_tint;
uniform vec3 u_tint_soft;
uniform float u_wash;
uniform vec4 u_wash_corners;
uniform vec2 u_centers[4];
uniform vec2 u_radii[4];
uniform vec3 u_spot_tints[4];
uniform float u_peaks[4];

void main()
{
    // The full-screen triangle is in OpenGL clip space (y = -1 at the
    // bottom), while the wash weights and bloom centers are in ImGui screen
    // coordinates (y = 0 at the top). Flip once so the glow lands where the
    // CPU sampler says it should.
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
    vec2 px = uv * u_resolution;

    vec3 tl = u_tint;
    vec3 tr = u_tint_soft;
    vec3 br = u_tint;
    vec3 bl = u_tint;
    float atl = u_wash * u_wash_corners.x;
    float atr = u_wash * u_wash_corners.y;
    float abr = u_wash * u_wash_corners.z;
    float abl = u_wash * u_wash_corners.w;

    vec3 top = mix(tl, tr, uv.x);
    float atop = mix(atl, atr, uv.x);
    vec3 bottom = mix(bl, br, uv.x);
    float abot = mix(abl, abr, uv.x);
    vec3 col = mix(u_background, mix(top, bottom, uv.y),
                   mix(atop, abot, uv.y));

    for (int i = 0; i < 4; ++i)
    {
        float peak = u_peaks[i];
        if (peak <= 0.0001)
            continue;
        vec2 d = (px - u_centers[i]) / u_radii[i];
        float dist = length(d);
        if (dist < 1.0)
        {
            // Continuous form of the old 22-ring falloff.
            float t = pow(clamp((1.0 - dist) / 0.94, 0.0, 1.0), 1.0 / 0.72);
            float cov = 1.0 - exp(-peak * t);
            col = mix(col, u_spot_tints[i], cov);
        }
    }

    // One LSB of static dither. Enough to break 8-bit contour lines without
    // looking like grain.
    float n = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    col += (n - 0.5) / 255.0;
    frag_color = vec4(col, 1.0);
}
)GLSL";

    const GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertex);
    const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs)
    {
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
        g_backdrop.shader_failed = true;
        return false;
    }

    g_shader_program = p_glCreateProgram();
    p_glAttachShader(g_shader_program, vs);
    p_glAttachShader(g_shader_program, fs);
    p_glLinkProgram(g_shader_program);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);

    GLint linked = GL_FALSE;
    p_glGetProgramiv(g_shader_program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[1024] = {0};
        p_glGetProgramInfoLog(g_shader_program, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "Backdrop shader link failed: %s\n", log);
        p_glDeleteProgram(g_shader_program);
        g_shader_program = 0;
        g_backdrop.shader_failed = true;
        return false;
    }

    g_shader_uniforms.resolution = p_glGetUniformLocation(g_shader_program, "u_resolution");
    g_shader_uniforms.background = p_glGetUniformLocation(g_shader_program, "u_background");
    g_shader_uniforms.tint = p_glGetUniformLocation(g_shader_program, "u_tint");
    g_shader_uniforms.tint_soft = p_glGetUniformLocation(g_shader_program, "u_tint_soft");
    g_shader_uniforms.wash = p_glGetUniformLocation(g_shader_program, "u_wash");
    g_shader_uniforms.wash_corners = p_glGetUniformLocation(g_shader_program, "u_wash_corners");
    g_shader_uniforms.centers = p_glGetUniformLocation(g_shader_program, "u_centers[0]");
    g_shader_uniforms.radii = p_glGetUniformLocation(g_shader_program, "u_radii[0]");
    g_shader_uniforms.spot_tints = p_glGetUniformLocation(g_shader_program, "u_spot_tints[0]");
    g_shader_uniforms.peaks = p_glGetUniformLocation(g_shader_program, "u_peaks[0]");

    p_glGenVertexArrays(1, &g_shader_vao);
    return true;
}
} // namespace

void ui_backdrop_render_shader()
{
    if (!g_backdrop.use_shader || g_backdrop.shader_failed)
        return;
    if (ui_backdrop_visibility() <= 0.001f)
        return;

    const BackdropParams p = backdrop_params();
    if (p.intensity <= 0.0005f || p.wash <= 0.0005f)
        return;
    if (!init_backdrop_shader())
        return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const GLfloat w = (GLfloat)display.x;
    const GLfloat h = (GLfloat)display.y;
    if (w <= 1.0f || h <= 1.0f)
        return;

    GLfloat centers[8] = {0};
    GLfloat radii[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    GLfloat tints[12] = {0};
    GLfloat peaks[4] = {0};
    const int n = std::min(4, p.spot_count);
    for (int i = 0; i < n; ++i)
    {
        centers[i * 2 + 0] = p.spots[i].center.x;
        centers[i * 2 + 1] = p.spots[i].center.y;
        radii[i * 2 + 0] = std::max(1.0f, p.spots[i].radius.x);
        radii[i * 2 + 1] = std::max(1.0f, p.spots[i].radius.y);
        tints[i * 3 + 0] = p.spots[i].tint.x;
        tints[i * 3 + 1] = p.spots[i].tint.y;
        tints[i * 3 + 2] = p.spots[i].tint.z;
        peaks[i] = p.spots[i].peak;
    }

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, (GLsizei)w, (GLsizei)h);

    p_glUseProgram(g_shader_program);
    p_glUniform2f(g_shader_uniforms.resolution, w, h);
    p_glUniform3f(g_shader_uniforms.background, ui_c().bg.x, ui_c().bg.y, ui_c().bg.z);
    p_glUniform3f(g_shader_uniforms.tint, p.tint.x, p.tint.y, p.tint.z);
    p_glUniform3f(g_shader_uniforms.tint_soft, p.tint_soft.x, p.tint_soft.y, p.tint_soft.z);
    p_glUniform1f(g_shader_uniforms.wash, p.wash);
    p_glUniform4f(g_shader_uniforms.wash_corners, p.wash_c[0], p.wash_c[1],
                  p.wash_c[2], p.wash_c[3]);
    p_glUniform2fv(g_shader_uniforms.centers, 4, centers);
    p_glUniform2fv(g_shader_uniforms.radii, 4, radii);
    p_glUniform3fv(g_shader_uniforms.spot_tints, 4, tints);
    p_glUniform1fv(g_shader_uniforms.peaks, 4, peaks);

    p_glBindVertexArray(g_shader_vao);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
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

    // Blooms, in the same order they are drawn. The shader uses a continuous
    // exponential falloff; the primitive path uses stacked ellipses. Match the
    // active path so list/lyrics edge fades dissolve into the real backdrop.
    for (int i = 0; i < p.spot_count; ++i)
    {
        float a = 0.0f;
        if (g_backdrop.use_shader)
        {
            const float nx = (screen_pos.x - p.spots[i].center.x) /
                             std::max(1.0f, p.spots[i].radius.x);
            const float ny = (screen_pos.y - p.spots[i].center.y) /
                             std::max(1.0f, p.spots[i].radius.y);
            const float dist = std::sqrt(nx * nx + ny * ny);
            if (dist < 1.0f)
            {
                const float t = std::pow(clampf((1.0f - dist) / 0.94f, 0.0f, 1.0f),
                                         1.0f / 0.72f);
                a = 1.0f - std::exp(-p.spots[i].peak * t);
            }
        }
        else
        {
            a = ellipse_coverage(screen_pos, p.spots[i].center,
                                 p.spots[i].radius, p.spots[i].peak);
        }
        if (a > 0.0f)
            color = blend_over(color, with_alpha(p.spots[i].tint, a));
    }

    // The panels fade their opacity in, so the perceived colour is the
    // backdrop composited at `visibility` over the base surface.
    return lerp4(base, color, visibility);
}
