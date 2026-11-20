#include "ui.h"
#include "ui_audio_reactive.h"
#include "ui_backdrop.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include "mpvplayer.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// ----------------------------------------------------------------------
// Full-window lyrics takeover
//
// Entered from the bottom-bar expand button. The docked panel stays the
// everyday view; this is the performance view: one huge active line held at a
// fixed focal point, one or two faint upcoming lines below it, and a small
// pulse on every line change.
//
// There is deliberately no live audio analysis here. The "reactive" pulse is
// timestamp-driven: the active line is resolved by the shared helpers in
// ui_lyrics.cpp, and every index change fires a decaying glow/scale envelope.
// That keeps it honest and cheap, and it works for any track the lyrics
// provider already timestamps.
//
// Motion is entirely optional. With animations off the view becomes a static,
// centred reading layout: no entrance, no pulse, no auto-scroll.
//
// The whole view is a modal popup, following draw_party_fullscreen(), so it
// covers and blocks every window underneath without changing the OS window
// state. Esc is handled in the main loop (app.cpp) like the other takeovers.
// ----------------------------------------------------------------------

namespace
{
const float kChromeIdle = 2.5f;    // seconds before the chrome hides
const float kPulseDecay = 3.6f;    // higher = snappier pulse
const float kLineEnter = 0.42f;    // active-line entrance duration
const float kViewEnter = 0.45f;    // whole-view fade-in

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float ease_out(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t);
}

bool blank(const std::string &s)
{
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            return false;
    return true;
}

std::string trim_line(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

float measure(ImFont *font, float size, const std::string &text)
{
    if (!font || text.empty())
        return 0.0f;
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str(),
                               text.c_str() + text.size()).x;
}

struct TextRow
{
    std::string text;
    float width = 0.0f;
};

struct TextLayout
{
    std::vector<TextRow> rows;
    float size = 0.0f;
    float line_h = 0.0f;
    float height = 0.0f;
};

// Advance one UTF-8 code point (never split a continuation byte when forcing
// progress after a wrap failure).
const char *next_cp(const char *p, const char *end)
{
    if (p >= end)
        return end;
    ++p;
    while (p < end && (((unsigned char)*p) & 0xC0) == 0x80)
        ++p;
    return p;
}

