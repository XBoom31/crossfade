#include "app_internal.h"

#include "ui.h"
#include "ui_notify.h"

#include <algorithm>
#include <cmath>

// Touchscreen: SDL's synthetic mouse events are disabled, so taps and
// one-finger drags are translated here. A tap becomes a real click across
// two frames; a long press becomes a right-click.
struct TouchGesture
{
    bool active = false;
    SDL_FingerID id = 0;
    float start_x = 0.0f, start_y = 0.0f;
    float last_x = 0.0f, last_y = 0.0f;
    Uint32 start_ticks = 0;
    Uint32 last_move_ticks = 0;
    bool scrolling = false;
    bool long_fired = false;
    float velocity = 0.0f; // pixels/second in scroll direction
};

static TouchGesture g_touch;
static float g_touch_fling_velocity = 0.0f;
static bool g_touch_fling_active = false;
static Uint32 g_touch_fling_last_ms = 0;

// Wheel/touchpad scrolling — browser model. A virtual scroll target
// integrates wheel deltas 1:1 (plus post-release kinetic fling); the rendered
// position chases the target with a critically-damped filter every frame, so
// unevenly-spaced touchpad packets become continuous sub-pixel motion and
// even tiny flicks glide. Children report edge clamping back via
// scroll_clamp_feedback() so the target stays in the reachable range.
static float g_scroll_target = 0.0f;        // virtual scroll position, px
static float g_scroll_pos = 0.0f;           // chased render position, px
static float g_wheel_velocity = 0.0f;      // active fling speed, px/s
static float g_wheel_input_velocity = 0.0f; // recent event speed, px/s
static bool g_wheel_inertia_active = false;
static Uint32 g_wheel_last_event_ms = 0;
static Uint32 g_wheel_last_frame_ms = 0;
static int g_wheel_event_count = 0;

static constexpr float kWheelPixelsPerDelta = 70.0f;
static constexpr float kWheelChaseTau = 0.05f; // input→display settle, s
static constexpr float kWheelFlingDecay = 3.0f;
static constexpr float kWheelFlingMin = 5.0f;
static constexpr float kWheelFlingMax = 5000.0f;
static constexpr Uint32 kWheelReleaseMs = 80;
static constexpr Uint32 kWheelGestureGapMs = 120;

// Horizontal two-finger swipe (back/forward navigation). See the
// SDL_MOUSEWHEEL handler for the threshold semantics.
static float g_hwheel_accum = 0.0f;
static Uint32 g_hwheel_last_ms = 0;
static constexpr float kHWheelTrigger = 2.0f; // wheel deltas per navigation step
static constexpr Uint32 kHWheelGapMs = 150;

void reset_wheel_scroll()
{
    g_scroll_target = 0.0f;
    g_scroll_pos = 0.0f;
    g_wheel_velocity = 0.0f;
    g_wheel_input_velocity = 0.0f;
    g_wheel_inertia_active = false;
    g_wheel_last_event_ms = 0;
    g_wheel_event_count = 0;
}

enum class TouchClickPhase
{
    None,
    Down,
    Up
};

static TouchClickPhase g_touch_click_phase = TouchClickPhase::None;
static int g_touch_click_button = 0;
static ImVec2 g_touch_click_pos;
// ImGui::GetTime() of the last tap we synthesized. Track rows treat a recent
// tap as the "play" gesture (the double-click idiom has no touch analogue);
// see ui_touch_tap_recent().
double g_touch_click_time = -1.0e6;

static void queue_touch_click(float x, float y, int button)
{
    if (g_touch_click_phase != TouchClickPhase::None)
        return;
    g_touch_click_time = ImGui::GetTime();
    g_touch_click_phase = TouchClickPhase::Down;
    g_touch_click_button = button;
    g_touch_click_pos = ImVec2(x, y);
}

// Feed one half of a synthetic click per frame. Sending down and up in the
// same NewFrame would leave ImGui with no pressed transition, so the down
// lands now and the up lands on the next loop iteration.
void process_touch_click()
{
    ImGuiIO &io = ImGui::GetIO();
    if (g_touch_click_phase == TouchClickPhase::Down)
    {
        io.AddMousePosEvent(g_touch_click_pos.x, g_touch_click_pos.y);
        io.AddMouseButtonEvent(g_touch_click_button, true);
        g_touch_click_phase = TouchClickPhase::Up;
    }
    else if (g_touch_click_phase == TouchClickPhase::Up)
    {
        io.AddMouseButtonEvent(g_touch_click_button, false);
        g_touch_click_phase = TouchClickPhase::None;
    }
}

