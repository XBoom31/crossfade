#include "paths.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifndef P100_WINDOWS
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace fs = std::filesystem;

static std::string env_string(const char *name)
{
    const char *value = getenv(name);
    return value ? std::string(value) : std::string();
}

static std::string home_dir()
{
#ifdef P100_WINDOWS
    // Roaming profile == closest analogue of $HOME for per-user data.
    std::string home = env_string("APPDATA");
    if (home.empty())
        home = env_string("USERPROFILE");
    return home.empty() ? std::string(".") : home;
#else
    std::string home = env_string("HOME");
    return home.empty() ? std::string(".") : home;
#endif
}

static bool is_dir(const std::string &path)
{
    std::error_code ec;
    return fs::is_directory(path, ec);
}

static bool is_file(const std::string &path)
{
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

std::string p100_exe_dir()
{
#ifdef P100_WINDOWS
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        std::string path(buf);
        size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos)
            return path.substr(0, slash);
    }
    return std::string(".");
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0)
    {
        buf[n] = '\0';
        std::string path(buf);
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos)
            return path.substr(0, slash);
    }
    return std::string(".");
#endif
}

std::string p100_config_dir()
{
    std::string override_dir = env_string("P100_CONFIG_DIR");
    if (!override_dir.empty())
        return override_dir;

#ifdef P100_WINDOWS
    return (fs::path(home_dir()) / "crossfade").string();
#else
    std::string xdg = env_string("XDG_CONFIG_HOME");
    if (!xdg.empty())
        return (fs::path(xdg) / "crossfade").string();

    return (fs::path(home_dir()) / ".config" / "crossfade").string();
#endif
}

bool p100_ensure_config_dir()
{
    std::string dir = p100_config_dir();
    if (is_dir(dir))
        return true;
    // One-time migration from the project's previous name: move the whole
    // legacy p100 config dir (auth, settings, caches, history) so existing
    // installs keep working under the crossfade name.
    if (env_string("P100_CONFIG_DIR").empty())
    {
        std::string xdg = env_string("XDG_CONFIG_HOME");
        std::string base = !xdg.empty()
                               ? fs::path(xdg).string()
                               : (fs::path(home_dir()) / ".config").string();
        std::string legacy = (fs::path(base) / "p100").string();
        if (is_dir(legacy))
        {
            std::error_code mv_ec;
            fs::rename(legacy, dir, mv_ec);
            if (!mv_ec)
                return true;
            // Cross-device rename can fail; fall through to a fresh dir.
        }
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec && is_dir(dir);
}

std::string p100_config_path(const std::string &leaf)
{
    if (leaf.empty())
        return p100_config_dir();
    return (fs::path(p100_config_dir()) / leaf).string();
}

std::string p100_share_dir()
{
    std::string override_dir = env_string("P100_SHARE_DIR");
    if (!override_dir.empty())
        return override_dir;

    const std::string exe = p100_exe_dir();
    std::vector<std::string> candidates = {
        exe + "/../share/crossfade", // ~/.local/bin -> ~/.local/share/crossfade
        exe + "/..",            // build/p100 -> project root
        exe,
        exe + "/../..",
    };

#ifdef P100_SOURCE_DIR
    candidates.push_back(P100_SOURCE_DIR);
#endif

#ifdef P100_WINDOWS
    {
        char cwd[MAX_PATH];
        if (GetCurrentDirectoryA(MAX_PATH, cwd))
            candidates.push_back(cwd);
    }
#else
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
        candidates.push_back(cwd);
#endif

    for (const auto &candidate : candidates)
    {
        fs::path dir = fs::path(candidate).lexically_normal();
        if (is_dir((dir / "assets" / "fonts").string()) ||
            is_file((dir / "p100-backend.py").string()))
            return dir.string();
    }

    return fs::path(exe + "/../share/crossfade").lexically_normal().string();
}

std::string p100_asset_dir()
{
    std::string override_dir = env_string("P100_ASSETS");
    if (!override_dir.empty())
        return override_dir;

    const std::string share = p100_share_dir();
    std::vector<std::string> candidates = {
        share + "/assets",
        (fs::path(p100_exe_dir()) / "assets").string(),
        (fs::path(p100_exe_dir()) / ".." / "assets").string(),
    };

    for (const auto &candidate : candidates)
    {
        if (is_dir((fs::path(candidate) / "fonts").string()))
            return fs::path(candidate).lexically_normal().string();
    }

    return (fs::path(share) / "assets").string();
}
