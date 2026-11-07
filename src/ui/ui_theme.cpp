#include "ui_theme.h"
#include "ui.h"
#include "ui_backdrop.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

UiFonts g_ui_fonts;

static UiMetrics g_metrics;
static UiColors g_colors;
static ImGuiStyle g_base_style;
static bool g_base_style_ready = false;

// Two palettes that are blended per-frame for the smooth day/night transition.
static const UiColors k_colors_dark = {
    /*bg*/ ImVec4(0.0f, 0.0f, 0.0f, 1.0f),
    /*surface*/ ImVec4(0.0f, 0.0f, 0.0f, 1.0f),
    /*surface_hover*/ ImVec4(0.075f, 0.075f, 0.085f, 1.0f),
    /*surface_active*/ ImVec4(0.055f, 0.055f, 0.06f, 1.0f),
    /*frame_bg*/ ImVec4(0.018f, 0.018f, 0.021f, 1.0f),
    /*frame_bg_hovered*/ ImVec4(0.055f, 0.055f, 0.062f, 1.0f),
    /*frame_bg_active*/ ImVec4(0.018f, 0.018f, 0.021f, 1.0f),
    /*overlay*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*border*/ ImVec4(0.12f, 0.12f, 0.135f, 1.0f),
    /*border_soft*/ ImVec4(0.075f, 0.075f, 0.085f, 1.0f),
    /*text*/ ImVec4(0.95f, 0.95f, 0.965f, 1.0f),
    /*text_dim*/ ImVec4(0.62f, 0.62f, 0.66f, 1.0f),
    /*text_muted*/ ImVec4(0.40f, 0.40f, 0.44f, 1.0f),
    /*accent*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*accent_hover*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*accent_active*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*accent_text*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*on_accent*/ ImVec4(0.0f, 0.0f, 0.0f, 1.0f),
};

static const UiColors k_colors_light = {
    /*bg*/ ImVec4(0.96f, 0.965f, 0.975f, 1.0f),
    /*surface*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*surface_hover*/ ImVec4(0.90f, 0.905f, 0.92f, 1.0f),
    /*surface_active*/ ImVec4(0.86f, 0.865f, 0.89f, 1.0f),
    /*frame_bg*/ ImVec4(0.92f, 0.925f, 0.945f, 1.0f),
    /*frame_bg_hovered*/ ImVec4(0.87f, 0.875f, 0.90f, 1.0f),
    /*frame_bg_active*/ ImVec4(0.92f, 0.925f, 0.945f, 1.0f),
    /*overlay*/ ImVec4(0.0f, 0.0f, 0.0f, 1.0f),
    /*border*/ ImVec4(0.78f, 0.785f, 0.81f, 1.0f),
    /*border_soft*/ ImVec4(0.86f, 0.865f, 0.885f, 1.0f),
    /*text*/ ImVec4(0.10f, 0.105f, 0.13f, 1.0f),
    /*text_dim*/ ImVec4(0.38f, 0.385f, 0.42f, 1.0f),
    /*text_muted*/ ImVec4(0.52f, 0.525f, 0.56f, 1.0f),
    /*accent*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*accent_hover*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*accent_active*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*accent_text*/ ImVec4(1.0f, 1.0f, 1.0f, 1.0f),
    /*on_accent*/ ImVec4(0.0f, 0.0f, 0.0f, 1.0f),
};

static float g_theme_t = 0.0f;        // 0 = dark, 1 = light (current, animated)
static float g_theme_target = 0.0f;   // target
static bool g_theme_initialized = false;

// Glowing separators: setting mirror, target and eased strength.
static bool g_glow_separators = true;
static float g_glow_strength = 0.75f;
static float g_glow_level = 0.0f;

