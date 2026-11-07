#pragma once

// Shared helpers for the standalone test binaries. Header-only so each test
// stays a single translation unit without a separate support library.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace testutil
{
inline int failures = 0;

inline void check(bool cond, const char *what)
{
    if (!cond)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

inline void write_file(const std::filesystem::path &path, const std::string &text)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << text;
}

inline void write_file(const std::filesystem::path &path, const char *text)
{
    write_file(path, std::string(text ? text : ""));
}

// Point P100_CONFIG_DIR at a fresh temporary directory and return it.
inline std::filesystem::path make_config_dir(const char *name)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("P100_CONFIG_DIR", root.string().c_str());
#else
    setenv("P100_CONFIG_DIR", root.string().c_str(), 1);
#endif
    return root;
}
} // namespace testutil
