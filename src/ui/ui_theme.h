#pragma once

#include "imgui.h"
#include <string>

struct Settings;

struct UiFonts
{
    ImFont *body = nullptr;
    ImFont *medium = nullptr;
    ImFont *semibold = nullptr;
};

struct UiMetrics
{
    float scale = 1.0f;
    float text_scale = 1.0f;
    float widget_scale = 1.0f;
    float layout_scale = 1.0f;

    float font_micro = 18.0f;
    float font_small = 20.0f;
    float font_body = 26.0f;
    float font_label = 20.0f;
    float font_heading = 30.0f;
    float font_title = 34.0f;
    float font_display = 40.0f;

    float space_1 = 4.0f;
    float space_2 = 8.0f;
    float space_3 = 12.0f;
    float space_4 = 16.0f;
    float space_5 = 24.0f;
    float space_6 = 32.0f;

    float radius_xs = 4.0f;
    float radius_sm = 7.0f;
    float radius_md = 10.0f;
    float radius_lg = 14.0f;

    float control_h = 38.0f;
    float button_h = 40.0f;
    float row_art = 48.0f;
    float row_pad_x = 12.0f;
    float row_pad_y = 9.0f;
    float row_gap = 4.0f;
    float card_gap = 18.0f;
    float sidebar_w = 232.0f;
    float sidebar_rail_w = 68.0f;
    float header_h = 68.0f;
    float bottom_h = 92.0f;
    float content_pad = 24.0f;
    float lyrics_min = 300.0f;
    float lyrics_max = 520.0f;

    bool sidebar_collapsed = false;
};

struct UiColors
{
    ImVec4 bg = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 surface = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 surface_hover = ImVec4(0.035f, 0.035f, 0.04f, 1.0f);
    ImVec4 surface_active = ImVec4(0.055f, 0.055f, 0.06f, 1.0f);
    ImVec4 frame_bg = ImVec4(0.012f, 0.012f, 0.015f, 1.0f);
    ImVec4 frame_bg_hovered = ImVec4(0.03f, 0.03f, 0.035f, 1.0f);
    ImVec4 frame_bg_active = ImVec4(0.012f, 0.012f, 0.015f, 1.0f);
    // Overlay tint used for hover/selection washes: white on dark, black on light.
    ImVec4 overlay = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 border = ImVec4(0.12f, 0.12f, 0.135f, 1.0f);
    ImVec4 border_soft = ImVec4(0.075f, 0.075f, 0.085f, 1.0f);
    ImVec4 text = ImVec4(0.95f, 0.95f, 0.965f, 1.0f);
    ImVec4 text_dim = ImVec4(0.62f, 0.62f, 0.66f, 1.0f);
    ImVec4 text_muted = ImVec4(0.40f, 0.40f, 0.44f, 1.0f);
    ImVec4 accent = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 accent_hover = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 accent_active = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 accent_text = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    ImVec4 on_accent = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
};

extern UiFonts g_ui_fonts;

void ui_theme_init_fonts(ImGuiIO &io, const std::string &asset_dir);
void ui_theme_apply(const Settings &settings, const ImGuiIO &io);
void ui_theme_refresh_accent();
const UiMetrics &ui_m();
const UiColors &ui_c();

ImU32 ui_col(const ImVec4 &c);
ImU32 ui_accent_u32(float alpha = 1.0f);
ImU32 ui_accent_text_u32();
ImU32 ui_text_u32();
ImU32 ui_text_dim_u32();
ImU32 ui_text_muted_u32();
ImU32 ui_border_u32();
ImU32 ui_border_soft_u32();

// Glowing separators (Settings -> Appearance).
// `ui_glow_enabled` mirrors the toggle; `ui_glow_level` is its eased strength
// (0 when off), so switching the option fades instead of snapping.
bool ui_glow_enabled();
float ui_glow_level();

// Structural hairline colour: the soft border, tinted toward the ambient album
// colour while glowing separators are on. Falls back to `ui_border_soft_u32()`.
ImU32 ui_hairline_u32();
ImU32 ui_overlay_u32(float alpha = 1.0f);
ImVec4 ui_overlay_vec4(float alpha = 1.0f);
bool ui_theme_is_light();
float ui_theme_progress();
void ui_theme_tick();

// Multiply an existing packed colour's alpha by `alpha` (0..1). Useful for
// fading raw text/draw-list colours during row reveals and crossfades.
inline ImU32 ui_with_alpha(ImU32 col, float alpha)
{
    if (alpha <= 0.0f)
        return col & ~IM_COL32_A_MASK;
    if (alpha >= 1.0f)
        return col;
    const ImU32 a = (ImU32)(((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
    return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

float ui_scaled(float v);
float ui_font_size(float base);
void ui_push_regular(float base_size = 0.0f);
void ui_push_medium(float base_size = 0.0f);
void ui_push_semibold(float base_size = 0.0f);
void ui_push_micro();
void ui_push_small();
void ui_push_body();
void ui_push_heading();
void ui_push_title();
void ui_push_display();

float ui_measure_width(ImFont *font, float base_size, const std::string &text);
std::string ui_ellipsize(ImFont *font, float base_size, const std::string &text, float max_width);
bool ui_accent_is_light();