static ImVec4 lerp4(const ImVec4 &a, const ImVec4 &b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t,
                  a.y + (b.y - a.y) * t,
                  a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float srgb_luma(const ImVec4 &c)
{
    return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
}

ImU32 ui_col(const ImVec4 &c)
{
    return ImGui::GetColorU32(c);
}

ImU32 ui_accent_u32(float alpha)
{
    return ImGui::GetColorU32(accent_alpha(g_accent, alpha));
}

ImU32 ui_accent_text_u32()
{
    return ImGui::GetColorU32(g_accent_text);
}

ImU32 ui_text_u32()
{
    return ImGui::GetColorU32(g_colors.text);
}

ImU32 ui_text_dim_u32()
{
    return ImGui::GetColorU32(g_colors.text_dim);
}

ImU32 ui_text_muted_u32()
{
    return ImGui::GetColorU32(g_colors.text_muted);
}

ImU32 ui_border_u32()
{
    return ImGui::GetColorU32(g_colors.border);
}

ImU32 ui_border_soft_u32()
{
    return ImGui::GetColorU32(g_colors.border_soft);
}

// Shared by the packed and ImVec4 forms of the structural hairline, so the
// draw-list paths and the ImGui style colours always agree.
static ImVec4 hairline_color()
{
    const ImVec4 base = g_colors.border_soft;
    if (g_glow_level <= 0.002f)
        return base;

    // Borrow the hue of the ambient tint but keep the hairline as dim as the
    // neutral border, so structural edges stay quiet instead of turning into
    // bright rules.
    const ImVec4 tint = ui_backdrop_tint();
    const float tint_peak = std::max({tint.x, tint.y, tint.z});
    const float base_peak = std::max({base.x, base.y, base.z});
    if (tint_peak < 0.001f || base_peak < 0.001f)
        return base;

    const float lift = ui_theme_is_light() ? 1.0f : 1.30f;
    const float scale = base_peak / tint_peak * (1.0f + (lift - 1.0f) * g_glow_level);
    const float k = std::min(1.0f, 0.90f * g_glow_level);

    ImVec4 c = base;
    c.x += (std::min(1.0f, tint.x * scale) - c.x) * k;
    c.y += (std::min(1.0f, tint.y * scale) - c.y) * k;
    c.z += (std::min(1.0f, tint.z * scale) - c.z) * k;
    return c;
}

float ui_glow_level()
{
    return g_glow_level;
}

ImU32 ui_hairline_u32()
{
    return ImGui::GetColorU32(hairline_color());
}

ImU32 ui_overlay_u32(float alpha)
{
    ImVec4 c = g_colors.overlay;
    c.w = alpha;
    return ImGui::GetColorU32(c);
}

ImVec4 ui_overlay_vec4(float alpha)
{
    ImVec4 c = g_colors.overlay;
    c.w = alpha;
    return c;
}

bool ui_theme_is_light()
{
    return g_theme_t > 0.5f;
}

float ui_theme_progress()
{
    return g_theme_t;
}

bool ui_theme_animating()
{
    // True while the day/night palette or the separator glow is still easing.
    // apply_ui_scale() uses this to avoid re-scaling the whole ImGui style on
    // every frame while still letting theme transitions finish smoothly.
    return g_theme_t != g_theme_target ||
           std::fabs(g_glow_level - g_glow_strength) > 0.002f;
}

void ui_theme_tick()
{
    // animate g_theme_t toward g_theme_target
    float dt = ImGui::GetIO().DeltaTime;
    if (!(dt > 0.0f) || dt > 0.1f)
        dt = 1.0f / 60.0f;
    const float speed = 4.0f;
    if (g_theme_t < g_theme_target)
        g_theme_t = std::min(g_theme_target, g_theme_t + speed * dt);
    else if (g_theme_t > g_theme_target)
        g_theme_t = std::max(g_theme_target, g_theme_t - speed * dt);

    // blend palettes
    const UiColors &D = k_colors_dark;
    const UiColors &L = k_colors_light;
    const float t = g_theme_t;
    g_colors.bg = lerp4(D.bg, L.bg, t);
    g_colors.surface = lerp4(D.surface, L.surface, t);
    g_colors.surface_hover = lerp4(D.surface_hover, L.surface_hover, t);
    g_colors.surface_active = lerp4(D.surface_active, L.surface_active, t);
    g_colors.border = lerp4(D.border, L.border, t);
    g_colors.border_soft = lerp4(D.border_soft, L.border_soft, t);
    g_colors.text = lerp4(D.text, L.text, t);
    g_colors.text_dim = lerp4(D.text_dim, L.text_dim, t);
    g_colors.text_muted = lerp4(D.text_muted, L.text_muted, t);
    g_colors.frame_bg = lerp4(D.frame_bg, L.frame_bg, t);
    g_colors.frame_bg_hovered = lerp4(D.frame_bg_hovered, L.frame_bg_hovered, t);
    g_colors.frame_bg_active = lerp4(D.frame_bg_active, L.frame_bg_active, t);
    g_colors.overlay = lerp4(D.overlay, L.overlay, t);

    if (g_theme_t != g_theme_target)
        ui_anim_request();
}

const UiMetrics &ui_m()
{
    return g_metrics;
}

const UiColors &ui_c()
{
    return g_colors;
}

float ui_font_size(float base)
{
    return base * g_metrics.text_scale;
}

bool ui_accent_is_light()
{
    return srgb_luma(g_accent) > 0.58f;
}

void ui_theme_refresh_accent()
{
    g_colors.accent = g_accent;
    g_colors.accent_hover = g_accent_hover;
    g_colors.accent_active = g_accent_active;
    g_colors.accent_text = g_accent_text;
    g_colors.on_accent = ui_accent_is_light() ? ImVec4(0.02f, 0.02f, 0.025f, 1.0f) : ImVec4(0.98f, 0.98f, 0.99f, 1.0f);
}

static void build_glyph_ranges(ImFontGlyphRangesBuilder &builder, ImGuiIO &io)
{
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    static const ImWchar latin_ext_a_ranges[] = {0x0100, 0x017F, 0};
    static const ImWchar latin_ext_b_ranges[] = {0x0180, 0x024F, 0};
    static const ImWchar punctuation_ranges[] = {0x2000, 0x206F, 0};
    builder.AddRanges(latin_ext_a_ranges);
    builder.AddRanges(latin_ext_b_ranges);
    builder.AddRanges(punctuation_ranges);
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    builder.AddRanges(io.Fonts->GetGlyphRangesGreek());
    builder.AddChar(0x0103);
    builder.AddChar(0x00E2);
    builder.AddChar(0x00EE);
    builder.AddChar(0x0219);
    builder.AddChar(0x021B);
    builder.AddChar(0x015F);
    builder.AddChar(0x0163);
    builder.AddChar(0x2665);
    builder.AddChar(0x25CF);
}

static ImFont *load_font(ImGuiIO &io, const std::string &path, float size, ImVector<ImWchar> &ranges, ImFont *fallback)
{
    ImFontConfig cfg;
    cfg.SizePixels = size;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;
    ImFont *font = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg, ranges.Data);
    return font ? font : fallback;
}

