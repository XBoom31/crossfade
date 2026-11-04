#include "ui.h"
#include "ui_backdrop.h"
#include "ui_icons.h"
#include "ui_components.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "mpvplayer.h"
#include "texture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

static constexpr float kTau = 6.28318530717958647692f;
static std::unordered_map<ImGuiID, float> g_control_hover;

template <typename LabelFn>
static bool draw_progress_bar(const char *id, float *value01, float width, float height,
                              ImU32 fill_color, ImU32 bg_color, LabelFn hover_label)
{
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    bool changed = false;

    if (active && width > 0.0f)
    {
        float t = (io.MousePos.x - pos.x) / width;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        *value01 = t;
        changed = true;
    }

    const float track_h = (hovered || active) ? ui_px(4.5f) : ui_px(3.0f);
    const float track_y = pos.y + (height - track_h) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(pos.x, track_y), ImVec2(pos.x + width, track_y + track_h), bg_color, track_h * 0.5f);

    float fill_w = width * (*value01 < 0.0f ? 0.0f : (*value01 > 1.0f ? 1.0f : *value01));
    if (fill_w > 0.5f)
        dl->AddRectFilled(ImVec2(pos.x, track_y), ImVec2(pos.x + fill_w, track_y + track_h), fill_color, track_h * 0.5f);
    if (hovered || active)
        dl->AddCircleFilled(ImVec2(pos.x + fill_w, track_y + track_h * 0.5f),
                            active ? ui_px(6.0f) : ui_px(5.0f), fill_color, 16);

    if ((hovered || active) && width > 0.0f)
    {
        float hover_t = active ? *value01 : (io.MousePos.x - pos.x) / width;
        hover_t = hover_t < 0.0f ? 0.0f : (hover_t > 1.0f ? 1.0f : hover_t);
        std::string label = hover_label(hover_t);
        if (!label.empty())
            ImGui::SetTooltip("%s", label.c_str());
    }
    return changed;
}

static std::string fmt_time(int secs)
{
    if (secs < 0)
        secs = 0;
    int m = secs / 60, s = secs % 60;
    return std::to_string(m) + ":" + (s < 10 ? "0" : "") + std::to_string(s);
}

static void draw_round_control(AppState &state, const char *id, UiIcon icon, float size,
                               bool primary, bool active, const char *tip)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();

    ui_prune_hover_map(g_control_hover);
    float &hover_t = g_control_hover[ImGui::GetItemID()];
    const float target = pressed ? -0.7f : (hovered ? 1.0f : 0.0f);
    hover_t = ui_anim_approach(hover_t, target, 18.0f);
    if (ui_animations_enabled() && fabsf(hover_t - target) > 0.002f)
        ui_anim_request();
    const float scale = ui_animations_enabled() ? 1.0f + 0.045f * hover_t : 1.0f;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    const float r = size * 0.5f * scale;
    if (primary)
        dl->AddCircleFilled(c, r, ui_accent_u32(hovered ? 1.0f : 0.92f), 36);
    else if (hovered || active)
        dl->AddCircleFilled(c, r, active ? ui_accent_u32(0.18f) : ui_overlay_u32(0.07f), 32);
    const ImU32 col = primary ? ui_col(ui_c().on_accent)
                              : (active ? ui_accent_text_u32() : (hovered ? ui_text_u32() : ui_text_dim_u32()));
    ui_draw_icon(dl, icon, c, size * (primary ? 0.40f : 0.36f) * scale, col, primary ? 1.6f : 1.5f);
    if (tip && hovered)
        ImGui::SetTooltip("%s", tip);
    if (clicked)
    {
        if (icon == UiIcon::Play)
            state.user_play_requested = true;
        else if (icon == UiIcon::Pause)
            state.user_pause_requested = true;
        else if (icon == UiIcon::Prev)
            state.prev_requested = true;
        else if (icon == UiIcon::Next)
            state.next_requested = true;
    }
}

