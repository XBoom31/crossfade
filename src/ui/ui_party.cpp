#include "ui.h"

#include "party.h"
#include "party_util.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_layout.h"
#include "ui_notify.h"
#include "ui_theme.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"
#include "qrcodegen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

// ----------------------------------------------------------------------
// Party mode — host UI
//
// The HTTP server, guest sessions and rate limits live in party.cpp and
// party_server.cpp. This file draws the host experience: a Radio-style hero,
// a live QR card, approvals and a guest-request summary. Configuration lives
// in Settings → Party so the Party tab stays a single, obvious action.
// ----------------------------------------------------------------------

namespace
{

// Mirrors Radio's motion clocks so the Party tab feels like the rest of the
// app. The page epoch is bumped on tab changes, so returning to Party always
// replays the entrance instead of depending on a frame-gap heuristic.
using PartyAnim = UiPageAnim;


void party_help(const char *text)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ui_c().text_muted);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextWrapped("%s", text ? text : "");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void party_gap()
{
    ImGui::Dummy(ImVec2(0.0f, ui_m().space_4));
}

std::string guest_count_label(int guests)
{
    return std::to_string(guests) + (guests == 1 ? " guest" : " guests");
}

float text_width(const char *text, float base_size = 18.0f)
{
    return ui_measure_width(g_ui_fonts.body, base_size, text ? text : "");
}

// White quiet zone plus dark modules, centred in a box. Drawing straight into
// ImDrawList avoids a texture upload and stays crisp at any scale.
float draw_qr_code_centered(ImDrawList *dl, const std::string &text, ImVec2 centre,
                            float max_size)
{
    if (text.empty() || max_size <= 0.0f)
        return 0.0f;
    try
    {
        qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int modules = qr.getSize();
        const int border = 4;
        const int total = modules + border * 2;
        const float px = std::floor(max_size / (float)total);
        if (px < 2.0f)
            return 0.0f;
        const float side = px * (float)total;
        const ImVec2 top_left(centre.x - side * 0.5f, centre.y - side * 0.5f);

        dl->AddRectFilled(top_left, ImVec2(top_left.x + side, top_left.y + side),
                          IM_COL32(255, 255, 255, 255), ui_m().radius_sm);
        const ImU32 dark = IM_COL32(10, 12, 16, 255);
        for (int y = 0; y < modules; ++y)
        {
            for (int x = 0; x < modules; ++x)
            {
                if (!qr.getModule(x, y))
                    continue;
                const ImVec2 a(top_left.x + (float)(x + border) * px,
                               top_left.y + (float)(y + border) * px);
                dl->AddRectFilled(a, ImVec2(a.x + px, a.y + px), dark);
            }
        }
        return side;
    }
    catch (const std::exception &)
    {
        return 0.0f;
    }
    catch (...)
    {
        return 0.0f;
    }
}

void draw_live_dot(ImDrawList *dl, ImVec2 centre, float alpha, bool live, double now)
{
    const float r = 3.4f * ui_m().layout_scale;
    const float pulse = live ? 0.62f + 0.38f * (0.5f + 0.5f * std::sin((float)now * 3.0f))
                             : 1.0f;
    dl->AddCircleFilled(centre, r, ui_accent_u32(alpha * pulse), 16);
    if (live)
        dl->AddCircle(centre, r * 2.5f, ui_accent_u32(alpha * 0.18f), 20, 1.2f);
}


void draw_party_orb(ImDrawList *dl, ImVec2 centre, float size, float alpha, double now)
{
    const UiMetrics &m = ui_m();
    const float half = size * 0.5f;

    dl->AddCircleFilled(centre, size * 0.66f, ui_accent_u32(0.05f * alpha), 64);
    dl->AddCircleFilled(centre, size * 0.50f, ui_accent_u32(0.035f * alpha), 64);

    if (ui_animations_enabled())
    {
        for (int k = 0; k < 2; ++k)
        {
            const float phase = std::fmod((float)now * 0.45f + (float)k * 0.5f, 1.0f);
            const float grow = (6.0f + 26.0f * phase) * m.layout_scale;
            const float a = 0.15f * (1.0f - phase) * alpha;
            if (a > 0.004f)
                dl->AddRect(ImVec2(centre.x - half - grow, centre.y - half - grow),
                            ImVec2(centre.x + half + grow, centre.y + half + grow),
                            ui_accent_u32(a), m.radius_lg + grow, 0, 1.4f);
        }
    }

    const ImVec2 mn(centre.x - half, centre.y - half);
    const ImVec2 mx(centre.x + half, centre.y + half);
    dl->AddRectFilled(ImVec2(mn.x + 3.0f, mn.y + 4.0f), ImVec2(mx.x + 3.0f, mx.y + 4.0f),
                      IM_COL32(0, 0, 0, (int)(110.0f * alpha)), m.radius_lg);
    dl->AddRectFilled(mn, mx, ui_with_alpha(ui_col(ui_c().surface_hover), alpha), m.radius_lg);
    dl->AddRectFilled(mn, mx, ui_accent_u32(0.055f * alpha), m.radius_lg);
    dl->AddRect(mn, mx, ui_with_alpha(ui_border_u32(), alpha), m.radius_lg, 0, 1.0f);
    ui_draw_icon(dl, UiIcon::Party, centre, size * 0.44f,
                 ui_with_alpha(ui_accent_text_u32(), alpha), 2.0f);
}