void ui_theme_init_fonts(ImGuiIO &io, const std::string &asset_dir)
{
    io.Fonts->Clear();
    ImFontGlyphRangesBuilder builder;
    build_glyph_ranges(builder, io);
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);

    const std::string regular_path = asset_dir + "/Inter-Regular.ttf";
    const std::string medium_path = asset_dir + "/Inter-Medium.ttf";
    const std::string semibold_path = asset_dir + "/Inter-SemiBold.ttf";

    ImFontConfig fallback_cfg;
    fallback_cfg.SizePixels = 26.0f;
    fallback_cfg.OversampleH = 2;
    fallback_cfg.OversampleV = 2;
    ImFont *fallback = nullptr;
    if (std::filesystem::exists(regular_path))
        fallback = io.Fonts->AddFontFromFileTTF(regular_path.c_str(), 26.0f, &fallback_cfg, ranges.Data);
    if (!fallback)
        fallback = io.Fonts->AddFontDefault(&fallback_cfg);

    g_ui_fonts.body = fallback;
    g_ui_fonts.medium = load_font(io, medium_path, 26.0f, ranges, fallback);
    g_ui_fonts.semibold = load_font(io, semibold_path, 34.0f, ranges, fallback);
    io.FontDefault = g_ui_fonts.body;
}

