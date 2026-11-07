#pragma once

#include <string>

namespace diagnostics
{
void init();
void install_crash_handler();
// Start the main-loop hang watchdog (after startup work is done), stamp its
// liveness clock, and stop it during shutdown. The watchdog writes a hang log
// and raises SIGABRT on the main thread when the clock stalls for ~60 s.
void start_watchdog();
void heartbeat();
void stop_watchdog();
void log(const char *category, const std::string &message);
std::string recent_log();
std::string bundle();
bool safe_mode_requested();
void open_folder(const std::string &path);
} // namespace diagnostics
