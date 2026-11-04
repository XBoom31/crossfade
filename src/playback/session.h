#pragma once

#include "ui.h"

#include <string>

// Session JSON builder. Exposed so tests can round-trip the exact file format
// without constructing an mpv player.
std::string session_json(const AppState &state, double position, int volume);