static void init_base_style()
{
    if (g_base_style_ready)
        return;
    ImGui::StyleColorsDark();
    ImGuiStyle &s = g_base_style;
    s.WindowPadding = ImVec2(16.0f, 14.0f);
    s.FramePadding = ImVec2(12.0f, 8.0f);
    s.CellPadding = ImVec2(10.0f, 8.0f);
    s.ItemSpacing = ImVec2(8.0f, 8.0f);
    s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    s.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 10.0f;
    s.GrabMinSize = 9.0f;
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.TabBorderSize = 0.0f;
    s.TabBarBorderSize = 1.0f;
    s.WindowRounding = 12.0f;
    s.ChildRounding = 10.0f;
    s.FrameRounding = 8.0f;
    s.PopupRounding = 10.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding = 8.0f;
    s.TabRounding = 8.0f;
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    s.SeparatorTextPadding = ImVec2(16.0f, 8.0f);
    g_base_style_ready = true;
}

void ui_theme_apply(const Settings &settings, const ImGuiIO &io)
{
    (void)io;

    const float master = clampf(settings.ui_scale, 0.75f, 2.0f);
    const float text = master * clampf(settings.ui_text_scale, 0.5f, 2.0f);
    const float widget = master * clampf(settings.ui_widget_scale, 0.5f, 2.0f);
    const float layout = master * clampf(settings.ui_layout_scale, 0.5f, 2.0f);

    // Metrics/style only change when a scale slider moves; rebuilding the
    // whole ImGuiStyle every rendered frame is pure waste. Theme colours and
    // the glow/transition animation still update on every call below.
    static float cached_master = -1.0f, cached_text = -1.0f;
    static float cached_widget = -1.0f, cached_layout = -1.0f;
    const bool style_dirty = master != cached_master || text != cached_text ||
                             widget != cached_widget || layout != cached_layout;
    if (style_dirty)
    {
        cached_master = master;
        cached_text = text;
        cached_widget = widget;
        cached_layout = layout;
        init_base_style();

        g_ui_layout_scale = layout;
        g_metrics.scale = master;
        g_metrics.text_scale = text;
        g_metrics.widget_scale = widget;
        g_metrics.layout_scale = layout;

        g_metrics.space_1 = 4.0f * layout;
        g_metrics.space_2 = 8.0f * layout;
        g_metrics.space_3 = 12.0f * layout;
        g_metrics.space_4 = 16.0f * layout;
        g_metrics.space_5 = 24.0f * layout;
        g_metrics.space_6 = 32.0f * layout;

        g_metrics.radius_xs = 4.0f * layout;
        g_metrics.radius_sm = 7.0f * layout;
        g_metrics.radius_md = 10.0f * layout;
        g_metrics.radius_lg = 14.0f * layout;

        g_metrics.control_h = 38.0f * layout;
        g_metrics.button_h = 40.0f * layout;
        g_metrics.row_art = 48.0f * layout;
        g_metrics.row_pad_x = 12.0f * layout;
        g_metrics.row_pad_y = 9.0f * layout;
        g_metrics.row_gap = 4.0f * layout;
        g_metrics.card_gap = 18.0f * layout;
        g_metrics.sidebar_w = 232.0f * layout;
        g_metrics.sidebar_rail_w = 68.0f * layout;
        g_metrics.header_h = 68.0f * layout;
        g_metrics.bottom_h = 92.0f * layout;
        g_metrics.content_pad = 24.0f * layout;
        g_metrics.lyrics_min = 300.0f * layout;
        g_metrics.lyrics_max = 520.0f * layout;

        ImGuiStyle style = g_base_style;
        style.ScaleAllSizes(widget);
        style.FontScaleMain = text;
        style.FontScaleDpi = 1.0f;
        ImGui::GetStyle() = style;
    }

    // Theme target + first-frame snap.
    g_theme_target = settings.day_mode ? 1.0f : 0.0f;
    if (!g_theme_initialized)
    {
        g_theme_t = g_theme_target;
        g_theme_initialized = true;
    }

    // Animate + blend (ui_theme_tick does the lerp + style colors).
    ui_theme_tick();

    // Glowing separators follow their own toggle so they work with the full
    // backdrop switched off. The eased level doubles as the fade-in/out.
    g_glow_separators = settings.glow_separators;
    g_glow_strength = settings.glow_separators
                          ? clampf(settings.glow_separator_strength, 0.10f, 1.50f)
                          : 0.0f;
    g_glow_level = ui_anim_approach(g_glow_level, g_glow_strength, 10.0f);
    if (std::fabs(g_glow_level - g_glow_strength) > 0.002f)
        ui_anim_request();

    ui_theme_refresh_accent();

    // Apply ImGui style colors from the (possibly mid-transition) palette.
    ImGuiStyle &live = ImGui::GetStyle();
    live.Colors[ImGuiCol_Text] = g_colors.text;
    live.Colors[ImGuiCol_TextDisabled] = g_colors.text_muted;
    live.Colors[ImGuiCol_WindowBg] = g_colors.bg;
    // Child panels stay transparent so they never stack a second opaque layer
    // over the ambient backdrop. In normal mode the parent panel is opaque and
    // already supplies the same background colour, so this looks identical.
    live.Colors[ImGuiCol_ChildBg] = ImVec4(g_colors.bg.x, g_colors.bg.y, g_colors.bg.z, 0.0f);
    live.Colors[ImGuiCol_PopupBg] = g_colors.bg;
    live.Colors[ImGuiCol_Border] = g_colors.border;
    live.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    live.Colors[ImGuiCol_FrameBg] = g_colors.frame_bg;
    live.Colors[ImGuiCol_FrameBgHovered] = g_colors.frame_bg_hovered;
    live.Colors[ImGuiCol_FrameBgActive] = accent_alpha(g_accent, 0.16f);
    live.Colors[ImGuiCol_TitleBg] = g_colors.bg;
    live.Colors[ImGuiCol_TitleBgActive] = g_colors.bg;
    live.Colors[ImGuiCol_TitleBgCollapsed] = g_colors.bg;
    live.Colors[ImGuiCol_MenuBarBg] = g_colors.bg;
    live.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    live.Colors[ImGuiCol_ScrollbarGrab] = lerp4(ImVec4(0.25f, 0.25f, 0.28f, 0.65f), ImVec4(0.68f, 0.68f, 0.70f, 0.65f), g_theme_t);
    live.Colors[ImGuiCol_ScrollbarGrabHovered] = lerp4(ImVec4(0.38f, 0.38f, 0.42f, 0.85f), ImVec4(0.55f, 0.55f, 0.58f, 0.85f), g_theme_t);
    live.Colors[ImGuiCol_ScrollbarGrabActive] = g_accent;
    live.Colors[ImGuiCol_CheckMark] = g_accent_active;
    live.Colors[ImGuiCol_CheckboxSelectedBg] = accent_alpha(g_accent, 0.45f);
    live.Colors[ImGuiCol_SliderGrab] = g_accent_hover;
    live.Colors[ImGuiCol_SliderGrabActive] = g_accent_active;
    live.Colors[ImGuiCol_Button] = g_colors.bg;
    live.Colors[ImGuiCol_ButtonHovered] = g_colors.surface_hover;
    live.Colors[ImGuiCol_ButtonActive] = accent_alpha(g_accent, 0.20f);
    live.Colors[ImGuiCol_Header] = accent_alpha(g_accent, 0.13f);
    live.Colors[ImGuiCol_HeaderHovered] = accent_alpha(g_accent, 0.20f);
    live.Colors[ImGuiCol_HeaderActive] = accent_alpha(g_accent, 0.28f);
    live.Colors[ImGuiCol_Separator] = hairline_color();
    live.Colors[ImGuiCol_SeparatorHovered] = g_colors.border;
    live.Colors[ImGuiCol_SeparatorActive] = g_accent;
    live.Colors[ImGuiCol_ResizeGrip] = accent_alpha(g_accent, 0.15f);
    live.Colors[ImGuiCol_ResizeGripHovered] = accent_alpha(g_accent, 0.45f);
    live.Colors[ImGuiCol_ResizeGripActive] = accent_alpha(g_accent, 0.70f);
    live.Colors[ImGuiCol_Tab] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    live.Colors[ImGuiCol_TabHovered] = accent_alpha(g_accent, 0.15f);
    live.Colors[ImGuiCol_TabSelected] = accent_alpha(g_accent, 0.18f);
    live.Colors[ImGuiCol_TabSelectedOverline] = g_accent;
    live.Colors[ImGuiCol_TabDimmed] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    live.Colors[ImGuiCol_TabDimmedSelected] = accent_alpha(g_accent, 0.10f);
    live.Colors[ImGuiCol_TextLink] = g_accent_text;
    live.Colors[ImGuiCol_TextSelectedBg] = accent_alpha(g_accent, 0.30f);
    live.Colors[ImGuiCol_NavCursor] = g_accent_active;
    live.Colors[ImGuiCol_DragDropTarget] = g_accent_active;
    live.Colors[ImGuiCol_PlotLines] = g_accent_hover;
    live.Colors[ImGuiCol_PlotLinesHovered] = g_accent_text;
    live.Colors[ImGuiCol_PlotHistogram] = g_accent;
    live.Colors[ImGuiCol_PlotHistogramHovered] = g_accent_hover;
    live.Colors[ImGuiCol_TableHeaderBg] = g_colors.bg;
    live.Colors[ImGuiCol_TableBorderStrong] = g_colors.border;
    live.Colors[ImGuiCol_TableBorderLight] = g_colors.border_soft;
    live.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    live.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    live.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.72f);

    // Request repaint while the theme is mid-transition.
    if (g_theme_t != g_theme_target)
        ui_anim_request();
}