// Shared hero shell: a lifted, slightly scaled card that settles into place.
void draw_hero_shell(ImDrawList *dl, ImVec2 origin, float avail, float hero_h, float t)
{
    const UiMetrics &m = ui_m();
    const float lift = (1.0f - t) * ui_anim_offset(12.0f);
    const float scale = 0.972f + 0.028f * t;
    const ImVec2 centre(origin.x + avail * 0.5f, origin.y + hero_h * 0.5f);
    auto C = [&](ImVec2 p) {
        return ImVec2(centre.x + (p.x - centre.x) * scale,
                      centre.y + (p.y - centre.y) * scale);
    };
    const ImVec2 card_min = C(ImVec2(origin.x, origin.y + lift));
    const ImVec2 card_max = C(ImVec2(origin.x + avail, origin.y + hero_h + lift));
    dl->AddRectFilled(card_min, card_max,
                      ui_with_alpha(ui_col(ui_c().surface_hover), t), m.radius_lg);
    dl->AddRectFilled(card_min, card_max, ui_accent_u32(0.035f * t), m.radius_lg);
    dl->AddRect(card_min, card_max,
                ui_with_alpha(ui_border_u32(), (0.4f + 0.6f * t) * t),
                m.radius_lg, 0, 1.0f);
}

void draw_stopped_hero(AppState &state, const PartyAnim &a, const PartyStatus &st)
{
    (void)state;
    const UiMetrics &m = ui_m();
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool narrow = avail < 560.0f * m.layout_scale;
    const float pad = m.space_5;
    const float tile = narrow ? std::min(avail - pad * 2.0f, 132.0f * m.layout_scale)
                              : std::min(176.0f * m.layout_scale, avail * 0.22f);
    const float hero_h = (narrow ? 430.0f : 248.0f) * m.layout_scale;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(avail, hero_h));
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const float t = a.pop(0.0f, 0.62f);
    const float lift = (1.0f - t) * ui_anim_offset(12.0f);
    draw_hero_shell(dl, origin, avail, hero_h, t);

    const float art_cx = narrow ? origin.x + avail * 0.5f
                                : origin.x + pad + tile * 0.5f;
    const float art_cy = narrow ? origin.y + pad + tile * 0.5f + lift
                                : origin.y + hero_h * 0.5f + lift;
    draw_party_orb(dl, ImVec2(art_cx, art_cy), tile, t, a.now);

    const float text_x = narrow ? origin.x + pad : origin.x + pad + tile + m.space_5;
    const float text_right = origin.x + avail - pad;
    const float text_w = std::max(1.0f, text_right - text_x);
    const float text_top = narrow ? (origin.y + pad + tile + m.space_4 + lift)
                                  : (origin.y + pad + lift);
    const float centre_x = origin.x + avail * 0.5f;

    const float title_base = narrow ? 26.0f : 31.0f;
    const float title_line = title_base * m.text_scale * 1.18f;
    const float eyebrow_y = text_top;
    const float title_y = eyebrow_y + 22.0f * m.layout_scale;
    const float sub_y = title_y + title_line + 3.0f * m.layout_scale;
    const float sub_base = 15.0f;
    const float sub_line = sub_base * m.text_scale * 1.30f;
    const float chips_y = sub_y + sub_line * 2.0f + m.space_3;
    const float btn_h = m.button_h;
    const float btn_y = origin.y + hero_h - pad - btn_h + lift;

    auto centred_x = [&](ImFont *font, float base, const std::string &s) {
        if (!narrow)
            return text_x;
        return std::max(text_x, centre_x - ui_measure_width(font, base, s) * 0.5f);
    };

    const std::string eyebrow = "LISTEN TOGETHER";
    {
        const float fs = 12.0f * m.text_scale;
        const float dot_gap = 12.0f * m.layout_scale;
        const float group = dot_gap + ui_measure_width(g_ui_fonts.medium, 12.0f, eyebrow);
        const float ex = narrow ? centre_x - group * 0.5f : text_x;
        draw_live_dot(dl, ImVec2(ex + 3.5f * m.layout_scale, eyebrow_y + fs * 0.5f), t,
                      false, a.now);
        dl->AddText(g_ui_fonts.medium, fs, ImVec2(ex + dot_gap, eyebrow_y),
                    ui_with_alpha(ui_accent_text_u32(), 0.86f * t), eyebrow.c_str());
    }

    const std::string title = ui_ellipsize(g_ui_fonts.semibold, title_base,
                                           "Start a party", text_w);
    dl->AddText(g_ui_fonts.semibold, title_base * m.text_scale,
                ImVec2(centred_x(g_ui_fonts.semibold, title_base, title), title_y),
                ui_with_alpha(ui_text_u32(), t), title.c_str());

    const std::string sub = "Guests scan a QR code and add songs to Up Next.";
    const std::string sub_draw = ui_ellipsize(g_ui_fonts.body, sub_base, sub, text_w);
    dl->AddText(g_ui_fonts.body, sub_base * m.text_scale,
                ImVec2(centred_x(g_ui_fonts.body, sub_base, sub_draw), sub_y),
                ui_with_alpha(ui_text_dim_u32(), 0.92f * t), sub_draw.c_str());

    // Meta chips, packed from the left (or centred above the buttons when the
    // artwork sits on top).
    {
        struct Chip
        {
            std::string label;
            bool accent;
        };
        const Chip chips[] = {{"SAME WI-FI", true}, {"NO ACCOUNT", false}, {"NO CLOUD", false}};
        float total = 0.0f;
        size_t fit = 0;
        for (const Chip &c : chips)
        {
            const float cw = ui_measure_width(g_ui_fonts.body, 13.0f, c.label) +
                             20.0f * m.layout_scale;
            const float add = cw + (fit > 0 ? m.space_2 : 0.0f);
            if (total + add > text_w)
                break;
            total += add;
            ++fit;
        }
        float cx = narrow ? centre_x - total * 0.5f : text_x;
        for (size_t i = 0; i < fit; ++i)
        {
            const float cw = ui_meta_chip(dl, m, chips[i].label.c_str(),
                                             ImVec2(cx, chips_y), chips[i].accent, t);
            cx += cw + m.space_2;
        }
    }

    const float start_w = 160.0f * m.layout_scale;
    const float settings_w = 166.0f * m.layout_scale;
    const float buttons_w = start_w + settings_w + m.space_2;
    const float buttons_x = narrow ? centre_x - buttons_w * 0.5f : text_x;
    if (ui_pill_button("party_start", "Start party", UiIcon::Party,
                       ImVec2(buttons_x, btn_y), ImVec2(start_w, btn_h), true, t, true))
    {
        std::string error;
        if (party_start(state.settings.party, error))
        {
            state.party_fullscreen = false;
            state.repaint_requested = true;
            ui_notify(UiNotifyType::Success, "Party is live",
                      "The QR code is ready. Guests can scan it now.", 4.0f);
        }
        else
        {
            state.repaint_requested = true;
            ui_notify(UiNotifyType::Error, "Couldn't start the party",
                      error.empty() ? "Check the network address and port." : error, 7.0f);
        }
    }
    if (ui_pill_button("party_settings", "Party settings", UiIcon::Settings,
                       ImVec2(buttons_x + start_w + m.space_2, btn_y),
                       ImVec2(settings_w, btn_h), false, t, true))
    {
        state.settings_focus_party = true;
        state.current_tab = Tab::Settings;
        state.repaint_requested = true;
        ui_anim_request();
    }

    (void)st;
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + hero_h + m.space_5));
}

