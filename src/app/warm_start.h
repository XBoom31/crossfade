#pragma once

#include <functional>
#include <string>

// Single-instance warm start.
//
// A running crossfade owns a UNIX socket. Later launches deliver a command and
// exit immediately, so reopening the app costs a few milliseconds instead of
// a full process/GL/backend boot.
namespace warm_start
{
enum class Command
{
    Show,
    Hide,
    Toggle,
    Quit,
};

// Deliver a command to a running instance. Returns false when none is
// listening (no socket, stale socket, or unsupported platform).
bool send(Command cmd);

// Become the single instance. `on_command` may be called from the accept
// thread; marshal it to the main thread (atomics + SDL event). Returns false
// when another instance owns the socket or the platform is unsupported.
bool start_server(std::function<void(Command)> on_command, std::string &error);
void stop_server();
bool server_active();

// Stable socket path (empty on unsupported platforms).
std::string socket_path();
} // namespace warm_start