std::vector<std::string> paragraphs(const std::string &text)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i)
    {
        if (i == text.size() || text[i] == '\n')
        {
            out.push_back(trim_line(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (out.empty())
        out.push_back(std::string());
    return out;
}

std::vector<TextRow> wrap_paragraph(ImFont *font, float size, const std::string &para,
                                    float max_w)
{
    std::vector<TextRow> rows;
    const char *begin = para.c_str();
    const char *end = begin + para.size();
    const char *p = begin;
    if (max_w < 1.0f)
        max_w = 1.0f;

    while (p < end)
    {
        const char *e = font->CalcWordWrapPosition(size, p, end, max_w);
        if (e <= p)
            e = next_cp(p, end);
        const char *row_end = e;
        while (row_end > p && (row_end[-1] == ' ' || row_end[-1] == '\t'))
            --row_end;
        if (row_end > p)
            rows.push_back({std::string(p, row_end), 0.0f});
        p = e;
        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;
    }

    if (rows.empty())
        rows.push_back({std::string(), 0.0f});

    for (TextRow &r : rows)
        r.width = measure(font, size, r.text);
    return rows;
}

// Shrink a line from `preferred` down to `floor` until its widest paragraph
// fits, then wrap whatever is left. This is the "shrink, then wrap" rule for
// long lyric lines.
TextLayout layout_text(ImFont *font, const std::string &text, float max_w,
                       float preferred, float floor)
{
    TextLayout out;
    if (!font)
        return out;
    if (floor > preferred)
        floor = preferred;

    std::vector<std::string> paras = paragraphs(text);
    float best = 0.0f;
    for (const std::string &p : paras)
        best = std::max(best, measure(font, preferred, p));

    float size = preferred;
    if (best > max_w)
    {
        const float step = std::max(1.0f, preferred * 0.035f);
        while (size > floor)
        {
            best = 0.0f;
            for (const std::string &p : paras)
                best = std::max(best, measure(font, size, p));
            if (best <= max_w)
                break;
            size = std::max(floor, size - step);
            if (size == floor)
            {
                best = 0.0f;
                for (const std::string &p : paras)
                    best = std::max(best, measure(font, size, p));
                break;
            }
        }
    }

    out.size = size;
    out.line_h = size * 1.26f;
    for (const std::string &p : paras)
    {
        std::vector<TextRow> rows = wrap_paragraph(font, size, p, max_w);
        out.rows.insert(out.rows.end(), rows.begin(), rows.end());
    }
    if (out.rows.empty())
        out.rows.push_back({std::string(), 0.0f});
    out.height = out.rows.size() * out.line_h;
    return out;
}

struct ImmersiveAnim
{
    std::string track;
    int active = -2;
    double active_since = 0.0;
    double view_start = 0.0;
    float enter = 0.0f;
    float pulse = 0.0f;
    std::string prev_text;
    float prev_size = 0.0f;
    float last_size = 0.0f;
    double prev_since = -100.0;
    double last_input = 0.0;
    ImVec2 last_mouse = ImVec2(-1.0f, -1.0f);
    float chrome = 1.0f;
    bool chrome_init = false;
};

// Soft radial bloom used to carry the app's album-colour backdrop into the
// takeover. Drawn as a vertex-coloured triangle fan so the falloff is
// interpolated per-pixel by the GPU rather than stacked as visible
// concentric rings: a shader-style gradient with a smooth (1 - t)^2 profile.
void soft_glow(ImDrawList *dl, ImVec2 centre, float radius, ImVec4 col, float peak)
{
    if (radius <= 0.5f || peak <= 0.0005f)
        return;
    const int rings = 8;
    const int segs = 96;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();

    const int vtx_count = 1 + (rings + 1) * segs;
    const int idx_count = segs * 3 + rings * segs * 6;
    const unsigned int base = dl->VtxBuffer.Size;
    dl->PrimReserve(idx_count, vtx_count);

    dl->PrimWriteVtx(centre, uv,
                     ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, peak)));
    for (int r = 0; r <= rings; ++r)
    {
        const float t = (float)r / (float)rings;
        const float rr = radius * t;
        // Quadratic falloff keeps the core bright and the rim invisible.
        const float a = peak * (1.0f - t) * (1.0f - t);
        const ImU32 c = ImGui::GetColorU32(ImVec4(col.x, col.y, col.z, a));
        for (int s = 0; s < segs; ++s)
        {
            const float ang = (float)s / (float)segs * 6.28318530718f;
            dl->PrimWriteVtx(ImVec2(centre.x + std::cos(ang) * rr,
                                    centre.y + std::sin(ang) * rr),
                             uv, c);
        }
    }

    // Centre fan.
    const unsigned int ring0 = base + 1;
    for (int s = 0; s < segs; ++s)
    {
        dl->PrimWriteIdx((ImDrawIdx)base);
        dl->PrimWriteIdx((ImDrawIdx)(ring0 + s));
        dl->PrimWriteIdx((ImDrawIdx)(ring0 + (s + 1) % segs));
    }
    // Ring-to-ring quads.
    for (int r = 0; r < rings; ++r)
    {
        const unsigned int a0 = base + 1 + r * segs;
        const unsigned int a1 = a0 + segs;
        for (int s = 0; s < segs; ++s)
        {
            const int n = (s + 1) % segs;
            dl->PrimWriteIdx((ImDrawIdx)(a0 + s));
            dl->PrimWriteIdx((ImDrawIdx)(a1 + s));
            dl->PrimWriteIdx((ImDrawIdx)(a1 + n));
            dl->PrimWriteIdx((ImDrawIdx)(a0 + s));
            dl->PrimWriteIdx((ImDrawIdx)(a1 + n));
            dl->PrimWriteIdx((ImDrawIdx)(a0 + n));
        }
    }
}
ImVec4 mix4(const ImVec4 &a, const ImVec4 &b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

// Four anchor points the ambient blooms wander between. Normalised so the
// layout survives window resizes.
const ImVec2 kBgCorners[4] = {
    ImVec2(0.17f, 0.24f), ImVec2(0.83f, 0.21f),
    ImVec2(0.80f, 0.79f), ImVec2(0.20f, 0.77f)};

unsigned bg_rand()
{
    static unsigned s = 0x2545F491u;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

struct ImmersiveBg
{
    bool init = false;
    ImVec2 pos[3];
    ImVec2 target[3];
    int corner[3] = {0, 2, 1};
    double next_move[3] = {0.0, 0.0, 0.0};
};
ImmersiveBg g_bg;

ImmersiveAnim g_imm;
bool g_hint_done = false;
double g_hint_start = 0.0;

std::string fmt_time(int secs)
{
    if (secs < 0)
        secs = 0;
    const int m = secs / 60;
    const int s = secs % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return std::string(buf);
}

int count_cp(const std::string &s)
{
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80)
            ++n;
    return n;
}

int count_cp_layout(const TextLayout &lay)
{
    int n = 0;
    for (const TextRow &r : lay.rows)
        n += count_cp(r.text);
    return n;
}

ImU32 lerp_col(ImU32 a, ImU32 b, float t)
{
    t = clampf(t, 0.0f, 1.0f);
    const ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a);
    const ImVec4 cb = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::GetColorU32(ImVec4(ca.x + (cb.x - ca.x) * t,
                                     ca.y + (cb.y - ca.y) * t,
                                     ca.z + (cb.z - ca.z) * t,
                                     ca.w + (cb.w - ca.w) * t));
}

void draw_rows(ImDrawList *dl, ImFont *font, const TextLayout &lay, ImVec2 center,
               ImU32 col, float alpha, float scale, float glow,
               float karaoke = -1.0f)
{
    if (alpha <= 0.001f)
        return;
    const float size = lay.size * scale;
    const float line_h = lay.line_h * scale;
    const float total_h = (float)lay.rows.size() * line_h;
    float y = center.y - total_h * 0.5f;
    const ImU32 glow_col = ui_accent_text_u32();
    // Un-sung text stays readable but recedes; the accent "fills" it in as the
    // line plays, like a karaoke wipe.
    const ImU32 unsung = ui_with_alpha(ui_text_dim_u32(), alpha * 0.80f);
    const int total = karaoke >= 0.0f ? std::max(1, count_cp_layout(lay)) : 0;
    const float reveal = clampf(karaoke, 0.0f, 1.0f) * (float)total;
    int seen = 0;

    for (const TextRow &row : lay.rows)
    {
        const float w = measure(font, size, row.text);
        const float x = center.x - w * 0.5f;
        if (karaoke >= 0.0f && !row.text.empty())
        {
            const char *p = row.text.c_str();
            const char *end = p + row.text.size();
            float tx = x;
            while (p < end)
            {
                const bool space = (*p == ' ' || *p == '\t');
                const char *tok = p;
                int chars = 0;
                while (p < end)
                {
                    const bool sp = (*p == ' ' || *p == '\t');
                    if (sp != space)
                        break;
                    ++p;
                    while (p < end && (((unsigned char)*p) & 0xC0) == 0x80)
                        ++p;
                    ++chars;
                }
                const float frac =
                    clampf((reveal - (float)seen) / std::max(1, chars), 0.0f, 1.0f);
                const ImU32 c = lerp_col(unsung, ui_with_alpha(col, alpha), frac);
                const std::string piece(tok, p - tok);
                if (!space && glow > 0.01f && frac > 0.0f)
                {
                    const float o = (1.4f + 1.8f * glow) * scale;
                    const float a = alpha * 0.075f * glow * frac;
                    dl->AddText(font, size, ImVec2(tx - o, y),
                                ui_with_alpha(glow_col, a), piece.c_str());
                    dl->AddText(font, size, ImVec2(tx + o, y),
                                ui_with_alpha(glow_col, a), piece.c_str());
                    dl->AddText(font, size, ImVec2(tx, y - o),
                                ui_with_alpha(glow_col, a), piece.c_str());
                    dl->AddText(font, size, ImVec2(tx, y + o),
                                ui_with_alpha(glow_col, a), piece.c_str());
                }
                dl->AddText(font, size, ImVec2(tx, y), c, piece.c_str());
                tx += measure(font, size, piece);
                seen += chars;
            }
        }
        else
        {
            if (glow > 0.01f && !row.text.empty())
            {
                const float o = (1.4f + 1.8f * glow) * scale;
                const float a = alpha * 0.075f * glow;
                dl->AddText(font, size, ImVec2(x - o, y), ui_with_alpha(glow_col, a), row.text.c_str());
                dl->AddText(font, size, ImVec2(x + o, y), ui_with_alpha(glow_col, a), row.text.c_str());
                dl->AddText(font, size, ImVec2(x, y - o), ui_with_alpha(glow_col, a), row.text.c_str());
                dl->AddText(font, size, ImVec2(x, y + o), ui_with_alpha(glow_col, a), row.text.c_str());
            }
            if (!row.text.empty())
                dl->AddText(font, size, ImVec2(x, y), ui_with_alpha(col, alpha), row.text.c_str());
        }
        y += line_h;
    }
}

bool draw_transport_button(AppState &state, const char *id, UiIcon icon, ImVec2 centre,
                           float size, bool primary, float alpha, const char *tip)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(ImVec2(centre.x - size * 0.5f, centre.y - size * 0.5f));
    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool pressed = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked();

    const float s = size * (primary ? 0.40f : 0.36f) *
                    (hovered ? 1.06f : 1.0f) * (pressed ? 0.97f : 1.0f);
    if (primary)
        dl->AddCircleFilled(centre, size * 0.5f, ui_with_alpha(ui_accent_u32(), alpha), 40);
    else if (hovered)
        dl->AddCircleFilled(centre, size * 0.5f, ui_with_alpha(ui_overlay_u32(0.10f), alpha), 32);

    const ImU32 col = primary ? ui_with_alpha(ui_col(ui_c().on_accent), alpha)
                              : ui_with_alpha(hovered ? ui_text_u32() : ui_text_dim_u32(), alpha);
    ui_draw_icon(dl, icon, centre, s, col, primary ? 1.6f : 1.5f);
    if (hovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (tip)
            ImGui::SetTooltip("%s", tip);
    }

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
    return clicked;
}