void draw_running_hero(AppState &state, const PartyAnim &a, const PartyStatus &st)
{
    const UiMetrics &m = ui_m();
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool narrow = avail < 620.0f * m.layout_scale;
    const float pad = m.space_5;
    const float tile = narrow ? std::min(avail - pad * 2.0f, 210.0f * m.layout_scale)
                              : std::min(196.0f * m.layout_scale, avail * 0.26f);
    const float hero_h = (narrow ? 500.0f : 278.0f) * m.layout_scale;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(avail, hero_h));
    ImDrawList *dl = ImGui::GetWindowDrawList();

    const float t = a.pop(0.0f, 0.62f);
    const float lift = (1.0f - t) * ui_anim_offset(12.0f);
    draw_hero_shell(dl, origin, avail, hero_h, t);

    const float art_cx = narrow ? origin.x + avail * 0.5f
                                : origin.x + pad + tile * 0.5f;
    const float art_cy = narrow ? origin.y + pad + tile * 0.5f + lift
                                : origin.y + hero_h * 0.5f + lift;
    const ImVec2 art_centre(art_cx, art_cy);

    // Glow and card behind the QR.
    dl->AddCircleFilled(art_centre, tile * 0.68f, ui_accent_u32(0.05f * t), 64);
    dl->AddCircleFilled(art_centre, tile * 0.52f, ui_accent_u32(0.035f * t), 64);
    const ImVec2 panel_min(art_centre.x - tile * 0.5f, art_centre.y - tile * 0.5f);
    const ImVec2 panel_max(art_centre.x + tile * 0.5f, art_centre.y + tile * 0.5f);
    dl->AddRectFilled(ImVec2(panel_min.x + 3.0f, panel_min.y + 4.0f),
                      ImVec2(panel_max.x + 3.0f, panel_max.y + 4.0f),
                      IM_COL32(0, 0, 0, (int)(120.0f * t)), m.radius_lg);
    dl->AddRectFilled(panel_min, panel_max,
                      ui_with_alpha(ui_col(ui_c().surface_hover), t), m.radius_lg);
    dl->AddRect(panel_min, panel_max, ui_with_alpha(ui_border_u32(), t), m.radius_lg, 0,
                1.0f);

    if (ui_animations_enabled())
    {
        for (int k = 0; k < 2; ++k)
        {
            const float phase = std::fmod((float)a.now * 0.45f + (float)k * 0.5f, 1.0f);
            const float grow = (6.0f + 26.0f * phase) * m.layout_scale;
            const float alpha = 0.15f * (1.0f - phase) * t;
            if (alpha > 0.004f)
                dl->AddRect(ImVec2(panel_min.x - grow, panel_min.y - grow),
                            ImVec2(panel_max.x + grow, panel_max.y + grow),
                            ui_accent_u32(alpha), m.radius_lg + grow, 0, 1.4f);
        }
    }

    const float inset = 12.0f * m.layout_scale;
    const float max_qr = tile - inset * 2.0f;
    const float side = draw_qr_code_centered(dl, st.url, art_centre, max_qr);
    if (side > 0.0f && ui_animations_enabled())
    {
        // A soft scanning highlight, clipped to the quiet zone so the modules
        // stay readable. The fullscreen view keeps the code completely clean.
        const ImVec2 qr_min(art_centre.x - side * 0.5f, art_centre.y - side * 0.5f);
        const float phase = std::fmod((float)a.now * 0.35f, 1.0f);
        const float y = qr_min.y + side * phase;
        dl->PushClipRect(qr_min, ImVec2(qr_min.x + side, qr_min.y + side), true);
        dl->AddRectFilled(ImVec2(qr_min.x, y - 10.0f), ImVec2(qr_min.x + side, y + 10.0f),
                          ui_accent_u32(0.10f * t));
        dl->AddRectFilled(ImVec2(qr_min.x, y - 0.7f), ImVec2(qr_min.x + side, y + 0.7f),
                          ui_accent_u32(0.45f * t));
        dl->PopClipRect();
        ui_anim_request();
    }

    const float text_x = narrow ? origin.x + pad : origin.x + pad + tile + m.space_5;
    const float text_right = origin.x + avail - pad;
    const float text_w = std::max(1.0f, text_right - text_x);
    const float text_top = narrow ? (origin.y + pad + tile + m.space_4 + lift)
                                  : (origin.y + pad + lift);
    const float centre_x = origin.x + avail * 0.5f;

    const float title_base = narrow ? 25.0f : 30.0f;
    const float title_line = title_base * m.text_scale * 1.18f;
    const float eyebrow_y = text_top;
    const float title_y = eyebrow_y + 22.0f * m.layout_scale;
    const float sub_y = title_y + title_line + 3.0f * m.layout_scale;
    const float sub_base = 15.0f;
    const float sub_line = sub_base * m.text_scale * 1.30f;
    const float url_y = sub_y + sub_line + 3.0f * m.layout_scale;
    const float chips_y = url_y + sub_line + m.space_3;
    const float btn_h = m.button_h;
    const float btn_y = origin.y + hero_h - pad - btn_h + lift;

    auto centred_x = [&](ImFont *font, float base, const std::string &s) {
        if (!narrow)
            return text_x;
        return std::max(text_x, centre_x - ui_measure_width(font, base, s) * 0.5f);
    };

    const std::string eyebrow = "PARTY LIVE";
    {
        const float fs = 12.0f * m.text_scale;
        const float dot_gap = 12.0f * m.layout_scale;
        const float group = dot_gap + ui_measure_width(g_ui_fonts.medium, 12.0f, eyebrow);
        const float ex = narrow ? centre_x - group * 0.5f : text_x;
        draw_live_dot(dl, ImVec2(ex + 3.5f * m.layout_scale, eyebrow_y + fs * 0.5f), t,
                      true, a.now);
        dl->AddText(g_ui_fonts.medium, fs, ImVec2(ex + dot_gap, eyebrow_y),
                    ui_with_alpha(ui_accent_text_u32(), 0.86f * t), eyebrow.c_str());
    }

    const PartySettings &ps = party_active_settings();
    const std::string title =
        ui_ellipsize(g_ui_fonts.semibold, title_base, ps.name, text_w);
    dl->AddText(g_ui_fonts.semibold, title_base * m.text_scale,
                ImVec2(centred_x(g_ui_fonts.semibold, title_base, title), title_y),
                ui_with_alpha(ui_text_u32(), t), title.c_str());

    const std::string sub = st.guests == 0
                                ? "Waiting for the first guest to scan"
                                : guest_count_label(st.guests) + " connected";
    const std::string sub_draw = ui_ellipsize(g_ui_fonts.body, sub_base, sub, text_w);
    dl->AddText(g_ui_fonts.body, sub_base * m.text_scale,
                ImVec2(centred_x(g_ui_fonts.body, sub_base, sub_draw), sub_y),
                ui_with_alpha(ui_text_dim_u32(), 0.92f * t), sub_draw.c_str());

    const std::string url_draw =
        ui_ellipsize(g_ui_fonts.body, 13.0f, st.url, text_w);
    dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale,
                ImVec2(centred_x(g_ui_fonts.body, 13.0f, url_draw), url_y),
                ui_with_alpha(ui_text_muted_u32(), 0.95f * t), url_draw.c_str());

    {
        struct Chip
        {
            std::string label;
            bool accent;
        };
        const Chip chips[] = {{"SCAN TO JOIN", true},
                              {ps.require_approval ? "APPROVAL ON" : "OPEN QUEUE", false},
                              {"LAN ONLY", false}};
        float total = 0.0f;
        size_t fit = 0;
        for (const Chip &c : chips)
        {
            const float cw = ui_measure_width(g_ui_fonts.body, 13.0f, c.label) +
                             20.0f * m.layout_scale;
            const float add = cw + (fit > 0 ? m.space_2 : 0.0f);
            if (total + add > text_w)
                break;
            total += add;
            ++fit;
        }
        float cx = narrow ? centre_x - total * 0.5f : text_x;
        for (size_t i = 0; i < fit; ++i)
        {
            const float cw = ui_meta_chip(dl, m, chips[i].label.c_str(),
                                             ImVec2(cx, chips_y), chips[i].accent, t);
            cx += cw + m.space_2;
        }
    }

    const float copy_w = 146.0f * m.layout_scale;
    const float present_w = 158.0f * m.layout_scale;
    const float end_w = 132.0f * m.layout_scale;
    const float buttons_w = copy_w + present_w + end_w + m.space_2 * 2.0f;
    const float buttons_x = narrow ? centre_x - buttons_w * 0.5f : text_x;
    if (ui_pill_button("party_copy", "Copy link", UiIcon::Check,
                       ImVec2(buttons_x, btn_y), ImVec2(copy_w, btn_h), true, t, true))
    {
        ImGui::SetClipboardText(st.url.c_str());
        ui_notify(UiNotifyType::Success, "Party link copied",
                  "Paste it into a message if a guest can't scan the code.", 4.0f);
    }
    if (ui_pill_button("party_present", "Present QR", UiIcon::Qr,
                       ImVec2(buttons_x + copy_w + m.space_2, btn_y),
                       ImVec2(present_w, btn_h), false, t, true))
    {
        state.party_fullscreen = true;
        state.repaint_requested = true;
        ui_anim_request();
    }
    if (ui_pill_button("party_end", "End party", UiIcon::Close,
                       ImVec2(buttons_x + copy_w + present_w + m.space_2 * 2.0f, btn_y),
                       ImVec2(end_w, btn_h), false, t, true))
    {
        party_stop();
        state.party_fullscreen = false;
        state.repaint_requested = true;
        ui_anim_request();
        ui_notify(UiNotifyType::Info, "Party ended",
                  "The guest link and every session are no longer valid.", 4.0f);
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + hero_h + m.space_5));
}

