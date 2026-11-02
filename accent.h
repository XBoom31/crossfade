#pragma once
#include "imgui.h"

// Recompute the accent colour.
//
// Order:
//   1. P100_ACCENT=#rrggbb override
//   2. org.freedesktop.appearance / accent-color via the XDG desktop portal
//   3. the deterministic date-seeded daily colour
//
// Updates g_accent and the derived hover/active/text variants declared in
// ui.h. Safe to call from the main loop when the day rolls over.
void update_accent();
