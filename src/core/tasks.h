#pragma once

#include <functional>

// Tracked short-lived background workers.
//
// The app historically used `std::thread(...).detach()` everywhere, so a
// shutdown could destroy AppState while a fetch/cache worker was still
// running. Workers spawned here are joined by shutdown(); if a network call
// is still blocked after the grace period they are detached as a last resort
// (the same behaviour as before, but only for genuinely stuck workers).
namespace tasks
{
void spawn(std::function<void()> fn);
void shutdown();
} // namespace tasks