void draw_pending_requests(AppState &state, const PartyAnim &a)
{
    const UiMetrics &m = ui_m();
    std::vector<PartyPendingRequest> pending = party_pending_requests();
    if (pending.empty())
        return;

    party_gap();
    const float ht = a.stagger(0.12f, 0.36f);
    const std::string meta = std::to_string(pending.size()) + " waiting";
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ht);
    if (ui_section_header("Waiting for approval", meta.c_str(),
                          pending.size() > 1 ? "Approve all" : nullptr))
        party_approve_all();
    ImGui::PopStyleVar();

    const float pad = m.space_4;
    const float art_sz = 46.0f * m.layout_scale;
    const float btn_w = 96.0f * m.layout_scale;
    const float btn_h = m.control_h;
    const float row_h = std::max(76.0f * m.layout_scale, art_sz + pad * 2.0f);

    for (int i = 0; i < (int)pending.size(); ++i)
    {
        const PartyPendingRequest &p = pending[i];
        const float rt = a.stagger(0.16f + (float)i * 0.045f, 0.34f);
        const float lift = (1.0f - rt) * ui_anim_offset(8.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(avail, row_h));

        ImGui::PushID(p.request_id.c_str());
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##pending_row", ImVec2(avail, row_h));
        const bool hovered = ImGui::IsItemHovered();

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 mn(origin.x, origin.y + lift);
        const ImVec2 mx(origin.x + avail, origin.y + row_h + lift);
        dl->AddRectFilled(mn, mx,
                          ui_with_alpha(ui_col(hovered ? ui_c().surface_active
                                                       : ui_c().surface_hover),
                                        rt),
                          m.radius_md);
        dl->AddRect(mn, mx,
                    ui_with_alpha(hovered ? ui_accent_u32(0.38f) : ui_border_u32(), rt),
                    m.radius_md, 0, 1.0f);

        const ImVec2 art_min(origin.x + pad, origin.y + (row_h - art_sz) * 0.5f + lift);
        const ImVec2 art_max(art_min.x + art_sz, art_min.y + art_sz);
        std::string thumb;
        if (!p.id.empty())
            thumb = "https://img.youtube.com/vi/" + p.id + "/hqdefault.jpg";
        ui_draw_cover(dl, thumb, art_min, art_max, m.radius_sm, rt, UiIcon::Album,
                      "party", true);

        const float buttons_x = origin.x + avail - pad - btn_w * 2.0f - m.space_2;
        const float text_x = art_max.x + pad;
        const float text_w = std::max(40.0f, buttons_x - text_x - pad);
        const std::string title =
            ui_ellipsize(g_ui_fonts.medium, 17.0f,
                         p.title.empty() ? "Unknown title" : p.title, text_w);
        const float title_y = origin.y + pad * 0.75f + lift;
        dl->AddText(g_ui_fonts.medium, 17.0f * m.text_scale, ImVec2(text_x, title_y),
                    ui_with_alpha(ui_text_u32(), rt), title.c_str());

        std::string sub = p.artist.empty() ? "Unknown artist" : p.artist;
        if (!p.by.empty())
            sub += "  \xC2\xB7  " + p.by;
        if (p.duration_sec > 0)
            sub += "  \xC2\xB7  " + ui_format_duration(p.duration_sec);
        sub = ui_ellipsize(g_ui_fonts.body, 13.5f, sub, text_w);
        dl->AddText(g_ui_fonts.body, 13.5f * m.text_scale,
                    ImVec2(text_x, title_y + 23.0f * m.layout_scale),
                    ui_with_alpha(ui_text_dim_u32(), 0.92f * rt), sub.c_str());

        const float by = origin.y + (row_h - btn_h) * 0.5f + lift;
        if (ui_pill_button("party_approve", "Approve", UiIcon::Check,
                           ImVec2(buttons_x, by), ImVec2(btn_w, btn_h), true, rt, true))
            party_approve(p.request_id);
        if (ui_pill_button("party_reject", "Reject", UiIcon::Close,
                           ImVec2(buttons_x + btn_w + m.space_2, by),
                           ImVec2(btn_w, btn_h), false, rt, true))
            party_reject(p.request_id);

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + row_h + m.space_2));
        ImGui::PopID();
    }
}

