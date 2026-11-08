#include "ui_hero.h"

#include "ui_theme.h"
#include "ui_backdrop.h"
#include "texture.h"
#include "power.h"

#include <algorithm>
#include <cmath>

int ui_hero(const std::string &eyebrow, const std::string &title,
            const std::string &subtitle, const std::string &art_url,
            UiIcon fallback, const char *art_scope,
            const std::vector<UiHeroChip> &chips,
            const std::vector<UiHeroAction> &actions,
            const UiPageAnim &anim, bool live_dot,
            int backdrop_style, float backdrop_strength)
{
    const UiMetrics &m = ui_m();
    const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const bool compact = avail < 620.0f * m.layout_scale;
    const bool narrow = avail < 460.0f * m.layout_scale;
    const float hero_h = (narrow ? 272.0f : 210.0f) * m.layout_scale;
    const float pad = m.space_4;
    float art_size = narrow ? std::min(avail - pad * 2.0f, avail * 0.30f)
                            : std::min(hero_h - pad * 2.0f, avail * (compact ? 0.18f : 0.30f));
    art_size = std::max(art_size, 44.0f * m.layout_scale);
    const float title_base = narrow ? 21.0f : (compact ? 24.5f : 32.0f);
    const float sub_base = narrow ? 13.5f : (compact ? 14.5f : 17.0f);
    const float text_gap = compact ? m.space_4 : m.space_5;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float art_x = narrow ? origin.x + (avail - art_size) * 0.5f : origin.x + pad;
    const float art_y = origin.y + pad;
    ImGui::Dummy(ImVec2(avail, hero_h));

    const float hero_t = anim.pop(0.0f, 0.62f);
    const float lift = (1.0f - hero_t) * ui_anim_offset(10.0f);
    const float scale = 0.965f + 0.035f * hero_t;
    const ImVec2 centre(origin.x + avail * 0.5f, origin.y + hero_h * 0.5f);
    auto C = [&](ImVec2 p) {
        return ImVec2(centre.x + (p.x - centre.x) * scale,
                      centre.y + (p.y - centre.y) * scale);
    };

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 art_min(art_x, art_y + lift);
    const ImVec2 art_max(art_x + art_size, art_y + art_size + lift);
    const ImVec2 art_centre((art_min.x + art_max.x) * 0.5f, (art_min.y + art_max.y) * 0.5f);
    dl->AddCircleFilled(art_centre, art_size * 0.78f, ui_accent_u32(0.045f * hero_t), 48);
    dl->AddCircleFilled(art_centre, art_size * 0.62f, ui_accent_u32(0.035f * hero_t), 40);

    const ImVec2 card_min = C(origin);
    const ImVec2 card_max = C(ImVec2(origin.x + avail, origin.y + hero_h));
    const float strength = std::max(0.0f, std::min(1.0f, backdrop_strength));

    if (backdrop_style == 1 && !power::low_power())
    {
        // Frosted glass: no cover image at all. The page's ambient backdrop
        // shows through a slightly tinted, lightly darkened pane, so this
        // reads as "blurred background" without any per-frame shader work.
        const ImVec4 tint = ui_backdrop_tint();
        dl->AddRectFilled(card_min, card_max,
                          IM_COL32(18, 18, 22, (int)((0.16f + 0.10f * strength) * hero_t * 255.0f)),
                          m.radius_lg);
        dl->AddRectFilled(card_min, card_max,
                          ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z,
                                                    0.10f * strength * hero_t)),
                          m.radius_lg);
        // A faint top highlight sells the glass edge.
        const float hl = 1.0f * m.layout_scale;
        dl->AddRectFilled(ImVec2(card_min.x + m.radius_lg * 0.5f, card_min.y + hl),
                          ImVec2(card_max.x - m.radius_lg * 0.5f, card_min.y + hl * 2.0f),
                          IM_COL32(255, 255, 255, (int)(0.035f * hero_t * 255.0f)), hl);
    }
    else if (backdrop_style >= 2 && !power::low_power() && !art_url.empty())
    {
        // Cover blur: one shader-generated texture per cover, cached and
        // reused. Per frame this is only a textured quad.
        ui_hero_blur_request(art_url);
        const GLuint blur = ui_hero_blur_texture(art_url);
        int bw = 0, bh = 0;
        const bool have_size = blur != 0 && ui_hero_blur_size(art_url, bw, bh);
        if (have_size)
        {
            dl->AddRectFilled(card_min, card_max,
                              ui_with_alpha(ui_col(ui_c().surface_hover), hero_t), m.radius_lg);
            const float image_a = 0.85f * strength * hero_t;
            const float scrim_a = 0.20f * strength * hero_t;
            float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
            const TextureInfo info{blur, bw, bh};
            const ImVec2 box(card_max.x - card_min.x, card_max.y - card_min.y);
            if (!cover_uv(info, box.x, box.y, u0, v0, u1, v1))
            {
                u0 = v0 = 0.0f;
                u1 = v1 = 1.0f;
            }
            dl->AddImageRounded((ImTextureID)(intptr_t)blur, card_min, card_max,
                                ImVec2(u0, v0), ImVec2(u1, v1),
                                IM_COL32(255, 255, 255, (int)(image_a * 255.0f)),
                                m.radius_lg);
            if (scrim_a > 0.002f)
                dl->AddRectFilled(card_min, card_max,
                                  IM_COL32(0, 0, 0, (int)(scrim_a * 255.0f)), m.radius_lg);
        }
        else
        {
            dl->AddRectFilled(card_min, card_max,
                              ui_with_alpha(ui_col(ui_c().surface_hover), hero_t), m.radius_lg);
        }
    }
    else
    {
        dl->AddRectFilled(card_min, card_max,
                          ui_with_alpha(ui_col(ui_c().surface_hover), hero_t), m.radius_lg);
    }
    dl->AddRectFilled(card_min, card_max, ui_accent_u32(0.040f * hero_t), m.radius_lg);
    dl->AddRect(card_min, card_max,
                ui_with_alpha(ui_border_u32(), (0.45f + 0.55f * hero_t) * hero_t),
                m.radius_lg, 0, 1.0f);

    const float elapsed = (float)(anim.now - anim.start);
    if (ui_animations_enabled() && elapsed >= 0.0f && elapsed < 4.8f)
    {
        for (int k = 0; k < 2; ++k)
        {
            const float phase = std::fmod(elapsed * 0.55f + (float)k * 0.5f, 1.0f);
            const float grow = (6.0f + 30.0f * phase) * m.layout_scale;
            const float a = 0.16f * (1.0f - phase) * hero_t;
            if (a > 0.005f)
                dl->AddRect(ImVec2(art_min.x - grow, art_min.y - grow),
                            ImVec2(art_max.x + grow, art_max.y + grow),
                            ui_accent_u32(a), m.radius_lg + grow, 0, 1.5f);
        }
        ui_anim_request();
    }

    dl->AddRectFilled(ImVec2(art_min.x + 3.0f * m.layout_scale, art_min.y + 4.0f * m.layout_scale),
                      ImVec2(art_max.x + 3.0f * m.layout_scale, art_max.y + 4.0f * m.layout_scale),
                      IM_COL32(0, 0, 0, (int)(120.0f * hero_t)), m.radius_lg);
    ui_draw_cover(dl, art_url, art_min, art_max, m.radius_lg, 1.0f, fallback,
                  art_scope, true, true);

    const float text_right = origin.x + avail - pad;
    const float text_x = narrow ? origin.x + pad
                                : std::min(art_max.x + text_gap, text_right - 1.0f);
    const float text_w = std::max(1.0f, text_right - text_x);
    const float centre_x = origin.x + avail * 0.5f;
    const float text_top = narrow ? (art_y + art_size + m.space_2 + lift)
                                  : (origin.y + pad + lift);
    const float title_y = text_top + 22.0f * m.layout_scale;
    const float title_line = title_base * m.text_scale * 1.20f;
    const float sub_y = title_y + title_line + 2.0f * m.layout_scale;
    const float sub_line = sub_base * m.text_scale * 1.24f;
    const float chips_y = sub_y + sub_line + m.space_3;
    const float chip_h = 24.0f * m.layout_scale;
    const float btn_h = m.button_h;
    const float buttons_y = origin.y + hero_h - pad - btn_h + lift;

    dl->PushClipRect(ImVec2(text_x - 2.0f, origin.y),
                     ImVec2(text_right + 2.0f, origin.y + hero_h), true);

    {
        const float fs = 12.0f;
        const float dot_r = 3.2f * m.layout_scale;
        const float dot_a = !ui_animations_enabled() ? 1.0f
                            : live_dot ? 0.55f + 0.45f * (0.5f + 0.5f * std::sin(elapsed * 3.0f))
                                       : 0.85f;
        float ex = text_x;
        if (narrow)
            ex = centre_x - (dot_r * 3.0f +
                             ui_measure_width(g_ui_fonts.medium, fs, eyebrow.c_str())) * 0.5f;
        dl->AddCircleFilled(ImVec2(ex + dot_r, text_top + 8.0f * m.layout_scale), dot_r,
                            ui_accent_u32(dot_a * hero_t), 16);
        dl->AddText(g_ui_fonts.medium, fs * m.text_scale,
                    ImVec2(ex + dot_r * 3.0f, text_top),
                    ui_with_alpha(ui_accent_text_u32(), 0.82f * hero_t), eyebrow.c_str());
    }

    const std::string title_draw = ui_ellipsize(g_ui_fonts.semibold, title_base, title, text_w);
    const float title_x = narrow ? std::max(text_x, centre_x -
                                                       ui_measure_width(g_ui_fonts.semibold,
                                                                        title_base, title_draw) * 0.5f)
                                 : text_x;
    dl->AddText(g_ui_fonts.semibold, title_base * m.text_scale, ImVec2(title_x, title_y),
                ui_with_alpha(ui_text_u32(), hero_t), title_draw.c_str());

    const std::string sub_draw = ui_ellipsize(g_ui_fonts.body, sub_base, subtitle, text_w);
    const float sub_x = narrow ? std::max(text_x, centre_x -
                                                   ui_measure_width(g_ui_fonts.body, sub_base,
                                                                    sub_draw) * 0.5f)
                               : text_x;
    dl->AddText(g_ui_fonts.body, sub_base * m.text_scale, ImVec2(sub_x, sub_y),
                ui_with_alpha(ui_text_dim_u32(), 0.9f * hero_t), sub_draw.c_str());

    if (chips_y + chip_h <= buttons_y - 2.0f * m.layout_scale)
    {
        float total_w = 0.0f;
        size_t fit = 0;
        std::vector<std::string> fitted(chips.size());
        for (size_t i = 0; i < chips.size(); ++i)
        {
            const float avail_w = std::max(60.0f, text_w - total_w -
                                                   (fit > 0 ? m.space_2 : 0.0f) -
                                                   20.0f * m.layout_scale);
            fitted[i] = ui_ellipsize(g_ui_fonts.body, 13.0f, chips[i].label, avail_w);
            const float cw = ui_measure_width(g_ui_fonts.body, 13.0f, fitted[i].c_str()) +
                             20.0f * m.layout_scale;
            const float add = cw + (fit > 0 ? m.space_2 : 0.0f);
            if (total_w + add > text_w)
                break;
            total_w += add;
            ++fit;
        }
        float cx = narrow ? centre_x - total_w * 0.5f : text_x;
        for (size_t i = 0; i < fit; ++i)
        {
            const float a = anim.stagger(0.16f + 0.08f * (float)i, 0.45f) * hero_t;
            cx += ui_meta_chip(dl, m, fitted[i].c_str(), ImVec2(cx, chips_y),
                               chips[i].accent, a) + m.space_2;
        }
    }
    dl->PopClipRect();

    const bool wide_actions = text_w >= 288.0f * m.layout_scale;
    std::vector<float> widths;
    float total_actions = 0.0f;
    size_t action_fit = 0;
    for (const UiHeroAction &a : actions)
    {
        const char *label = (!wide_actions && !a.compact_label.empty())
                                ? a.compact_label.c_str()
                                : a.label.c_str();
        const float w = std::max(118.0f * m.layout_scale,
                                 ui_measure_width(g_ui_fonts.medium, 16.0f, label) +
                                     48.0f * m.layout_scale);
        const float add = w + (action_fit > 0 ? m.space_2 : 0.0f);
        if (total_actions + add > text_w)
            break;
        widths.push_back(w);
        total_actions += add;
        ++action_fit;
    }

    float bx = wide_actions ? text_x : centre_x - total_actions * 0.5f;
    int clicked = -1;
    for (size_t i = 0; i < action_fit; ++i)
    {
        const char *label = (!wide_actions && !actions[i].compact_label.empty())
                                ? actions[i].compact_label.c_str()
                                : actions[i].label.c_str();
        if (ui_pill_button(("hero_action_" + std::to_string(i)).c_str(), label,
                           actions[i].icon, ImVec2(bx, buttons_y),
                           ImVec2(widths[i], btn_h), actions[i].primary, hero_t,
                           actions[i].enabled))
            clicked = (int)i;
        bx += widths[i] + m.space_2;
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + hero_h + m.space_5));
    return clicked;
}
