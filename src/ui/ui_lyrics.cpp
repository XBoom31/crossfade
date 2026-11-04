#include "ui.h"
#include "ui_backdrop.h"
#include "mpvplayer.h"
#include "texture.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cfloat>

// ----------------------------------------------------------------------
// Lyrics side panel. Shown next to the track list on the list tabs when
// the bottom-bar toggle is on. The payload is published atomically by the
// fetch worker (see load_lyrics_worker in main.cpp) as a LyricsView that
// bundles the track id with the lines, so a reader never sees lyrics for
// the wrong track.
//
// Layout: a header (album art / track / artist / source) is pinned to the top
// of the panel, above an inner, self-driven scroll region. Every row -- album
// art, title, artist, the sync status and each lyric line -- is centred
// horizontally in the panel. The header deliberately never moves: sizing the
// scroll region from the previous frame's measured height made the first frame
// after a track change lay out differently from the second, so the lyrics
// appeared half-way down the panel and then jumped up. Lyrics now always start
// at the top of the region (with just enough clearance for the edge fade) and
// the active line eases toward the centre of the viewport, clamped to whatever
// scroll range actually exists.
//
// Motion (Spotify-ish): the active line is larger and painted in the app
// accent; other lines dim with distance from it. On a verse change that
// emphasis morphs -- size, colour and the glow used to weight the scroll all
// ease from the old line to the new one (exponential decay, the same feel as
// the app's wheel scroll), and the list eases toward a centre that glides
// along with the morph instead of snapping to the new line. Content fades in
// on load and the top/bottom edges fade into the background. While any of
// that is in motion the panel flags the main loop to hold 60 FPS
// (set_lyrics_animating) so the app's power-saving frame throttling doesn't
// make it choppy.
//
// ImGui 1.92 (vendored here) rasterises fonts on demand, so per-line sizes
// are pushed with PushFont(NULL, size) rather than needing extra fonts.
// ----------------------------------------------------------------------

namespace
{
// App accent (daily colour, synced with dwm/dmenu).
static const ImVec4 &kAccent = g_accent_text;

// Height of the top/bottom fade overlays (also the clearance kept clear of
// them when lyrics have no auto-centre padding).
const float kEdgeFade = 34.0f;

// Ease rate: higher settles faster. ~0.5 s to visually settle.
const float kEaseRate = 6.0f;

// Line morph rate: how fast a line slides to its new size/colour when the
// active line moves on. ~0.5 s to settle.
const float kMorphRate = 7.0f;

// How long the auto-centre keeps owning the scroll target after the morph
// has finished, before handing control back to manual wheel scrolling.
const float kHoldSec = 0.75f;

// Font scale of the active line (1.0 = ordinary lines).
const float kActiveScale = 1.32f;

// Draw a single-line string horizontally centred in the available width,
// using the currently-pushed font/colour. Advances the cursor like Text().
void centered_text(const char *text)
{
    float avail = ImGui::GetContentRegionAvail().x;
    float w = ImGui::CalcTextSize(text).x;
    float x = ImGui::GetCursorPosX();
    if (w < avail)
        ImGui::SetCursorPosX(x + (avail - w) * 0.5f);
    ImGui::TextUnformatted(text);
}

// A centred text link. Draws with the currently-pushed font/colour and
// submits a hit-box matching the glyph width. Returns true on click.
bool centered_link(const char *id, const char *text, bool enabled,
                   const char *tooltip)
{
    ImFont *font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const ImVec2 ts = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
    float avail = ImGui::GetContentRegionAvail().x;
    float x = ImGui::GetCursorPosX();
    if (ts.x < avail)
        ImGui::SetCursorPosX(x + (avail - ts.x) * 0.5f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, ts.x), ImGui::GetTextLineHeight()));
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool clicked = hovered && ImGui::IsItemClicked();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddText(font, font_size, pos, ImGui::GetColorU32(ImGuiCol_Text), text);
    if (hovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const float y = pos.y + ImGui::GetTextLineHeight() - ui_px(1.0f);
        dl->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + ts.x, y),
                    ui_accent_text_u32(), ui_px(1.2f));
        if (tooltip && *tooltip)
            ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