void draw_guest_queue(AppState &state, const PartyAnim &a)
{
    const UiMetrics &m = ui_m();
    int guest_items = 0;
    for (const Track &t : state.play_queue)
        if (t.from_party)
            ++guest_items;
    if (guest_items == 0)
        return;

    party_gap();
    const float ht = a.stagger(0.24f, 0.36f);
    const std::string meta = std::to_string(guest_items) +
                             (guest_items == 1 ? " song" : " songs");
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ht);
    if (ui_section_header("Guest requests", meta.c_str(), "Open Up Next"))
    {
        state.current_tab = Tab::Queue;
        state.repaint_requested = true;
        ui_anim_request();
    }
    ImGui::PopStyleVar();

    const float pad = m.space_3;
    const float art_sz = 40.0f * m.layout_scale;
    const float row_h = std::max(60.0f * m.layout_scale, art_sz + pad * 2.0f);

    int shown = 0;
    for (const Track &t : state.play_queue)
    {
        if (!t.from_party)
            continue;
        if (shown >= 6)
            break;
        const float rt = a.stagger(0.26f + (float)shown * 0.04f, 0.34f);
        const float lift = (1.0f - rt) * ui_anim_offset(7.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::Dummy(ImVec2(avail, row_h));

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 mn(origin.x, origin.y + lift);
        const ImVec2 mx(origin.x + avail, origin.y + row_h + lift);
        dl->AddRectFilled(mn, mx,
                          ui_with_alpha(ui_col(ui_c().surface_hover), 0.72f * rt),
                          m.radius_sm);

        const ImVec2 art_min(origin.x + pad, origin.y + (row_h - art_sz) * 0.5f + lift);
        const ImVec2 art_max(art_min.x + art_sz, art_min.y + art_sz);
        const std::string thumb =
            t.thumbnail_url.empty() && !t.id.empty()
                ? "https://img.youtube.com/vi/" + t.id + "/hqdefault.jpg"
                : t.thumbnail_url;
        ui_draw_cover(dl, thumb, art_min, art_max, m.radius_xs, rt, UiIcon::Album,
                      "party", true);

        const float text_x = art_max.x + pad;
        const float right_w = 92.0f * m.layout_scale;
        const float text_w = std::max(40.0f, origin.x + avail - pad - right_w - text_x);
        const std::string title =
            ui_ellipsize(g_ui_fonts.medium, 16.0f,
                         t.title.empty() ? "Unknown title" : t.title, text_w);
        const float title_y = origin.y + pad + lift;
        dl->AddText(g_ui_fonts.medium, 16.0f * m.text_scale, ImVec2(text_x, title_y),
                    ui_with_alpha(ui_text_u32(), rt), title.c_str());

        std::string sub = t.artist.empty() ? "Unknown artist" : t.artist;
        if (!t.requested_by.empty())
            sub += "  \xC2\xB7  " + t.requested_by;
        sub = ui_ellipsize(g_ui_fonts.body, 13.0f, sub, text_w);
        dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale,
                    ImVec2(text_x, title_y + 21.0f * m.layout_scale),
                    ui_with_alpha(ui_text_dim_u32(), 0.9f * rt), sub.c_str());

        const std::string right = "in Up Next";
        dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale,
                    ImVec2(origin.x + avail - pad - ui_measure_width(g_ui_fonts.body, 13.0f, right),
                           origin.y + (row_h - 13.0f * m.text_scale) * 0.5f + lift),
                    ui_with_alpha(ui_text_muted_u32(), 0.92f * rt), right.c_str());

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + row_h + m.space_1));
        ++shown;
    }
    if (guest_items > shown)
    {
        ui_push_small();
        ImGui::TextColored(ui_c().text_muted, "and %d more in the queue",
                           guest_items - shown);
        ImGui::PopFont();
    }
}