int next_nonempty_index(const Lyrics &lx, int from)
{
    for (int i = std::max(0, from); i < (int)lx.lines.size(); ++i)
        if (!blank(lx.lines[i]))
            return i;
    return -1;
}

} // namespace

void draw_lyrics_immersive(AppState &state, MpvPlayer &player)
{
    static bool s_open = false;
    const bool want = state.lyrics_immersive;

    if (want && !s_open)
    {
        ImGui::OpenPopup("##lyrics_immersive");
        s_open = true;
        ui_audio_start();

        const double now = ImGui::GetTime();
        g_imm = ImmersiveAnim{};
        g_imm.enter = ui_animations_enabled() ? 0.0f : 1.0f;
        g_imm.view_start = now;
        g_imm.last_input = now;
        g_imm.last_mouse = ImGui::GetIO().MousePos;
        g_imm.chrome = 1.0f;
        g_imm.chrome_init = true;
        g_imm.active = -2;
        if (!g_hint_done)
        {
            g_hint_done = true;
            g_hint_start = now;
        }
        ui_anim_request();
    }
    if (!s_open)
        return;

    ImGuiIO &io = ImGui::GetIO();
    const UiMetrics &m = ui_m();
    const double now = ImGui::GetTime();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);

    // Transparent window background so the app's ambient backdrop shows
    // through. We dim it with a translucent wash and add the accent spotlight
    // ourselves, which is what makes the takeover feel continuous with the
    // rest of the app.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));

    bool close = !want;
    if (ImGui::BeginPopupModal("##lyrics_immersive", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings))
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        const float w = std::max(1.0f, ws.x);
        const float h = std::max(1.0f, ws.y);

        float dt = io.DeltaTime;
        if (!(dt > 0.0f) || dt > 0.1f)
            dt = 1.0f / 60.0f;

        const bool motion = ui_animations_enabled();
        const float pad = std::max(ui_px(24.0f), std::min(w, h) * 0.045f);
        const float max_text_w = std::min(w - pad * 2.0f, w * 0.84f);
        const float focal_y = h * 0.455f;
        const ImVec2 focal(w * 0.5f, focal_y);
        bool animating = false;

        // ---- Resolve the track + payload, exactly like the docked panel ----
        const Track display = lyrics_display_track(state);
        auto snap = std::atomic_load(&state.lyrics);
        const bool has_payload = snap && snap->track_id == display.id;

        if (g_imm.track != display.id)
        {
            g_imm.track = display.id;
            g_imm.enter = motion ? 0.0f : 1.0f;
            g_imm.view_start = now;
            g_imm.active = -2;
            g_imm.pulse = 0.0f;
            g_imm.prev_text.clear();
            g_imm.prev_size = 0.0f;
            g_imm.prev_since = -100.0;
        }

        if (motion && g_imm.enter < 1.0f)
        {
            g_imm.enter = std::min(1.0f, g_imm.enter + dt / kViewEnter);
            animating = true;
        }
        if (!motion)
            g_imm.enter = 1.0f;
        const float view_a = ease_out(g_imm.enter);

        // Seek target survives the chrome fading out mid-drag.
        static float s_seek = -1.0f;

        // ---- Chrome idle detection ----
        const ImVec2 mp = io.MousePos;
        if (mp.x != g_imm.last_mouse.x || mp.y != g_imm.last_mouse.y ||
            io.MouseWheel != 0.0f || ImGui::IsMouseClicked(0) ||
            ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2))
        {
            g_imm.last_input = now;
        }
        g_imm.last_mouse = mp;

        const float chrome_target = (now - g_imm.last_input < kChromeIdle) ? 1.0f : 0.0f;
        if (!motion)
            g_imm.chrome = chrome_target;
        else
        {
            const float e = 1.0f - std::exp(-9.0f * dt);
            g_imm.chrome += (chrome_target - g_imm.chrome) * e;
            if (std::fabs(chrome_target - g_imm.chrome) > 0.002f)
                animating = true;
        }
        const float chrome_a = clampf(g_imm.chrome, 0.0f, 1.0f);
        if (chrome_a < 0.03f)
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);

        // ---- Background: cover art + audio-reactive ambient blooms ----
        const ImVec4 tint = ui_backdrop_tint();
        const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(ui_accent_u32(1.0f));
        const float day = clampf(ui_theme_progress(), 0.0f, 1.0f);
        const AudioReactive &au = ui_audio_levels();
        const float energy = clampf(au.rms, 0.0f, 1.0f);
        const bool reactive = au.active && energy > 0.008f && motion;

        dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h), ui_col(ui_c().bg));

        // Cover art, blurred and held well back so the lyrics stay the focal
        // point. This uses the full-screen (512px) blur rather than the hero
        // card's 128px one, which looks blocky when stretched over a window.
        if (!display.thumbnail_url.empty())
        {
            ui_immersive_blur_request(display.thumbnail_url);
            const GLuint blur = ui_immersive_blur_texture(display.thumbnail_url);
            int bw = 0, bh = 0;
            if (blur != 0 && ui_immersive_blur_size(display.thumbnail_url, bw, bh))
            {
                float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                const TextureInfo info{blur, bw, bh};
                if (cover_uv(info, w, h, u0, v0, u1, v1))
                {
                    const float zoom = 1.04f + (reactive ? 0.020f * au.beat +
                                                              0.008f * au.bass
                                                         : 0.0f);
                    const float cx = (u0 + u1) * 0.5f;
                    const float cy = (v0 + v1) * 0.5f;
                    const float hw = (u1 - u0) * 0.5f / zoom;
                    const float hh = (v1 - v0) * 0.5f / zoom;
                    const float drift = reactive ? 0.004f * std::sin((float)now * 0.17f)
                                                 : 0.0f;
                    const float a = (0.34f - 0.14f * day) * (0.85f + 0.35f * energy);
                    dl->AddImage((ImTextureID)(intptr_t)blur, wp,
                                 ImVec2(wp.x + w, wp.y + h),
                                 ImVec2(cx - hw + drift, cy - hh),
                                 ImVec2(cx + hw + drift, cy + hh),
                                 IM_COL32(255, 255, 255,
                                          (int)(clampf(a, 0.0f, 1.0f) * 255.0f)));
                }
            }
        }

        // Scrim plus a theme/cover colour blend: keeps the takeover readable in
        // both day and night themes while making the artwork feel like part of
        // the existing palette rather than a separate layer.
        dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h),
                          ui_with_alpha(ui_col(ui_c().bg), 0.52f + 0.14f * day));
        dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h),
                          ui_with_alpha(ui_col(mix4(tint, accent, 0.25f)),
                                        0.10f * (1.0f - 0.45f * day)));

        // Centred ambient colour sampled from the artwork itself. The palette
        // is computed once when the cover is decoded; invalid bands fall back
        // to the dominant tint, and a touch of the theme accent keeps the
        // gradient in the same family as the rest of the UI.
        TextureColor palette[3];
        if (texture_cover_palette(display.thumbnail_url, palette))
        {
            const ImVec2 spots[3] = {
                ImVec2(wp.x + w * 0.28f, wp.y + focal.y * 0.94f),
                ImVec2(wp.x + w * 0.50f, wp.y + focal.y),
                ImVec2(wp.x + w * 0.72f, wp.y + focal.y * 0.94f)};
            const float radii[3] = {0.62f, 0.82f, 0.62f};
            const float peaks[3] = {0.075f, 0.105f, 0.075f};
            for (int i = 0; i < 3; ++i)
            {
                ImVec4 col = tint;
                if (palette[i].valid)
                    col = ImVec4(palette[i].r, palette[i].g, palette[i].b, 1.0f);
                col = mix4(col, accent, 0.18f);
                const float peak = peaks[i] * (1.0f - 0.35f * day) *
                                   (0.85f + 0.30f * energy) * view_a;
                soft_glow(dl, spots[i], std::min(w, h) * radii[i], col, peak);
            }
        }

        // Bloom anchors wander between the four corners, easing between them
        // and re-targeting more often as the music gets louder. When nothing
        // is playing they simply hold their last position.
        if (!g_bg.init)
        {
            for (int i = 0; i < 3; ++i)
            {
                g_bg.pos[i] = kBgCorners[g_bg.corner[i]];
                g_bg.target[i] = g_bg.pos[i];
                g_bg.next_move[i] = now + 2.0 + 1.5 * i;
            }
            g_bg.init = true;
        }

        const ImVec4 bloom_col[3] = {mix4(tint, accent, 0.22f),
                                     mix4(tint, accent, 0.48f),
                                     mix4(accent, tint, 0.35f)};
        const float move_speed = 1.1f + 2.2f * energy + 3.2f * au.beat;

        for (int i = 0; i < 3; ++i)
        {
            ImVec2 &p = g_bg.pos[i];
            ImVec2 &t = g_bg.target[i];
            if (reactive)
            {
                const float e = 1.0f - std::exp(-move_speed * dt);
                p.x += (t.x - p.x) * e;
                p.y += (t.y - p.y) * e;
                animating = true;
                const float dx = t.x - p.x;
                const float dy = t.y - p.y;
                if (now >= g_bg.next_move[i] && dx * dx + dy * dy < 0.0025f)
                {
                    int nc = (int)(bg_rand() % 4u);
                    if (nc == g_bg.corner[i])
                        nc = (nc + 1) % 4;
                    g_bg.corner[i] = nc;
                    g_bg.target[i] = kBgCorners[nc];
                    g_bg.next_move[i] = now + 1.4 + (double)(bg_rand() % 1400u) / 1000.0;
                }
            }

            const ImVec2 gc(wp.x + p.x * w, wp.y + p.y * h);
            const float base_r = std::min(w, h) * (i == 0 ? 0.70f : (i == 1 ? 0.55f : 0.46f));
            const float r = base_r * (1.0f + 0.14f * au.bass + 0.22f * au.beat);
            const float base_peak = i == 0 ? 0.15f : (i == 1 ? 0.11f : 0.085f);
            const float peak = base_peak * (0.75f + 0.75f * energy + 0.50f * au.beat) *
                               (0.70f + 0.30f * (1.0f - day));
            soft_glow(dl, gc, r, bloom_col[i], peak * view_a);
        }

        if (g_imm.pulse > 0.002f && motion)
        {
            g_imm.pulse *= std::exp(-kPulseDecay * dt);
            if (g_imm.pulse < 0.002f)
                g_imm.pulse = 0.0f;
            animating = true;
        }
        else if (!motion)
            g_imm.pulse = 0.0f;

        const float glow_r = std::min(w, h) * 0.46f;
        soft_glow(dl, focal, glow_r,
                  accent, (0.030f + g_imm.pulse * 0.045f + 0.020f * au.bass +
                           0.025f * au.beat) * view_a);
        soft_glow(dl, focal, glow_r * 0.72f,
                  accent, (0.022f + g_imm.pulse * 0.030f + 0.014f * au.mid +
                           0.018f * au.beat) * view_a);
        if (g_imm.pulse > 0.0f || reactive)
        {
            const float ring = 0.70f + 0.03f * g_imm.pulse + 0.02f * au.beat;
            dl->AddCircle(focal, glow_r * ring,
                          ui_accent_u32((0.06f * g_imm.pulse + 0.035f * au.beat +
                                         0.020f * au.bass) * view_a),
                          96, ui_px(1.5f));
        }

        // ---- Lyrics ----
        ImFont *font = g_ui_fonts.semibold;
        const float active_pref = clampf(h * 0.075f * m.text_scale, 30.0f,
                                         h * 0.16f);

        bool show_message = false;
        std::string message;

        if (display.id.empty())
        {
            show_message = true;
            message = "Nothing playing.";
        }
        else if (!has_payload)
        {
            show_message = true;
            message = state.lyrics_loading.load() ? "Loading lyrics" : "Lyrics not loaded.";
        }
        else if (!snap->lyrics.ok)
        {
            show_message = true;
            message = "Couldn't load lyrics.";
        }
        else if (snap->lyrics.none || snap->lyrics.lines.empty())
        {
            show_message = true;
            message = "No lyrics found for this track.";
        }

        const Lyrics *lx = has_payload ? &snap->lyrics : nullptr;

        if (show_message)
        {
            TextLayout msg = layout_text(font, message, max_text_w * 0.8f,
                                         active_pref * 0.46f, 20.0f);
            draw_rows(dl, font, msg, focal,
                      ui_text_dim_u32(), 0.85f * view_a, 1.0f, 0.0f);
            if (state.lyrics_loading.load() && motion)
            {
                const float dot_y = focal.y + msg.height * 0.5f + ui_px(24.0f);
                for (int i = 0; i < 3; ++i)
                {
                    const float ph = (float)now * 2.6f - (float)i * 0.6f;
                    const float a = 0.20f + 0.50f * (0.5f + 0.5f * std::sin(ph));
                    const float dx = ((float)i - 1.0f) * ui_px(14.0f);
                    dl->AddCircleFilled(ImVec2(focal.x + dx, dot_y + std::sin(ph) * ui_px(2.0f)),
                                        ui_px(3.2f), ui_accent_u32(a * view_a), 14);
                }
                animating = true;
            }
        }
        else if (!lx->synced)
        {
            // ---- Unsynced fallback: a slowly read, poem-like block ----
            float size = clampf(active_pref * 0.55f, 18.0f, 58.0f);
            const float floor_size = std::max(13.0f, size * 0.55f);
            const float block_w = max_text_w * 0.82f;

            auto build = [&](float s, float &total_h) {
                std::vector<TextRow> rows;
                for (const std::string &line : lx->lines)
                {
                    if (blank(line))
                    {
                        rows.push_back({std::string(), 0.0f});
                        continue;
                    }
                    TextLayout one = layout_text(font, line, block_w, s, s);
                    rows.insert(rows.end(), one.rows.begin(), one.rows.end());
                }
                total_h = (float)rows.size() * (s * 1.26f);
                return rows;
            };

            float total_h = 0.0f;
            std::vector<TextRow> rows = build(size, total_h);
            const float avail_h = h - pad * 2.0f;
            // With motion off, shrink a little first so most unsynced blocks
            // still fit without any scrolling at all.
            if (!motion)
            {
                while (total_h > avail_h && size > floor_size)
                {
                    size = std::max(floor_size, size - 2.0f);
                    rows = build(size, total_h);
                }
            }

            const float line_h = size * 1.26f;
            const bool overflow = total_h > avail_h;
            float scroll = 0.0f;
            if (overflow && motion)
            {
                const float speed = std::max(12.0f, h * 0.018f);
                scroll = std::min((float)(now - g_imm.view_start) * speed,
                                  total_h - avail_h + ui_px(40.0f));
                animating = true;
            }

            float y = overflow ? (pad - scroll) : (h - total_h) * 0.5f;
            const float edge = std::max(ui_px(42.0f), h * 0.09f);
            const float top = pad;
            const float bottom = h - pad;

            dl->PushClipRect(ImVec2(wp.x, wp.y + top), ImVec2(wp.x + w, wp.y + bottom), true);
            for (const TextRow &row : rows)
            {
                float a = view_a * 0.86f;
                if (y < top + edge)
                    a *= clampf((y - top) / edge, 0.0f, 1.0f);
                if (y + line_h > bottom - edge)
                    a *= clampf((bottom - (y + line_h)) / edge, 0.0f, 1.0f);
                if (a > 0.001f && !row.text.empty())
                {
                    const float tw = measure(font, size, row.text);
                    dl->AddText(font, size, ImVec2(w * 0.5f - tw * 0.5f, y),
                                ui_with_alpha(ui_text_dim_u32(), a), row.text.c_str());
                }
                y += line_h;
            }
            dl->PopClipRect();
        }
        else
        {
            // ---- Synced: fixed focal point + active/upcoming/exit ----
            const int active = lyrics_active_line(*lx, state, player);
            if (g_imm.active == -2)
            {
                g_imm.active = active;
                g_imm.active_since = now;
                if (active >= 0)
                    g_imm.pulse = motion ? 1.0f : 0.0f;
            }
            else if (active != g_imm.active)
            {
                if (g_imm.active >= 0 && g_imm.active < (int)lx->lines.size() &&
                    !blank(lx->lines[g_imm.active]))
                {
                    g_imm.prev_text = lx->lines[g_imm.active];
                    g_imm.prev_size = g_imm.last_size > 0.0f ? g_imm.last_size
                                                             : active_pref;
                    g_imm.prev_since = now;
                }
                g_imm.active = active;
                g_imm.active_since = now;
                if (motion && active >= 0)
                    g_imm.pulse = 1.0f;
            }

            const float line_t = motion
                                     ? ease_out((float)((now - g_imm.active_since) / kLineEnter))
                                     : 1.0f;
            if (motion && line_t < 1.0f)
                animating = true;

            std::string active_text;
            int active_idx = -1;
            if (active >= 0 && active < (int)lx->lines.size())
            {
                active_idx = active;
                if (!blank(lx->lines[active]))
                    active_text = lx->lines[active];
            }
            if (active_text.empty())
            {
                active_idx = next_nonempty_index(*lx, active >= 0 ? active : 0);
                if (active_idx >= 0)
                    active_text = lx->lines[active_idx];
            }

            if (!active_text.empty())
            {
                const bool preview = active < 0;

                // Karaoke wipe: use the gap to the next timed line as this
                // line's duration, so the accent colour sweeps the words in
                // step with playback.
                float karaoke = -1.0f;
                if (!preview && active_idx < (int)lx->times.size())
                {
                    const double t = lyrics_playback_time(state, player) -
                                     state.settings.lyrics_offset;
                    const double t0 = lx->times[active_idx];
                    double t1 = active_idx + 1 < (int)lx->times.size()
                                    ? lx->times[active_idx + 1]
                                    : t0 + 4.0;
                    if (t1 <= t0)
                        t1 = t0 + 0.5;
                    karaoke = clampf((float)((t - t0) / (t1 - t0)), 0.0f, 1.0f);
                    if (karaoke < 1.0f && state.playing)
                        animating = true;
                }
                const float pref = preview ? active_pref * 0.62f : active_pref;
                TextLayout act = layout_text(font, active_text, max_text_w, pref,
                                             std::max(20.0f, pref * 0.5f));
                g_imm.last_size = act.size;
                const float act_h = act.rows.size() * act.line_h;

                const float line_dir = ((active_idx & 1) ? 1.0f : -1.0f);
                const float slide = (1.0f - line_t) * ui_px(22.0f);
                const float slide_x = (1.0f - line_t) * line_dir * ui_px(22.0f);
                const float scale = motion ? (0.965f + 0.035f * line_t +
                                              0.018f * g_imm.pulse)
                                           : 1.0f;
                const float alpha = (preview ? 0.58f : 1.0f) * view_a *
                                    (motion ? (0.35f + 0.65f * line_t) : 1.0f);

                draw_rows(dl, font, act,
                          ImVec2(focal.x + slide_x, focal.y + slide),
                          preview ? ui_text_dim_u32() : ui_accent_text_u32(),
                          alpha, scale, preview ? 0.0f : (0.35f + 0.65f * g_imm.pulse),
                          karaoke);

                // Previous line lifts away above the active one.
                if (motion && !g_imm.prev_text.empty())
                {
                    const float exit_t = clampf((float)((now - g_imm.prev_since) / 0.42), 0.0f, 1.0f);
                    if (exit_t < 1.0f)
                    {
                        TextLayout prev = layout_text(font, g_imm.prev_text, max_text_w * 0.9f,
                                                      g_imm.prev_size * 0.70f, 16.0f);
                        const float prev_h = prev.rows.size() * prev.line_h;
                        const float y = focal.y - act_h * 0.5f - ui_px(28.0f) -
                                        prev_h * 0.5f - exit_t * ui_px(26.0f);
                        draw_rows(dl, font, prev, ImVec2(focal.x, y),
                                  ui_text_dim_u32(), (1.0f - exit_t) * 0.30f * view_a,
                                  1.0f, 0.0f);
                        animating = true;
                    }
                }

                // Faint upcoming lines below.
                const float up_pref = std::max(17.0f, act.size * 0.37f);
                float up_y = focal.y + act_h * 0.5f + ui_px(30.0f);
                int taken = 0;
                for (int i = active_idx + 1; i < (int)lx->lines.size() && taken < 2; ++i)
                {
                    if (blank(lx->lines[i]))
                        continue;
                    TextLayout up = layout_text(font, lx->lines[i],
                                                max_text_w * 0.82f, up_pref, up_pref);
                    const float a = view_a * (taken == 0 ? 0.34f : 0.17f) *
                                    (motion ? (0.55f + 0.45f * line_t) : 1.0f);
                    draw_rows(dl, font, up, ImVec2(focal.x, up_y + up.height * 0.5f),
                              ui_text_dim_u32(), a, 1.0f, 0.0f);
                    up_y += up.height + ui_px(12.0f);
                    ++taken;
                }
            }
        }

        // ---- Auto-hiding chrome ----
        if (chrome_a > 0.003f)
        {
            const float ca = chrome_a * view_a;
            const float top_h = std::max(ui_px(96.0f), h * 0.16f);
            const float bot_h = std::max(ui_px(128.0f), h * 0.20f);
            dl->AddRectFilledMultiColor(
                wp, ImVec2(wp.x + w, wp.y + top_h),
                ui_with_alpha(ui_col(ui_c().bg), 0.62f * ca),
                ui_with_alpha(ui_col(ui_c().bg), 0.62f * ca),
                ui_with_alpha(ui_col(ui_c().bg), 0.0f),
                ui_with_alpha(ui_col(ui_c().bg), 0.0f));
            dl->AddRectFilledMultiColor(
                ImVec2(wp.x, wp.y + h - bot_h), ImVec2(wp.x + w, wp.y + h),
                ui_with_alpha(ui_col(ui_c().bg), 0.0f),
                ui_with_alpha(ui_col(ui_c().bg), 0.0f),
                ui_with_alpha(ui_col(ui_c().bg), 0.72f * ca),
                ui_with_alpha(ui_col(ui_c().bg), 0.72f * ca));

            // Top-left now-playing block.
            const float title_base = 20.0f;
            const float artist_base = 14.0f;
            const float meta_base = 11.0f;
            const float text_x = pad;
            const float max_title_w = std::max(80.0f, w - pad * 2.0f);
            const std::string title =
                ui_ellipsize(g_ui_fonts.medium, title_base,
                             display.title.empty() ? "Nothing playing" : display.title,
                             max_title_w);
            const std::string artist =
                ui_ellipsize(g_ui_fonts.body, artist_base, display.artist, max_title_w);
            const float meta_y = pad * 0.62f;
            const float dot_r = ui_px(2.4f);
            dl->AddCircleFilled(ImVec2(text_x + dot_r, meta_y + meta_base * m.text_scale * 0.55f),
                                dot_r, ui_with_alpha(ui_accent_text_u32(), 0.85f * ca), 10);
            dl->AddText(g_ui_fonts.medium, meta_base * m.text_scale,
                        ImVec2(text_x + dot_r * 3.0f, meta_y),
                        ui_with_alpha(ui_accent_text_u32(), 0.75f * ca), "NOW PLAYING");
            dl->AddText(g_ui_fonts.medium, title_base * m.text_scale,
                        ImVec2(text_x, meta_y + meta_base * m.text_scale * 1.7f),
                        ui_with_alpha(ui_text_u32(), ca), title.c_str());
            if (!artist.empty())
                dl->AddText(g_ui_fonts.body, artist_base * m.text_scale,
                            ImVec2(text_x, meta_y + meta_base * m.text_scale * 1.7f +
                                              title_base * m.text_scale * 1.38f),
                            ui_with_alpha(ui_text_dim_u32(), ca), artist.c_str());

            // Progress + transport.
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
            const bool have_dur = duration > 0.0f;

            const float bar_w = std::max(20.0f, w - pad * 2.0f);
            const float bar_x = pad;
            const float bar_y = h - pad - ui_px(12.0f);
            const float ctrl_y = bar_y - ui_px(40.0f);
            const float time_y = bar_y - ui_px(26.0f);

            // Times.
            dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale,
                        ImVec2(bar_x, time_y),
                        ui_with_alpha(ui_text_muted_u32(), ca), fmt_time((int)current).c_str());
            const std::string dur_s = have_dur ? fmt_time((int)duration) : "--:--";
            const float dur_w = measure(g_ui_fonts.body, 13.0f * m.text_scale, dur_s);
            dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale,
                        ImVec2(bar_x + bar_w - dur_w, time_y),
                        ui_with_alpha(ui_text_muted_u32(), ca), dur_s.c_str());

            // Seek bar (disabled while a crossfade owns both players).
            const float bar_h = ui_px(4.0f);
            const float frac = have_dur ? clampf(current / duration, 0.0f, 1.0f) : 0.0f;
            dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h),
                              ui_with_alpha(ui_overlay_u32(0.16f), ca), bar_h * 0.5f);
            if (frac > 0.0f)
                dl->AddRectFilled(ImVec2(bar_x, bar_y),
                                  ImVec2(bar_x + bar_w * frac, bar_y + bar_h),
                                  ui_with_alpha(ui_accent_u32(1.0f), ca), bar_h * 0.5f);

            if (ca > 0.15f && have_dur)
            {
                ImGui::SetCursorScreenPos(ImVec2(bar_x, bar_y - ui_px(9.0f)));
                ImGui::InvisibleButton("##lyrics_immersive_seek",
                                       ImVec2(bar_w, ui_px(22.0f)));
                const bool hov = ImGui::IsItemHovered();
                const bool act = ImGui::IsItemActive();
                if (fading)
                {
                    s_seek = -1.0f;
                    if (hov)
                        ImGui::SetTooltip("Seeking is unavailable during a crossfade");
                }
                else if (act)
                {
                    float t = clampf((io.MousePos.x - bar_x) / std::max(1.0f, bar_w), 0.0f, 1.0f);
                    s_seek = t * duration;
                    if (hov || act)
                        ImGui::SetTooltip("%s", fmt_time((int)s_seek).c_str());
                }
                else if (s_seek >= 0.0f)
                {
                    player.seek(s_seek);
                    s_seek = -1.0f;
                }
                if (hov || act)
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (hov || act)
                    dl->AddCircleFilled(ImVec2(bar_x + bar_w * frac, bar_y + bar_h * 0.5f),
                                        act ? ui_px(6.5f) : ui_px(5.0f),
                                        ui_with_alpha(ui_accent_u32(1.0f), ca), 16);
            }
            else if (s_seek >= 0.0f && !fading)
            {
                // Chrome hid between press and release: don't strand the seek.
                player.seek(s_seek);
                s_seek = -1.0f;
            }

            // Transport, centred.
            const float transport = std::min(ui_px(44.0f), h * 0.07f);
            const float side = transport * 0.72f;
            const float gap = ui_px(28.0f);
            const float cx = w * 0.5f;
            draw_transport_button(state, "##imm_prev", UiIcon::Prev,
                                  ImVec2(cx - transport * 0.5f - gap - side * 0.5f, ctrl_y),
                                  side, false, ca, "Previous");
            draw_transport_button(state, state.playing ? "##imm_pause" : "##imm_play",
                                  state.playing ? UiIcon::Pause : UiIcon::Play,
                                  ImVec2(cx, ctrl_y), transport, true, ca,
                                  state.playing ? "Pause" : "Play");
            draw_transport_button(state, "##imm_next", UiIcon::Next,
                                  ImVec2(cx + transport * 0.5f + gap + side * 0.5f, ctrl_y),
                                  side, false, ca, "Next");

            // Volume, bottom-right, when there is room.
            const float vol_w = std::min(ui_px(150.0f), w * 0.18f);
            if (w > ui_px(700.0f) && vol_w > ui_px(70.0f))
            {
                const int vol = state.crossfade_active
                                    ? state.crossfade_user_volume
                                    : (int)player.get_property_double("volume");
                const float v01 = clampf((float)(vol < 0 ? 0 : vol) / 100.0f, 0.0f, 1.0f);
                const float icon = ui_px(18.0f);
                const float vx = w - pad - vol_w - icon - ui_px(8.0f);
                const float vy = ctrl_y;
                ui_draw_icon(dl, UiIcon::Volume, ImVec2(vx + icon * 0.5f, vy),
                             icon, ui_with_alpha(ui_text_dim_u32(), ca), 1.5f);

                const float slider_x = vx + icon + ui_px(8.0f);
                const float slider_w = vol_w;
                const float track_h = ui_px(3.0f);
                dl->AddRectFilled(ImVec2(slider_x, vy - track_h * 0.5f),
                                  ImVec2(slider_x + slider_w, vy + track_h * 0.5f),
                                  ui_with_alpha(ui_overlay_u32(0.16f), ca), track_h * 0.5f);
                dl->AddRectFilled(ImVec2(slider_x, vy - track_h * 0.5f),
                                  ImVec2(slider_x + slider_w * v01, vy + track_h * 0.5f),
                                  ui_with_alpha(ui_accent_u32(1.0f), ca), track_h * 0.5f);
                dl->AddCircleFilled(ImVec2(slider_x + slider_w * v01, vy),
                                    ui_px(4.5f),
                                    ui_with_alpha(ui_accent_u32(1.0f), ca), 14);

                ImGui::SetCursorScreenPos(ImVec2(slider_x, vy - ui_px(10.0f)));
                ImGui::InvisibleButton("##imm_volume", ImVec2(slider_w, ui_px(20.0f)));
                if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsItemActive())
                    {
                        const float t = clampf((io.MousePos.x - slider_x) /
                                                   std::max(1.0f, slider_w),
                                               0.0f, 1.0f);
                        state.volume_request = (int)std::lround(t * 100.0f);
                    }
                    ImGui::SetTooltip("%d%%", (int)std::lround(v01 * 100.0f));
                }
            }
        }

        // ---- One-time Esc hint ----
        const float hint_age = (float)(now - g_hint_start);
        if (hint_age < 5.0f)
        {
            float a = 1.0f;
            if (hint_age < 0.35f)
                a = hint_age / 0.35f;
            else if (hint_age > 4.2f)
                a = clampf((5.0f - hint_age) / 0.8f, 0.0f, 1.0f);
            const float y = h - pad - ui_px(106.0f);
            const std::string hint = "Esc to exit";
            const float tw = measure(g_ui_fonts.body, 13.0f * m.text_scale, hint);
            dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale,
                        ImVec2((w - tw) * 0.5f, y),
                        ui_with_alpha(ui_text_muted_u32(), a * 0.9f), hint.c_str());
            animating = true;
        }

        // ---- Close / exit ----
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;

        if (close)
        {
            ImGui::CloseCurrentPopup();
            s_open = false;
            state.lyrics_immersive = false;
            ui_audio_stop();
        }

        if (animating)
        {
            ui_anim_request();
            set_lyrics_animating(true);
        }
        else
        {
            set_lyrics_animating(false);
        }

        ImGui::EndPopup();
    }
    else
    {
        s_open = false;
        state.lyrics_immersive = false;
        ui_audio_stop();
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(5);
}
