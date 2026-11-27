#include "ui.h"
#include "ui_audio_reactive.h"
#include "ui_backdrop.h"
#include "ui_icons.h"
#include "ui_layout.h"
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
// Line activation stays timestamp-driven. Rows whose intervals overlap are
// grouped as simultaneous singers, so every row in that group gets the accent
// sweep and glow together. Each group change fires a decaying glow envelope;
// the horizontal energy ribbon lands on its NOW boundary at that same moment,
// while live bass/guitar/treble levels only colour and thicken it.
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
const float kViewExit = 0.30f;     // fade back to the app on close
const double kSwitchDur = 0.80;    // column-to-column cover hand-off
const double kCenterLeft = 0.26;   // centred card returns to the column
const double kCenterUp = 0.46;     // then leaves upward
const double kCenterIn = 0.52;     // then the next cover rises in
const double kLyricReveal = 0.34;  // one lyric line's fade/rise
const double kLyricStagger = 0.05; // delay between consecutive lines
const int kWaveLayers = 5;         // main carrier + four quiet echoes

float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

float ease_out(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t);
}

// Smooth acceleration and deceleration for the song hand-off. A linear or
// ease-out travel over a full screen height reads as a mechanical scroll;
// this cubic gives the cover weight at both ends.
float ease_in_out(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return t < 0.5f ? 4.0f * t * t * t
                    : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}