void draw_how_it_works(const PartyAnim &a)
{
    party_gap();
    const float ht = a.stagger(0.20f, 0.4f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ht);
    ui_section_header("How it works", nullptr);
    party_help("1. Click Start party. The QR code appears right here, ready to scan.");
    party_help("2. Guests scan it, paste a YouTube or YouTube Music link, and optionally set a name.");
    party_help("3. Approve requests if you want, or let them join Up Next automatically.");
    party_help("4. Press P any time to reopen this page. Ending the party invalidates every guest link.");
    ImGui::PopStyleVar();
}

void draw_connection_help(const PartyAnim &a, const PartyStatus &st)
{
    party_gap();
    const float ht = a.stagger(0.24f, 0.4f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ht);
    ui_section_header("Can't connect?", nullptr);
    party_help("Everyone must be on the same Wi-Fi. Hotspots and public networks often isolate clients, so the hotspot owner can reach the laptop while another phone cannot. Use a normal router, make this laptop the hotspot, or turn off client isolation if your network exposes it.");
    party_help("Android 16 and newer can block local-network access for browsers. If Chrome shows ERR_TOO_MANY_RETRIES, allow Nearby devices for Chrome (App info - Permissions) or open the link in Firefox.");
    if (st.state == PartyRunState::Running && !st.ip.empty() && st.port > 0)
    {
        const std::string probe =
            "http://" + st.ip + ":" + std::to_string(st.port) + "/healthz";
        party_help(("Quick test from the phone: open " + probe +
                    " - it should say ok. If that fails, the phone cannot reach this laptop yet.")
                       .c_str());
    }
    ImGui::PopStyleVar();
}

} // namespace

