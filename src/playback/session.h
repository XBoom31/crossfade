#pragma once

#include "ui.h"

#include <string>

// Session JSON builder. Exposed so tests can round-trip the exact file format
// without constructing an mpv player.
std::string session_json(const AppState &state, double position, int volume);

// Stable string form used by session.json. Both directions live together so
// the writer and reader can never drift apart.
const char *playlist_source_name(PlaylistSource source);
PlaylistSource playlist_source_from_name(const std::string &name);