void handle_touch_event(const SDL_Event &event, SDL_Window *window)
{
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return;
    auto px = [&](float nx) { return nx * (float)ww; };
    auto py = [&](float ny) { return ny * (float)wh; };

    if (event.type == SDL_FINGERDOWN)
    {
        if (g_touch.active)
            return;
        g_touch.active = true;
        g_touch.id = event.tfinger.fingerId;
        g_touch.start_x = g_touch.last_x = px(event.tfinger.x);
        g_touch.start_y = g_touch.last_y = py(event.tfinger.y);
        g_touch.start_ticks = SDL_GetTicks();
        g_touch.last_move_ticks = g_touch.start_ticks;
        g_touch.scrolling = false;
        g_touch.long_fired = false;
        g_touch.velocity = 0.0f;
        g_touch_fling_active = false;
        g_touch_fling_velocity = 0.0f;
        reset_wheel_scroll();
        ImGui::GetIO().AddMousePosEvent(g_touch.last_x, g_touch.last_y);
        return;
    }

    if (!g_touch.active || event.tfinger.fingerId != g_touch.id)
        return;

    if (event.type == SDL_FINGERMOTION)
    {
        const float x = px(event.tfinger.x);
        const float y = py(event.tfinger.y);
        const float dx = x - g_touch.start_x;
        const float dy = y - g_touch.start_y;
        if (!g_touch.scrolling && dx * dx + dy * dy > 14.0f * 14.0f)
            g_touch.scrolling = true;
        const Uint32 now_ms = SDL_GetTicks();
        const float pixel_dy = y - g_touch.last_y;
        if (g_touch.scrolling)
        {
            const float dt = (now_ms - g_touch.last_move_ticks) / 1000.0f;
            if (dt > 0.001f && dt < 0.2f)
            {
                const float instant = -pixel_dy / dt;
                // Low-pass the flick velocity so one jittery sample doesn't
                // dominate the momentum after the finger lifts.
                g_touch.velocity = g_touch.velocity * 0.65f + instant * 0.35f;
            }
            else if (dt >= 0.2f)
            {
                g_touch.velocity = -pixel_dy / std::max(dt, 0.001f);
            }
            // Direct pixel scroll: content follows the finger exactly.
            add_scroll_pixels(-pixel_dy);
        }
        g_touch.last_move_ticks = now_ms;
        g_touch.last_x = x;
        g_touch.last_y = y;
        ImGui::GetIO().AddMousePosEvent(x, y);
        return;
    }

    if (event.type == SDL_FINGERUP)
    {
        const float x = px(event.tfinger.x);
        const float y = py(event.tfinger.y);
        const bool tap = !g_touch.scrolling && !g_touch.long_fired;
        const Uint32 now_ms = SDL_GetTicks();
        float velocity = g_touch.velocity;
        // Holding still before lifting should not fling.
        if (now_ms - g_touch.last_move_ticks > 120)
            velocity = 0.0f;
        velocity = std::max(-5000.0f, std::min(velocity, 5000.0f));
        g_touch.active = false;
        if (g_touch.scrolling && !g_touch.long_fired && std::fabs(velocity) >= 80.0f)
        {
            g_touch_fling_velocity = velocity;
            g_touch_fling_active = true;
            g_touch_fling_last_ms = now_ms;
        }
        if (tap)
            queue_touch_click(x, y, ImGuiMouseButton_Left);
        return;
    }
}

void update_touch_long_press()
{
    if (!g_touch.active || g_touch.scrolling || g_touch.long_fired)
        return;
    if (SDL_GetTicks() - g_touch.start_ticks < 550)
        return;
    g_touch.long_fired = true;
    queue_touch_click(g_touch.last_x, g_touch.last_y, ImGuiMouseButton_Right);
}

// Kinetic touch scrolling: keep moving after the finger lifts, with an
// exponential decay similar to a browser's fling.
bool update_touch_inertia()
{
    if (!g_touch_fling_active)
        return false;
    if (g_touch.active)
    {
        g_touch_fling_active = false;
        g_touch_fling_velocity = 0.0f;
        return false;
    }
    const Uint32 now_ms = SDL_GetTicks();
    float dt = (now_ms - g_touch_fling_last_ms) / 1000.0f;
    g_touch_fling_last_ms = now_ms;
    if (dt <= 0.0f)
        return true;
    if (dt > 0.05f)
        dt = 0.05f;
    add_scroll_pixels(g_touch_fling_velocity * dt);
    g_touch_fling_velocity *= std::exp(-3.2f * dt);
    if (std::fabs(g_touch_fling_velocity) < 20.0f)
    {
        g_touch_fling_velocity = 0.0f;
        g_touch_fling_active = false;
        return false;
    }
    return true;
}

