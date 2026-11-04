#include "ui_notify.h"
#include "ui_icons.h"
#include "ui_components.h"
#include "ui_theme.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <vector>

struct Toast
{
    uint32_t id = 0;
    UiNotifyType type = UiNotifyType::Info;
    std::string title;
    std::string message;
    std::string action;      // optional button label
    void (*action_fn)() = nullptr; // optional callback for the action button
    double spawn = 0.0;      // notify_now() when shown
    float duration = 6.0f;   // <= 0: sticky until closed
    bool closing = false;
    double close_start = 0.0;
    // Two-finger swipe-to-dismiss: the toast tracks horizontal touchpad
    // deltas while the cursor rests on it; past the threshold an idle swipe
    // flings it off-screen, otherwise it springs back to 0.
    float swipe_x = 0.0f;
    double last_swipe = -1e9; // notify_now() of the last swipe delta
    bool flung = false;       // dismissal triggered by a swipe (fling out)
    float fling_dir = 1.0f;   // direction the fling travels (drag sign)
    // wrap_lines() is a greedy text measurement; the message never changes
    // and the width only changes on resize, so memoize per (width).
    int cached_lines = -1;
    float cached_wrap_w = -1.0f;
};

static std::mutex g_notify_mutex;
static std::vector<Toast> g_toasts;
static uint32_t g_next_toast_id = 1;
static void (*g_open_settings)() = nullptr;

static const int kMaxToasts = 4;
static const float kSlideIn = 0.28f;
static const float kFadeOut = 0.22f;

// Swipe-to-dismiss tuning: how far the toast must be dragged and how long
// the fingers must rest before the idle swipe resolves (fling or spring).
static const float kSwipeDismissPx = 130.0f;
static const double kSwipeIdleSec = 0.22;

// Last drawn geometry of each open toast (for hit-testing swipe input that
// arrives between frames, before NewFrame recomputes the layout).
struct ToastRect
{
    uint32_t id;
    ImVec2 p0, p1;
};
static std::vector<ToastRect> g_toast_rects;