void draw_party_panel(AppState &state)
{
    const UiMetrics &m = ui_m();
    const PartyStatus st = party_status();
    const double now = ImGui::GetTime();

    static uint64_t s_epoch = ~0ull;
    static PartyRunState s_state = PartyRunState::Stopped;
    static double s_start = 0.0;
    const uint64_t epoch = ui_page_epoch();
    if (s_epoch != epoch || s_state != st.state)
    {
        s_epoch = epoch;
        s_state = st.state;
        s_start = now;
    }
    PartyAnim a;
    a.now = now;
    a.start = s_start;

    if (st.state == PartyRunState::Running)
    {
        draw_running_hero(state, a, st);
        draw_pending_requests(state, a);
        draw_guest_queue(state, a);
        draw_connection_help(a, st);
    }
    else
    {
        draw_stopped_hero(state, a, st);
        if (st.state == PartyRunState::Error && !st.error.empty())
        {
            party_gap();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
            ImGui::TextWrapped("%s", st.error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        draw_how_it_works(a);
        draw_connection_help(a, st);
    }

    if (ui_animations_enabled() && now - s_start < 6.0)
        ui_anim_request();
    if (st.state == PartyRunState::Running && ui_animations_enabled())
        ui_anim_request(); // keeps the live dot and scan highlight moving
    ImGui::Dummy(ImVec2(0.0f, m.space_5));
}

void draw_party_fullscreen(AppState &state)
{
    static bool s_open = false;
    const bool want = state.party_fullscreen && party_running();

    if (want && !s_open)
    {
        ImGui::OpenPopup("##party_qr_fullscreen");
        s_open = true;
        ui_anim_request();
    }
    if (!s_open)
        return;

    ImGuiIO &io = ImGui::GetIO();
    const UiMetrics &m = ui_m();
    const PartyStatus st = party_status();
    const double now = ImGui::GetTime();
    const bool running = st.state == PartyRunState::Running && !st.url.empty();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui_c().bg);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ui_c().bg);
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));

    bool close = !want || !running;
    if (ImGui::BeginPopupModal("##party_qr_fullscreen", nullptr,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings))
    {
        const ImVec2 wp = ImGui::GetWindowPos();
        const ImVec2 ws = ImGui::GetWindowSize();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float w = ws.x;
        const float h = ws.y;
        const float pad = m.content_pad * 1.5f;

        dl->AddRectFilled(wp, ImVec2(wp.x + w, wp.y + h), ui_col(ui_c().bg));

        const float glow = std::min(w, h);
        dl->AddCircleFilled(ImVec2(w * 0.5f, h * 0.48f), glow * 0.44f,
                            ui_accent_u32(0.028f), 96);
        dl->AddCircleFilled(ImVec2(w * 0.5f, h * 0.48f), glow * 0.30f,
                            ui_accent_u32(0.024f), 96);

        ImGui::SetCursorScreenPos(ImVec2(w - pad - m.control_h, pad));
        if (ui_icon_button("##party_fs_close", UiIcon::Close, m.control_h, false, 0,
                           "Close (Esc)"))
            close = true;

        const std::string guests = guest_count_label(st.guests);
        ui_meta_chip(dl, m, guests.c_str(), ImVec2(pad, pad + m.space_2), true, 1.0f);

        const PartySettings &ps = party_active_settings();
        const float display_base = std::min(38.0f, w * 0.045f);
        const std::string title =
            ui_ellipsize(g_ui_fonts.semibold, display_base, ps.name, w - pad * 2.0f);
        const float title_w = ui_measure_width(g_ui_fonts.semibold, display_base, title);
        const float title_y = pad + m.space_5;
        dl->AddText(g_ui_fonts.semibold, display_base * m.text_scale,
                    ImVec2((w - title_w) * 0.5f, title_y), ui_text_u32(), title.c_str());

        const char *eyebrow = "SCAN TO JOIN";
        const float eyebrow_fs = 12.0f * m.text_scale;
        const float dot_gap = 12.0f * m.layout_scale;
        const float eyebrow_w =
            dot_gap + ui_measure_width(g_ui_fonts.medium, 12.0f, eyebrow);
        const float ex = (w - eyebrow_w) * 0.5f;
        const float eyebrow_y = title_y + display_base * m.text_scale * 1.2f + m.space_2;
        draw_live_dot(dl, ImVec2(ex + 3.5f * m.layout_scale,
                                 eyebrow_y + eyebrow_fs * 0.5f),
                      1.0f, true, now);
        dl->AddText(g_ui_fonts.medium, eyebrow_fs,
                    ImVec2(ex + dot_gap, eyebrow_y),
                    ui_with_alpha(ui_accent_text_u32(), 0.86f), eyebrow);

        const float qr_centre_y = h * 0.50f;
        const float max_qr = std::min(w * 0.52f, h * 0.50f);
        const float side = draw_qr_code_centered(dl, st.url, ImVec2(w * 0.5f, qr_centre_y),
                                                 max_qr);

        const std::string url_draw =
            ui_ellipsize(g_ui_fonts.body, 15.0f, st.url, w - pad * 2.0f);
        const float url_w = ui_measure_width(g_ui_fonts.body, 15.0f, url_draw);
        const float url_y = qr_centre_y + side * 0.5f + m.space_5;
        dl->AddText(g_ui_fonts.body, 15.0f * m.text_scale,
                    ImVec2((w - url_w) * 0.5f, url_y),
                    ui_with_alpha(ui_text_dim_u32(), 0.95f), url_draw.c_str());

        const float copy_w = 152.0f * m.layout_scale;
        const float copy_h = m.button_h;
        if (ui_pill_button("party_fs_copy", "Copy link", UiIcon::Check,
                           ImVec2((w - copy_w) * 0.5f, url_y +
                                                        24.0f * m.layout_scale),
                           ImVec2(copy_w, copy_h), false, 1.0f, true))
        {
            ImGui::SetClipboardText(st.url.c_str());
            ui_notify(UiNotifyType::Success, "Party link copied",
                      "Paste it into a message if a guest can't scan the code.", 4.0f);
        }

        const char *hint = "Scan with a phone camera  \xC2\xB7  Esc to close";
        const float hint_w = ui_measure_width(g_ui_fonts.body, 13.0f, hint);
        dl->AddText(g_ui_fonts.body, 13.0f * m.text_scale,
                    ImVec2((w - hint_w) * 0.5f, h - pad - 13.0f * m.text_scale),
                    ui_with_alpha(ui_text_muted_u32(), 0.9f), hint);

        if (!close && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            close = true;

        if (close)
        {
            ImGui::CloseCurrentPopup();
            s_open = false;
            state.party_fullscreen = false;
        }
        ImGui::EndPopup();
    }
    else
    {
        s_open = false;
        state.party_fullscreen = false;
    }

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(5);
}