// Wheel/touchpad scrolling. Input is drained with a short time constant while
// events are arriving; kinetic inertia is only started after the pad goes
// quiet, using the speed measured from the events themselves.
bool update_wheel_inertia()
{
    const Uint32 now_ms = SDL_GetTicks();
    const bool input_active = (now_ms - g_wheel_last_event_ms) <= kWheelReleaseMs;
    if (!input_active && !g_wheel_inertia_active &&
        std::fabs(g_scroll_target - g_scroll_pos) < 0.05f)
        return false;

    float dt = (now_ms - g_wheel_last_frame_ms) / 1000.0f;
    g_wheel_last_frame_ms = now_ms;
    if (dt <= 0.0f)
        return true;
    if (dt > 0.05f)
        dt = 0.05f;

    // Edge feedback from the last frame: the hovered child clamped the
    // scroll, so pull the target back into the reachable range and stop any
    // fling pushing into the edge.
    const float unapplied = take_scroll_clamp_feedback();
    if (unapplied != 0.0f)
    {
        g_scroll_target += unapplied;
        if (g_wheel_inertia_active)
        {
            g_wheel_velocity = 0.0f;
            g_wheel_inertia_active = false;
        }
    }

    if (input_active)
    {
        g_wheel_inertia_active = false;
        g_wheel_velocity = 0.0f;
    }
    else if (!g_wheel_inertia_active)
    {
        const float fling = std::max(-kWheelFlingMax,
                                     std::min(g_wheel_input_velocity, kWheelFlingMax));
        if (std::fabs(fling) >= kWheelFlingMin)
        {
            g_wheel_velocity = fling;
            g_wheel_inertia_active = true;
        }
        g_wheel_input_velocity = 0.0f;
        g_wheel_event_count = 0;
    }

    if (g_wheel_inertia_active)
    {
        g_scroll_target += g_wheel_velocity * dt;
        g_wheel_velocity *= std::exp(-kWheelFlingDecay * dt);
        if (std::fabs(g_wheel_velocity) < kWheelFlingMin)
        {
            g_wheel_velocity = 0.0f;
            g_wheel_inertia_active = false;
        }
    }

    // Critically-damped chase: the rendered position follows the target,
    // turning discrete wheel packets into continuous sub-pixel motion.
    const float chase = 1.0f - std::exp(-dt / kWheelChaseTau);
    const float new_pos = g_scroll_pos + (g_scroll_target - g_scroll_pos) * chase;
    add_scroll_pixels(new_pos - g_scroll_pos);
    g_scroll_pos = new_pos;

    if (!input_active && !g_wheel_inertia_active &&
        std::fabs(g_scroll_target - g_scroll_pos) < 0.05f)
    {
        reset_wheel_scroll();
        return false;
    }
    return true;
}


static void stop_touch_fling()
{
    g_touch_fling_active = false;
    g_touch_fling_velocity = 0.0f;
}

void reset_pointer_gestures()
{
    reset_wheel_scroll();
    g_hwheel_accum = 0.0f;
    stop_touch_fling();
}

void handle_wheel_event(const SDL_Event &event)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    const float delta = event.wheel.preciseY;
#else
    const float delta = (float)event.wheel.y;
#endif
    if (delta != 0.0f)
    {
        // Wheel-up is negative scroll. Deltas integrate into the virtual
        // target 1:1; the chase filter in update_wheel_inertia() renders
        // them continuously.
        g_touch_fling_active = false;
        g_touch_fling_velocity = 0.0f;

        const Uint32 now_ms = SDL_GetTicks();
        const Uint32 gap_ms = now_ms - g_wheel_last_event_ms;
        const float pixels = -delta * kWheelPixelsPerDelta;

        if (gap_ms <= kWheelGestureGapMs)
        {
            if (gap_ms > 0)
            {
                const float instant = pixels / std::max(gap_ms / 1000.0f, 0.001f);
                g_wheel_input_velocity =
                    g_wheel_input_velocity * 0.65f + instant * 0.35f;
            }
            ++g_wheel_event_count;
        }
        else
        {
            g_wheel_input_velocity = 0.0f;
            g_wheel_last_frame_ms = now_ms;
            g_wheel_event_count = 1;
        }

        g_scroll_target += pixels;
        g_wheel_last_event_ms = now_ms;
        g_wheel_inertia_active = false;
        g_wheel_velocity = 0.0f;
    }

    // Horizontal touchpad swipe: two-finger navigation, like a browser.
    // Deltas accumulate during one swipe (events arriving closer than
    // kHWheelGapMs); crossing the threshold fires once and re-arms, so a long
    // swipe can navigate several steps. A rightward finger swipe sends
    // wheel.x > 0 (Forward); wheel.x < 0 is Back.
#if SDL_VERSION_ATLEAST(2, 0, 18)
    const float xdelta = event.wheel.preciseX;
#else
    const float xdelta = (float)event.wheel.x;
#endif
    if (xdelta == 0.0f)
        return;

    // A toast under the cursor owns the horizontal gesture: it tracks the
    // fingers (swipe-to-dismiss) instead of triggering navigation.
    if (ui_notify_swipe_feed(xdelta * kWheelPixelsPerDelta))
    {
        g_state.repaint_requested = true;
        return;
    }

    const Uint32 now_ms = SDL_GetTicks();
    if (now_ms - g_hwheel_last_ms > kHWheelGapMs ||
        (g_hwheel_accum != 0.0f && g_hwheel_accum * xdelta < 0.0f))
    {
        // New swipe: enough time passed since the last event, or the
        // direction flipped.
        g_hwheel_accum = 0.0f;
    }
    g_hwheel_accum += xdelta;
    g_hwheel_last_ms = now_ms;
    while (std::abs(g_hwheel_accum) >= kHWheelTrigger)
    {
        const bool back = g_hwheel_accum < 0.0f;
        g_hwheel_accum -= back ? -kHWheelTrigger : kHWheelTrigger;
        if (back)
            ui_go_back(g_state);
        else
            ui_go_forward(g_state);
        g_state.repaint_requested = true;
    }
}