// Button horizontally centred in the available width.
bool centered_button(const char *label)
{
    float avail = ImGui::GetContentRegionAvail().x;
    float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float x = ImGui::GetCursorPosX();
    if (w < avail)
        ImGui::SetCursorPosX(x + (avail - w) * 0.5f);
    return ImGui::Button(label);
}

// Loading treatment: three breathing dots plus a shimmering skeleton of
// lyric lines. The skeleton fades in place, so the panel never looks frozen
// while the multi-provider lyrics cascade is running.
void draw_lyrics_loading()
{
    const float avail = ImGui::GetContentRegionAvail().x;
    const float scale = ui_m().layout_scale;
    const double now = ImGui::GetTime();
    const float anim = ui_animations_enabled() ? 1.0f : 0.0f;

    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_dim);
    centered_text("Loading lyrics");
    ImGui::PopStyleColor();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 dot_pos = ImGui::GetCursorScreenPos();
    const float dot_r = 3.2f * scale;
    const float dot_gap = 13.0f * scale;
    const float dots_w = dot_gap * 2.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float phase = (float)now * 2.8f - (float)i * 0.55f;
        const float bob = sinf(phase) * 2.2f * scale * anim;
        const float a = 0.20f + 0.55f * (0.5f + 0.5f * sinf(phase)) * anim;
        const ImVec2 c(dot_pos.x + (avail - dots_w) * 0.5f + (float)i * dot_gap,
                       dot_pos.y + 6.0f * scale + bob);
        dl->AddCircleFilled(c, dot_r, ui_with_alpha(ui_accent_u32(), a), 14);
    }
    ImGui::Dummy(ImVec2(0.0f, 16.0f * scale + ui_px(10.0f)));

    static const float widths[] = {0.62f, 0.80f, 0.48f, 0.72f, 0.56f};
    const float bar_h = 9.0f * scale;
    const float row_gap = 13.0f * scale;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int i = 0; i < 5; ++i)
    {
        const float w = std::max(24.0f, avail * widths[i]);
        const float x = origin.x + (avail - w) * 0.5f;
        const float y = origin.y + (float)i * (bar_h + row_gap);
        const float breath = 0.5f + 0.5f * sinf((float)now * 1.8f + (float)i * 0.7f);
        const float base_a = anim > 0.0f ? 0.028f + 0.026f * breath : 0.045f;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + bar_h),
                          ImGui::GetColorU32(ImVec4(1, 1, 1, base_a)), bar_h * 0.5f);

        const float phase = fmodf((float)now * 0.42f + (float)i * 0.17f, 1.55f) - 0.25f;
        if (anim > 0.0f && phase > 0.0f && phase < 1.0f)
        {
            const float band = w * 0.18f;
            const float centre = x + w * phase;
            const float x0 = std::max(x, centre - band);
            const float x1 = std::min(x + w, centre + band);
            if (x1 > x0)
            {
                const ImU32 clear = ui_with_alpha(ui_accent_u32(), 0.0f);
                const ImU32 glow = ui_with_alpha(ui_accent_u32(), 0.085f);
                dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(centre, y + bar_h),
                                            clear, glow, glow, clear);
                dl->AddRectFilledMultiColor(ImVec2(centre, y), ImVec2(x1, y + bar_h),
                                            glow, clear, clear, glow);
            }
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 5.0f * bar_h + 4.0f * row_gap));
    if (anim > 0.0f)
        ui_anim_request();
}

