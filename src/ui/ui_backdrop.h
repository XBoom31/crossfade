#pragma once
#include "imgui.h"
#include <GL/gl.h>
#include <string>

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

// Optional full-screen shader path. Call after glClear() and before
// ImGui_ImplOpenGL3_RenderDrawData(); it is a no-op unless the setting is on
// and the album backdrop is visible. Using a shader lets the gradient be
// dithered before 8-bit output, removing the bands OLED panels expose.
void ui_backdrop_render_shader();

// 0 = fully hidden (panels opaque), 1 = fully visible (panels transparent).
float ui_backdrop_visibility();

// Background colour for the four top-level app panels. Opaque when the
// backdrop is off, transparent as it fades in.
ImVec4 ui_backdrop_window_bg();

// Current ambient tint (lifted dominant cover colour, or the app accent when
// nothing is loaded). Independent of whether the full backdrop is enabled, so
// the glowing separators can follow the album colour on their own. Alpha = 1.
ImVec4 ui_backdrop_tint();

// Hero card background blur. The shader-generated texture is produced once
// per cover URL (128x64, two separable passes) and then reused; request it
// while drawing, then call ui_hero_blur_process() once on the render thread
// before ImGui_ImplOpenGL3_RenderDrawData(). Returns 0 until it is ready.
void ui_hero_blur_request(const std::string &url);
void ui_hero_blur_process();
GLuint ui_hero_blur_texture(const std::string &url);
bool ui_hero_blur_size(const std::string &url, int &w, int &h);
void ui_hero_blur_clear();