void draw_party_chip(AppState &state)
{
    if (!party_running() || state.party_fullscreen || state.show_shortcuts)
        return;
    const UiMetrics &m = ui_m();
    ImGuiIO &io = ImGui::GetIO();
    const AppLayout layout = ui_compute_layout(state, io);
    const PartyStatus st = party_status();

    char label[96];
    std::snprintf(label, sizeof(label), "Party · %d %s", st.guests,
                  st.guests == 1 ? "guest" : "guests");
    const float h = std::max(32.0f * m.layout_scale, m.control_h * 0.78f);
    const float w = text_width(label, 15.0f) + h * 1.9f;
    // Sit at the bottom-right of the main content column, not the window
    // edge: the lyrics/now-playing panel and the toast stack both live there
    // and would hide the chip.
    float chip_right = layout.content_pos.x + layout.content_size.x;
    if (layout.lyrics_open && layout.lyrics_docked)
        chip_right -= layout.lyrics_w + layout.gap;
    chip_right = std::max(layout.content_pos.x + w + m.space_3,
                          chip_right - m.space_5);
    const ImVec2 pos(chip_right - w,
                     layout.bottom_pos.y - h - m.space_5 - 12.0f * m.layout_scale);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, h * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##party_chip", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // Like the toasts, the chip must float above the app windows: the right
    // panel and the page child are focused after it is created and would
    // otherwise render on top of the indicator forever.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    ImGui::InvisibleButton("##party_chip_hit", ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetWindowPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ui_col(ui_c().surface), h * 0.5f);
    dl->AddRect(p0, p1, ui_accent_u32(hovered ? 0.65f : 0.32f), h * 0.5f, 0, 1.0f);
    draw_live_dot(dl, ImVec2(p0.x + h * 0.62f, p0.y + h * 0.5f),
                  hovered ? 1.0f : 0.9f, true, ImGui::GetTime());
    dl->AddText(g_ui_fonts.body, 15.0f * m.text_scale,
                ImVec2(p0.x + h * 1.0f, p0.y + (h - 15.0f * m.text_scale) * 0.5f),
                ui_text_u32(), label);

    if (clicked)
    {
        state.current_tab = Tab::Party;
        state.repaint_requested = true;
    }
    if (hovered)
        ImGui::SetTooltip("Open party mode");

    ImGui::End();
}