// Draw a word-wrapped paragraph with EACH visual line centred in the available
// width. Submits a single Dummy of the total height so the caller can hover /
// click / measure the paragraph as one item.
//
// wrap_size: the font size the BREAK DECISIONS are measured at (0 = use the
// size currently drawn at). Splitting it from the draw size is what keeps the
// synced lyrics from re-wrapping mid-morph: those lines are eased between
// base and active size every verse change, and letting the break points follow
// that size made words jump between visual lines and the line snap to a new
// centre. Measuring at a fixed size -- the active size, which is the widest
// the line is ever drawn -- pins the breaks for the payload's lifetime, so a
// verse change only changes glyph size/colour (continuous) and row height.
// Centring and AddText stay on the drawn size, and drawn is never wider than
// the measured one, so nothing overflows.
void centered_wrapped_text(const char *text, ImU32 col, float wrap_size = 0.0f,
                           float max_width = 0.0f)
{
    ImFont *font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();
    const float line_h = ImGui::GetTextLineHeight();
    float wrap_w = ImGui::GetContentRegionAvail().x;
    if (max_width > 0.0f)
        wrap_w = std::min(wrap_w, max_width);
    if (wrap_w < 1.0f)
        wrap_w = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    if (wrap_size <= 0.0f)
        wrap_size = font_size;

    dl->PushClipRect(origin, ImVec2(origin.x + wrap_w, origin.y + 1.0e6f), true);

    float y = origin.y;
    const char *p = text;
    while (*p)
    {
        while (*p == ' ')
            ++p;
        if (!*p)
            break;

        const char *line_start = p;
        const char *line_end = p;
        const char *last_space = nullptr;
        const char *q = p;
        while (*q)
        {
            if (*q == ' ')
                last_space = q;
            const char *cand = q + 1;
            float w = font->CalcTextSizeA(wrap_size, FLT_MAX, 0.0f, line_start, cand).x;
            if (w > wrap_w)
            {
                if (last_space && last_space > line_start)
                    line_end = last_space; // break on the last word boundary
                else if (q > line_start)
                    line_end = q; // unbreakable word: break just before it
                else
                    line_end = cand; // single glyph wider than the line
                break;
            }
            line_end = cand;
            ++q;
        }
        if (*q == '\0')
            line_end = q;

        float w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line_start, line_end).x;
        float x = origin.x + (wrap_w - w) * 0.5f;
        if (x < origin.x)
            x = origin.x;
        dl->AddText(font, font_size, ImVec2(x, y), col, line_start, line_end);
        y += line_h;

        p = line_end;
        while (*p == ' ')
            ++p;
    }

    dl->PopClipRect();

    float h = y - origin.y;
    if (h <= 0.0f)
        h = line_h;
    ImGui::Dummy(ImVec2(wrap_w, h));
}

// Per-line eased state. A verse change morphs size/colour/glow from the old
// active line to the new one rather than snapping both in one frame, which is
// what makes the transition read as a single continuous movement.
struct LineState
{
    float scale = 1.0f;     // font scale (1.0 .. kActiveScale)
    float glow = 0.0f;      // eased active-ness 0..1 (weights the scroll centre)
    ImVec4 col = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // colour before the global fade
};

// Per-track UI animation state (main-thread render only).
struct LyricsAnim
{
    std::string track;
    int last_active = -2;
    bool scroll_init = false;
    float scroll_target = 0.0f;
    float alpha = 1.0f; // fade-in on load
    float enter = 1.0f; // whole-panel fade/slide when the track changes
    std::vector<LineState> lines;
    bool auto_scroll = false; // auto-centre currently owns scroll_target
    float hold = 0.0f;        // seconds left before it hands control back
};
LyricsAnim g_anim;

} // namespace