float smooth_unit(float t)
{
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Spectrum needs a taller pocket than Waves: its bars grow up from a baseline
// near the bottom of the rail, so the whole band has to be deeper to read as
// tall. Both styles still share the same timestamp-driven loader.
float energy_band_height(float size, int style)
{
    return style == 1
               ? clampf(size * 2.25f, ui_px(86.0f), ui_px(210.0f))
               : clampf(size * 1.75f, ui_px(64.0f), ui_px(156.0f));
}

float am_bell(float u, float centre, float width)
{
    const float d = (u - centre) / width;
    return std::exp(-d * d);
}

// Amplitude envelope in the spirit of the Arctic Monkeys "AM" cover: three
// smooth lobes and a shallow central notch hint at the letterform without ever
// drawing readable text. Both loader styles share it, so Waves and Spectrum
// read as the same signal rather than two unrelated visualisers.
float am_envelope(float u, float phase, float seed)
{
    const float a = am_bell(u, 0.26f, 0.15f);
    const float m_left = am_bell(u, 0.51f, 0.17f);
    const float m_right = am_bell(u, 0.76f, 0.14f);
    const float notch = am_bell(u, 0.50f, 0.045f);
    const float drift =
        0.88f + 0.12f * std::sin(phase * 0.34f + u * 2.3f + seed);
    return clampf((0.20f + 0.52f * a + 0.46f * m_left +
                   0.40f * m_right - 0.12f * notch) *
                      drift,
                  0.06f, 1.0f);
}

// Carrier riding inside that envelope. Two close harmonics keep the signal
// crisp like an AM waveform, but the slower one dominates so it still reads as
// a smooth curve rather than a jagged analyser trace.
float am_carrier(float u, float phase, float seed, float layer,
                 float freq_scale = 1.0f)
{
    const float p = phase + seed * 1.4f + layer * 0.73f;
    const float scale = (1.0f + 0.12f * layer) * freq_scale;
    const float c1 =
        std::sin(u * 6.4f * scale * 6.2831853f + p * 1.35f);
    const float c2 =
        std::sin(u * 9.7f * scale * 6.2831853f - p * 0.82f + 1.3f);
    return (0.80f * c1 + 0.20f * c2) *
           am_envelope(u, phase, seed + layer * 0.41f);
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
    // All lyric rows currently being sung. More than one entry when a provider
    // gives two singers overlapping timestamps.
    std::vector<int> active_group;
    // Eased line index the view is centred on (continuous, not snapped).
    float scroll = -1.0f;
    double active_since = 0.0;
    double view_start = 0.0;
    double switch_start = -100.0;
    Track prev_display;
    Track current_display;
    // Where the outgoing cover was when the song changed, and how it was
    // drawn, so it can slide away without jumping layouts mid-flight.
    float prev_cover_x = -1.0f;
    bool prev_cover_card = false;
    bool switch_from_center = false;
    bool switch_both_cards = false;
    // The incoming track is known (from the lyrics cache) to have no lyrics.
    // Used until the live payload confirms it, so the card does not briefly
    // flash as a column while the cache is loaded.
    bool switch_next_card = false;
    // Last drawn cover position/layout; captured when the song changes.
    float cover_x = -1.0f;
    bool cover_card = false;
    // Timed 0..1 blend from the cover column to the window centre. Only used
    // on split layouts; a track without lyrics settles in the centre.
    float cover_center = 0.0f;
    float center_goal = 0.0f;
    float center_from = 0.0f;
    double center_start = -100.0;
    float unsynced_scroll = 0.0f;
    float enter = 0.0f;
    float pulse = 0.0f;
    bool exiting = false;
    float exit = 0.0f;
    std::string prev_text;
    float prev_size = 0.0f;
    float last_size = 0.0f;
    double prev_since = -100.0;
    double last_input = 0.0;
    ImVec2 last_mouse = ImVec2(-1.0f, -1.0f);
    float chrome = 1.0f;
    bool chrome_init = false;
    // Intro/gap loading bar state. Keys are lyric-index based so a loader can
    // only run once per track, even after seeking backwards.
    std::vector<std::string> energy_done;
    std::string energy_active_key;
    double energy_hit_start = -100.0;
    // Eases the reserved lyric pocket back after the completion flash so the
    // rows settle instead of snapping when the loader vanishes.
    float energy_pocket = 0.0f;
    std::uint64_t energy_seek_gen = 0;
    // Wave motion is integrated from playback time, never wall time, so it
    // freezes on pause/buffering/stop and follows the player's speed.
    double energy_phase = 0.0;
    double energy_drive = 0.55;
    double energy_last_pos = -1.0;
    float energy_held_rms = 0.0f;
    float energy_held_bass = 0.0f;
    float energy_held_mid = 0.0f;
    float energy_held_treble = 0.0f;
    float energy_held_instrument = 0.0f;
    // Slow per-track peak reference. Quiet guitar intros are compared against
    // this instead of an absolute level, so they do not look inert next to a
    // much louder, drum-heavy song.
    double energy_inst_ref = 0.18;
    float energy_held_density = 0.0f;
    float energy_held_beat = 0.0f;
    float energy_held_pluck = 0.0f;
};

// Soft radial bloom used to carry the app's album-colour backdrop into the
// takeover. Drawn as a vertex-coloured triangle fan so the falloff is
// interpolated per-pixel by the GPU rather than stacked as visible
// concentric rings: a shader-style gradient with a smooth (1 - t)^2 profile.
void soft_glow(ImDrawList *dl, ImVec2 centre, float radius, ImVec4 col,
               float peak, int rings = 8, int segs = 96)
{
    if (radius <= 0.5f || peak <= 0.0005f)
        return;
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

// Two-colour quad for the loader's gradient fills. ImGui has no shader hook
// here, but a four-vertex gradient is cheap and stays in the same draw batch.
void add_gradient_quad(ImDrawList *dl, const ImVec2 &tl, const ImVec2 &tr,
                       const ImVec2 &br, const ImVec2 &bl, ImU32 top,
                       ImU32 bottom)
{
    if (!dl)
        return;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const unsigned int base = dl->VtxBuffer.Size;
    dl->PrimReserve(6, 4);
    dl->PrimWriteVtx(tl, uv, top);
    dl->PrimWriteVtx(tr, uv, top);
    dl->PrimWriteVtx(br, uv, bottom);
    dl->PrimWriteVtx(bl, uv, bottom);
    dl->PrimWriteIdx((ImDrawIdx)(base + 0));
    dl->PrimWriteIdx((ImDrawIdx)(base + 1));
    dl->PrimWriteIdx((ImDrawIdx)(base + 2));
    dl->PrimWriteIdx((ImDrawIdx)(base + 0));
    dl->PrimWriteIdx((ImDrawIdx)(base + 2));
    dl->PrimWriteIdx((ImDrawIdx)(base + 3));
}

void add_gradient_rect(ImDrawList *dl, float x0, float y0, float x1, float y1,
                       ImU32 top, ImU32 bottom)
{
    add_gradient_quad(dl, ImVec2(x0, y0), ImVec2(x1, y0), ImVec2(x1, y1),
                      ImVec2(x0, y1), top, bottom);
}

// One karaoke row, drawn glyph by glyph. `reveal` is measured in glyphs, so
// the colour front can sit between two letters. The letter at the front also
// grows a little and settles back as the front moves on, which marks the note
// being sung without changing the line's size or position.
void draw_sweep_glyphs(ImDrawList *dl, ImFont *font, float size, float x, float y,
                       const std::string &text, ImU32 unsung, ImU32 lit,
                       float reveal, float glow)
{
    const ImU32 glow_col = ui_accent_text_u32();
    const char *p = text.c_str();
    const char *end = p + text.size();
    int gi = 0;
    float gx = x;
    while (p < end)
    {
        const char *q = next_cp(p, end);
        const std::string glyph(p, (std::size_t)(q - p));
        const float w = measure(font, size, glyph);
        const bool space = glyph[0] == ' ' || glyph[0] == '\t';
        if (!space)
        {
            const float frac = clampf(reveal - (float)gi, 0.0f, 1.0f);
            // 1 exactly while the front crosses this glyph's centre, 0 once
            // it is a glyph away in either direction.
            const float emphasis =
                clampf(1.0f - std::fabs(reveal - ((float)gi + 0.5f)) * 2.0f,
                       0.0f, 1.0f);
            const float gs = size * (1.0f + 0.16f * emphasis);
            const float gw = measure(font, gs, glyph);
            const float dx = (w - gw) * 0.5f;
            const float dy = (size - gs) * 0.5f;
            if (glow > 0.001f && emphasis > 0.001f)
            {
                const float o = (1.4f + 1.8f * glow) * (gs / std::max(1.0f, size));
                const float a = glow * 0.075f * emphasis;
                dl->AddText(font, gs, ImVec2(gx + dx - o, y + dy),
                            ui_with_alpha(glow_col, a), glyph.c_str());
                dl->AddText(font, gs, ImVec2(gx + dx + o, y + dy),
                            ui_with_alpha(glow_col, a), glyph.c_str());
                dl->AddText(font, gs, ImVec2(gx + dx, y + dy - o),
                            ui_with_alpha(glow_col, a), glyph.c_str());
                dl->AddText(font, gs, ImVec2(gx + dx, y + dy + o),
                            ui_with_alpha(glow_col, a), glyph.c_str());
            }
            dl->AddText(font, gs, ImVec2(gx + dx, y + dy),
                        lerp_col(unsung, lit, frac), glyph.c_str());
        }
        gx += w;
        p = q;
        ++gi;
    }
}

void draw_rows(ImDrawList *dl, ImFont *font, const TextLayout &lay, ImVec2 center,
               ImU32 col, float alpha, float scale, float glow,
               float karaoke = -1.0f, float unsung_tint = 0.0f)
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
    const ImU32 unsung_base =
        unsung_tint > 0.0f
            ? lerp_col(ui_text_dim_u32(), ui_accent_text_u32(),
                       0.42f * unsung_tint)
            : ui_text_dim_u32();
    const ImU32 unsung = ui_with_alpha(unsung_base, alpha * 0.80f);
    const int total = karaoke >= 0.0f ? std::max(1, count_cp_layout(lay)) : 0;
    const float reveal = clampf(karaoke, 0.0f, 1.0f) * (float)total;
    int seen = 0;

    for (const TextRow &row : lay.rows)
    {
        const float w = measure(font, size, row.text);
        const float x = center.x - w * 0.5f;
        if (karaoke >= 0.0f && !row.text.empty())
        {
            draw_sweep_glyphs(dl, font, size, x, y, row.text, unsung,
                              ui_with_alpha(col, alpha), reveal - (float)seen,
                              glow);
            seen += count_cp(row.text);
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

// Active-line word renderer in the app's own style: the words light up one by
// one, with no bounce, particles or theme swap. The only extra is a very
// subtle glow around the letter currently being sung.
bool draw_words_app(ImDrawList *dl, ImFont *font, float size, ImVec2 center,
                    float max_w, const RichLyricLine &line, double t,
                    double line_start_at, double line_end_at, float alpha)
{
    struct W
    {
        std::string text;
        double start;
        double end;
    };
    std::vector<W> words;
    if (!line.words.empty())
        for (const LyricWord &w : line.words)
            words.push_back({w.text, w.start_ms / 1000.0, w.end_ms / 1000.0});
    if (words.empty())
        return false; // line-only lyrics use the normal line renderer
    const double line_start = line_start_at >= 0.0 ? line_start_at
                                                   : line.start_ms / 1000.0;
    double line_end = line_end_at > line_start ? line_end_at
                                               : line.end_ms / 1000.0;
    if (line_end <= line_start)
        line_end = line_start + 4.0;
    if (words.size() > 1 && words.front().start <= 0.0)
    {
        double total = 0.0;
        for (const W &w : words)
            total += std::max(1, (int)w.text.size());
        double at = line_start;
        for (W &w : words)
        {
            const double share = (line_end - line_start) *
                                 (std::max(1, (int)w.text.size()) / total);
            w.start = at;
            w.end = at + share;
            at += share;
        }
    }
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        if (words[i].start <= 0.0)
            words[i].start = line_start;
        if (words[i].end <= words[i].start)
            words[i].end = i + 1 < words.size() ? words[i + 1].start
                                                : words[i].start + 0.4;
    }

    const float space_w = measure(font, size, " ");
    std::vector<float> advance;
    float total_w = 0.0f;
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        const float ww = measure(font, size, words[i].text);
        const bool has_space = !words[i].text.empty() &&
                               (words[i].text.back() == ' ' || words[i].text.back() == '\t');
        const bool needs_gap = !has_space && i + 1 < words.size() &&
                               line.text.find(' ') != std::string::npos;
        advance.push_back(ww + (needs_gap ? space_w : 0.0f));
        total_w += advance.back();
    }
    if (total_w > max_w)
        return false;

    float x = center.x - total_w * 0.5f;
    const float y = center.y - size * 0.5f;
    const ImU32 accent = ui_accent_text_u32();
    const ImU32 dim = ui_text_dim_u32();
    for (std::size_t wi = 0; wi < words.size(); ++wi)
    {
        const W &w = words[wi];
        const bool past = t >= w.end;
        const bool current = t >= w.start && t < w.end;
        if (past)
        {
            dl->AddText(font, size, ImVec2(x, y), ui_with_alpha(accent, alpha),
                        w.text.c_str());
        }
        else if (!current)
        {
            dl->AddText(font, size, ImVec2(x, y), ui_with_alpha(dim, alpha),
                        w.text.c_str());
        }
        else
        {
            // The word is a karaoke sweep, not a snap: keep the unsung text
            // dim and brighten it one glyph at a time from left to right.
            // The glyph at the leading edge grows slightly as it is sung.
            std::string glyphs = w.text;
            while (!glyphs.empty() && (glyphs.back() == ' ' || glyphs.back() == '\t'))
                glyphs.pop_back();
            const double span = std::max(0.05, w.end - w.start);
            const float p = clampf((float)((t - w.start) / span), 0.0f, 1.0f);
            if (glyphs.empty())
            {
                dl->AddText(font, size, ImVec2(x, y), ui_with_alpha(dim, alpha),
                            w.text.c_str());
            }
            else
            {
                const int glyph_count = std::max(1, count_cp(glyphs));
                draw_sweep_glyphs(dl, font, size, x, y, glyphs,
                                  ui_with_alpha(dim, alpha),
                                  ui_with_alpha(accent, alpha),
                                  p * (float)glyph_count, alpha);
            }
        }

        x += advance[wi];
    }
    return true;
}

// Artwork-only state: while lyrics are loading (or when a track has none),
// show the cover, title and artist instead of an empty message. The blurred
// cover already lives in the backdrop; this is the sharp card on top.
void draw_cover_card(ImDrawList *dl, const Track &display, ImVec2 focal,
                     float w, float h, float alpha, float y_shift = 0.0f)
{
    if (alpha <= 0.003f)
        return;
    const float s = std::min(w, h);
    const ImVec2 fc(focal.x, focal.y + y_shift);
    const float art = std::min(s * 0.48f, std::min(w * 0.44f, 520.0f));
    const ImVec2 p0(fc.x - art * 0.5f, fc.y - art * 0.62f);
    const ImVec2 p1(p0.x + art, p0.y + art);
    const float rounding = std::max(8.0f, art * 0.035f);
    dl->AddRectFilled(ImVec2(p0.x + 8.0f, p0.y + 14.0f), ImVec2(p1.x + 8.0f, p1.y + 14.0f),
                      ui_with_alpha(IM_COL32(0, 0, 0, 255), 0.30f * alpha), rounding);
    TextureInfo ti = texture_info(texture_highres_url(display.thumbnail_url, display.id));
    if (ti.id == 0)
        ti = texture_info(display.thumbnail_url);
    if (ti.id != 0)
    {
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        if (cover_uv(ti, art, art, u0, v0, u1, v1))
            dl->AddImageRounded((ImTextureID)(intptr_t)ti.id, p0, p1, ImVec2(u0, v0),
                                ImVec2(u1, v1), ui_with_alpha(IM_COL32_WHITE, alpha),
                                rounding);
    }
    else
    {
        dl->AddRectFilled(p0, p1, ui_with_alpha(ui_accent_u32(0.22f), alpha), rounding);
        dl->AddRect(p0, p1, ui_with_alpha(ui_accent_u32(0.35f), alpha), rounding, 0, 1.0f);
    }
    float ty = p1.y + std::max(18.0f, art * 0.06f);
    const float title_size = std::max(22.0f, std::min(38.0f, art * 0.09f));
    const std::string title = ui_ellipsize(g_ui_fonts.semibold, title_size,
                                           display.title.empty() ? "Nothing playing" : display.title,
                                           w * 0.78f);
    const float tw = measure(g_ui_fonts.semibold, title_size, title);
    dl->AddText(g_ui_fonts.semibold, title_size,
                ImVec2(fc.x - tw * 0.5f, ty),
                ui_with_alpha(ui_text_u32(), 0.94f * alpha), title.c_str());
    ty += title_size * 1.35f;
    if (!display.artist.empty())
    {
        const float as = std::max(14.0f, title_size * 0.52f);
        const std::string ar = ui_ellipsize(g_ui_fonts.body, as, display.artist, w * 0.72f);
        const float aw = measure(g_ui_fonts.body, as, ar);
        dl->AddText(g_ui_fonts.body, as, ImVec2(fc.x - aw * 0.5f, ty),
                    ui_with_alpha(ui_text_dim_u32(), 0.80f * alpha), ar.c_str());
    }
}

// The artwork column kept from the immersive experiment: wide windows put the
// sharp cover on the left and the existing line-timed lyrics on the right.
void draw_cover_column(ImDrawList *dl, const Track &display, ImVec2 centre,
                       float col_w, float h, float alpha, float y_shift = 0.0f)
{
    if (alpha <= 0.003f)
        return;
    const ImVec2 cc(centre.x, centre.y + y_shift);
    const float art = std::min(std::min(col_w * 0.86f, h * 0.56f), 560.0f);
    const ImVec2 p0(cc.x - art * 0.5f, cc.y - art * 0.55f);
    const ImVec2 p1(p0.x + art, p0.y + art);
    const float rounding = std::max(8.0f, art * 0.035f);
    dl->AddRectFilled(ImVec2(p0.x + 10.0f, p0.y + 18.0f), ImVec2(p1.x + 10.0f, p1.y + 18.0f),
                      ui_with_alpha(IM_COL32(0, 0, 0, 255), 0.34f * alpha), rounding);
    TextureInfo ti = texture_info(texture_highres_url(display.thumbnail_url, display.id));
    if (ti.id == 0)
        ti = texture_info(display.thumbnail_url);
    if (ti.id != 0)
    {
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        if (cover_uv(ti, art, art, u0, v0, u1, v1))
            dl->AddImageRounded((ImTextureID)(intptr_t)ti.id, p0, p1, ImVec2(u0, v0),
                                ImVec2(u1, v1), ui_with_alpha(IM_COL32_WHITE, alpha),
                                rounding);
    }
    else
    {
        dl->AddRectFilled(p0, p1, ui_with_alpha(ui_accent_u32(0.22f), alpha), rounding);
        dl->AddRect(p0, p1, ui_with_alpha(ui_accent_u32(0.35f), alpha), rounding, 0, 1.0f);
    }
    float ty = p1.y + std::max(20.0f, art * 0.065f);
    const float title_size = std::max(22.0f, std::min(36.0f, art * 0.085f));
    const std::string title = ui_ellipsize(g_ui_fonts.semibold, title_size,
                                           display.title.empty() ? "Nothing playing" : display.title,
                                           col_w * 0.92f);
    const float tw = measure(g_ui_fonts.semibold, title_size, title);
    dl->AddText(g_ui_fonts.semibold, title_size,
                ImVec2(cc.x - tw * 0.5f, ty),
                ui_with_alpha(ui_text_u32(), 0.94f * alpha), title.c_str());
    ty += title_size * 1.35f;
    if (!display.artist.empty())
    {
        const float as = std::max(14.0f, title_size * 0.52f);
        const std::string ar = ui_ellipsize(g_ui_fonts.body, as, display.artist, col_w * 0.88f);
        const float aw = measure(g_ui_fonts.body, as, ar);
        dl->AddText(g_ui_fonts.body, as, ImVec2(cc.x - aw * 0.5f, ty),
                    ui_with_alpha(ui_text_dim_u32(), 0.80f * alpha), ar.c_str());
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

// Instrumental rows count as blank for the loading bar: empty strings and
// notation such as ♪ / ♫ / "...". Real words never do.
bool is_blank_lyric(const std::string &s)
{
    const std::string marks[] = {"♪", "♫", "♩", "♬", "…", "...", "--", "-",
                                 "*", "•", "·"};
    std::size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            ++i;
            continue;
        }
        bool matched = false;
        for (const std::string &mark : marks)
        {
            if (!mark.empty() && s.compare(i, mark.size(), mark) == 0)
            {
                i += mark.size();
                matched = true;
                break;
            }
        }
        if (!matched)
            return false;
    }
    return true;
}

int next_real_index(const Lyrics &lx, int from)
{
    for (int i = std::max(0, from); i < (int)lx.lines.size(); ++i)
        if (!is_blank_lyric(lx.lines[(std::size_t)i]))
            return i;
    return -1;
}

double lyric_start_time(const Lyrics &lx, int i)
{
    if (i < 0 || i >= (int)lx.lines.size())
        return -1.0;
    if (i < (int)lx.rich.size() && lx.rich[(std::size_t)i].start_ms > 0)
        return lx.rich[(std::size_t)i].start_ms / 1000.0;
    if (i < (int)lx.times.size() && lx.times[(std::size_t)i] >= 0)
        return lx.times[(std::size_t)i];
    return -1.0;
}

double lyric_end_time(const Lyrics &lx, int i)
{
    if (i < 0 || i >= (int)lx.lines.size())
        return -1.0;
    if (i < (int)lx.rich.size() &&
        lx.rich[(std::size_t)i].end_ms > lx.rich[(std::size_t)i].start_ms)
        return lx.rich[(std::size_t)i].end_ms / 1000.0;
    return -1.0;
}

bool has_word_timings(const Lyrics &lx, int i)
{
    return i >= 0 && i < (int)lx.rich.size() &&
           !lx.rich[(std::size_t)i].words.empty();
}

// Most cached lyrics only carry line timestamps. In that case the original
// start is often placed a little before the vocal, so synthesising word times
// from it makes letters brighten early. Hold those lines back by a fraction of
// their own gap; lines with real word timings keep the provider's timing.
double lyric_vocal_guard(const Lyrics &lx, int i)
{
    if (has_word_timings(lx, i))
        return 0.0;
    const double start = lyric_start_time(lx, i);
    if (start < 0.0)
        return 0.0;
    double end = lyric_end_time(lx, i);
    if (end <= start && i + 1 < (int)lx.lines.size())
        end = lyric_start_time(lx, i + 1);
    if (end <= start)
        end = start + 2.5;
    return clampf((float)((end - start) * 0.12), 0.10f, 0.40f);
}

double effective_start_time(const Lyrics &lx, int i)
{
    const double start = lyric_start_time(lx, i);
    return start < 0.0 ? start : start + lyric_vocal_guard(lx, i);
}

// Two singers are not reliably encoded with the same timestamp. Providers may
// give them overlapping intervals (A 10-14, B 12-16) or identical ones. Group
// by real interval overlap instead of start-time proximity, otherwise the
// second singer is treated as the next distinct line and the first stops
// sweeping too early.
const double kOverlapMinSec = 0.18;
const double kOverlapMinRatio = 0.12;
const double kStartEpsilon = 0.05;

double inferred_line_end(const Lyrics &lx, int i)
{
    const double start = lyric_start_time(lx, i);
    if (start < 0.0)
        return -1.0;

    double end = lyric_end_time(lx, i);
    if (end <= start && i < (int)lx.rich.size() &&
        !lx.rich[(std::size_t)i].words.empty())
    {
        const auto &words = lx.rich[(std::size_t)i].words;
        end = words.back().end_ms / 1000.0;
    }
    if (end <= start)
    {
        // Duplicate rows share a start; skip them and use the next genuinely
        // new line as this row's end.
        for (int j = i + 1; j < (int)lx.lines.size(); ++j)
        {
            const double sj = lyric_start_time(lx, j);
            if (sj > start + kStartEpsilon)
            {
                end = sj;
                break;
            }
        }
    }
    if (end <= start)
        end = start + 2.5;
    return end;
}

// Connected components of overlapping lyric intervals. Rows in one component
// are sung together and stay lit until the next component starts.
std::vector<std::vector<int>> lyric_overlap_components(const Lyrics &lx)
{
    std::vector<std::vector<int>> components;
    std::vector<int> current;
    double component_start = 0.0;
    double component_end = 0.0;

    for (int i = 0; i < (int)lx.lines.size(); ++i)
    {
        const double start = lyric_start_time(lx, i);
        const double end = inferred_line_end(lx, i);
        if (start < 0.0 || end <= start)
            continue;

        if (current.empty())
        {
            current.push_back(i);
            component_start = start;
            component_end = end;
            continue;
        }

        const double overlap = std::min(component_end, end) - start;
        const double shorter = std::min(component_end - component_start,
                                        end - start);
        const bool same_voice = overlap >= kOverlapMinSec &&
                                overlap >= kOverlapMinRatio * shorter;
        if (same_voice)
        {
            current.push_back(i);
            component_end = std::max(component_end, end);
        }
        else
        {
            components.push_back(current);
            current.clear();
            current.push_back(i);
            component_start = start;
            component_end = end;
        }
    }
    if (!current.empty())
        components.push_back(current);
    return components;
}

std::vector<int> lyric_component_for_index(const Lyrics &lx, int index)
{
    for (const auto &component : lyric_overlap_components(lx))
        for (int i : component)
            if (i == index)
                return component;
    return std::vector<int>{index};
}

double effective_end_time(const Lyrics &lx, int i)
{
    const double start = lyric_start_time(lx, i);
    if (start < 0.0)
        return -1.0;

    // Keep the sweep alive through every row in the same singer group. The
    // next distinct component, not the next timestamp, ends this line.
    const std::vector<int> group = lyric_component_for_index(lx, i);
    double next = -1.0;
    for (int j = i + 1; j < (int)lx.lines.size(); ++j)
    {
        if (std::find(group.begin(), group.end(), j) != group.end())
            continue;
        const double sj = lyric_start_time(lx, j);
        if (sj > start)
        {
            next = sj;
            break;
        }
    }

    double end = inferred_line_end(lx, i);
    if (next > 0.0 && (end <= start || next > end))
        end = next;
    return end + lyric_vocal_guard(lx, i);
}

float line_karaoke_fraction(const Lyrics &lx, int i, double t)
{
    const double t0 = effective_start_time(lx, i);
    if (t0 < 0.0)
        return -1.0f;
    double t1 = effective_end_time(lx, i);
    if (t1 <= t0)
        t1 = t0 + 0.5;
    return clampf((float)((t - t0) / (t1 - t0)), 0.0f, 1.0f);
}

std::vector<int> immersive_active_group(const Lyrics &lx, const AppState &state,
                                        const MpvPlayer &player)
{
    std::vector<int> group;
    if (!lx.synced || lx.lines.empty() || lx.times.empty())
        return group;
    const double t = lyrics_playback_time(state, player) -
                     state.settings.lyrics_offset;

    int last = -1;
    for (int i = 0; i < (int)lx.lines.size(); ++i)
        if (lyric_start_time(lx, i) <= t)
            last = i;
    if (last < 0)
        return group;

    // The component owns the whole overlap interval, including the tail after
    // the shorter singer finishes, so both rows stay lit until the next
    // distinct line begins.
    return lyric_component_for_index(lx, last);
}

// Horizontal loading bar for the opening intro and long instrumental runs.
// Timestamps own the fill; live bass/guitar/treble levels only give the wave
// layers motion and weight. Each loader is consumed by lyric index so it never
// replays inside the same track.
struct EnergyLoader
{
    bool active = false;
    bool hit = false;
    bool intro = false;
    float progress = 0.0f;
    float flash = 0.0f;
    float seed = 0.0f;
    float phase = 0.0f;
    bool moving = false;
    float rms = 0.0f;
    float bass = 0.0f;
    float mid = 0.0f;
    float treble = 0.0f;
    float instrument = 0.0f;
    float density = 0.0f;
    float beat = 0.0f;
    float pluck = 0.0f;
};

EnergyLoader prepare_energy_loader(const AppState &state, MpvPlayer &player,
                                   const Lyrics &lx, const AudioReactive &au,
                                   std::vector<std::string> &energy_done,
                                   std::string &energy_active_key,
                                   double &energy_hit_start,
                                   double &energy_phase, double &energy_drive,
                                   double &energy_last_pos,
                                   std::uint64_t &energy_seek_gen,
                                   float &energy_held_rms,
                                   float &energy_held_bass,
                                   float &energy_held_mid,
                                   float &energy_held_treble,
                                   float &energy_held_instrument,
                                   double &energy_inst_ref,
                                   float &energy_held_density,
                                   float &energy_held_beat,
                                   float &energy_held_pluck, double now,
                                   bool motion)
{
    EnergyLoader out;
    const double raw_pos = lyrics_playback_time(state, player);
    const double t = raw_pos - state.settings.lyrics_offset;
    const double hit_window = 0.34;

    // A seek re-arms the intro/gap loader at its new position. Seeking into a
    // gap (or back into the intro) should show the effect again, not stay
    // hidden because that region played earlier in the same track.
    const std::uint64_t seek_gen = player.seek_generation();
    if (energy_seek_gen != seek_gen)
    {
        energy_seek_gen = seek_gen;
        energy_done.clear();
        energy_active_key.clear();
        energy_hit_start = -100.0;
    }

    // Advance the wave clock only when the song actually advances. A paused,
    // stopped, buffering or seeked player gives no usable delta, so the wave
    // simply holds its current shape instead of drifting on wall time.
    double dt = 0.0;
    if (energy_last_pos >= 0.0)
    {
        dt = raw_pos - energy_last_pos;
        if (dt < 0.0 || dt > 0.25)
            dt = 0.0;
    }
    energy_last_pos = raw_pos;
    out.moving = motion && dt > 0.0;

    if (out.moving)
    {
        // There is no tempo/FFT source here. Live sink bands act as the
        // "instrumental drive": bass hits still push the low layer, but the
        // guitar/mid band and its plucked onsets carry the same weight so a
        // quiet guitar intro does not look slower than a drum-heavy one.
        energy_held_rms = au.active ? clampf(au.rms, 0.0f, 1.0f) : 0.0f;
        energy_held_bass = au.active ? clampf(au.bass, 0.0f, 1.0f) : 0.0f;
        energy_held_mid = au.active ? clampf(au.mid, 0.0f, 1.0f) : 0.0f;
        energy_held_treble = au.active ? clampf(au.treble, 0.0f, 1.0f) : 0.0f;
        const float raw_instrument =
            au.active ? clampf(au.instrument, 0.0f, 1.0f) : 0.0f;
        // Peak reference: rises quickly to a new loud section, then falls
        // slowly so a quiet guitar passage still reads as active relative to
        // the song's own level. The floor stops silence from being amplified.
        if (raw_instrument > energy_inst_ref)
            energy_inst_ref +=
                (raw_instrument - energy_inst_ref) *
                clampf((float)dt * 1.2f, 0.0f, 1.0f);
        else
            energy_inst_ref =
                std::max(0.18, energy_inst_ref - dt * 0.035);
        const float norm_instrument = clampf(
            raw_instrument / std::max(0.18, energy_inst_ref), 0.0f, 1.0f);
        energy_held_instrument +=
            (norm_instrument - energy_held_instrument) *
            clampf((float)dt * 7.0f, 0.0f, 1.0f);
        const float raw_density =
            au.active ? clampf(au.density, 0.0f, 1.0f) : 0.0f;
        energy_held_density +=
            (raw_density - energy_held_density) *
            clampf((float)dt * 8.0f, 0.0f, 1.0f);
        energy_held_beat = au.active ? clampf(au.beat, 0.0f, 1.0f) : 0.0f;
        energy_held_pluck = au.active ? clampf(au.pluck, 0.0f, 1.0f) : 0.0f;
        // Overall loudness stays only a modest input. Without this, a loud
        // vocal/drum mix always outruns a quiet acoustic intro.
        const float target =
            clampf(0.42f + 0.65f * energy_held_rms + 0.45f * energy_held_beat +
                       0.28f * energy_held_bass + 0.18f * energy_held_mid +
                       1.65f * energy_held_instrument +
                       0.85f * energy_held_pluck + 0.20f * energy_held_treble,
                   0.32f, 2.90f);
        energy_drive +=
            (target - energy_drive) * clampf((float)dt * 9.0f, 0.0f, 1.0f);
        energy_phase += dt * energy_drive;
    }
    // Held levels keep every wave amplitude and glow frozen with the song.
    out.rms = energy_held_rms;
    out.bass = energy_held_bass;
    out.mid = energy_held_mid;
    out.treble = energy_held_treble;
    out.instrument = energy_held_instrument;
    out.density = energy_held_density;
    out.beat = energy_held_beat;
    out.pluck = energy_held_pluck;
    out.phase = (float)std::fmod(energy_phase, 100000.0);

    auto consumed = [&energy_done](const std::string &key) {
        return std::find(energy_done.begin(), energy_done.end(), key) !=
               energy_done.end();
    };
    auto finish = [&](const std::string &key) {
        if (!consumed(key))
            energy_done.push_back(key);
        if (energy_active_key == key)
        {
            energy_hit_start = now;
            energy_active_key.clear();
        }
    };

    std::string loader_key;
    double progress = -1.0;

    // Intro: playback 0 -> first non-blank lyric line. Even a short intro gets
    // a loader; only an immediate first word (t <= 0) skips it.
    const int first_real = next_real_index(lx, 0);
    if (first_real >= 0 && !consumed("i:" + std::to_string(first_real)))
    {
        const double end = effective_start_time(lx, first_real);
        if (end > 0.0)
        {
            const std::string key = "i:" + std::to_string(first_real);
            if (t >= end)
            {
                out.intro = true;
                finish(key);
            }
            else if (t >= 0.0)
            {
                out.active = true;
                out.intro = true;
                loader_key = key;
                progress = t / end;
            }
        }
    }

    // Instrumental runs: one loader for the whole blank run, starting at the
    // previous real line's end (or the first blank row when no end is cached),
    // and finishing at the next real line's timestamp.
    if (!out.active)
    {
        for (int i = 0; i < (int)lx.lines.size();)
        {
            if (!is_blank_lyric(lx.lines[(std::size_t)i]))
            {
                ++i;
                continue;
            }
            const int run_start = i;
            while (i < (int)lx.lines.size() &&
                   is_blank_lyric(lx.lines[(std::size_t)i]))
                ++i;
            const int next_real = i;
            if (run_start == 0 || next_real >= (int)lx.lines.size())
                continue;

            const double blank_start = effective_start_time(lx, run_start);
            const double prev_end = effective_end_time(lx, run_start - 1);
            double start = blank_start;
            if (prev_end > 0.0 && (start < 0.0 || prev_end <= start))
                start = prev_end;
            const double end = effective_start_time(lx, next_real);
            if (start < 0.0 || end <= start || end - start < 1.0)
                continue;

            const std::string key = "g:" + std::to_string(run_start);
            if (consumed(key))
                continue;
            if (t >= end)
            {
                // The next real line is now focal, so the finished gap sits
                // *above* it. Keeping the old below-focal anchor here is what
                // made the completion flash appear over the lyrics underneath.
                out.intro = true;
                finish(key);
                continue;
            }
            if (t >= start)
            {
                out.active = true;
                out.intro = false;
                loader_key = key;
                progress = (t - start) / (end - start);
                break;
            }
            // Still before this run: later runs cannot be current either.
            break;
        }
    }

    if (out.active)
        energy_active_key = loader_key;
    else if (now - energy_hit_start >= hit_window)
        energy_active_key.clear();

    const double hit_age = now - energy_hit_start;
    out.hit = hit_age >= 0.0 && hit_age < hit_window;
    out.progress = clampf((float)progress, 0.0f, 1.0f);
    out.flash = out.hit
                    ? clampf(1.0f - (float)(hit_age / hit_window), 0.0f, 1.0f)
                    : 0.0f;
    for (char c : loader_key)
        out.seed += (float)(unsigned char)c * 0.173f;
    out.seed = std::fmod(out.seed + 1.7f, 6.283f);
    return out;
}

void draw_energy_ribbon(ImDrawList *dl, const EnergyLoader &loader,
                        const ImVec2 &wp,
                        float left, float right, float center_y, float pitch,
                        float size, float view_a, int style, bool motion,
                        bool &animating)
{
    if (!dl || view_a <= 0.003f || left >= right ||
        (!loader.active && !loader.hit))
        return;

    const float x0 = wp.x + left;
    const float x1 = wp.x + right;
    const float span = x1 - x0;
    const float text_h = size * 1.26f;

    // The loader is much taller now, but capped by the space we reserved in
    // `pitch`: intro sits above the first line, gaps sit between two lines.
    const float wanted_h = energy_band_height(size, style);
    const float room_h = std::max(ui_px(24.0f), pitch - text_h - ui_px(14.0f));
    const float band_h = loader.intro ? wanted_h : std::min(wanted_h, room_h);
    const float half = band_h * 0.5f;
    const float rail_y = loader.intro
                             ? wp.y + center_y - text_h * 0.5f - half - ui_px(12.0f)
                             : wp.y + center_y + pitch * 0.5f;
    const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(ui_accent_u32(1.0f));

    // Smoothstep keeps the same timestamp-owned endpoints but gives the front
    // a softer launch and settle instead of a mechanical linear crawl.
    const float raw_p = clampf(loader.progress, 0.0f, 1.0f);
    const float p = raw_p * raw_p * (3.0f - 2.0f * raw_p);
    const float fx = x0 + span * p;
    const float energy = loader.rms;
    const float bass = loader.bass;
    const float mid = loader.mid;
    const float treble = loader.treble;
    const float instrument = loader.instrument;
    const float beat = loader.beat;
    const float pluck = loader.pluck;
    const float density = loader.density;
    const float phase = loader.phase;
    // Height comes from stable absolute loudness/density, not the per-track
    // normalised instrument level. That normaliser rises during a loader and
    // would otherwise make the waves shrink as the axis grows. Spectrum keeps
    // its own bar math.
    const float wave_level =
        clampf(std::max(energy * 1.6f, density * 1.15f), 0.0f, 1.0f);
    const float gate = smooth_unit((wave_level - 0.03f) / 0.22f);

    // Frosted-glass rail. It belongs to the live loader only: once the gap
    // finishes, the container must vanish instead of lingering over the next
    // lyric while the short NOW flash fades.
    if (loader.active)
    {
        const ImVec2 rail0(x0, rail_y - half * 1.12f);
        const ImVec2 rail1(x1, rail_y + half * 1.12f);
        const float rail_round = ui_px(std::max(6.0f, half * 0.42f));
        dl->AddRectFilled(rail0, rail1,
                          ui_with_alpha(ui_overlay_u32(1.0f), 0.052f * view_a),
                          rail_round);
        dl->AddRectFilled(rail0, rail1, ui_accent_u32(0.020f * view_a),
                          rail_round);
        dl->AddRect(rail0, rail1, ui_accent_u32(0.065f * view_a), rail_round,
                    ui_px(1.0f));
    }

    dl->PushClipRect(ImVec2(x0, rail_y - half * 1.34f),
                     ImVec2(x1, rail_y + half * 1.34f), true);

    if (loader.active)
    {
        // Faint track, solid filled portion. This is the unrolled loading
        // ring: everything alive is behind the travelling front.
        dl->AddLine(ImVec2(x0, rail_y), ImVec2(x1, rail_y),
                    ui_accent_u32(0.035f * view_a), ui_px(1.0f));
        add_gradient_rect(dl, x0, rail_y - half * 0.34f, fx,
                          rail_y + half * 0.34f,
                          ui_with_alpha(ui_accent_text_u32(),
                                        (0.048f + 0.022f * energy) * view_a),
                          ui_accent_u32(0.008f * view_a));

        // Short afterglow behind the front; it fades out over ~6% of the span
        // so the leading edge feels warm without smearing the whole track.
        const float tail = std::min(span * 0.07f, ui_px(90.0f));
        for (int i = 5; i >= 1; --i)
        {
            const float f = (float)i / 5.0f;
            const float ax = fx - tail * f;
            const float aw = tail * 0.28f;
            add_gradient_rect(dl, ax - aw, rail_y - half * 0.95f, ax + aw,
                              rail_y + half * 0.95f,
                              ui_with_alpha(ui_accent_text_u32(),
                                            (0.006f + 0.016f * (1.0f - f)) * view_a),
                              ui_accent_u32(0.0f));
        }

        soft_glow(dl, ImVec2(x1, rail_y), half * 1.45f, accent,
                  (0.022f + 0.028f * beat) * view_a, 4, 24);
        dl->AddLine(ImVec2(x1, rail_y - half * 0.92f),
                    ImVec2(x1, rail_y + half * 0.92f),
                    ui_accent_u32((0.11f + 0.06f * beat) * view_a), ui_px(1.4f));

        if (style == 1)
        {
            // Spectrum AM variant: bar heights follow the shared AM envelope,
            // so the silhouette reads like the "AM" carrier instead of a
            // generic EQ. Live bands only push the envelope up and down;
            // timing is still owned by the lyrics timestamps.
            const int bars = 28;
            const float step = span / (float)bars;
            const float baseline = rail_y + half * 0.98f;
            // The bars still jump, but the shape behind them drifts slowly.
            // Only the spectrum gets this slowed clock; Waves keeps its own.
            const float slow_phase = phase * 0.32f;
            float prev_amp = 0.0f;
            for (int i = 0; i < bars; ++i)
            {
                const float u = ((float)i + 0.5f) / (float)bars;
                if (u > p)
                    break;
                const float env = am_envelope(u, slow_phase, loader.seed);
                const float shimmer =
                    0.5f + 0.5f * std::sin(slow_phase * 0.9f + u * 19.0f +
                                           loader.seed);
                // Per-bar transient: spectrum should snap upward like a real
                // analyser, not sway like the wave ribbon. The golden-angle
                // offset keeps neighbouring bars from rising in lockstep, but
                // the clock is slowed so the backdrop does not strobe.
                const float jitter =
                    0.5f + 0.5f * std::sin(slow_phase * 1.7f +
                                           (float)i * 2.399f + loader.seed);
                const float spike =
                    std::pow(jitter, 6.0f) *
                    (0.22f + 0.24f * beat + 0.16f * treble +
                     0.26f * instrument + 0.34f * pluck);
                const float du = u - p;
                const float front = std::exp(-du * du * 150.0f);
                const float drive = 0.50f + 0.10f * bass + 0.14f * mid +
                                    0.10f * treble + 0.08f * beat +
                                    0.38f * instrument + 0.40f * pluck +
                                    0.08f * energy;
                const float raw = clampf(
                    0.06f + env * (drive + 0.08f * shimmer) + 0.22f * front +
                        spike,
                    0.04f, 1.60f);
                // Fast attack, almost no smoothing; peaks may pass the rail
                // limit before settling, so this stays visibly jumpy.
                const float amp = 0.92f * raw + 0.08f * prev_amp;
                prev_amp = amp;
                // Bars now grow from near the bottom of the taller rail and
                // can reach almost to its top, instead of occupying a shallow
                // strip around the centre line.
                const float bh = half * (0.08f + amp * 1.34f);
                const float bw = step * 0.30f;
                const float bx = x0 + u * span;
                const float top_y = baseline - bh;

                add_gradient_rect(dl, bx - bw, top_y, bx + bw, baseline,
                                  ui_with_alpha(ui_accent_text_u32(),
                                                (0.13f + 0.12f * amp +
                                                 0.10f * front +
                                                 0.08f * pluck) * view_a),
                                  ui_accent_u32((0.022f + 0.038f * amp) * view_a));
                dl->AddRectFilled(
                    ImVec2(bx - bw, top_y),
                    ImVec2(bx + bw, top_y + std::max(ui_px(2.0f), bh * 0.07f)),
                    ui_with_alpha(ui_accent_text_u32(),
                                  (0.30f + 0.16f * front) * view_a),
                    ui_px(1.2f));
                add_gradient_rect(dl, bx - bw, baseline, bx + bw,
                                  baseline + bh * 0.28f,
                                  ui_accent_u32((0.035f + 0.035f * amp) * view_a),
                                  ui_accent_u32(0.0f));
            }
            dl->AddLine(ImVec2(x0, baseline), ImVec2(x1, baseline),
                        ui_accent_u32(0.045f * view_a), ui_px(1.0f));
        }
        else
        {
            // Waves AM variant: a slim accent-tinted carrier rides the shared
            // amplitude envelope, with two quieter carrier layers behind it.
            // Loudness opens the waveform and waveform density sets how many
            // low-to-high points it draws; near silence collapses to a still
            // line instead of carrying on with a fixed decorative sine.
            const int samples = 192;
            ImVec2 spine[samples + 1];
            float thick[samples + 1];
            ImVec2 points[samples + 1];
            const float low = clampf(bass * 1.8f, 0.0f, 1.0f);
            const float middle = clampf(mid * 1.8f, 0.0f, 1.0f);
            const float high = clampf(treble * 1.8f, 0.0f, 1.0f);
            const float detail =
                clampf(0.42f * wave_level + 0.58f * density, 0.0f, 1.0f);
            // The carrier is drawn in a local 0..1 coordinate behind the
            // front dot, which stretches a fixed number of peaks as the bar
            // grows. Drive the peak count from the actual filled pixel width
            // instead, so a longer axis literally has more waves. Audio only
            // adds a modest density/bass multiplier on top of that width.
            const float filled_px = span * p;
            const float base_cycles = clampf(
                filled_px / std::max(1.0f, ui_px(34.0f)), 3.0f, 32.0f);
            const float audio_cycles =
                0.85f + 0.25f * detail + 0.30f * low + 0.25f * beat;
            const float carrier_scale =
                std::min(4.0f, base_cycles * audio_cycles / 6.4f);
            // Bring back a gentle bass-driven bounce: enough to feel alive,
            // far below the old kick/pluck spikes that made the line jump.
            const float wave_phase = phase * 0.30f;
            const float bounce = 0.10f * low + 0.12f * beat;
            const float amps[kWaveLayers] = {
                half * gate * (0.40f + 0.10f * low + 0.38f * wave_level + bounce),
                half * gate * (0.28f + 0.20f * middle + 0.36f * wave_level +
                               0.85f * bounce),
                half * gate * (0.22f + 0.16f * high + 0.34f * wave_level +
                               0.70f * bounce),
                half * gate * (0.16f + 0.24f * middle + 0.14f * high +
                               0.55f * bounce),
                half * gate * (0.10f + 0.20f * high + 0.16f * density +
                               0.40f * bounce)};

            // The front dot is the pen that draws the ribbon. Work in a
            // coordinate measured back from that dot so the AM shape stays
            // anchored to it instead of drifting along the absolute X axis.
            const float front_span = std::max(0.001f, p);
            const float front_pad = std::max(0.02f, p * 0.18f);
            const float front_carrier =
                am_carrier(0.0f, wave_phase, loader.seed, 0.0f,
                           carrier_scale);

            int count = 0;
            for (int s = 0; s <= samples; ++s)
            {
                const float u = (float)s / (float)samples;
                if (u > p)
                    break;
                const float local = (p - u) / front_span;
                const float anchor = smooth_unit(local / front_pad);
                const float left_fade =
                    smooth_unit(u / std::max(0.001f, p * 0.10f));
                const float breathe =
                    0.90f + 0.10f * std::sin(wave_phase * 0.70f +
                                             local * 1.6f + loader.seed);
                const float envelope = anchor * left_fade * breathe;
                // Subtracting the carrier at the front pins the line to the
                // circle's centre; the surrounding samples still move on Y.
                const float wave =
                    am_carrier(local, wave_phase, loader.seed, 0.0f,
                               carrier_scale) -
                    front_carrier;
                spine[count] = ImVec2(x0 + span * u,
                                      rail_y + wave * amps[0] * envelope);
                thick[count] =
                    half * gate *
                    (0.10f + 0.05f * low + 0.06f * wave_level) *
                    envelope;
                ++count;
            }

            for (int k = 1; k < count; ++k)
            {
                const float du = (float)k / (float)samples - p;
                const float front = std::exp(-du * du * 150.0f);
                add_gradient_quad(
                    dl,
                    ImVec2(spine[k - 1].x, spine[k - 1].y - thick[k - 1]),
                    ImVec2(spine[k].x, spine[k].y - thick[k]),
                    ImVec2(spine[k].x, spine[k].y + thick[k]),
                    ImVec2(spine[k - 1].x, spine[k - 1].y + thick[k - 1]),
                    ui_with_alpha(ui_accent_text_u32(),
                                  (0.10f + 0.09f * front +
                                   0.07f * wave_level) *
                                      view_a),
                    ui_accent_u32((0.008f + 0.018f * energy) * view_a));
            }

            // Mirrored, faint echo below the rail. It stays inside the clipped
            // band but gives the ribbon a soft reflection instead of a hard cut.
            const float axis = rail_y + half * 0.70f;
            for (int k = 1; k < count; ++k)
            {
                const float y0 = axis + (axis - spine[k - 1].y) * 0.42f;
                const float y1 = axis + (axis - spine[k].y) * 0.42f;
                const float t0 = thick[k - 1] * 0.42f;
                const float t1 = thick[k] * 0.42f;
                add_gradient_quad(
                    dl, ImVec2(spine[k - 1].x, y0 - t0),
                    ImVec2(spine[k].x, y1 - t1), ImVec2(spine[k].x, y1 + t1),
                    ImVec2(spine[k - 1].x, y0 + t0),
                    ui_accent_u32(0.026f * view_a), ui_accent_u32(0.0f));
            }

            if (count >= 2)
                dl->AddPolyline(spine, count,
                                ui_with_alpha(ui_accent_text_u32(),
                                              (0.12f + 0.12f * gate +
                                               0.10f * wave_level) *
                                                  view_a),
                                ui_px(1.8f));

            for (int layer = 1; layer < kWaveLayers; ++layer)
            {
                const float layer_front =
                    am_carrier(0.0f, wave_phase, loader.seed, (float)layer,
                               carrier_scale);
                int n = 0;
                for (int s = 0; s <= samples; ++s)
                {
                    const float u = (float)s / (float)samples;
                    if (u > p)
                        break;
                    const float local = (p - u) / front_span;
                    const float anchor = smooth_unit(local / front_pad);
                    const float left_fade =
                        smooth_unit(u / std::max(0.001f, p * 0.10f));
                    const float breathe =
                        0.90f + 0.10f * std::sin(wave_phase * 0.70f +
                                                 local * 1.6f + loader.seed +
                                                 (float)layer);
                    const float wave =
                        am_carrier(local, wave_phase, loader.seed, (float)layer,
                                   carrier_scale) -
                        layer_front;
                    points[n++] =
                        ImVec2(x0 + span * u,
                               rail_y + wave * amps[layer] * anchor *
                                            left_fade * breathe);
                }
                if (n >= 2)
                    dl->AddPolyline(points, n,
                                    ui_accent_u32((0.06f + 0.06f * gate -
                                                   0.015f * (float)layer +
                                                   0.07f * wave_level) *
                                      view_a),
                                    ui_px(1.3f));
            }
        }

        soft_glow(dl, ImVec2(fx, rail_y), half * 1.25f, accent,
                  (0.035f + 0.055f * gate + 0.035f * instrument +
                   0.045f * pluck + 0.045f * beat) * view_a, 4, 24);
        dl->AddLine(ImVec2(fx, rail_y - half * 0.88f),
                    ImVec2(fx, rail_y + half * 0.88f),
                    ui_with_alpha(ui_accent_text_u32(),
                                  (0.34f + 0.30f * beat) * view_a),
                    ui_px(1.8f));
        dl->AddCircleFilled(ImVec2(fx, rail_y),
                            ui_px(2.6f) + half * 0.045f,
                            ui_with_alpha(ui_accent_text_u32(), 0.88f * view_a), 14);
    }

    if (loader.hit)
    {
        soft_glow(dl, ImVec2(x1, rail_y), half * 1.75f, accent,
                  (0.16f * loader.flash + 0.05f * beat) * view_a, 4, 24);
        dl->AddCircle(
            ImVec2(x1, rail_y),
            half * (0.25f + 0.90f * (1.0f - loader.flash)),
            ui_with_alpha(ui_accent_text_u32(), 0.18f * loader.flash * view_a),
            32, ui_px(2.0f));
        dl->AddLine(ImVec2(x1, rail_y - half * 1.05f),
                    ImVec2(x1, rail_y + half * 1.05f),
                    ui_accent_u32((0.48f * loader.flash + 0.08f * beat) * view_a),
                    ui_px(2.4f));
        dl->AddCircleFilled(
            ImVec2(x1, rail_y), ui_px(2.8f) + half * 0.06f,
            ui_with_alpha(ui_accent_text_u32(), 0.95f * loader.flash * view_a), 14);
    }

    if (span > ui_px(240.0f))
    {
        const float label_s = clampf(size * 0.22f, 11.0f, 15.0f);
        const char *label = "NOW";
        const float label_w = measure(g_ui_fonts.body, label_s, label);
        dl->AddText(g_ui_fonts.body, label_s,
                    ImVec2(x1 - label_w - ui_px(6.0f),
                           rail_y - half * 1.18f),
                    ui_with_alpha(ui_accent_text_u32(),
                                  (0.14f + 0.40f * loader.flash +
                                   0.12f * beat) * view_a),
                    label);
    }

    dl->PopClipRect();

    if (motion && loader.moving)
        animating = true;
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
        g_imm.active_group.clear();
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
    const AppLayout l = ui_compute_layout(state, io);
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
        bool animating = false;

        // Closing fades the takeover back to the app instead of cutting. The
        // app flips `lyrics_immersive` off on Esc/close; re-opening before the
        // fade finishes cancels it.
        if (want && g_imm.exiting)
        {
            g_imm.exiting = false;
            g_imm.exit = 0.0f;
        }
        if (!want && !g_imm.exiting)
        {
            g_imm.exiting = true;
            g_imm.exit = 0.0f;
            g_imm.last_input = now;
        }
        if (g_imm.exiting)
        {
            g_imm.exit = motion ? std::min(1.0f, g_imm.exit + dt / kViewExit) : 1.0f;
            if (motion)
                animating = true;
        }
        const float exit_a = g_imm.exiting ? (1.0f - ease_out(g_imm.exit)) : 1.0f;

        const float pad = std::max(ui_px(24.0f), std::min(w, h) * 0.045f);
        const bool split = w >= 1080.0f && w > h * 0.90f;
        const float cover_col_w = split ? w * 0.40f : 0.0f;
        const float lyrics_x = split ? cover_col_w : 0.0f;
        const float lyrics_w = split ? w - cover_col_w : w;
        const float max_text_w = std::min(lyrics_w - pad * 2.0f, lyrics_w * 0.90f);
        const float focal_y = h * 0.455f;
        const ImVec2 focal(lyrics_x + lyrics_w * 0.5f, focal_y);

        // ---- Resolve the track + payload, exactly like the docked panel ----
        const Track display = lyrics_display_track(state);
        auto snap = std::atomic_load(&state.lyrics);
        const bool has_payload = snap && snap->track_id == display.id;
        const bool no_lyrics_only =
            has_payload && (snap->lyrics.none || snap->lyrics.lines.empty());

        if (g_imm.track != display.id)
        {
            const bool had_track = !g_imm.track.empty();
            g_imm.track = display.id;
            // Loading-bar consumption is per track; seeking inside a track
            // never re-arms an intro or instrumental run.
            g_imm.energy_done.clear();
            g_imm.energy_active_key.clear();
            g_imm.energy_hit_start = -100.0;
            g_imm.energy_pocket = 0.0f;
            g_imm.energy_seek_gen = player.seek_generation();
            g_imm.energy_phase = 0.0;
            g_imm.energy_drive = 0.55;
            g_imm.energy_last_pos = -1.0;
            g_imm.energy_held_rms = 0.0f;
            g_imm.energy_held_bass = 0.0f;
            g_imm.energy_held_mid = 0.0f;
            g_imm.energy_held_treble = 0.0f;
            g_imm.energy_held_instrument = 0.0f;
            g_imm.energy_inst_ref = 0.18;
            g_imm.energy_held_density = 0.0f;
            g_imm.energy_held_beat = 0.0f;
            g_imm.energy_held_pluck = 0.0f;
            if (had_track)
            {
                // Songs change while the takeover stays open. Keep the view
                // fully opaque; the cover slide and background crossfade use
                // the previous display snapshot below.
                g_imm.prev_display = g_imm.current_display;
                g_imm.prev_cover_x = g_imm.cover_x;
                g_imm.prev_cover_card = g_imm.cover_card;

                // If both tracks have no lyrics, both cards stay centred and
                // simply slide past each other. Only a centred card followed
                // by a lyrics track needs the staged left -> up -> in trip.
                bool next_known = false;
                bool next_has_lyrics = false;
                if (!display.id.empty())
                {
                    Lyrics cached;
                    if (load_lyrics_cache(display.id, cached))
                    {
                        next_known = true;
                        next_has_lyrics = cached.ok && !cached.none &&
                                          !cached.lines.empty();
                    }
                    else if (has_payload)
                    {
                        next_known = true;
                        next_has_lyrics = snap->lyrics.ok && !snap->lyrics.none &&
                                          !snap->lyrics.lines.empty();
                    }
                }
                const bool next_card = split && next_known && !next_has_lyrics;
                g_imm.switch_next_card = next_card;
                g_imm.switch_both_cards =
                    split && g_imm.prev_cover_card && next_card;
                g_imm.switch_from_center =
                    split && g_imm.prev_cover_card && !next_card;
                g_imm.enter = 1.0f;
                g_imm.switch_start = now;
                // The incoming cover normally arrives in the column first; a
                // track without lyrics glides to the centre once it lands. A
                // card-to-card change starts and stays centred.
                g_imm.cover_center = g_imm.switch_both_cards ? 1.0f : 0.0f;
                g_imm.center_goal = g_imm.cover_center;
            }
            else
            {
                g_imm.enter = motion ? 0.0f : 1.0f;
                // Opening directly on a track without lyrics: start centred
                // instead of sliding in from the empty column.
                g_imm.cover_center = (split && no_lyrics_only) ? 1.0f : 0.0f;
                g_imm.center_goal = g_imm.cover_center;
            }
            g_imm.cover_x = -1.0f;
            g_imm.view_start = now;
            g_imm.active = -2;
            g_imm.active_group.clear();
            g_imm.scroll = -1.0f;
            g_imm.pulse = 0.0f;
            g_imm.prev_text.clear();
            g_imm.prev_size = 0.0f;
            g_imm.prev_since = -100.0;
            g_imm.unsynced_scroll = 0.0f;
        }
        g_imm.current_display = display;

        if (motion && g_imm.enter < 1.0f)
        {
            g_imm.enter = std::min(1.0f, g_imm.enter + dt / kViewEnter);
            animating = true;
        }
        if (!motion)
            g_imm.enter = 1.0f;
        const float view_a = ease_out(g_imm.enter) * exit_a;
        // The lyrics payload can arrive a frame or two after the track id.
        // If a just-started centred-card exit turns out to lead to another
        // card, restart it as a centred conveyor before it has moved far.
        if (g_imm.switch_from_center && no_lyrics_only &&
            g_imm.switch_start > 0.0 && now - g_imm.switch_start < 0.12)
        {
            g_imm.switch_from_center = false;
            g_imm.switch_both_cards = true;
            g_imm.switch_next_card = true;
            g_imm.cover_center = 1.0f;
            g_imm.center_goal = 1.0f;
            g_imm.switch_start = now;
        }
        const double switch_age =
            g_imm.switch_start > 0.0 ? now - g_imm.switch_start : 1.0e9;
        double switch_total = kSwitchDur;
        float switch_p = 1.0f;
        float left_p = 1.0f;
        float out_p = 1.0f;
        float in_p = 1.0f;
        if (motion && switch_age >= 0.0)
        {
            if (g_imm.switch_from_center)
            {
                // Staged hand-off: back left, then up, then the next cover.
                switch_total = kCenterLeft + kCenterUp + kCenterIn;
                left_p = ease_in_out(clampf((float)(switch_age / kCenterLeft), 0.0f, 1.0f));
                out_p = ease_in_out(clampf(
                    (float)((switch_age - kCenterLeft) / kCenterUp), 0.0f, 1.0f));
                in_p = ease_in_out(clampf(
                    (float)((switch_age - kCenterLeft - kCenterUp) / kCenterIn),
                    0.0f, 1.0f));
            }
            else
            {
                // Column-to-column keeps the single conveyor: the old cover
                // up and the new one in from below at the same time.
                switch_total = kSwitchDur;
                out_p = in_p = ease_in_out(clampf(
                    (float)(switch_age / kSwitchDur), 0.0f, 1.0f));
            }
            switch_p = ease_in_out(clampf((float)(switch_age / switch_total), 0.0f, 1.0f));
            if (switch_age < switch_total + kLyricReveal + kLyricStagger * 8.0)
                animating = true;
        }
        const bool transitioning = switch_age < switch_total &&
                                   !g_imm.prev_display.id.empty();

        // The cache hint is only needed until the real payload arrives. If the
        // payload has lyrics, the incoming cover belongs in the column.
        if (has_payload && !no_lyrics_only)
            g_imm.switch_next_card = false;

        // Lyrics wait for the covers to finish their trip. Starting the line
        // fade during the slide painted text over the artwork.
        const bool lyric_intro = motion && switch_age >= 0.0 &&
                                 switch_age < switch_total + kLyricReveal +
                                                    kLyricStagger * 8.0;

        // A track with no lyrics shows the centred cover card. Move there only
        // after the incoming cover has finished sliding up, so the hand-off
        // reads as "slide in, then settle in the middle".
        const float center_target =
            (split && (no_lyrics_only || g_imm.switch_next_card)) ? 1.0f : 0.0f;
        if (!split)
        {
            g_imm.cover_center = 0.0f;
            g_imm.center_goal = 0.0f;
        }
        else if (!motion)
        {
            g_imm.cover_center = center_target;
            g_imm.center_goal = center_target;
        }
        else if (!transitioning || in_p > 0.90f)
        {
            if (center_target != g_imm.center_goal)
            {
                // Re-anchor so a target change mid-glide curves from wherever
                // the cover is now instead of snapping.
                g_imm.center_goal = center_target;
                g_imm.center_from = g_imm.cover_center;
                g_imm.center_start = now;
            }
            const double age = now - g_imm.center_start;
            const double glide = 0.55;
            if (age >= glide)
                g_imm.cover_center = center_target;
            else if (age >= 0.0)
            {
                g_imm.cover_center =
                    g_imm.center_from +
                    (center_target - g_imm.center_from) *
                        ease_in_out((float)(age / glide));
                animating = true;
            }
        }

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

        // The real backdrop shader is painted below ImGui, so a transparent
        // takeover would show the app's panels underneath. Sample that same
        // backdrop on a coarse grid and fill it opaquely with interpolated
        // quads: smooth like the shader, but covering everything.
        if (ui_backdrop_visibility() > 0.001f)
        {
            auto opaque = [view_a](const ImVec2 &p) {
                ImVec4 c = ui_backdrop_sample(p);
                c.w = view_a; // opaque at rest, fades with the takeover
                return ui_col(c);
            };
            const int cols = 14, rows = 9;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                {
                    const ImVec2 p0(wp.x + w * (float)c / (float)cols,
                                    wp.y + h * (float)r / (float)rows);
                    const ImVec2 p1(wp.x + w * (float)(c + 1) / (float)cols,
                                    wp.y + h * (float)(r + 1) / (float)rows);
                    dl->AddRectFilledMultiColor(p0, p1, opaque(p0),
                                                opaque(ImVec2(p1.x, p0.y)),
                                                opaque(p1),
                                                opaque(ImVec2(p0.x, p1.y)));
                }
        }
        else
            dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h),
                              ui_with_alpha(ui_col(ui_c().bg), view_a));

        // Cover art, blurred and held well back so the lyrics stay the focal
        // point. This uses the full-screen (512px) blur rather than the hero
        // card's 128px one, which looks blocky when stretched over a window.
        if (!display.thumbnail_url.empty())
        {
            const std::string hi = texture_highres_url(display.thumbnail_url, display.id);
            ui_immersive_blur_request(hi);
            GLuint blur = ui_immersive_blur_texture(hi);
            int bw = 0, bh = 0;
            bool have_blur = blur != 0 && ui_immersive_blur_size(hi, bw, bh);
            if (!have_blur && hi != display.thumbnail_url)
            {
                ui_immersive_blur_request(display.thumbnail_url);
                blur = ui_immersive_blur_texture(display.thumbnail_url);
                have_blur = blur != 0 && ui_immersive_blur_size(display.thumbnail_url, bw, bh);
            }
            if (have_blur)
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
                    const float a = (0.34f - 0.14f * day) * (0.85f + 0.35f * energy) *
                                    view_a * (transitioning ? switch_p : 1.0f);
                    dl->AddImage((ImTextureID)(intptr_t)blur, wp,
                                 ImVec2(wp.x + w, wp.y + h),
                                 ImVec2(cx - hw + drift, cy - hh),
                                 ImVec2(cx + hw + drift, cy + hh),
                                 IM_COL32(255, 255, 255,
                                          (int)(clampf(a, 0.0f, 1.0f) * 255.0f)));
                }
            }
        }

        // Morph the backdrop: the outgoing track's blurred cover fades over
        // the incoming one during the cover slide.
        if (transitioning && !g_imm.prev_display.thumbnail_url.empty())
        {
            const std::string ohi = texture_highres_url(
                g_imm.prev_display.thumbnail_url, g_imm.prev_display.id);
            ui_immersive_blur_request(ohi);
            GLuint oblur = ui_immersive_blur_texture(ohi);
            int ow = 0, oh = 0;
            bool have_old = oblur != 0 && ui_immersive_blur_size(ohi, ow, oh);
            if (!have_old && ohi != g_imm.prev_display.thumbnail_url)
            {
                ui_immersive_blur_request(g_imm.prev_display.thumbnail_url);
                oblur = ui_immersive_blur_texture(g_imm.prev_display.thumbnail_url);
                have_old = oblur != 0 &&
                           ui_immersive_blur_size(g_imm.prev_display.thumbnail_url, ow, oh);
            }
            if (have_old)
            {
                float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
                const TextureInfo info{oblur, ow, oh};
                if (cover_uv(info, w, h, u0, v0, u1, v1))
                {
                    const float a = (0.34f - 0.14f * day) * (1.0f - switch_p) * view_a;
                    dl->AddImage((ImTextureID)(intptr_t)oblur, wp,
                                 ImVec2(wp.x + w, wp.y + h), ImVec2(u0, v0),
                                 ImVec2(u1, v1),
                                 IM_COL32(255, 255, 255,
                                          (int)(clampf(a, 0.0f, 1.0f) * 255.0f)));
                }
            }
        }

        // Scrim plus a theme/cover colour blend: keeps the takeover readable in
        // both day and night themes while making the artwork feel like part of
        // the existing palette rather than a separate layer.
        dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h),
                          ui_with_alpha(ui_col(ui_c().bg),
                                        (0.52f + 0.14f * day) * view_a));
        dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h),
                          ui_with_alpha(ui_col(mix4(tint, accent, 0.25f)),
                                        0.10f * (1.0f - 0.45f * day) * view_a));

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

        // ---- Covers ----
        // Split layouts keep the sharp cover in the left column; a track with
        // no lyrics uses the larger card and glides from that column to the
        // centre of the window. Narrow layouts only draw the card while there
        // are no lyrics to show.
        const float column_cx = cover_col_w * 0.5f;
        const float centre_cx = w * 0.5f;
        const float cover_x =
            split ? column_cx + (centre_cx - column_cx) * clampf(g_imm.cover_center, 0.0f, 1.0f)
                  : centre_cx;
        const bool cover_card = no_lyrics_only || g_imm.switch_next_card || !split;
        const bool cover_visible =
            !display.id.empty() &&
            (split || no_lyrics_only || g_imm.switch_next_card || !has_payload ||
             !snap->lyrics.ok);
        auto draw_cover_at = [&](const Track &t, bool card, float cx, float alpha,
                                 float y_shift) {
            if (card)
                draw_cover_card(dl, t, ImVec2(cx, focal_y), w, h, alpha, y_shift);
            else
                draw_cover_column(dl, t, ImVec2(cx, h * 0.43f), cover_col_w, h, alpha,
                                  y_shift);
        };
        if (cover_visible)
        {
            if (transitioning)
            {
                // No opacity fade: the two covers travel as one vertical strip.
                // The outgoing card leaves the top, the incoming one arrives
                // from the bottom into exactly the same place.
                const float old_x = g_imm.prev_cover_x >= 0.0f ? g_imm.prev_cover_x
                                                               : column_cx;
                // A centred card first travels back to the column while it is
                // still fully visible, then exits upward. The incoming cover
                // waits below until that exit has finished.
                const float exit_x =
                    (g_imm.switch_from_center && split)
                        ? old_x + (column_cx - old_x) * left_p
                        : old_x;
                draw_cover_at(g_imm.prev_display, g_imm.prev_cover_card, exit_x,
                              view_a, -h * out_p);
                draw_cover_at(display, cover_card, cover_x, view_a,
                              h * (1.0f - in_p));
            }
            else
                draw_cover_at(display, cover_card, cover_x, view_a, 0.0f);
            g_imm.cover_x = cover_x;
            g_imm.cover_card = cover_card;
        }

        // ---- Lyrics ----
        ImFont *font = g_ui_fonts.semibold;
        const float active_pref = clampf(h * 0.075f * m.text_scale, 30.0f,
                                         h * 0.16f);

        bool show_message = false;
        bool draw_cover = false;
        std::string message;
        std::string note;

        if (display.id.empty())
        {
            show_message = true;
            message = "Nothing playing.";
        }
        else if (!has_payload)
        {
            draw_cover = true;
            if (state.lyrics_loading.load())
                note = "Loading lyrics";
        }
        else if (!snap->lyrics.ok)
        {
            draw_cover = true;
            note = "Couldn't load lyrics";
        }
        else if (snap->lyrics.none || snap->lyrics.lines.empty())
        {
            draw_cover = true;
        }

        const Lyrics *lx = has_payload ? &snap->lyrics : nullptr;

        if (draw_cover)
        {
            const bool centered_cover = !split || no_lyrics_only;
            const float note_x = centered_cover ? cover_x : focal.x;
            if (!note.empty())
            {
                const float ns = std::max(12.0f, std::min(16.0f, h * 0.016f));
                const float nw = measure(g_ui_fonts.body, ns, note);
                dl->AddText(g_ui_fonts.body, ns,
                            ImVec2(note_x - nw * 0.5f,
                                   focal.y +
                                       (centered_cover ? std::min(w, h) * 0.33f : 0.0f)),
                            ui_with_alpha(ui_text_dim_u32(), 0.62f * view_a), note.c_str());
            }
            if (state.lyrics_loading.load() && motion)
            {
                const float dot_y = (split && !no_lyrics_only)
                                        ? focal.y + ui_px(32.0f)
                                        : focal.y + std::min(w, h) * 0.37f;
                for (int i = 0; i < 3; ++i)
                {
                    const float ph = (float)now * 2.6f - (float)i * 0.6f;
                    const float a = 0.20f + 0.50f * (0.5f + 0.5f * std::sin(ph));
                    const float dx = ((float)i - 1.0f) * ui_px(14.0f);
                    dl->AddCircleFilled(ImVec2(note_x + dx, dot_y + std::sin(ph) * ui_px(2.0f)),
                                        ui_px(3.2f), ui_accent_u32(a * view_a), 14);
                }
                animating = true;
            }
        }
        else if (show_message)
        {
            TextLayout msg = layout_text(font, message, max_text_w * 0.8f,
                                         active_pref * 0.46f, 20.0f);
            draw_rows(dl, font, msg, focal,
                      ui_text_dim_u32(), 0.85f * view_a, 1.0f, 0.0f);
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
            const float max_scroll = overflow ? std::max(0.0f, total_h - avail_h) : 0.0f;
            g_imm.unsynced_scroll = clampf(g_imm.unsynced_scroll, 0.0f, max_scroll);
            // Unsynced lyrics are read at the listener's pace. No timer scroll:
            // the wheel moves the block, and a change of track resets it.
            if (overflow && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                io.MouseWheel != 0.0f)
            {
                g_imm.unsynced_scroll = clampf(
                    g_imm.unsynced_scroll - io.MouseWheel * ui_px(64.0f), 0.0f,
                    max_scroll);
                animating = true;
            }
            const float scroll = g_imm.unsynced_scroll;

            float y = overflow ? (pad - scroll) : (h - total_h) * 0.5f;
            const float edge = std::max(ui_px(42.0f), h * 0.09f);
            const float top = pad;
            const float bottom = h - pad;

            const float intro =
                lyric_intro
                    ? ease_out(clampf((float)((switch_age - switch_total) / kLyricReveal),
                                      0.0f, 1.0f))
                    : 1.0f;
            dl->PushClipRect(ImVec2(wp.x, wp.y + top), ImVec2(wp.x + w, wp.y + bottom), true);
            for (const TextRow &row : rows)
            {
                float a = view_a * 0.86f * intro;
                if (y < top + edge)
                    a *= clampf((y - top) / edge, 0.0f, 1.0f);
                if (y + line_h > bottom - edge)
                    a *= clampf((bottom - (y + line_h)) / edge, 0.0f, 1.0f);
                if (a > 0.001f && !row.text.empty())
                {
                    const float tw = measure(font, size, row.text);
                    dl->AddText(font, size, ImVec2(focal.x - tw * 0.5f, y),
                                ui_with_alpha(ui_text_dim_u32(), a), row.text.c_str());
                }
                y += line_h;
            }
            dl->PopClipRect();
        }
        else
        {
            // ---- Synced: continuous scroll through the lyric lines ----
            // Each line is drawn at its distance from the eased scroll
            // position, so the next line glides up and grows into focus
            // instead of being swapped in from a corner.
            const std::vector<int> active_group =
                immersive_active_group(*lx, state, player);
            const int active = active_group.empty() ? -1 : active_group.front();
            if (g_imm.active == -2)
            {
                g_imm.active = active;
                g_imm.active_group = active_group;
                g_imm.scroll = active >= 0 ? (float)active : 0.0f;
                g_imm.active_since = now;
                if (active >= 0)
                    g_imm.pulse = motion ? 1.0f : 0.0f;
            }
            else if (active_group != g_imm.active_group)
            {
                g_imm.active = active;
                g_imm.active_group = active_group;
                g_imm.active_since = now;
                if (motion && active >= 0)
                    g_imm.pulse = 1.0f;
            }

            const bool preview = active_group.empty();
            int center_idx = active;
            if (center_idx < 0 || center_idx >= (int)lx->lines.size() ||
                blank(lx->lines[center_idx]))
                center_idx = next_nonempty_index(*lx, center_idx < 0 ? 0 : center_idx);
            auto is_active_line = [&active_group](int i) {
                return std::find(active_group.begin(), active_group.end(), i) !=
                       active_group.end();
            };

            if (center_idx >= 0)
            {
                // Ease toward the active line (~300 ms feel). A seek that jumps
                // several lines scrolls through them instead of cutting.
                if (!motion || g_imm.scroll < 0.0f)
                    g_imm.scroll = (float)center_idx;
                else
                {
                    const float k = 1.0f - std::exp(-7.5f * dt);
                    const float before = g_imm.scroll;
                    g_imm.scroll += ((float)center_idx - g_imm.scroll) * k;
                    if (std::fabs((float)center_idx - g_imm.scroll) < 0.002f)
                        g_imm.scroll = (float)center_idx;
                    if (std::fabs(g_imm.scroll - before) > 0.0004f)
                        animating = true;
                }

                // Every lyric line keeps one font size. The active line is
                // marked by colour and by the growing letter at the karaoke
                // front, not by zooming neighbours in and out.
                float uniform_size = active_pref;
                const float uniform_floor = std::max(18.0f, active_pref * 0.5f);
                {
                    const float step = std::max(1.0f, active_pref * 0.035f);
                    while (uniform_size > uniform_floor)
                    {
                        float widest = 0.0f;
                        for (const std::string &ln : lx->lines)
                            if (!ln.empty())
                                widest = std::max(widest,
                                                  measure(font, uniform_size, ln));
                        if (widest <= max_text_w)
                            break;
                        uniform_size = std::max(uniform_floor, uniform_size - step);
                    }
                }

                // Translation for the focal line. It sits under the active
                // lyric in a frosted-glass pill and deliberately has no
                // karaoke/glow pass of its own. Reserve its height in the
                // scroll pitch so it never collides with the next line.
                const bool has_translation =
                    !preview && center_idx < (int)lx->rich.size() &&
                    !lx->rich[(std::size_t)center_idx].translation.empty();
                TextLayout trans_lay;
                float trans_gap = 0.0f;
                float trans_pill_h = 0.0f;
                if (has_translation)
                {
                    const float trans_size =
                        clampf(uniform_size * 0.42f, 14.0f, 30.0f);
                    const float trans_max_w = std::max(80.0f, max_text_w * 0.78f);
                    trans_lay = layout_text(g_ui_fonts.body,
                                            lx->rich[(std::size_t)center_idx].translation,
                                            trans_max_w, trans_size,
                                            std::max(12.0f, trans_size * 0.85f));
                    trans_gap = ui_px(10.0f);
                    trans_pill_h = trans_lay.height + ui_px(18.0f);
                }

                const EnergyLoader energy = prepare_energy_loader(
                    state, player, *lx, au, g_imm.energy_done,
                    g_imm.energy_active_key, g_imm.energy_hit_start,
                    g_imm.energy_phase, g_imm.energy_drive,
                    g_imm.energy_last_pos, g_imm.energy_seek_gen,
                    g_imm.energy_held_rms,
                    g_imm.energy_held_bass, g_imm.energy_held_mid,
                    g_imm.energy_held_treble, g_imm.energy_held_instrument,
                    g_imm.energy_inst_ref, g_imm.energy_held_density,
                    g_imm.energy_held_beat, g_imm.energy_held_pluck,
                    now, motion);

                // Style 2 turns the whole intro/gap effect off: no rail, no
                // animation, and no extra row spacing for a hidden loader.
                const bool effects_enabled =
                    state.settings.lyrics_effect_style != 2;
                const bool want_pocket =
                    effects_enabled && (energy.active || energy.hit);
                if (!effects_enabled)
                    g_imm.energy_pocket = 0.0f;
                if (want_pocket)
                    g_imm.energy_pocket = 1.0f;
                else if (!motion)
                    g_imm.energy_pocket = 0.0f;
                else if (g_imm.energy_pocket > 0.0f)
                {
                    // Let the row spacing settle after the completion flash
                    // instead of snapping shut the instant the loader exits.
                    g_imm.energy_pocket =
                        std::max(0.0f, g_imm.energy_pocket - dt / 0.20f);
                    animating = true;
                }

                float pitch = uniform_size * 1.92f;
                if (g_imm.energy_pocket > 0.0f)
                {
                    // Open a real pocket between lyric rows so the taller
                    // loader never sits on top of the text. The spectrum's
                    // taller band is reserved here as well, not just drawn.
                    const float wanted_h = energy_band_height(
                        uniform_size, state.settings.lyrics_effect_style);
                    const float full_pitch =
                        std::max(pitch, wanted_h + uniform_size * 1.26f +
                                            ui_px(22.0f));
                    pitch += (full_pitch - pitch) *
                             smooth_unit(g_imm.energy_pocket);
                }
                if (has_translation)
                {
                    // Reserve a real gap below the glass card. Measuring the
                    // focal line instead of assuming one row also covers a
                    // wrapped translation/original pair.
                    float active_h = uniform_size * 1.26f;
                    if (center_idx >= 0 &&
                        center_idx < (int)lx->lines.size())
                    {
                        const TextLayout active_lay =
                            layout_text(font, lx->lines[(std::size_t)center_idx],
                                        max_text_w, uniform_size, uniform_size);
                        active_h = active_lay.height;
                    }
                    const float min_pitch =
                        active_h * 0.5f + trans_gap + trans_pill_h +
                        ui_px(30.0f) + uniform_size * 0.5f;
                    pitch = std::max(pitch, min_pitch);
                }

                // Drawn before the glyphs and translation card, inside the
                // pitch pocket reserved above. "Off" skips it entirely.
                if (effects_enabled)
                    draw_energy_ribbon(dl, energy, wp,
                                       lyrics_x + pad * 0.45f, w - pad * 0.65f,
                                       focal.y, pitch, uniform_size, view_a,
                                       state.settings.lyrics_effect_style,
                                       motion, animating);

                const double tplay = lyrics_playback_time(state, player) -
                                     state.settings.lyrics_offset;
                const int lo = std::max(0, (int)std::floor(g_imm.scroll) - 3);
                const int hi = std::min((int)lx->lines.size() - 1,
                                        (int)std::ceil(g_imm.scroll) + 3);
                float active_line_h = 0.0f;
                float active_line_center_y = 0.0f;
                float active_line_alpha = 0.0f;
                for (int i = lo; i <= hi; ++i)
                {
                    if (blank(lx->lines[i]))
                        continue;
                    const float d = (float)i - g_imm.scroll;
                    // Opacity still falls off with distance, but the font
                    // size is fixed so a line never looks pre-highlighted
                    // just because it is sliding toward the centre.
                    const float focus = clampf(1.0f - std::fabs(d) * 0.55f, 0.0f, 1.0f);
                    const float f2 = focus * focus;
                    float alpha =
                        (preview ? 0.60f : 1.0f) * (0.08f + 0.92f * f2) * view_a;
                    float reveal = 1.0f;
                    if (lyric_intro)
                    {
                        const int order = std::max(0, i - center_idx);
                        reveal = ease_out(clampf(
                            (float)((switch_age - switch_total -
                                     (double)order * kLyricStagger) /
                                    kLyricReveal),
                            0.0f, 1.0f));
                        alpha *= reveal;
                    }
                    // Every row in the simultaneous group is fully active:
                    // providers use adjacent/equal timestamps for two singers,
                    // and both need the accent sweep instead of only the top
                    // row being lit.
                    const bool is_active = !preview && is_active_line(i);
                    if (is_active)
                        alpha = std::max(alpha, view_a * reveal);
                    TextLayout lay = layout_text(font, lx->lines[i], max_text_w,
                                                 uniform_size, uniform_size);
                    const float size = lay.size;
                    bool drew_words = false;
                    const ImVec2 line_center(focal.x,
                                             focal.y + d * pitch +
                                                 (1.0f - reveal) * ui_px(16.0f));
                    if (is_active)
                    {
                        // A soft accent halo makes the whole simultaneous row
                        // read as "being sung", even before its sweep arrives.
                        // Non-focal singers get a slightly stronger halo so a
                        // line below the focal one does not disappear into the
                        // normal distance fade.
                        const ImU32 glow_col = ui_accent_text_u32();
                        const float o = ui_px(1.8f) + 1.6f * g_imm.pulse;
                        const float ga = (i == center_idx ? 0.05f : 0.10f) *
                                         view_a * reveal;
                        float gy = line_center.y - lay.height * 0.5f;
                        for (const TextRow &grow : lay.rows)
                        {
                            if (grow.text.empty())
                            {
                                gy += lay.line_h;
                                continue;
                            }
                            const float gw = measure(font, size, grow.text);
                            const float gx = focal.x - gw * 0.5f;
                            dl->AddText(font, size, ImVec2(gx - o, gy),
                                        ui_with_alpha(glow_col, ga), grow.text.c_str());
                            dl->AddText(font, size, ImVec2(gx + o, gy),
                                        ui_with_alpha(glow_col, ga), grow.text.c_str());
                            dl->AddText(font, size, ImVec2(gx, gy - o),
                                        ui_with_alpha(glow_col, ga), grow.text.c_str());
                            dl->AddText(font, size, ImVec2(gx, gy + o),
                                        ui_with_alpha(glow_col, ga), grow.text.c_str());
                            gy += lay.line_h;
                        }
                    }
                    if (i == center_idx)
                    {
                        active_line_h = lay.height;
                        active_line_center_y = line_center.y;
                        active_line_alpha = alpha;
                    }
                    float karaoke = -1.0f;
                    if (is_active)
                    {
                        karaoke = line_karaoke_fraction(*lx, i, tplay);
                        if (state.playing && karaoke >= 0.0f && karaoke < 1.0f)
                            animating = true;
                    }
                    if (is_active && i < (int)lx->rich.size())
                        drew_words = draw_words_app(
                            dl, font, size, line_center, max_text_w, lx->rich[(size_t)i],
                            tplay, effective_start_time(*lx, i),
                            effective_end_time(*lx, i), alpha);
                    if (!drew_words)
                        draw_rows(dl, font, lay, line_center,
                                  is_active ? ui_accent_text_u32() : ui_text_dim_u32(),
                                  alpha, 1.0f,
                                  is_active ? (0.22f + 0.38f * g_imm.pulse) : 0.0f,
                                  karaoke, is_active ? (i == center_idx ? 0.45f : 1.0f) : 0.0f);
                }

                // Liquid-glass translation card. It is static: only the
                // original lyric receives the per-letter karaoke sweep.
                if (has_translation && active_line_h > 0.0f)
                {
                    float trans_w = 0.0f;
                    for (const TextRow &row : trans_lay.rows)
                        trans_w = std::max(trans_w,
                                           measure(g_ui_fonts.body, trans_lay.size, row.text));
                    const float pad_x = ui_px(20.0f);
                    const float pad_y = ui_px(9.0f);
                    const float pill_w =
                        std::min(max_text_w * 0.92f,
                                 std::max(ui_px(120.0f), trans_w + pad_x * 2.0f));
                    const float pill_h = trans_lay.height + pad_y * 2.0f;
                    const ImVec2 p0(focal.x - pill_w * 0.5f,
                                    active_line_center_y + active_line_h * 0.5f + trans_gap);
                    const ImVec2 p1(p0.x + pill_w, p0.y + pill_h);
                    const float rounding = pill_h * 0.5f;
                    const float ga = clampf(active_line_alpha, 0.0f, 1.0f);

                    // Soft drop shadow, translucent theme wash, and a thin
                    // accent rim. The white top edge is the "glass" catch.
                    dl->AddRectFilled(ImVec2(p0.x + ui_px(2.0f), p0.y + ui_px(4.0f)),
                                      ImVec2(p1.x + ui_px(2.0f), p1.y + ui_px(4.0f)),
                                      ui_with_alpha(IM_COL32(0, 0, 0, 255), 0.16f * ga),
                                      rounding);
                    dl->AddRectFilled(p0, p1,
                                      ui_with_alpha(ui_overlay_u32(1.0f), 0.075f * ga),
                                      rounding);
                    dl->AddRectFilled(p0, p1,
                                      ui_with_alpha(ui_accent_u32(1.0f), 0.045f * ga),
                                      rounding);
                    dl->AddRect(p0, p1,
                                ui_with_alpha(ui_accent_text_u32(), 0.16f * ga),
                                rounding, ImDrawFlags_None, ui_px(1.0f));
                    dl->AddRectFilled(
                        ImVec2(p0.x + rounding * 0.55f, p0.y + ui_px(1.0f)),
                        ImVec2(p1.x - rounding * 0.55f, p0.y + ui_px(1.6f)),
                        ui_with_alpha(IM_COL32(255, 255, 255, 255),
                                      (ui_theme_is_light() ? 0.55f : 0.12f) * ga),
                        ui_px(0.6f));

                    float ty = p0.y + pad_y;
                    for (const TextRow &row : trans_lay.rows)
                    {
                        const float rw =
                            measure(g_ui_fonts.body, trans_lay.size, row.text);
                        dl->AddText(g_ui_fonts.body, trans_lay.size,
                                    ImVec2(focal.x - rw * 0.5f, ty),
                                    ui_with_alpha(ui_text_u32(), 0.86f * ga),
                                    row.text.c_str());
                        ty += trans_lay.line_h;
                    }
                }
            }
        }

        // Narrow layouts put the lyrics across the whole window, so the cover
        // column (and its title/artist) is not there. Keep a quiet caption at
        // the bottom that hands over to the chrome when it appears.
        if (!split && !draw_cover && !show_message && !display.id.empty())
        {
            const float cap_a = (1.0f - chrome_a) * view_a * 0.88f *
                                (transitioning ? switch_p : 1.0f);
            if (cap_a > 0.003f)
            {
                const float title_s = std::max(15.0f, std::min(20.0f, h * 0.021f));
                const float artist_s = std::max(12.0f, title_s * 0.72f);
                const std::string title = ui_ellipsize(
                    g_ui_fonts.semibold, title_s,
                    display.title.empty() ? "Nothing playing" : display.title,
                    w - pad * 2.0f);
                const std::string artist = ui_ellipsize(
                    g_ui_fonts.body, artist_s, display.artist, w - pad * 2.0f);
                const float ty = h - pad - l.bottom_safe - ui_px(40.0f);
                const float tw = measure(g_ui_fonts.semibold, title_s, title);
                dl->AddText(g_ui_fonts.semibold, title_s,
                            ImVec2((w - tw) * 0.5f, ty),
                            ui_with_alpha(ui_text_u32(), 0.82f * cap_a), title.c_str());
                if (!artist.empty())
                {
                    const float aw = measure(g_ui_fonts.body, artist_s, artist);
                    dl->AddText(g_ui_fonts.body, artist_s,
                                ImVec2((w - aw) * 0.5f, ty + title_s * 1.32f),
                                ui_with_alpha(ui_text_dim_u32(), 0.72f * cap_a),
                                artist.c_str());
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
            // Same dock safe band as the main bar: without this the immersive
            // transport and seek bar land inside the shell's bottom hot zone
            // and every click goes to the desktop instead of the app.
            const float bar_y = h - pad - ui_px(12.0f) - l.bottom_safe;
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
            const float y = h - pad - ui_px(106.0f) - l.bottom_safe;
            const std::string hint = "Esc to exit";
            const float tw = measure(g_ui_fonts.body, 13.0f * m.text_scale, hint);
            dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale,
                        ImVec2((w - tw) * 0.5f, y),
                        ui_with_alpha(ui_text_muted_u32(), a * 0.9f), hint.c_str());
            animating = true;
        }

        // ---- Close / exit ----
        if (g_imm.exiting && g_imm.exit >= 1.0f)
        {
            ImGui::CloseCurrentPopup();
            s_open = false;
            state.lyrics_immersive = false;
            ui_audio_stop();
            g_imm.exiting = false;
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