void draw_bottom_bar(AppState &state, MpvPlayer &player)
{
    ImGuiIO &io = ImGui::GetIO();
    const AppLayout l = ui_compute_layout(state, io);
    const UiMetrics &m = ui_m();
    const float bar_h = l.bottom_h;
    const float bar_w = io.DisplaySize.x;
    const float bar_y = io.DisplaySize.y - bar_h;

    ScopedNoMinSize no_min_size;
    ImGui::SetNextWindowPos(ImVec2(0, bar_y));
    ImGui::SetNextWindowSize(ImVec2(bar_w, bar_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_backdrop_window_bg());
    ImGui::Begin("BottomBar", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();

    // The separator between the main UI and the bar doubles as the seek
    // indicator: it fills with accent as the track progresses, and the strip
    // can be clicked / dragged to seek. The knob rides a spring (slightly
    // under-damped, so it overshoots and settles with a bounce) and a sine
    // wave ripples along the line while the strip is hovered.
    {
        const bool fading = state.crossfade_active;
        const bool show_incoming = fading && state.crossfade_progress >= 0.5f;
        float edge_pos = show_incoming ? (float)state.crossfade_position
                                       : (float)player.get_property_double("time-pos");
        float edge_dur = show_incoming ? (float)state.crossfade_duration
                                       : (float)player.get_property_double("duration");
        if (edge_pos < 0.0f)
            edge_pos = 0.0f;
        const bool have_dur = edge_dur > 0.0f;
        const bool seekable = !state.crossfade_active;
        const float edge_frac = have_dur ? std::clamp(edge_pos / edge_dur, 0.0f, 1.0f) : 0.0f;

        static bool s_edge_seeking = false;
        static float s_knob_frac = -1.0f;   // spring position (0..1 of the bar)
        static float s_knob_vel = 0.0f;     // spring velocity
        static float s_knob_r = 0.0f;       // spring radius
        static float s_knob_rvel = 0.0f;    // spring radius velocity
        static float s_hover_amt = 0.0f;    // eased 0..1 hover weight (drives the wave)
        static double s_hold_until = 0.0;   // after a seek: trust the seeked frac until mpv catches up
        static float s_hold_frac = 0.0f;

        if (!seekable)
        {
            s_edge_seeking = false;
            s_hold_until = 0.0;
        }
        const float edge_hover_h = 12.0f * m.layout_scale;
        const bool edge_hover = have_dur && seekable && ImGui::IsMouseHoveringRect(
            ImVec2(wp.x, wp.y), ImVec2(wp.x + ws.x, wp.y + edge_hover_h));
        if (edge_hover)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(0))
                s_edge_seeking = true;
        }
        const float mouse_frac = std::clamp((io.MousePos.x - wp.x) / ws.x, 0.0f, 1.0f);
        if (s_edge_seeking && !ImGui::IsMouseDown(0))
        {
            // Seek once on release (same semantics as the in-bar seek bar,
            // so a drag doesn't hammer mpv with seeks). The spring keeps
            // gliding to the seeked position while mpv reports it back.
            player.seek(mouse_frac * edge_dur);
            s_hold_frac = mouse_frac;
            s_hold_until = ImGui::GetTime() + 0.7;
            s_edge_seeking = false;
        }

        const double now = ImGui::GetTime();
        const float dt = std::min(io.DeltaTime, 0.05f); // spring stays stable across hitches

        // Spring target: the mouse while grabbing, the seeked fraction during
        // the post-seek hold, otherwise the reported play position.
        const float spring_target = s_edge_seeking ? mouse_frac
                                    : now < s_hold_until ? s_hold_frac
                                    : edge_frac;

        s_hover_amt += ((edge_hover || s_edge_seeking ? 1.0f : 0.0f) - s_hover_amt) *
                       std::min(1.0f, dt * 10.0f);

        // ---- Position spring ----
        // While grabbing: stiff + heavily damped (tracks the cursor). While
        // free: softer and under-damped, so a seek lands with a small bounce.
        const float k = s_edge_seeking ? 380.0f : 170.0f;
        const float d = s_edge_seeking ? 30.0f : 12.5f;
        if (s_knob_frac < 0.0f)
        {
            s_knob_frac = edge_frac;
            s_knob_vel = 0.0f;
        }
        else if (!s_edge_seeking && now >= s_hold_until &&
                 std::fabs(edge_frac - s_knob_frac) > 0.15f)
        {
            s_knob_frac = edge_frac; // track change / scrub elsewhere: snap, don't sweep back
            s_knob_vel = 0.0f;
        }
        else
        {
            s_knob_vel += (spring_target - s_knob_frac) * k * dt;
            s_knob_vel *= std::exp(-d * dt);
            s_knob_frac += s_knob_vel * dt;
            s_knob_frac = std::clamp(s_knob_frac, -0.02f, 1.02f);
            if (std::fabs(spring_target - s_knob_frac) < 0.0004f && std::fabs(s_knob_vel) < 0.002f)
            {
                s_knob_frac = spring_target; // settled: snap to kill denormal creep
                s_knob_vel = 0.0f;
            }
        }

        // ---- Radius spring (same integrator; pops on grab, settles when idle) ----
        const float r_target = have_dur ? ((s_edge_seeking ? 9.5f : edge_hover ? 7.0f : 4.0f) *
                                            m.layout_scale)
                                        : 0.0f;
        s_knob_rvel += (r_target - s_knob_r) * 300.0f * dt;
        s_knob_rvel *= std::exp(-16.0f * dt);
        s_knob_r += s_knob_rvel * dt;
        if (std::fabs(r_target - s_knob_r) < 0.02f && std::fabs(s_knob_rvel) < 0.05f)
        {
            s_knob_r = r_target;
            s_knob_rvel = 0.0f;
        }

        const float display_frac = std::clamp(s_knob_frac, 0.0f, 1.0f);
        const float edge_h = (edge_hover || s_edge_seeking) ? 4.0f : 2.0f;
        dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + ws.x, wp.y + edge_h), ui_border_u32());
        if (display_frac > 0.0f)
            dl->AddRectFilled(ImVec2(wp.x, wp.y),
                              ImVec2(wp.x + ws.x * display_frac, wp.y + edge_h),
                              ui_accent_u32());

        // Foreground list: the knob pokes above the bar's top edge, but this
        // window's clip rectangle starts there -- the foreground list isn't
        // clipped, so the full circle stays visible.
        ImDrawList *fdl = ImGui::GetForegroundDrawList();

        // ---- Hover waves: a layered sine field travelling along the line ----
        // Three phase-shifted strokes over a soft filled ribbon between two
        // counter-phased curves, so the effect reads as water with width
        // rather than a single wobbly line. ~5 sampled curves of 48 segments
        // -- a few hundred vertices, all stateless.
        const float wave_amp = 3.0f * m.layout_scale * s_hover_amt;
        if (wave_amp > 0.15f)
        {
            const float tt = (float)now;
            const float mid_y = wp.y + edge_h * 0.5f;
            const int N = 48;

            auto wave_y = [&](float fx, float k_wl, float speed, float phase)
            {
                return std::sin(fx * ws.x * (6.2831853f / (k_wl * m.layout_scale)) -
                                tt * speed + phase);
            };

            // Soft body: the main curve drawn as three nested strokes --
            // wide + faint to thin + bright -- reads as a glowing band with
            // real width (a concave fill would need tessellation; this
            // doesn't).
            {
                const float wl = 240.0f, speed = 3.2f;
                struct Body
                {
                    float amp, alpha, thickness;
                };
                const Body body[] = {
                    {1.00f, 0.07f, 7.0f}, // wide haze
                    {1.00f, 0.14f, 4.0f}, // mid glow
                    {0.90f, 0.40f, 2.2f}, // bright core
                };
                for (const Body &B : body)
                {
                    const float amp = wave_amp * B.amp;
                    fdl->PathClear();
                    for (int i = 0; i <= N; ++i)
                    {
                        const float fx = (float)i / (float)N;
                        const float y = mid_y + wave_y(fx, wl, speed, 0.0f) * amp;
                        fdl->PathLineTo(ImVec2(wp.x + ws.x * fx, y));
                    }
                    fdl->PathStroke(ui_accent_u32(B.alpha * s_hover_amt), 0,
                                    B.thickness * m.layout_scale);
                }
            }

            // Stroke layers: three travelling sines at different wavelengths
            // and speeds, brightest at the centre, haze at the edges.
            struct WaveLayer
            {
                float wl, speed, phase, amp, alpha, thickness;
            };
            const WaveLayer layers[] = {
                {240.0f, 3.2f, 0.0f, 1.00f, 0.55f, 2.2f}, // main line
                {170.0f, 4.1f, 1.8f, 0.55f, 0.28f, 1.5f}, // quick shimmer
                {360.0f, 1.7f, 3.4f, 0.80f, 0.16f, 1.2f}, // slow deep swell
            };
            for (const WaveLayer &L : layers)
            {
                const float amp = wave_amp * L.amp;
                fdl->PathClear();
                for (int i = 0; i <= N; ++i)
                {
                    const float fx = (float)i / (float)N;
                    // Fade the swell toward both ends so it feels anchored.
                    const float edge_fade = 0.55f + 0.45f * std::sin(fx * 3.14159265f);
                    const float y = mid_y + wave_y(fx, L.wl, L.speed, L.phase) * amp * edge_fade;
                    fdl->PathLineTo(ImVec2(wp.x + ws.x * fx, y));
                }
                fdl->PathStroke(ui_accent_u32(L.alpha * s_hover_amt), 0, L.thickness * m.layout_scale);
            }
        }

        if (s_knob_r > 0.25f)
        {
            const ImVec2 knob_c(wp.x + ws.x * display_frac, wp.y + edge_h * 0.5f);
            // Halo fades in as the knob grows past its resting size.
            const float halo_a = std::clamp((s_knob_r / (7.0f * m.layout_scale) - 0.5f) * 0.9f,
                                            0.0f, 0.45f);
            if (halo_a > 0.004f)
                fdl->AddCircleFilled(knob_c, s_knob_r * 2.1f, ui_accent_u32(halo_a), 24);
            // Solid core, outlined so it stays crisp over the fill.
            fdl->AddCircleFilled(knob_c, s_knob_r, ui_accent_u32(), 24);
            fdl->AddCircle(knob_c, s_knob_r, ui_with_alpha(ui_col(ui_c().bg), 0.55f), 24, 1.2f);
        }
        else if (!have_dur)
            s_knob_frac = -1.0f; // re-snap position on the next track

        // Keep the loop at full rate while anything here moves: the wave
        // undulates, the springs are mid-flight, or the hover weight eases.
        if (wave_amp > 0.15f || std::fabs(s_knob_vel) > 0.002f ||
            std::fabs(s_knob_rvel) > 0.05f ||
            std::fabs((edge_hover || s_edge_seeking ? 1.0f : 0.0f) - s_hover_amt) > 0.01f)
            ui_anim_request();
    }

    static bool s_transition_init = false;
    static std::string s_last_track_id;
    static std::string s_cur_title, s_cur_artist, s_cur_thumb;
    static std::string s_old_title, s_old_artist, s_old_thumb;
    static double s_change_time = -1000.0;
    static float s_flash_timer = 0.0f;
    static float s_pulse = 0.0f;
    constexpr float kFlashDuration = 0.6f;

    const double now_time = ImGui::GetTime();
    if (!s_transition_init)
    {
        s_transition_init = true;
        s_last_track_id = state.current_track_id;
        s_cur_title = state.current_title;
        s_cur_artist = state.current_artist;
        s_cur_thumb = state.current_thumbnail_url;
    }
    else if (state.current_track_id != s_last_track_id)
    {
        // During a crossfade, hold the outgoing metadata until the midpoint,
        // then use the exact same quick hand-off as a manual track click.
        const bool wait_for_mid =
            state.crossfade_active && state.crossfade_progress < 0.5f;
        if (!wait_for_mid)
        {
            s_old_title = s_cur_title;
            s_old_artist = s_cur_artist;
            s_old_thumb = s_cur_thumb;
            s_last_track_id = state.current_track_id;
            s_cur_title = state.current_title;
            s_cur_artist = state.current_artist;
            s_cur_thumb = state.current_thumbnail_url;
            s_change_time = now_time;
            s_flash_timer = ui_animations_enabled() ? kFlashDuration : 0.0f;
            s_pulse = ui_animations_enabled() ? 1.0f : 0.0f;
        }
    }
    else
    {
        // Metadata can arrive asynchronously after the id changes; follow it.
        s_cur_title = state.current_title;
        s_cur_artist = state.current_artist;
        s_cur_thumb = state.current_thumbnail_url;
    }
    if (s_flash_timer > 0.0f)
        s_flash_timer = std::max(0.0f, s_flash_timer - io.DeltaTime);
    if (s_pulse > 0.0f)
    {
        const float pulse_dur = std::max(0.001f, ui_anim_duration(0.5f));
        s_pulse = std::max(0.0f, s_pulse - io.DeltaTime / pulse_dur);
        ui_anim_request();
    }

    float in_t = 1.0f;
    float out_t = 0.0f;
    if (ui_animations_enabled())
    {
        const float in_dur = std::max(0.001f, ui_anim_duration(0.24f));
        const float out_dur = std::max(0.001f, ui_anim_duration(0.17f));
        in_t = ui_anim_ease_out((float)((now_time - s_change_time) / in_dur));
        out_t = 1.0f - ui_anim_ease_out((float)((now_time - s_change_time) / out_dur));

        if (in_t < 1.0f || out_t > 0.0f)
            ui_anim_request();
    }

    const float side_pad = m.content_pad;
    const float art = std::min(64.0f * m.layout_scale, bar_h - m.space_4 * 2.0f);
    const float art_y = (bar_h - art) * 0.5f;

    ImGui::SetCursorPos(ImVec2(side_pad, art_y));
    const ImVec2 art_min = ImGui::GetCursorScreenPos();
    const ImVec2 art_max(art_min.x + art, art_min.y + art);

    auto draw_cover = [&](const std::string &url, float alpha, float y_offset)
    {
        if (alpha <= 0.001f)
            return;
        const ImVec2 pmin(art_min.x, art_min.y + y_offset);
        const ImVec2 pmax(art_max.x, art_max.y + y_offset);
        TextureInfo ti = url.empty() ? TextureInfo{} : texture_info(url);
        if (ti.id)
        {
            float u0, v0, u1, v1;
            cover_uv(ti, art, art, u0, v0, u1, v1);
            const float fade = ui_anim_texture_alpha(url);
            dl->AddImageRounded((ImTextureID)(intptr_t)ti.id, pmin, pmax,
                                ImVec2(u0, v0), ImVec2(u1, v1),
                                IM_COL32(255, 255, 255, (int)(255.0f * alpha * fade)), m.radius_sm);
        }
        else
        {
            dl->AddRectFilled(pmin, pmax, ui_with_alpha(ui_col(ui_c().surface_hover), alpha), m.radius_sm);
            ui_draw_icon(dl, UiIcon::Album,
                         ImVec2((pmin.x + pmax.x) * 0.5f, (pmin.y + pmax.y) * 0.5f),
                         art * 0.30f, ui_with_alpha(ui_text_muted_u32(), alpha), 1.4f);
        }
    };

    draw_cover(s_old_thumb, out_t, -(1.0f - out_t) * ui_px(4.0f));
    draw_cover(s_cur_thumb, in_t, (1.0f - in_t) * ui_px(6.0f));

    if (state.loading)
    {
        dl->AddRectFilled(art_min, art_max, IM_COL32(0, 0, 0, 155), m.radius_sm);
        const ImVec2 c((art_min.x + art_max.x) * 0.5f, (art_min.y + art_max.y) * 0.5f);
        const float radius = art * 0.16f;
        const float start_angle = (float)ImGui::GetTime() * 4.0f;
        const float arc_len = kTau * 0.72f;
        dl->PathClear();
        for (int i = 0; i <= 28; ++i)
        {
            float a = start_angle + arc_len * ((float)i / 28.0f);
            dl->PathLineTo(ImVec2(c.x + cosf(a) * radius, c.y + sinf(a) * radius));
        }
        dl->PathStroke(IM_COL32(255, 255, 255, 225), 0, 2.0f);
    }
    if (s_flash_timer > 0.0f)
        dl->AddRect(art_min, art_max, ui_accent_u32(s_flash_timer / kFlashDuration * 0.9f), m.radius_sm, 0, 2.0f);

    const float center_w = std::min(430.0f * m.layout_scale, bar_w * 0.36f);
    const float center_x = (bar_w - center_w) * 0.5f;
    const float info_x = art_max.x + m.space_3;
    const float info_w = std::max(0.0f, center_x - info_x - m.space_4);

    std::string title = state.loading ? "Loading..." : s_cur_title;
    if (title.empty())
        title = "Nothing playing";
    const float title_base = 21.0f;
    const float artist_base = 17.0f;
    const float title_line = title_base * m.text_scale * 1.25f;
    const float artist_line = artist_base * m.text_scale * 1.25f;
    const float text_block = title_line + m.space_1 + artist_line;
    const float text_y = (bar_h - text_block) * 0.5f;
    // The displayed row can be the outgoing track during the first half of a
    // crossfade. Keep the heart/link hit-boxes bound to what is actually on
    // screen instead of the already-published incoming metadata.
    Track display_track;
    if (state.crossfade_active && state.crossfade_progress < 0.5f &&
        !state.crossfade_from_track.id.empty())
    {
        display_track = state.crossfade_from_track;
    }
    else
    {
        display_track.id = s_last_track_id;
        display_track.title = s_cur_title;
        display_track.artist = s_cur_artist;
        display_track.thumbnail_url = s_cur_thumb;
        display_track.url = state.current_track_url;
        display_track.album_id = state.current_album_id;
        display_track.artist_id = state.current_artist_id;
    }
    const bool liked = !display_track.id.empty() && is_track_liked(state, display_track.id);
    const float heart_size = m.control_h;
    const bool show_heart = info_w - heart_size - m.space_2 >= 110.0f;
    const float text_limit = std::max(0.0f, info_w - (show_heart ? heart_size + m.space_2 : 0.0f));
    std::string title_draw = ui_ellipsize(g_ui_fonts.medium, title_base, title, text_limit);
    std::string artist_draw = ui_ellipsize(g_ui_fonts.body, artist_base, s_cur_artist, text_limit);
    const float title_w = ui_measure_width(g_ui_fonts.medium, title_base, title_draw);
    const float artist_w = ui_measure_width(g_ui_fonts.body, artist_base, artist_draw);
    const float text_w = std::max(title_w, artist_w);
    if (info_w > 70.0f)
    {
        const float new_dy = (1.0f - in_t) * ui_px(6.0f);
        const float old_dy = -(1.0f - out_t) * ui_px(4.0f);

        if (out_t > 0.001f && (!s_old_title.empty() || !s_old_artist.empty()))
        {
            std::string old_title = ui_ellipsize(g_ui_fonts.medium, title_base,
                                                 s_old_title.empty() ? "Unknown Title" : s_old_title,
                                                 text_limit);
            std::string old_artist = ui_ellipsize(g_ui_fonts.body, artist_base, s_old_artist, text_limit);
            dl->AddText(g_ui_fonts.medium, title_base * m.text_scale,
                        ImVec2(info_x, wp.y + text_y + old_dy),
                        ui_with_alpha(ui_text_u32(), out_t), old_title.c_str());
            dl->AddText(g_ui_fonts.body, artist_base * m.text_scale,
                        ImVec2(info_x, wp.y + text_y + title_line + m.space_1 + old_dy),
                        ui_with_alpha(ui_text_dim_u32(), out_t), old_artist.c_str());
        }

        // Clickable track title / artist. The hit-boxes are submitted before
        // the text so the hover underline can be drawn over them.
        const float title_y = wp.y + text_y + new_dy;
        const float artist_y = wp.y + text_y + title_line + m.space_1 + new_dy;
        ImGui::SetCursorScreenPos(ImVec2(info_x, title_y));
        ImGui::InvisibleButton("##bottom_album_link", ImVec2(std::max(1.0f, title_w), title_line));
        const bool title_hovered = !display_track.id.empty() && ImGui::IsItemHovered();
        const bool title_clicked = title_hovered && ImGui::IsItemClicked();
        if (title_hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Open album");
        }
        ImGui::SetCursorScreenPos(ImVec2(info_x, artist_y));
        ImGui::InvisibleButton("##bottom_artist_link", ImVec2(std::max(1.0f, artist_w), artist_line));
        const bool artist_hovered = !display_track.id.empty() &&
                                    !display_track.artist.empty() && ImGui::IsItemHovered();
        const bool artist_clicked = artist_hovered && ImGui::IsItemClicked();
        if (artist_hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Open artist");
        }

        dl->AddText(g_ui_fonts.medium, title_base * m.text_scale,
                    ImVec2(info_x, title_y),
                    ui_with_alpha(ui_text_u32(), in_t), title_draw.c_str());
        dl->AddText(g_ui_fonts.body, artist_base * m.text_scale,
                    ImVec2(info_x, artist_y),
                    ui_with_alpha(ui_text_dim_u32(), in_t), artist_draw.c_str());

        if (title_hovered)
            dl->AddLine(ImVec2(info_x, title_y + title_line - ui_px(1.0f)),
                        ImVec2(info_x + title_w, title_y + title_line - ui_px(1.0f)),
                        ui_with_alpha(ui_accent_text_u32(), in_t), ui_px(1.2f));
        if (artist_hovered)
            dl->AddLine(ImVec2(info_x, artist_y + artist_line - ui_px(1.0f)),
                        ImVec2(info_x + artist_w, artist_y + artist_line - ui_px(1.0f)),
                        ui_with_alpha(ui_accent_text_u32(), in_t), ui_px(1.2f));

        if (title_clicked || artist_clicked)
        {
            if (title_clicked)
                open_album_from_track(state, display_track);
            else
                open_artist_from_track(state, display_track);
        }

        float heart_x = info_x + text_w + m.space_2;
        heart_x = std::min(heart_x, info_x + info_w - heart_size);
        if (show_heart)
        {
            ImGui::SetCursorScreenPos(ImVec2(heart_x, wp.y + (bar_h - heart_size) * 0.5f));
            if (ui_icon_button("##bottom_like", UiIcon::Heart, heart_size, liked, liked ? ui_accent_text_u32() : 0,
                               liked ? "Unlike" : "Like"))
            {
                toggle_like(state, display_track);
            }
        }
    }

    const float transport_size = std::min(40.0f * m.layout_scale, bar_h * 0.38f);
    const float side_size = transport_size * 0.82f;
    const float gap = m.space_3;
    const float controls_w = side_size * 3.0f + transport_size + gap * 4.0f;
    const float row_h = transport_size;
    const float seek_h = 24.0f * m.layout_scale;
    const float block_h = row_h + m.space_1 + seek_h;
    const float block_y = (bar_h - block_h) * 0.5f;
    const float controls_x = center_x + (center_w - controls_w) * 0.5f;

    ImGui::SetCursorScreenPos(ImVec2(controls_x, wp.y + block_y + (row_h - side_size) * 0.5f));
    if (ui_icon_button("##shuffle", UiIcon::Shuffle, side_size, state.settings.shuffle_enabled, 0,
                       state.settings.shuffle_enabled ? "Shuffle: on" : "Shuffle: off"))
    {
        state.settings.shuffle_enabled = !state.settings.shuffle_enabled;
        save_settings(state.settings);
    }

    const float prev_x = controls_x + side_size + gap;
    ImGui::SetCursorScreenPos(ImVec2(prev_x, wp.y + block_y + (row_h - side_size) * 0.5f));
    draw_round_control(state, "##prev", UiIcon::Prev, side_size, false, false, "Previous");
    ImGui::SetCursorScreenPos(ImVec2(prev_x + side_size + gap, wp.y + block_y));
    draw_round_control(state, state.playing ? "##pause" : "##play", state.playing ? UiIcon::Pause : UiIcon::Play,
                       transport_size, true, false, state.playing ? "Pause" : "Play");
    if (s_pulse > 0.0f)
    {
        const ImVec2 pulse_c(prev_x + side_size + gap + transport_size * 0.5f,
                             wp.y + block_y + transport_size * 0.5f);
        const float r = transport_size * 0.5f + (1.0f - s_pulse) * ui_px(16.0f);
        dl->AddCircle(pulse_c, r, ui_accent_u32(s_pulse * s_pulse * 0.55f), 48, ui_px(2.0f));
        ui_anim_request();
    }
    const float next_x = prev_x + side_size + gap + transport_size + gap;
    ImGui::SetCursorScreenPos(ImVec2(next_x, wp.y + block_y + (row_h - side_size) * 0.5f));
    draw_round_control(state, "##next", UiIcon::Next, side_size, false, false, "Next");

    const UiIcon repeat_icon = state.settings.repeat_mode == 2 ? UiIcon::RepeatOne : UiIcon::Repeat;
    const char *repeat_tip = state.settings.repeat_mode == 0 ? "Repeat: off"
                            : state.settings.repeat_mode == 1 ? "Repeat: all"
                                                              : "Repeat: one";
    ImGui::SetCursorScreenPos(ImVec2(next_x + side_size + gap,
                                     wp.y + block_y + (row_h - side_size) * 0.5f));
    if (ui_icon_button("##repeat", repeat_icon, side_size, state.settings.repeat_mode != 0, 0, repeat_tip))
    {
        state.settings.repeat_mode = (state.settings.repeat_mode + 1) % 3;
        save_settings(state.settings);
    }

    // Cached observer values: a sync mpv round-trip here ran every frame.
    // -1 means "unknown" (no file / not started); the sync getter used to
    // return 0 there, so clamp to keep the slider identical.
    const bool fading = state.crossfade_active;
    const bool show_incoming = fading && state.crossfade_progress >= 0.5f;
    float current = show_incoming ? (float)state.crossfade_position
                                  : (float)player.cached_time_pos();
    float duration = show_incoming ? (float)state.crossfade_duration
                                   : (float)player.cached_duration();
    if (current < 0.0f)
        current = 0.0f;
    if (duration < 0.0f)
        duration = 0.0f;
    const bool have_duration = duration > 0.0f;
    if (current < 0.0f)
        current = 0.0f;
    const float time_w = 44.0f * m.layout_scale;
    const float seek_bar_w = std::max(40.0f, center_w - time_w * 2.0f - m.space_2 * 2.0f);
    const float seek_y = block_y + row_h + m.space_1;
    float seek_value01 = have_duration ? current / duration : 0.0f;
    static float seek_target = -1.0f;

    dl->AddText(g_ui_fonts.body, 15.0f * m.text_scale,
                ImVec2(center_x, wp.y + seek_y + (seek_h - 15.0f * m.text_scale * 1.2f) * 0.5f),
                ui_text_muted_u32(), fmt_time((int)current).c_str());
    ImGui::SetCursorScreenPos(ImVec2(center_x + time_w + m.space_2, wp.y + seek_y));
    ImGui::BeginDisabled(fading);
    const bool seek_changed =
        draw_progress_bar("##seek", &seek_value01, seek_bar_w, seek_h,
                          ui_accent_u32(1.0f), ui_overlay_u32(0.14f),
                          [&](float t) { return have_duration ? fmt_time((int)(t * duration)) : std::string(); });
    ImGui::EndDisabled();
    if (seek_changed && have_duration && !fading)
    {
        seek_target = seek_value01 * duration;
        current = seek_target;
    }
    if (fading)
    {
        // The displayed track lives on the incoming helper player; seeking it
        // here would target the outgoing player instead.
        seek_target = -1.0f;
    }
    else if (have_duration && !ImGui::IsItemActive() && seek_target >= 0.0f)
    {
        player.seek(seek_target);
        seek_target = -1.0f;
    }
    else if (!have_duration)
    {
        seek_target = -1.0f;
    }
    dl->AddText(g_ui_fonts.body, 15.0f * m.text_scale,
                ImVec2(center_x + center_w - time_w, wp.y + seek_y + (seek_h - 15.0f * m.text_scale * 1.2f) * 0.5f),
                ui_text_muted_u32(), have_duration ? fmt_time((int)duration).c_str() : "--:--");

    const bool lyrics_setting = state.settings.lyrics_panel_open;
    const bool lyrics_open = lyrics_setting && l.lyrics_open;

    // Right-hand utility cluster (sleep / queue / lyrics / volume / speed).
    // It is laid out right-to-left from the window edge: the volume slider
    // absorbs the free space, and the secondary readouts (speed first, then
    // the percentage) drop out when the window or the UI scale is too tight.
    // This keeps the icons from colliding with each other and with the
    // transport controls at every size.
    const float right_edge = bar_w - side_pad;
    const float icon_gap = m.space_2;
    float icon_size = std::min(m.control_h, bar_h * 0.48f);
    float icons_w = icon_size * 3.0f + icon_gap * 2.0f;
    const float right_min_x = center_x + center_w + m.space_3;
    const float right_avail = std::max(0.0f, right_edge - right_min_x);

    // Extremely narrow / over-scaled layouts: shrink the icons instead of
    // letting them run underneath the transport controls.
    if (right_avail < icons_w && icon_size > 0.0f)
    {
        icon_size = std::max(8.0f * m.layout_scale,
                             (right_avail - icon_gap * 2.0f) / 3.0f);
        icons_w = icon_size * 3.0f + icon_gap * 2.0f;
    }

    const float vol_icon = 20.0f * m.layout_scale;
    const float pct_w = std::max(38.0f * m.layout_scale,
                                 ui_measure_width(g_ui_fonts.body, 15.0f, "100%") + m.space_1);
    const float speed_text_w = ui_measure_width(g_ui_fonts.body, 14.0f, "0.75×");
    const float speed_w = std::max(42.0f * m.layout_scale, speed_text_w + m.space_2);
    const float min_bar = 40.0f * m.layout_scale;
    const float desired_bar = 150.0f * m.layout_scale;
    const float smallest_bar = 12.0f * m.layout_scale;

    auto utility_fixed_width = [&](bool show_pct_w, bool show_speed_w_)
    {
        float w = icons_w + icon_gap + vol_icon + icon_gap;
        if (show_pct_w)
            w += icon_gap + pct_w;
        if (show_speed_w_)
            w += icon_gap + speed_w;
        return w;
    };

    bool show_pct = true;
    bool show_speed = true;
    // Prefer a usable amount of slider travel over the secondary readouts:
    // drop the speed button first, then the percentage.
    if (right_avail < utility_fixed_width(true, true) + min_bar)
        show_speed = false;
    if (right_avail < utility_fixed_width(true, false) + min_bar)
        show_pct = false;

    const float utility_fixed = utility_fixed_width(show_pct, show_speed);
    const bool show_volume = right_avail >= utility_fixed + smallest_bar;
    const float vol_bar_w =
        show_volume ? std::clamp(right_avail - utility_fixed, smallest_bar, desired_bar) : 0.0f;

    float utility_w = icons_w;
    if (show_volume)
        utility_w += icon_gap + vol_icon + icon_gap + vol_bar_w +
                     (show_pct ? icon_gap + pct_w : 0.0f) +
                     (show_speed ? icon_gap + speed_w : 0.0f);

    const float sleep_x = std::max(right_min_x, right_edge - utility_w);
    const float queue_x = sleep_x + icon_size + icon_gap;
    const float lyrics_x = queue_x + icon_size + icon_gap;
    const float vol_x = lyrics_x + icon_size + icon_gap;
    const float vol_y = wp.y + (bar_h - seek_h) * 0.5f;

    // Sleep timer (Moon icon): click opens the arm/disarm popup. Accent-tinted
    // while armed; the tooltip shows the countdown or end-of-track mode.
    char sleep_tip[96];
    if (state.sleep_at_end_of_track)
        snprintf(sleep_tip, sizeof(sleep_tip), "Sleep timer: end of track");
    else if (state.sleep_armed)
    {
        long remain = std::chrono::duration_cast<std::chrono::seconds>(
                          state.sleep_deadline - std::chrono::steady_clock::now())
                          .count();
        if (remain < 0)
            remain = 0;
        snprintf(sleep_tip, sizeof(sleep_tip), "Sleep timer: %ld:%02ld remaining", remain / 60, remain % 60);
    }
    else
        snprintf(sleep_tip, sizeof(sleep_tip), "Sleep timer");

    ImGui::SetCursorScreenPos(ImVec2(sleep_x, wp.y + (bar_h - icon_size) * 0.5f));
    const bool sleep_active = state.sleep_armed || state.sleep_at_end_of_track;
    if (ui_icon_button("##sleep_toggle", UiIcon::Moon, icon_size, sleep_active, 0, sleep_tip))
        ImGui::OpenPopup("##sleep_popup");
    if (ImGui::BeginPopup("##sleep_popup"))
    {
        ui_section_header("Sleep timer");
        bool armed = state.sleep_armed || state.sleep_at_end_of_track;
        if (ImGui::MenuItem("Off", nullptr, !armed) && armed)
        {
            state.sleep_armed = false;
            state.sleep_at_end_of_track = false;
            state.sleep_fade_started = false;
            if (state.sleep_volume_restore >= 0)
                player.set_volume(state.sleep_volume_restore);
            state.sleep_volume_restore = -1;
        }
        if (ImGui::MenuItem("End of track", nullptr, state.sleep_at_end_of_track))
        {
            state.sleep_at_end_of_track = true;
            state.sleep_armed = false;
        }
        ImGui::Separator();
        const int minute_choices[] = {15, 30, 45, 60, 90};
        for (int minutes : minute_choices)
        {
            char label[24];
            snprintf(label, sizeof(label), "%d min", minutes);
            bool selected = state.sleep_armed &&
                            std::chrono::duration_cast<std::chrono::seconds>(
                                state.sleep_deadline - std::chrono::steady_clock::now())
                                    .count() <= minutes * 60 &&
                            std::chrono::duration_cast<std::chrono::seconds>(
                                state.sleep_deadline - std::chrono::steady_clock::now())
                                    .count() > (minutes - 15) * 60;
            if (ImGui::MenuItem(label, nullptr, selected))
            {
                state.sleep_armed = true;
                state.sleep_at_end_of_track = false;
                state.sleep_fade_started = false;
                state.sleep_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::minutes(minutes);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::SetCursorScreenPos(ImVec2(queue_x, wp.y + (bar_h - icon_size) * 0.5f));
    const bool queue_active = state.current_tab == Tab::Queue;
    if (ui_icon_button("##queue_toggle", UiIcon::Queue, icon_size, queue_active, 0, "Up next"))
    {
        if (state.current_tab != Tab::Queue)
        {
            state.pre_queue_tab = state.current_tab;
            ui_invalidate_forward(state);
            state.current_tab = Tab::Queue;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(lyrics_x, wp.y + (bar_h - icon_size) * 0.5f));
    if (ui_icon_button("##lyrics_toggle", UiIcon::Lyrics, icon_size, lyrics_open, 0, "Lyrics"))
    {
        state.settings.lyrics_panel_open = !lyrics_setting;
        save_settings(state.settings);
        if (state.settings.lyrics_panel_open)
        {
            state.lyrics_requested_id = state.current_track_id;
            state.lyrics_refresh_requested = true;
        }
    }

    const int vol = state.crossfade_active
                        ? state.crossfade_user_volume
                        : (int)player.get_property_double("volume");
    float vol_value01 = vol / 100.0f;
    const float pct_line = 15.0f * m.text_scale * 1.2f;
    const float speed_line = 14.0f * m.text_scale * 1.2f;

    if (show_volume)
    {
        const float bar_x = vol_x + vol_icon + icon_gap;
        const float pct_x = bar_x + vol_bar_w + icon_gap;
        const float speed_x = pct_x + (show_pct ? pct_w + icon_gap : 0.0f);

        ui_draw_icon(dl, UiIcon::Volume, ImVec2(vol_x + vol_icon * 0.5f, wp.y + bar_h * 0.5f),
                     vol_icon, ui_text_muted_u32(), 1.5f);
        ImGui::SetCursorScreenPos(ImVec2(bar_x, vol_y));
        if (draw_progress_bar("##volume", &vol_value01, vol_bar_w, seek_h,
                              ui_accent_u32(1.0f), ui_overlay_u32(0.14f),
                              [&](float t) { return std::to_string((int)(t * 100.0f)) + "%"; }))
        {
            // Let the main loop apply it so a fade updates both players and
            // the user's target volume, not just the outgoing one.
            state.volume_request = (int)(vol_value01 * 100.0f);
        }

        const std::string pct_label = std::to_string((int)(vol_value01 * 100.0f)) + "%";
        if (show_pct)
        {
            const float pct_text_w = ui_measure_width(g_ui_fonts.body, 15.0f, pct_label);
            dl->AddText(g_ui_fonts.body, 15.0f * m.text_scale,
                        ImVec2(pct_x + (pct_w - pct_text_w) * 0.5f,
                               wp.y + (bar_h - pct_line) * 0.5f),
                        ui_text_muted_u32(), pct_label.c_str());
        }

        // Playback speed: compact "1.0×" text button with a preset popup. Accent
        // tint while active so a non-1.0 speed is visible at a glance.
        if (show_speed)
        {
            char speed_label[16];
            snprintf(speed_label, sizeof(speed_label), "%.2g×", (double)state.settings.playback_speed);
            const float label_w = ui_measure_width(g_ui_fonts.body, 14.0f, speed_label);
            ImGui::SetCursorScreenPos(ImVec2(speed_x, vol_y));
            ImGui::PushID("##speed_btn");
            if (ImGui::InvisibleButton("##speed_hit", ImVec2(speed_w, seek_h)))
                ImGui::OpenPopup("##speed_popup");
            const bool speed_hovered = ImGui::IsItemHovered();
            dl->AddText(g_ui_fonts.body, 14.0f * m.text_scale,
                        ImVec2(speed_x + (speed_w - label_w) * 0.5f,
                               wp.y + (bar_h - speed_line) * 0.5f),
                        state.settings.playback_speed != 1.0f
                            ? ui_accent_u32(speed_hovered ? 1.0f : 0.85f)
                            : (speed_hovered ? ui_text_muted_u32() : ui_text_dim_u32()),
                        speed_label);
            if (ImGui::BeginPopup("##speed_popup"))
            {
                const float speeds[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
                for (float sp : speeds)
                {
                    char item[16];
                    snprintf(item, sizeof(item), "%.2g×", (double)sp);
                    if (ImGui::MenuItem(item, nullptr, state.settings.playback_speed == sp))
                    {
                        state.settings.playback_speed = sp;
                        save_settings(state.settings);
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }

    // Right-click anywhere over the now-playing block for the same actions
    // as a track row (like, queue, add to playlist, album/artist, radio).
    if (!display_track.id.empty() && ImGui::IsWindowHovered() &&
        ImGui::IsMouseHoveringRect(ImVec2(wp.x, wp.y),
                                   ImVec2(info_x + info_w, wp.y + bar_h)) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("TrackContextMenu");
    ui_track_context_menu(state, display_track, false);

    ImGui::End();
}
