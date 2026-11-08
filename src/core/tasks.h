#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Tracked short-lived background workers.
//
// The app historically used `std::thread(...).detach()` everywhere, so a
// shutdown could destroy AppState while a fetch/cache worker was still
// running. Workers spawned here are joined by shutdown(); if a network call
// is still blocked after the grace period they are detached as a last resort
// (the same behaviour as before, but only for genuinely stuck workers).
namespace tasks
{
struct Snapshot
{
    uint64_t id = 0;
    std::string name;
    bool active = false;
    double start_ms = 0.0;    // steady clock, ms
    double duration_ms = 0.0; // finished only
};

void spawn(std::function<void()> fn);
void spawn(const char *name, std::function<void()> fn);
void shutdown();

// Recent history for the diagnostics bundle. Useful names are supplied for
// the important jobs; uncategorised work appears as "task".
std::vector<Snapshot> snapshot();
size_t active_count();
uint64_t total_count();
void set_history_enabled(bool enabled);
} // namespace tasks
