#pragma once

#include <string>

// Windows auto-update check. Linux builds are intentionally manual for now:
// packages own their files, so the app only compiles a no-op stub there.
//
// Flow:
//   init(...)       once, after the app is ready
//   poll()          every frame on the main thread; surfaces the toast
//   check_async()   background thread; safe to call again later
//   start_update()  toast action; launches the one-click installer
namespace updater
{
enum class State
{
    Disabled, // not Windows
    Idle,
    Checking,
    Available,
    UpToDate,
    Updating,
    Failed
};

struct Status
{
    State state = State::Disabled;
    std::string current;
    std::string latest;
    std::string message;
};

void init(void (*wake)());
void check_async();
Status status();
void poll();
void start_update();
} // namespace updater
