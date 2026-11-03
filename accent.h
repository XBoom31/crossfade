#pragma once
#include "imgui.h"
#include <string>

struct Settings;

// Recompose the accent colour.
//
// Order:
//   1. P100_ACCENT=#rrggbb override
//   2. Settings -> Appearance -> Accent colour (custom "#rrggbb")
//   3. org.freedesktop.appearance / accent-color via the XDG desktop portal
//   4. the deterministic date-seeded daily colour
//
// Updates g_accent and the derived hover/active/text variants declared in
// ui.h. Safe to call from the main loop when the day rolls over.
void update_accent(const Settings &settings);

// Parse/serialise the "#rrggbb" form used by P100_ACCENT and settings.json.
bool accent_parse_hex(const char *text, ImVec4 &out);
std::string accent_to_hex(const ImVec4 &color);