void draw_lyrics_panel(AppState &state, MpvPlayer &player, float width, float height)
{
    const float panel_w = std::max(1.0f, width);

    ImGui::BeginChild("LyricsPanel", ImVec2(panel_w, height),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);

    const float content_w = std::max(1.0f, ImGui::GetContentRegionAvail().x);

    auto snap = std::atomic_load(&state.lyrics);

    // Keep the outgoing track on screen until the midpoint of a crossfade,
    // matching the bottom bar. `crossfade_from_track` is captured by the main
    // loop when the fade starts; lyrics are only requested for the incoming
    // track once the visual hand-off happens.
    const bool show_incoming = !state.crossfade_active ||
                               state.crossfade_progress >= 0.5f;
    Track display;
    if (!show_incoming && !state.crossfade_from_track.id.empty())
        display = state.crossfade_from_track;
    else
    {
        display.id = state.current_track_id;
        display.title = state.current_title;
        display.artist = state.current_artist;
        display.thumbnail_url = state.current_thumbnail_url;
        display.url = state.current_track_url;
        display.album_id = state.current_album_id;
        display.artist_id = state.current_artist_id;
    }
    const std::string &current_id = display.id;
    const bool has_payload = snap && snap->track_id == current_id;

    // Reset the per-track animation/layout state.
    if (g_anim.track != current_id)
    {
        g_anim.track = current_id;
        g_anim.last_active = -2;
        g_anim.scroll_init = false;
        g_anim.scroll_target = 0.0f;
        g_anim.alpha = 0.0f;
        g_anim.enter = 0.0f;
        g_anim.lines.clear();
        g_anim.auto_scroll = false;
        g_anim.hold = 0.0f;
    }

    float panel_dt = ImGui::GetIO().DeltaTime;
    if (!(panel_dt > 0.0f) || panel_dt > 0.1f)
        panel_dt = 1.0f / 60.0f;
    if (!ui_animations_enabled())
    {
        g_anim.enter = 1.0f;
    }
    else if (g_anim.enter < 1.0f)
    {
        const float enter_dur = std::max(0.001f, ui_anim_duration(0.32f));
        g_anim.enter = std::min(1.0f, g_anim.enter + panel_dt / enter_dur);
        ui_anim_request();
    }
    const float panel_alpha = ui_anim_ease_out(g_anim.enter);
    const float panel_slide = (1.0f - panel_alpha) * ui_px(10.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, panel_alpha);
    if (panel_slide > 0.0f)
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + panel_slide);

    // ---- Header sits pinned to the top of the panel ----
    // (The whole block used to be vertically centred on the previous frame's
    // height, which made the header sit low while lyrics were still loading
    // and then jump up the moment they arrived. Keeping it pinned means the
    // header never moves.)

    // ---- Header: album art + the track the panel is showing (fixed) ----
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(ImGui::GetStyle().ItemSpacing.x, ui_px(6.0f)));

    if (!display.thumbnail_url.empty())
    {
        float art = ui_elem("lyrics_art", 220.0f);
        art = std::min(art, std::max(96.0f, height * 0.32f));
        art = std::min(art, content_w);
        float avail = content_w;
        float x = ImGui::GetCursorPosX() + (avail - art) * 0.5f;
        if (x < ImGui::GetCursorPosX())
            x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x);

        TextureInfo ti = texture_info(display.thumbnail_url);
        if (ti.id)
        {
            float u0, v0, u1, v1;
            cover_uv(ti, art, art, u0, v0, u1, v1);
            ImGui::Image((void *)(intptr_t)ti.id, ImVec2(art, art),
                         ImVec2(u0, v0), ImVec2(u1, v1));
            ImVec2 amin = ImGui::GetItemRectMin();
            ImVec2 amax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(amin, amax,
                                                IM_COL32(255, 255, 255, 28), 4.0f);
        }
        else
        {
            ImGui::Dummy(ImVec2(art, art));
        }
    }

    std::string title = display.title.empty() ? "Lyrics" : display.title;
    std::string title_str = ui_ellipsize(state.title_font, 34.0f, title, content_w);
    ImGui::PushFont(state.title_font);
    if (centered_link("##lyrics_album_link", title_str.c_str(),
                      !display.id.empty(), "Open album"))
    {
        Track t;
        t.id = display.id;
        t.title = display.title;
        t.artist = display.artist;
        t.thumbnail_url = display.thumbnail_url;
        t.url = display.url;
        t.album_id = display.album_id;
        t.artist_id = display.artist_id;
        open_album_from_track(state, t);
    }
    ImGui::PopFont();
    if (!display.artist.empty())
    {
        std::string artist_str = ui_ellipsize(g_ui_fonts.body, 26.0f,
                                             display.artist, content_w);
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_dim);
        if (centered_link("##lyrics_artist_link", artist_str.c_str(),
                          !display.id.empty(), "Open artist"))
        {
            Track t;
            t.id = display.id;
            t.title = display.title;
            t.artist = display.artist;
            t.thumbnail_url = display.thumbnail_url;
            t.url = display.url;
            t.album_id = display.album_id;
            t.artist_id = display.artist_id;
            open_artist_from_track(state, t);
        }
        ImGui::PopStyleColor();
    }

    // Sync status -- shown for both states, so it's obvious whether the lines
    // can highlight / be clicked to seek. Centred as a single row.
    if (has_payload && snap->lyrics.ok && !snap->lyrics.none)
    {
        const char *bullet = "\xe2\x97\x8f "; // U+25CF black circle
        std::string stat = std::string(bullet) +
                           (snap->lyrics.synced ? "Synced" : "Not synced (plain text)");
        ImVec4 stat_col = snap->lyrics.synced ? g_accent_active
                                              : g_accent_hover;
        std::string src;
        if (!snap->lyrics.source.empty())
            src = std::string("\xc2\xb7 ") + snap->lyrics.source;

        float w1 = ImGui::CalcTextSize(stat.c_str()).x;
        float w2 = src.empty() ? 0.0f : ImGui::CalcTextSize(src.c_str()).x;
        float sp = src.empty() ? 0.0f : ImGui::GetStyle().ItemSpacing.x;
        if (w1 + sp + w2 > content_w)
        {
            src.clear();
            w2 = 0.0f;
            sp = 0.0f;
        }
        if (w1 > content_w)
        {
            stat = ui_ellipsize(g_ui_fonts.body, 26.0f, stat, content_w);
            w1 = ImGui::CalcTextSize(stat.c_str()).x;
        }
        float x = ImGui::GetCursorPosX() + (content_w - (w1 + sp + w2)) * 0.5f;
        if (x < ImGui::GetCursorPosX())
            x = ImGui::GetCursorPosX();
        ImGui::SetCursorPosX(x);
        ImGui::TextColored(stat_col, "%s", stat.c_str());
        if (!src.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ui_c().text_muted, "%s", src.c_str());
        }
    }
    else if (!has_payload)
    {
        // Hold the status row's slot while the lyrics are still loading so the
        // separator below doesn't shift when the status appears.
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    }

    ImGui::PopStyleVar();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool loading = state.lyrics_loading.load();

    if (current_id.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
        centered_text("Nothing playing.");
        ImGui::PopStyleColor();
    }
    else if (!has_payload)
    {
        if (loading)
        {
            draw_lyrics_loading();
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
            centered_text("Lyrics not loaded.");
            ImGui::PopStyleColor();
            if (centered_button("Load lyrics"))
                state.lyrics_refresh_requested = true;
        }
    }
    else
    {
        const Lyrics &lx = snap->lyrics;

        if (!lx.ok)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, g_accent_text);
            centered_text("Couldn't load lyrics.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (centered_button("Retry"))
                state.lyrics_refresh_requested = true;
        }
        else if (lx.none || lx.lines.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
            centered_text("No lyrics found for this track.");
            ImGui::PopStyleColor();
        }
        else
        {
            // ---- Animation clocks ----
            ImGuiIO &io = ImGui::GetIO();
            float dt = io.DeltaTime;
            if (dt <= 0.0f)
                dt = 1.0f / 60.0f;
            if (dt > 0.1f)
                dt = 0.1f;
            if (!ui_animations_enabled())
            {
                g_anim.alpha = 1.0f;
            }
            else if (g_anim.alpha < 1.0f)
            {
                g_anim.alpha += dt / 0.28f;
                if (g_anim.alpha > 1.0f)
                    g_anim.alpha = 1.0f;
            }
            const float alpha = g_anim.alpha;

            ImGuiStyle &st = ImGui::GetStyle();
            // GetFontSize() is already scaled by FontScaleMain. PushFont()
            // expects the unscaled base and applies that scale itself, so keep
            // both values: draw_size is what the glyphs end up at, font_base is
            // what we hand back to ImGui for the per-line size animation.
            const float draw_size = ImGui::GetFontSize();
            const float font_base = draw_size / std::max(0.0001f, st.FontScaleMain);
            const float item_sp_y = ui_px(12.0f);

            // The scroll region always fills the space under the header, and the
            // lyrics always start at the top of it. The old version sized the
            // child from the *previous* frame's measured content height and
            // branched on "does it fit": that read 0 on the first frame after a
            // track change, snapped the lines into a half-viewport pad, then
            // jumped up to the top on the very next frame once the measurement
            // arrived. Pinning the layout makes frame 1 identical to frame 2.
            // (The active line still eases toward the centre, just clamped to
            // whatever scroll range actually exists -- no big blank above the
            // first line.)
            const float remaining = ImGui::GetContentRegionAvail().y;
            const float child_h = remaining > ui_px(40.0f) ? remaining : ui_px(40.0f);
            // Clearance so the top edge fade never dims the first line.
            const float pad = ui_px(kEdgeFade) + ui_px(6.0f);

            // Zero the child's window padding so the lyrics align with the
            // header and the pad measurements stay exact.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::BeginChild("LyricsScroll", ImVec2(content_w, child_h), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, item_sp_y));

            if (pad > 0.0f)
                ImGui::Dummy(ImVec2(0.0f, pad));

            float active_center = -1.0f;
            int active = -1;
            bool seek_requested = false;
            double seek_pos = 0.0f;
            // Glow-weighted centre of the lyrics: a continuous blend of the
            // line centres, each weighted by that line's eased active-ness.
            // When the active line changes the weight slides from the old line
            // to the new one, so the value glides between the two centres and
            // the scroll follows along instead of jumping.
            float centre_sum = 0.0f;
            float centre_acc = 0.0f;
            bool morphing = false;
            if (lx.synced)
            {
                // User timing offset: positive delays the highlight (lyrics
                // shown later), negative advances it. The same offset is added
                // back when a line is clicked, so a click lands on its own
                // highlight straight away.
                const bool incoming =
                    state.crossfade_active && state.crossfade_progress >= 0.5f;
                float t = incoming ? (float)state.crossfade_position
                                   : (float)player.cached_time_pos();
                if (t < 0.0f)
                    t = 0.0f;
                t -= state.settings.lyrics_offset;
                for (int i = 0; i < (int)lx.lines.size() && i < (int)lx.times.size(); ++i)
                {
                    if ((float)lx.times[i] <= t)
                        active = i;
                }

                // (Re)size the per-line animation state. A fresh set is snapped
                // straight to its target so a newly arrived payload never
                // morphs in from black.
                bool snap_lines = false;
                if (g_anim.lines.size() != lx.lines.size())
                {
                    g_anim.lines.clear();
                    g_anim.lines.resize(lx.lines.size());
                    snap_lines = true;
                }
                const bool animate_lines = ui_animations_enabled() && !snap_lines;
                const float ease = animate_lines ? 1.0f - std::exp(-kMorphRate * dt) : 1.0f;
                if (!animate_lines)
                    snap_lines = true;
                // Constant break size in final screen pixels (the widest the
                // line is ever drawn), so the morph can never re-wrap a line
                // mid-transition. The actual draw size follows ls.scale and is
                // never above this.
                const float wrap_size = draw_size * kActiveScale;

                for (int i = 0; i < (int)lx.lines.size(); ++i)
                {
                    LineState &ls = g_anim.lines[i];
                    const bool is_active = (i == active);

                    const float tgt_scale = is_active ? kActiveScale : 1.0f;
                    const float tgt_glow = is_active ? 1.0f : 0.0f;
                    ImVec4 tgt_col;
                    if (is_active)
                    {
                        tgt_col = kAccent;
                    }
                    else
                    {
                        float dist = (active < 0) ? 0.0f : std::fabs((float)(i - active));
                        float f = dist / 7.0f;
                        if (f > 1.0f)
                            f = 1.0f;
                        float shade = 0.72f - 0.42f * f;
                        tgt_col = ImVec4(shade, shade, shade + 0.03f,
                                         0.30f + 0.70f * (1.0f - f * 0.6f));
                    }

                    ls.scale += (tgt_scale - ls.scale) * ease;
                    ls.glow += (tgt_glow - ls.glow) * ease;
                    ls.col.x += (tgt_col.x - ls.col.x) * ease;
                    ls.col.y += (tgt_col.y - ls.col.y) * ease;
                    ls.col.z += (tgt_col.z - ls.col.z) * ease;
                    ls.col.w += (tgt_col.w - ls.col.w) * ease;
                    if (std::fabs(ls.scale - tgt_scale) > 0.003f ||
                        std::fabs(ls.glow - tgt_glow) > 0.005f ||
                        std::fabs(ls.col.x - tgt_col.x) > 0.004f ||
                        std::fabs(ls.col.w - tgt_col.w) > 0.005f)
                        morphing = true;

                    ImVec4 col = ls.col;
                    const float line_top = ImGui::GetCursorScreenPos().y;
                    const float line_edge = ui_content_edge_alpha(
                        line_top, line_top + ImGui::GetTextLineHeightWithSpacing() * 1.8f);
                    col.w *= alpha * panel_alpha * line_edge;

                    float top_y = ImGui::GetCursorPosY();
                    // ImGui rounds font sizes to whole pixels, so animating
                    // the scale only ever walks the ~10 sizes between 28 and
                    // 37 px (packed incrementally by the GL3 backend) while
                    // colour, glow and the layout ease continuously.
                    ImGui::PushFont(nullptr, font_base * ls.scale);
                    centered_wrapped_text(lx.lines[i].c_str(),
                                          ImGui::ColorConvertFloat4ToU32(col),
                                          wrap_size, content_w);
                    ImGui::PopFont();
                    float centre = (top_y + ImGui::GetCursorPosY() - item_sp_y) * 0.5f;

                    centre_sum += ls.glow;
                    centre_acc += ls.glow * centre;
                    if (is_active)
                        active_center = centre;

                    // Click a line to seek to its timestamp; hover feedback + hand.
                    if (lx.synced && i < (int)lx.times.size() && ImGui::IsItemHovered())
                    {
                        ImVec2 rmin = ImGui::GetItemRectMin();
                        ImVec2 rmax = ImGui::GetItemRectMax();
                        ImGui::GetWindowDrawList()->AddRectFilled(
                            ImVec2(rmin.x - ui_px(4.0f), rmin.y - ui_px(2.0f)),
                            ImVec2(rmax.x + ui_px(4.0f), rmax.y + ui_px(2.0f)),
                            IM_COL32(255, 255, 255, 16), 4.0f);
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        if (ImGui::IsItemClicked())
                        {
                            seek_pos = (double)lx.times[i] + (double)state.settings.lyrics_offset;
                            seek_requested = true;
                        }
                    }
                }
            }
            else
            {
                // No active line here: make sure a leftover auto-centre from
                // the previous (synced) payload can't keep pinning 60 FPS.
                g_anim.auto_scroll = false;
                g_anim.hold = 0.0f;
                g_anim.last_active = -2;

                // Unsynced lyrics: a plain, evenly dimmed block (no highlight).
                for (const auto &line : lx.lines)
                {
                    const float line_top = ImGui::GetCursorScreenPos().y;
                    const float line_edge = ui_content_edge_alpha(
                        line_top, line_top + ImGui::GetTextLineHeightWithSpacing());
                    const ImU32 ucol = ui_with_alpha(
                        ui_text_dim_u32(), alpha * panel_alpha * 0.85f * line_edge);
                    centered_wrapped_text(line.c_str(), ucol, 0.0f, content_w);
                }
            }

            ImGui::Dummy(ImVec2(0.0f, pad)); // symmetric bottom padding
            ImGui::PopStyleVar();

            // ---- Scroll: unified target eased toward (wheel + auto-centre) ----
            float real = ImGui::GetScrollY();
            float max_scroll = ImGui::GetScrollMaxY();
            if (!g_anim.scroll_init)
            {
                g_anim.scroll_init = true;
                // Start at the top: before the first lyric timestamp the
                // active line is -1, and the lyrics should be pinned to the
                // top of the panel instead of wherever the previous song left
                // the scroll position.
                g_anim.scroll_target = 0.0f;
            }

            // Wheel scrolls both synced and unsynced lyrics (the child uses
            // NoScrollWithMouse, so this is the only scroll path). Touching the
            // wheel cancels any pending auto-centre: manual scrolling wins.
            if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f && max_scroll > 0.0f)
            {
                g_anim.auto_scroll = false;
                g_anim.hold = 0.0f;
                g_anim.scroll_target -= io.MouseWheel * ui_px(480.0f);
                if (g_anim.scroll_target < 0.0f)
                    g_anim.scroll_target = 0.0f;
                if (g_anim.scroll_target > max_scroll)
                    g_anim.scroll_target = max_scroll;
            }

            if (lx.synced)
            {
                if (active != g_anim.last_active)
                {
                    g_anim.last_active = active;
                    // Nothing to centre on before the first line, and an
                    // inactive state must not keep owning the target (the
                    // hold is only ever decremented below, while active >= 0).
                    g_anim.auto_scroll = (active >= 0);
                    g_anim.hold = g_anim.auto_scroll ? kHoldSec : 0.0f;
                    // Before the first timestamp the lyrics should sit at the
                    // top of the panel, not wherever the previous song left
                    // the scroll position.
                    if (active < 0)
                        g_anim.scroll_target = 0.0f;
                }

                // Follow the glow-weighted centre every frame while the auto-
                // centre owns the target, so the scroll eases along with the
                // morphing lines rather than waiting for the index to flip.
                // active/centre are content-space (GetCursorPosY ignores
                // scroll), so the target is the centre minus half a viewport,
                // clamped to [0, max_scroll] -- which keeps the early lines
                // pinned to the top when there is no range above them yet.
                if (g_anim.auto_scroll && active >= 0)
                {
                    float centre = centre_sum > 1.0e-3f ? centre_acc / centre_sum
                                                        : active_center;
                    if (centre >= 0.0f)
                    {
                        float tgt = centre - child_h * 0.5f;
                        if (tgt < 0.0f)
                            tgt = 0.0f;
                        if (tgt > max_scroll)
                            tgt = max_scroll;
                        g_anim.scroll_target = tgt;
                    }
                    if (morphing)
                        g_anim.hold = kHoldSec; // the hold only starts once settled
                    else if ((g_anim.hold -= dt) <= 0.0f)
                        g_anim.auto_scroll = false; // hand the target back
                }
            }

            float diff = g_anim.scroll_target - real;
            const bool animate_scroll = ui_animations_enabled();
            bool scroll_anim = animate_scroll && std::fabs(diff) > 0.5f;
            if (!animate_scroll)
            {
                ImGui::SetScrollY(g_anim.scroll_target);
            }
            else if (scroll_anim)
            {
                float e = 1.0f - std::exp(-kEaseRate * dt);
                ImGui::SetScrollY(real + diff * e);
            }
            else
            {
                ImGui::SetScrollY(g_anim.scroll_target);
            }

            ImGui::EndChild();
            ImGui::PopStyleVar(); // WindowPadding

            if (seek_requested)
                player.seek(seek_pos);

            // Keep the main loop at 60 FPS while any of it is in motion. The
            // morph must be included: without it a verse change would render
            // at the 100 ms playback cadence and look choppy.
            set_lyrics_animating(ui_animations_enabled() &&
                                 (scroll_anim || alpha < 1.0f || morphing ||
                                  g_anim.auto_scroll));
        }
    }

    ImGui::PopStyleVar(); // lyrics panel enter alpha
    ImGui::EndChild();
}
