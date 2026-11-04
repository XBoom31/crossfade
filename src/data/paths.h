#pragma once
#include <string>

#ifdef P100_WINDOWS
#include <direct.h>
// POSIX mkdir(path, mode): the mode argument does not exist on Windows.
inline int mkdir(const char *path, int)
{
    return _mkdir(path);
}
#endif

// Central path resolution for p100.
//
// Config rule: $P100_CONFIG_DIR, else $XDG_CONFIG_HOME/p100, else
// ~/.config/p100. The default matches the historical layout so existing
// installs (and data copied from another machine) keep working unchanged.

std::string p100_exe_dir();

std::string p100_config_dir();
bool p100_ensure_config_dir();
std::string p100_config_path(const std::string &leaf);

// Read-only app files: backend scripts and assets/fonts. Resolves the
// installed layout (<prefix>/share/p100), the dev tree, and $P100_SHARE_DIR.
std::string p100_share_dir();

// Directory containing the fonts: <share>/assets.
std::string p100_asset_dir();
