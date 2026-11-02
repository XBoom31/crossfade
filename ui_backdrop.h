#pragma once
#include "imgui.h"

struct AppState;

// Album-colour ambient backdrop.
//
// `ui_backdrop_update()` is called once per frame after NewFrame(), before the
// main panels are drawn. It eases the current tint toward the playing track's
// dominant cover colour (falling back to the app accent), and drives the
// fade-in/out when the setting is toggled.
//
// `ui_backdrop_draw()` paints the (very faint) coloured wash into the ImGui
// background draw list. The main panels become translucent by exactly the
// same amount via `ui_backdrop_window_bg()`, so the wash reads as one
// continuous surface instead of a patchwork.
void ui_backdrop_update(const AppState &state);
void ui_backdrop_draw();

// 0 = fully hidden (panels opaque), 1 = fully visible (panels transparent).
float ui_backdrop_visibility();

// Background colour for the four top-level app panels. Opaque when the
// backdrop is off, transparent as it fades in.
ImVec4 ui_backdrop_window_bg();

// Approximate final on-screen colour of the backdrop at a screen position
// (fully composited over the theme background, alpha = 1). Used by the list /
// lyrics edge fades so they dissolve into the ambient glow instead of
// smearing an opaque black band over it.
ImVec4 ui_backdrop_sample(const ImVec2 &screen_pos);

// Current ambient tint (lifted dominant cover colour, or the app accent when
// nothing is loaded). Independent of whether the full backdrop is enabled, so
// the glowing separators can follow the album colour on their own. Alpha = 1.
ImVec4 ui_backdrop_tint();
