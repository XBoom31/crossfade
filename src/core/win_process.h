#pragma once

// Console-window-free process helpers for the GUI Windows build.
//
// crossfade links as a Windows GUI application, so a console-subsystem child
// launched with the default flags allocates a new console window. These
// wrappers start `cmd.exe /c ...` with CREATE_NO_WINDOW and keep the pipe
// behaviour of popen/pclose (including waiting for the child on close).

#ifdef P100_WINDOWS

#include <cstdio>
#include <string>
#include <vector>

namespace winproc
{

// Run a shell command hidden and wait for it. Returns the child exit code
// (or -1 when it could not be started).
int system_hidden(const std::string &command);

// Same, but captures combined stdout/stderr (capped at 64 KB) for logs.
int system_hidden_capture(const std::string &command, std::string &output);

// Run a command line directly (no cmd.exe) with no console and capture its
// combined output. Used for yt-dlp, where shell quote parsing is a liability.
int direct_capture(const std::string &command_line, std::string &output);

// argv variants for code that already has an argument vector. Hidden and
// waited-for, or hidden and detached (for the import helper that has to
// outlive the app).
int run_argv_hidden(const std::vector<std::string> &args);
bool spawn_argv_hidden(const std::vector<std::string> &args);

// popen("r")/pclose equivalents. Only read modes are supported. The returned
// FILE* must be closed with pclose_hidden(); that waits for the child and
// releases its process handle.
FILE *popen_hidden(const std::string &command, const char *mode);
int pclose_hidden(FILE *pipe);

} // namespace winproc

#endif // P100_WINDOWS
