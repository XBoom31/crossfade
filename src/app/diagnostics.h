#pragma once

#include <functional>
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
// printf-style variant used by DLOG so console output also lands in the
// in-memory diagnostics ring.
void logf(const char *category, const char *fmt, ...);
std::string recent_log();

// Detailed history can be switched off from Settings; boot marks and live
// counters keep working either way.
void set_history_enabled(bool enabled);
bool history_enabled();

// Startup timing marks (milliseconds since start_boot_clock()).
void start_boot_clock();
void mark(const char *name);

// Supplies app/playback/settings context for the bundle. Called on demand.
void set_context_provider(std::function<std::string()> provider);

std::string bundle();
// Writes the bundle to <config>/diagnostics-<timestamp>.txt.
bool save_bundle(std::string &path, std::string &error);
bool safe_mode_requested();
void open_folder(const std::string &path);
} // namespace diagnostics
