#pragma once

#include <string>

namespace diagnostics
{
void init();
void install_crash_handler();
void log(const char *category, const std::string &message);
std::string recent_log();
std::string bundle();
bool safe_mode_requested();
void open_folder(const std::string &path);
} // namespace diagnostics
