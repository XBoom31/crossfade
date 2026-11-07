#pragma once

#include "ui.h"
#include "ui_components.h"

#include <string>
#include <vector>

// Shared hero band used by Radio, the list pages (Liked / Downloads /
// History) and Podcasts. One implementation keeps the geometry, artwork
// treatment, chip fitting and action layout identical across all of them.
struct UiHeroChip
{
    std::string label;
    bool accent = false;
};

struct UiHeroAction
{
    std::string label;
    UiIcon icon = UiIcon::Play;
    bool primary = true;
    bool enabled = true;
    std::string compact_label; // shown when the text column is narrow
};

// Draws the card and returns the index of the clicked action, or -1.
// `anim` supplies now/start; `live_dot` pulses the eyebrow dot.
int ui_hero(const std::string &eyebrow, const std::string &title,
            const std::string &subtitle, const std::string &art_url,
            UiIcon fallback, const char *art_scope,
            const std::vector<UiHeroChip> &chips,
            const std::vector<UiHeroAction> &actions,
            const UiPageAnim &anim, bool live_dot = false);