static void push_font(ImFont *font, float base)
{
    if (!font)
        font = g_ui_fonts.body;
    ImGui::PushFont(font, base > 0.0f ? base : 0.0f);
}

void ui_push_regular(float base_size)
{
    push_font(g_ui_fonts.body, base_size);
}

void ui_push_medium(float base_size)
{
    push_font(g_ui_fonts.medium, base_size);
}

void ui_push_semibold(float base_size)
{
    push_font(g_ui_fonts.semibold, base_size);
}

void ui_push_micro()
{
    ui_push_regular(18.0f);
}

void ui_push_small()
{
    ui_push_regular(20.0f);
}

void ui_push_body()
{
    ui_push_regular(26.0f);
}

float ui_measure_width(ImFont *font, float base_size, const std::string &text)
{
    if (!font || text.empty())
        return 0.0f;
    const float size = base_size * g_metrics.text_scale;
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str(), text.c_str() + text.size()).x;
}

std::string ui_ellipsize(ImFont *font, float base_size, const std::string &text, float max_width)
{
    if (!font || text.empty() || max_width <= 0.0f)
        return "";
    if (ui_measure_width(font, base_size, text) <= max_width)
        return text;

    const std::string ellipsis = "\xE2\x80\xA6";
    const float ell_w = ui_measure_width(font, base_size, ellipsis);
    if (ell_w > max_width)
        return "";

    size_t lo = 0;
    size_t hi = text.size();
    while (lo < hi)
    {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && mid < text.size() && (static_cast<unsigned char>(text[mid]) & 0xC0) == 0x80)
            --mid;
        if (mid == lo)
            break;
        std::string candidate = text.substr(0, mid) + ellipsis;
        if (ui_measure_width(font, base_size, candidate) <= max_width)
            lo = mid;
        else
            hi = mid - 1;
        while (hi > 0 && hi < text.size() && (static_cast<unsigned char>(text[hi]) & 0xC0) == 0x80)
            --hi;
    }
    return text.substr(0, lo) + ellipsis;
}