// Wall clock, NOT notify_now(): the ImGui clock only advances when a
// frame renders, but the frame loop now skips renders while idle -- expiry
// and dedupe must not freeze with it. (It is also read from worker threads.)
static double notify_now()
{
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static ImVec4 type_color(UiNotifyType t)
{
    switch (t)
    {
    case UiNotifyType::Success: return ImVec4(0.36f, 0.85f, 0.52f, 1.0f);
    case UiNotifyType::Warning: return ImVec4(0.98f, 0.72f, 0.28f, 1.0f);
    case UiNotifyType::Error:   return ImVec4(0.95f, 0.38f, 0.42f, 1.0f);
    default:                    return ImVec4(0.45f, 0.66f, 1.0f, 1.0f);
    }
}

void ui_notify_set_open_settings(void (*fn)())
{
    g_open_settings = fn;
}

void ui_notify(UiNotifyType type, const std::string &title, const std::string &message,
               float duration, const char *action_label, void (*action_fn)())
{
    std::lock_guard<std::mutex> lock(g_notify_mutex);
    Toast t;
    t.id = g_next_toast_id++;
    t.type = type;
    t.title = title;
    t.message = message;
    t.action = action_label ? action_label : "";
    t.action_fn = action_fn;
    t.spawn = notify_now();
    t.duration = duration;
    g_toasts.push_back(std::move(t));
    // Drop the oldest if the stack overflows.
    while ((int)g_toasts.size() > kMaxToasts)
        g_toasts.erase(g_toasts.begin());
}

void ui_notify_backend_error(const BackendError &err)
{
    static std::string s_last_msg;
    static double s_last_time = -1e9;
    const double now = notify_now();

    // Every failed backend command re-reports the same root error; only
    // surface it again if it went away or changed.
    if (err.message == s_last_msg && now - s_last_time < 10.0)
        return;
    s_last_msg = err.message;
    s_last_time = now;

    // Distill multi-line yt-dlp/ytmusicapi dumps to one toast-sized sentence.
    std::string detail = err.message;
    size_t nl = detail.find('\n');
    if (nl != std::string::npos)
        detail = detail.substr(0, nl);
    if (detail.size() > 160)
        detail = detail.substr(0, 157) + "...";

    switch (err.kind)
    {
    case BackendErrKind::Auth:
        ui_notify(UiNotifyType::Error, "YouTube Music sign-in problem", detail, 0.0f,
                  "Open settings");
        break;
    case BackendErrKind::Network:
        ui_notify(UiNotifyType::Warning, "No connection",
                  "Couldn't reach YouTube Music. Check your internet connection.",
                  8.0f);
        break;
    case BackendErrKind::Backend:
        ui_notify(UiNotifyType::Error, "Backend not running", detail, 0.0f,
                  "Open settings");
        break;
    default:
        ui_notify(UiNotifyType::Warning, "Backend error", detail, 8.0f);
        break;
    }
}

bool ui_notify_swipe_feed(float dx_pixels)
{
    std::lock_guard<std::mutex> lock(g_notify_mutex);
    if (g_toasts.empty() || g_toast_rects.empty())
        return false;
    // Wheel events arrive outside the frame; io.MousePos still holds the
    // position from the last drawn frame, which is what the rects match.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const double now = notify_now();
    bool over_toast = false;
    for (auto &t : g_toasts)
    {
        if (t.closing)
            continue;
        for (const auto &r : g_toast_rects)
        {
            if (r.id != t.id)
                continue;
            if (mouse.x >= r.p0.x && mouse.x <= r.p1.x &&
                mouse.y >= r.p0.y && mouse.y <= r.p1.y)
            {
                t.swipe_x += dx_pixels;
                t.last_swipe = now;
                // Fling the moment the drag crosses the dismiss threshold —
                // mid-gesture, so the motion is continuous (drag flows into
                // the fling without a dead stop at the fingers' end). The
                // fling starts from the current offset, picking up exactly
                // where the fingers left off.
                if (std::fabs(t.swipe_x) >= kSwipeDismissPx)
                {
                    t.flung = true;
                    t.fling_dir = t.swipe_x > 0.0f ? 1.0f : -1.0f;
                    t.closing = true;
                    t.close_start = now;
                }
                over_toast = true;
            }
            break;
        }
    }
    return over_toast;
}

bool ui_notify_needs_frame()
{
    std::lock_guard<std::mutex> lock(g_notify_mutex);
    if (g_toasts.empty())
        return false;
    const double now = notify_now();
    for (const auto &t : g_toasts)
    {
        if (t.closing)
        {
            if (now - t.close_start < kFadeOut)
                return true; // fade-out still running
        }
        else
        {
            if (now - t.spawn < (double)kSlideIn)
                return true; // slide-in still running
            if (t.duration > 0.0f && now - t.spawn >= t.duration)
                return true; // expired: next frame starts the fade-out
        }
    }
    return false;
}

void ui_notify_dismiss_all()
{
    std::lock_guard<std::mutex> lock(g_notify_mutex);
    const double now = notify_now();
    for (auto &t : g_toasts)
        if (!t.closing)
        {
            t.closing = true;
            t.close_start = now;
        }
}

// Greedy word-wrap line count, matching how TextWrapped will lay the message
// out -- needed because the window is positioned bottom-up and its height
// must be known before Begin().
static int wrap_lines(ImFont *font, float size, const std::string &text, float max_w)
{
    int lines = 1;
    float x = 0.0f;
    const float space_w = ui_measure_width(font, size, " ");
    std::string word;
    auto flush_word = [&](bool final_word)
    {
        if (word.empty())
            return;
        const float w = ui_measure_width(font, size, word);
        if (x > 0.0f && x + w > max_w)
        {
            lines++;
            x = w;
        }
        else
            x += w + (final_word ? 0.0f : space_w);
        word.clear();
    };
    for (char c : text)
    {
        if (c == ' ' || c == '\n')
        {
            flush_word(false);
            if (c == '\n' && !word.empty())
                lines++;
        }
        else
            word += c;
    }
    flush_word(true);
    return lines;
}

static float ease_out_cubic(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

static float ease_in_cubic(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * t;
}

void ui_notify_draw()
{
    // Action callbacks can call ui_notify() themselves (the undo buttons do).
    // Defer them until the toast list is no longer being iterated and the
    // mutex is released, otherwise the callback can reallocate g_toasts while
    // a Toast reference is live, or deadlock trying to lock the mutex again.
    void (*pending_action)() = nullptr;
    bool pending_open_settings = false;

    std::unique_lock<std::mutex> lock(g_notify_mutex);
    if (g_toasts.empty())
        return;

    const UiMetrics &m = ui_m();
    const ImGuiIO &io = ImGui::GetIO();
    const double now = notify_now();
    const float scale = m.layout_scale;
    const float width = std::min(400.0f * scale, io.DisplaySize.x * 0.86f);
    const float margin_x = m.space_4;
    const float margin_bottom = m.space_4;
    const float pad_x = m.space_4;
    const float pad_y = m.space_3;
    const float gap = m.space_3;

    const float body_size = ui_font_size(m.font_body);
    const float label_size = ui_font_size(m.font_label);
    const float title_h = label_size * 1.35f;
    const float icon_r = label_size * 0.62f;
    const float msg_max_w = width - pad_x * 2.0f;

    // Lay the stack out bottom-up above the bottom bar.
    float y_bottom = io.DisplaySize.y - m.bottom_h - margin_bottom;

    // Rebuilt below every frame; erased toasts drop out automatically.
    g_toast_rects.clear();

    // Swipe resolution: when the fingers rest, a dragged toast either flings
    // out (past the dismiss threshold) or springs back. Uses the time since
    // the previous draw for a frame-rate-independent spring.
    static double s_last_draw = -1e9;
    const float dt = (float)std::clamp(now - s_last_draw, 0.001, 0.1);
    s_last_draw = now;
    for (auto &t : g_toasts)
    {
        if (t.closing || t.swipe_x == 0.0f)
            continue;
        if (now - t.last_swipe < kSwipeIdleSec)
            continue;
        if (std::fabs(t.swipe_x) >= kSwipeDismissPx)
        {
            t.flung = true;
            t.fling_dir = t.swipe_x > 0.0f ? 1.0f : -1.0f;
            t.closing = true;
            t.close_start = now;
        }
        else
        {
            // Exponential spring back toward rest; snap when nearly done.
            t.swipe_x *= std::exp(-dt * 14.0f);
            if (std::fabs(t.swipe_x) < 0.5f)
                t.swipe_x = 0.0f;
        }
    }

    // Oldest at the bottom of the stack.
    for (size_t i = 0; i < g_toasts.size(); ++i)
    {
        Toast &t = g_toasts[g_toasts.size() - 1 - i]; // newest drawn closest to the bottom
        float age = (float)(now - t.spawn);
        if (t.closing)
            age = (float)(now - t.close_start) + 1000.0f; // expired immediately

        bool expired = t.duration > 0.0f && !t.closing && age >= t.duration;
        if (expired)
        {
            t.closing = true;
            t.close_start = now;
        }

        float slide_t = 1.0f, alpha = 1.0f;
        float swipe_offset = t.swipe_x;
        if (t.closing)
        {
            const float ct = (float)(now - t.close_start) / kFadeOut;
            if (ct >= 1.0f)
            {
                // Erase once the fade-out finished.
                g_toasts.erase(g_toasts.begin() + (g_toasts.size() - 1 - i));
                i--;
                continue;
            }
            slide_t = 1.0f - ct;
            alpha = 1.0f - ct;
            if (t.flung)
            {
                // Fling out in the drag direction, accelerating from where
                // the fingers left off until fully off-screen.
                swipe_offset = t.swipe_x +
                               t.fling_dir * ease_in_cubic(ct) *
                                   (io.DisplaySize.x * 0.6f + 80.0f * scale);
            }
        }
        else
        {
            slide_t = ease_out_cubic(age / kSlideIn);
            alpha = std::clamp(age / 0.18f, 0.0f, 1.0f);
        }

        if (t.cached_lines < 0 || t.cached_wrap_w != msg_max_w)
        {
            t.cached_lines = wrap_lines(g_ui_fonts.body, m.font_body, t.message, msg_max_w);
            t.cached_wrap_w = msg_max_w;
        }
        const int msg_lines = t.cached_lines;
        const float msg_h = msg_lines * body_size * 1.38f;
        const float action_h = t.action.empty() ? 0.0f : m.control_h + m.space_2;
        const float height = pad_y * 2.0f + title_h + m.space_1 + msg_h + action_h;

        const ImVec2 pos(io.DisplaySize.x - width - margin_x + (1.0f - slide_t) * 36.0f * scale + swipe_offset,
                         y_bottom - height);
        y_bottom -= height + gap;

        ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, m.radius_lg);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad_x, pad_y));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
        char win_id[32];
        snprintf(win_id, sizeof(win_id), "##toast%u", t.id);
        ImGui::Begin(win_id, nullptr, flags);
        // Toasts must float above the app windows. Without this they are
        // buried: NoBringToFrontOnFocus keeps them at their creation z-order,
        // and any focused app window then renders on top of them forever.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1 = ImVec2(p0.x + width, p0.y + height);

        // Publish geometry for between-frame swipe hit-testing.
        for (auto &r : g_toast_rects)
            if (r.id == t.id) { r.p0 = p0; r.p1 = p1; goto rect_done; }
        g_toast_rects.push_back({t.id, p0, p1});
        rect_done:;

        // Panel + accent strip.
        dl->AddRectFilled(p0, p1, ui_col(ui_c().surface), m.radius_lg);
        dl->AddRect(p0, p1, ui_border_u32(), m.radius_lg);
        const ImVec4 tc = type_color(t.type);
        dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f * scale, p1.y), ui_col(tc), m.radius_lg);

        // Icon in a tinted circle.
        const ImVec2 icon_center(p0.x + pad_x + icon_r, p0.y + pad_y + title_h * 0.5f);
        dl->AddCircleFilled(icon_center, icon_r + 4.0f * scale, ui_col(ImVec4(tc.x, tc.y, tc.z, 0.16f)));
        if (t.type == UiNotifyType::Error || t.type == UiNotifyType::Warning)
        {
            // "!" glyph: stem + dot (no matching UiIcon).
            const float stem_top = icon_center.y - icon_r * 0.62f;
            const float stem_bot = icon_center.y + icon_r * 0.10f;
            dl->AddLine(ImVec2(icon_center.x, stem_top), ImVec2(icon_center.x, stem_bot),
                        ui_col(tc), 2.4f * scale);
            dl->AddCircleFilled(ImVec2(icon_center.x, icon_center.y + icon_r * 0.48f),
                                1.4f * scale, ui_col(tc));
        }
        else if (t.type == UiNotifyType::Success)
            ui_draw_icon(dl, UiIcon::Check, icon_center, label_size, ui_col(tc));
        else
            ui_draw_icon(dl, UiIcon::Refresh, icon_center, label_size, ui_col(tc));

        // Title + close button.
        const float close_sz = m.control_h * 0.72f;
        const float title_x = p0.x + pad_x + (icon_r + 4.0f * scale) * 2.0f + m.space_2;
        ImGui::PushClipRect(ImVec2(p0.x, p0.y), ImVec2(p1.x - close_sz - m.space_1, p1.y), true);
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text);
        ui_push_semibold(m.font_label);
        ImGui::SetCursorScreenPos(ImVec2(title_x, p0.y + pad_y + (title_h - label_size) * 0.45f));
        ImGui::TextUnformatted(t.title.c_str());
        ImGui::PopFont();
        ImGui::PopStyleColor();
        ImGui::PopClipRect();

        const ImVec2 close_center(p1.x - pad_x - close_sz * 0.5f, p0.y + pad_y + title_h * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(close_center.x - close_sz * 0.5f,
                                         close_center.y - close_sz * 0.5f));
        ImGui::PushID((int)t.id);
        if (ui_icon_button("close", UiIcon::Close, close_sz, false, ui_text_muted_u32()))
        {
            t.closing = true;
            t.close_start = now;
        }

        // Message.
        ImGui::SetCursorScreenPos(ImVec2(p0.x + pad_x, p0.y + pad_y + title_h + m.space_1));
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_dim);
        ui_push_body();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + msg_max_w);
        ImGui::TextUnformatted(t.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::PopStyleColor();

        // Action.
        if (!t.action.empty())
        {
            ImGui::SetCursorScreenPos(ImVec2(p0.x + pad_x, p1.y - pad_y - m.control_h));
            if (ui_ghost_button(t.action.c_str(), ImVec2(0, m.control_h)))
            {
                if (t.action_fn)
                    pending_action = t.action_fn;
                else if (g_open_settings)
                    pending_open_settings = true;
                t.closing = true;
                t.close_start = now;
            }
        }
        ImGui::PopID();
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    lock.unlock();
    if (pending_action)
        pending_action();
    else if (pending_open_settings && g_open_settings)
        g_open_settings();
}

bool ui_notify_animating()
{
    std::lock_guard<std::mutex> lock(g_notify_mutex);
    if (g_toasts.empty())
        return false;
    const double now = notify_now();
    for (const auto &t : g_toasts)
    {
        if (t.closing)
        {
            if (now - t.close_start < kFadeOut)
                return true;
        }
        else
        {
            // A toast mid-swipe (or springing back) is actively moving.
            if (t.swipe_x != 0.0f)
                return true;
            // Only the slide-in actually moves; a resting toast (including
            // sticky ones) must not pin the frame rate. Expiry is driven by
            // the periodic ticks / activity in the main loop, so a duration
            // toast at rest reports false here and gets picked up again when
            // some other frame runs.
            if (now - t.spawn < (double)kSlideIn)
                return true;
        }
    }
    return false;
}
