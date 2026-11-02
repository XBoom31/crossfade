#include "ui_icons.h"
#include "ui_theme.h"

#include <cmath>

static ImVec2 p(ImVec2 c, float s, float x, float y)
{
    return ImVec2(c.x + x * s, c.y + y * s);
}

void ui_draw_icon(ImDrawList *dl, UiIcon icon, ImVec2 c, float s, ImU32 col, float thick, float rotation)
{
    const float h = s * 0.5f;
    const float t = thick;
    switch (icon)
    {
    case UiIcon::Home:
        dl->AddLine(p(c, s, -0.42f, -0.04f), p(c, s, 0.0f, -0.42f), col, t);
        dl->AddLine(p(c, s, 0.0f, -0.42f), p(c, s, 0.42f, -0.04f), col, t);
        dl->AddLine(p(c, s, -0.32f, -0.08f), p(c, s, -0.32f, 0.42f), col, t);
        dl->AddLine(p(c, s, 0.32f, -0.08f), p(c, s, 0.32f, 0.42f), col, t);
        dl->AddLine(p(c, s, -0.32f, 0.42f), p(c, s, 0.32f, 0.42f), col, t);
        dl->AddLine(p(c, s, -0.10f, 0.42f), p(c, s, -0.10f, 0.14f), col, t);
        dl->AddLine(p(c, s, 0.10f, 0.42f), p(c, s, 0.10f, 0.14f), col, t);
        break;
    case UiIcon::Radio:
        dl->AddCircle(c, h * 0.72f, col, 20, t);
        dl->AddCircleFilled(c, h * 0.18f, col, 12);
        dl->AddLine(p(c, s, 0.10f, -0.32f), p(c, s, 0.42f, -0.50f), col, t);
        break;
    case UiIcon::Heart:
        dl->PathLineTo(p(c, s, 0.0f, 0.40f));
        dl->PathBezierCubicCurveTo(p(c, s, -0.54f, 0.02f), p(c, s, -0.46f, -0.38f), p(c, s, -0.18f, -0.38f));
        dl->PathBezierCubicCurveTo(p(c, s, -0.02f, -0.38f), p(c, s, 0.0f, -0.20f), p(c, s, 0.0f, -0.12f));
        dl->PathBezierCubicCurveTo(p(c, s, 0.0f, -0.20f), p(c, s, 0.02f, -0.38f), p(c, s, 0.18f, -0.38f));
        dl->PathBezierCubicCurveTo(p(c, s, 0.46f, -0.38f), p(c, s, 0.54f, 0.02f), p(c, s, 0.0f, 0.40f));
        dl->PathStroke(col, 0, t);
        break;
    case UiIcon::Album:
        dl->AddRect(p(c, s, -0.40f, -0.40f), p(c, s, 0.40f, 0.40f), col, s * 0.10f, 0, t);
        dl->AddCircle(c, h * 0.42f, col, 18, t);
        dl->AddCircleFilled(c, h * 0.09f, col, 10);
        break;
    case UiIcon::Artist:
        dl->AddCircle(p(c, s, 0.0f, -0.18f), h * 0.30f, col, 18, t);
        dl->PathClear();
        dl->PathArcTo(p(c, s, 0.0f, 0.48f), h * 0.56f, 3.65f, 5.77f, 16);
        dl->PathStroke(col, 0, t);
        break;
    case UiIcon::Playlist:
        dl->AddLine(p(c, s, -0.42f, -0.25f), p(c, s, 0.18f, -0.25f), col, t);
        dl->AddLine(p(c, s, -0.42f, 0.02f), p(c, s, 0.18f, 0.02f), col, t);
        dl->AddLine(p(c, s, -0.42f, 0.29f), p(c, s, 0.02f, 0.29f), col, t);
        dl->AddCircleFilled(p(c, s, 0.29f, 0.10f), h * 0.13f, col, 10);
        dl->AddLine(p(c, s, 0.42f, 0.10f), p(c, s, 0.42f, -0.34f), col, t);
        dl->AddLine(p(c, s, 0.42f, -0.34f), p(c, s, 0.22f, -0.42f), col, t);
        break;
    case UiIcon::Download:
        dl->AddLine(p(c, s, 0.0f, -0.44f), p(c, s, 0.0f, 0.18f), col, t);
        dl->AddLine(p(c, s, -0.24f, -0.04f), p(c, s, 0.0f, 0.20f), col, t);
        dl->AddLine(p(c, s, 0.24f, -0.04f), p(c, s, 0.0f, 0.20f), col, t);
        dl->AddLine(p(c, s, -0.34f, 0.42f), p(c, s, 0.34f, 0.42f), col, t);
        break;
    case UiIcon::History:
        dl->AddCircle(c, h * 0.72f, col, 20, t);
        dl->AddLine(c, p(c, s, 0.0f, -0.12f), col, t);
        dl->AddLine(c, p(c, s, 0.24f, 0.12f), col, t);
        break;
    case UiIcon::Stats:
        dl->AddLine(p(c, s, -0.36f, 0.40f), p(c, s, -0.36f, 0.02f), col, t);
        dl->AddLine(p(c, s, -0.02f, 0.40f), p(c, s, -0.02f, -0.30f), col, t);
        dl->AddLine(p(c, s, 0.32f, 0.40f), p(c, s, 0.32f, -0.10f), col, t);
        break;
    case UiIcon::Settings:
        dl->AddCircle(c, h * 0.32f, col, 18, t);
        for (int i = 0; i < 8; ++i)
        {
            float a = (float)i * 0.785398f;
            dl->AddLine(p(c, s, cosf(a) * 0.42f, sinf(a) * 0.42f),
                        p(c, s, cosf(a) * 0.62f, sinf(a) * 0.62f), col, t);
        }
        dl->AddCircle(c, h * 0.70f, col, 18, t);
        break;
    case UiIcon::Search:
        dl->AddCircle(p(c, s, -0.08f, -0.08f), h * 0.46f, col, 18, t);
        dl->AddLine(p(c, s, 0.22f, 0.22f), p(c, s, 0.48f, 0.48f), col, t + 0.4f);
        break;
    case UiIcon::Close:
        dl->AddLine(p(c, s, -0.30f, -0.30f), p(c, s, 0.30f, 0.30f), col, t);
        dl->AddLine(p(c, s, 0.30f, -0.30f), p(c, s, -0.30f, 0.30f), col, t);
        break;
    case UiIcon::ChevronLeft:
        dl->AddLine(p(c, s, 0.16f, -0.34f), p(c, s, -0.18f, 0.0f), col, t);
        dl->AddLine(p(c, s, -0.18f, 0.0f), p(c, s, 0.16f, 0.34f), col, t);
        break;
    case UiIcon::ChevronRight:
        dl->AddLine(p(c, s, -0.16f, -0.34f), p(c, s, 0.18f, 0.0f), col, t);
        dl->AddLine(p(c, s, 0.18f, 0.0f), p(c, s, -0.16f, 0.34f), col, t);
        break;
    case UiIcon::Play:
        dl->AddTriangleFilled(p(c, s, -0.26f, -0.40f), p(c, s, -0.26f, 0.40f), p(c, s, 0.40f, 0.0f), col);
        break;
    case UiIcon::Pause:
        dl->AddRectFilled(p(c, s, -0.28f, -0.38f), p(c, s, -0.07f, 0.38f), col, s * 0.035f);
        dl->AddRectFilled(p(c, s, 0.07f, -0.38f), p(c, s, 0.28f, 0.38f), col, s * 0.035f);
        break;
    case UiIcon::Prev:
        dl->AddRectFilled(p(c, s, -0.38f, -0.34f), p(c, s, -0.26f, 0.34f), col, s * 0.03f);
        dl->AddTriangleFilled(p(c, s, 0.34f, -0.36f), p(c, s, 0.34f, 0.36f), p(c, s, -0.24f, 0.0f), col);
        break;
    case UiIcon::Next:
        dl->AddRectFilled(p(c, s, 0.26f, -0.34f), p(c, s, 0.38f, 0.34f), col, s * 0.03f);
        dl->AddTriangleFilled(p(c, s, -0.34f, -0.36f), p(c, s, -0.34f, 0.36f), p(c, s, 0.24f, 0.0f), col);
        break;
    case UiIcon::More:
        dl->AddCircleFilled(p(c, s, 0.0f, -0.28f), h * 0.075f, col, 8);
        dl->AddCircleFilled(c, h * 0.075f, col, 8);
        dl->AddCircleFilled(p(c, s, 0.0f, 0.28f), h * 0.075f, col, 8);
        break;
    case UiIcon::Volume:
        dl->AddTriangleFilled(p(c, s, -0.40f, -0.18f), p(c, s, -0.40f, 0.18f), p(c, s, -0.10f, 0.38f), col);
        dl->AddRectFilled(p(c, s, -0.18f, -0.18f), p(c, s, -0.02f, 0.18f), col);
        dl->PathArcTo(p(c, s, -0.02f, 0.0f), h * 0.52f, -0.82f, 0.82f, 12);
        dl->PathStroke(col, 0, t);
        dl->PathArcTo(p(c, s, -0.02f, 0.0f), h * 0.78f, -0.72f, 0.72f, 14);
        dl->PathStroke(col, 0, t);
        break;
    case UiIcon::Lyrics:
        dl->AddRect(p(c, s, -0.36f, -0.38f), p(c, s, 0.36f, 0.30f), col, s * 0.08f, 0, t);
        dl->AddLine(p(c, s, -0.20f, -0.16f), p(c, s, 0.20f, -0.16f), col, t);
        dl->AddLine(p(c, s, -0.20f, 0.04f), p(c, s, 0.08f, 0.04f), col, t);
        dl->AddLine(p(c, s, 0.0f, 0.30f), p(c, s, 0.12f, 0.48f), col, t);
        dl->AddLine(p(c, s, 0.12f, 0.48f), p(c, s, 0.12f, 0.30f), col, t);
        break;
    case UiIcon::Equalizer:
        dl->AddRectFilled(p(c, s, -0.34f, 0.02f), p(c, s, -0.24f, 0.38f), col, s * 0.02f);
        dl->AddRectFilled(p(c, s, -0.08f, -0.30f), p(c, s, 0.02f, 0.38f), col, s * 0.02f);
        dl->AddRectFilled(p(c, s, 0.18f, -0.08f), p(c, s, 0.28f, 0.38f), col, s * 0.02f);
        break;
    case UiIcon::Shuffle:
        dl->AddLine(p(c, s, -0.42f, -0.26f), p(c, s, -0.08f, -0.26f), col, t);
        dl->AddLine(p(c, s, -0.08f, -0.26f), p(c, s, 0.30f, 0.28f), col, t);
        dl->AddLine(p(c, s, 0.30f, 0.28f), p(c, s, 0.44f, 0.28f), col, t);
        dl->AddLine(p(c, s, 0.44f, 0.28f), p(c, s, 0.28f, 0.14f), col, t);
        dl->AddLine(p(c, s, 0.44f, 0.28f), p(c, s, 0.28f, 0.42f), col, t);
        dl->AddLine(p(c, s, -0.42f, 0.28f), p(c, s, -0.08f, 0.28f), col, t);
        dl->AddLine(p(c, s, -0.08f, 0.28f), p(c, s, 0.02f, 0.12f), col, t);
        dl->AddLine(p(c, s, 0.18f, -0.12f), p(c, s, 0.30f, -0.28f), col, t);
        dl->AddLine(p(c, s, 0.30f, -0.28f), p(c, s, 0.44f, -0.28f), col, t);
        dl->AddLine(p(c, s, 0.44f, -0.28f), p(c, s, 0.28f, -0.42f), col, t);
        dl->AddLine(p(c, s, 0.44f, -0.28f), p(c, s, 0.28f, -0.14f), col, t);
        break;
    case UiIcon::Repeat:
    case UiIcon::RepeatOne:
        dl->AddLine(p(c, s, -0.38f, -0.22f), p(c, s, 0.28f, -0.22f), col, t);
        dl->AddLine(p(c, s, 0.28f, -0.22f), p(c, s, 0.28f, 0.08f), col, t);
        dl->AddLine(p(c, s, 0.28f, 0.08f), p(c, s, 0.42f, -0.06f), col, t);
        dl->AddLine(p(c, s, -0.38f, 0.22f), p(c, s, 0.24f, 0.22f), col, t);
        dl->AddLine(p(c, s, -0.38f, 0.22f), p(c, s, -0.38f, -0.08f), col, t);
        dl->AddLine(p(c, s, -0.38f, -0.08f), p(c, s, -0.24f, 0.06f), col, t);
        if (icon == UiIcon::RepeatOne)
        {
            dl->AddLine(p(c, s, 0.04f, -0.04f), p(c, s, 0.04f, -0.24f), col, t);
            dl->AddLine(p(c, s, -0.04f, -0.16f), p(c, s, 0.04f, -0.24f), col, t);
        }
        break;
    case UiIcon::Queue:
        dl->AddLine(p(c, s, -0.38f, -0.28f), p(c, s, 0.16f, -0.28f), col, t);
        dl->AddLine(p(c, s, -0.38f, 0.00f), p(c, s, 0.16f, 0.00f), col, t);
        dl->AddLine(p(c, s, -0.38f, 0.28f), p(c, s, -0.04f, 0.28f), col, t);
        dl->AddTriangleFilled(p(c, s, 0.22f, 0.12f), p(c, s, 0.22f, 0.40f), p(c, s, 0.44f, 0.26f), col);
        break;
    case UiIcon::Check:
        dl->AddLine(p(c, s, -0.36f, 0.0f), p(c, s, -0.08f, 0.28f), col, t + 0.3f);
        dl->AddLine(p(c, s, -0.08f, 0.28f), p(c, s, 0.38f, -0.28f), col, t + 0.3f);
        break;
    case UiIcon::Refresh:
    {
        // Circular arrow: a near-full arc with a filled arrowhead at the end.
        // `rotation` lets callers spin it while a refresh is in flight.
        const float r = h * 0.68f;
        const float a0 = 1.15f + rotation;
        const float a1 = a0 + 5.15f;
        const int seg = 26;
        dl->PathClear();
        for (int i = 0; i <= seg; ++i)
        {
            const float a = a0 + (a1 - a0) * (float)i / (float)seg;
            dl->PathLineTo(ImVec2(c.x + cosf(a) * r, c.y + sinf(a) * r));
        }
        dl->PathStroke(col, 0, t);

        const ImVec2 tip(c.x + cosf(a1) * r, c.y + sinf(a1) * r);
        const ImVec2 tang(-sinf(a1), cosf(a1));
        const ImVec2 rad(cosf(a1), sinf(a1));
        const float ah = s * 0.16f;
        dl->AddTriangleFilled(
            tip,
            ImVec2(tip.x - tang.x * ah + rad.x * ah * 0.72f,
                   tip.y - tang.y * ah + rad.y * ah * 0.72f),
            ImVec2(tip.x - tang.x * ah - rad.x * ah * 0.72f,
                   tip.y - tang.y * ah - rad.y * ah * 0.72f),
            col);
        break;
    }
    case UiIcon::Sun:
    {
        const float r = h * 0.30f;
        dl->AddCircleFilled(c, r, col, 20);
        for (int i = 0; i < 8; ++i)
        {
            const float a = (float)i * 3.14159265f * 0.25f + rotation;
            const ImVec2 p1(c.x + cosf(a) * r * 1.45f, c.y + sinf(a) * r * 1.45f);
            const ImVec2 p2(c.x + cosf(a) * r * 2.05f, c.y + sinf(a) * r * 2.05f);
            dl->AddLine(p1, p2, col, t);
        }
        break;
    }
    case UiIcon::Moon:
    {
        const float r = h * 0.72f;
        dl->AddCircleFilled(c, r, col, 24);
        const float cs = cosf(rotation), sn = sinf(rotation);
        const float ox = r * 0.42f, oy = -r * 0.30f;
        const ImVec2 off(c.x + ox * cs - oy * sn, c.y + ox * sn + oy * cs);
        dl->AddCircleFilled(off, r * 0.78f, ImGui::GetColorU32(ui_c().bg), 24);
        break;
    }
    }
}

bool ui_icon_button(const char *id, UiIcon icon, float hit_size, bool active, ImU32 color, const char *tooltip)
{
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(hit_size, hit_size));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + hit_size, pos.y + hit_size);
    if (active)
        dl->AddRectFilled(pos, max, ui_accent_u32(0.18f), ui_m().radius_sm);
    else if (hovered)
        dl->AddRectFilled(pos, max, ui_overlay_u32(0.06f), ui_m().radius_sm);
    const ImU32 col = color ? color : (active ? ui_accent_text_u32() : (hovered ? ui_text_u32() : ui_text_dim_u32()));
    ui_draw_icon(dl, icon, ImVec2(pos.x + hit_size * 0.5f, pos.y + hit_size * 0.5f), hit_size * 0.46f, col, 1.6f);
    if (tooltip && hovered)
        ImGui::SetTooltip("%s", tooltip);
    return clicked;
}
