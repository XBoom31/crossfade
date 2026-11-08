// Windows platform smoke test for the two desktop integrations that Linux
// gets from MPRIS + libnotify:
//
//   * System Media Transport Controls (media keys, media flyout, lock screen)
//   * toast notifications
//
// Default mode creates a real top-level window, publishes metadata through
// WinMediaSession and keeps running so an external checker (PowerShell) can
// observe the SMTC session and send commands. `--self-test` runs the same
// plumbing headlessly and only asserts that both platforms initialized.
//
// This test is intentionally standalone: it links only the two win32 modules,
// not SDL/mpv/the app, so failures point straight at the platform code.

#include "media_session.h"
#include "notifications.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{

constexpr const wchar_t *kWindowClass = L"CrossfadeWinPlatformSmoke";
constexpr const char *kAumid = "Crossfade.Smoke";

struct Options
{
    bool self_test = false;
    int seconds = 20;
    std::string log_path = "win-platform-commands.log";
    std::string art;
};

Options parse_options(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--self-test")
            options.self_test = true;
        else if (arg.rfind("--seconds=", 0) == 0)
            options.seconds = std::max(1, atoi(arg.c_str() + 10));
        else if (arg.rfind("--log=", 0) == 0)
            options.log_path = arg.substr(6);
        else if (arg.rfind("--art=", 0) == 0)
            options.art = arg.substr(6);
    }
    return options;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND create_window()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    HWND window = CreateWindowExW(0, kWindowClass, L"crossfade platform smoke test",
                                  WS_OVERLAPPEDWINDOW, 80, 80, 520, 320, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    if (window)
    {
        ShowWindow(window, SW_SHOWNOACTIVATE);
        UpdateWindow(window);
    }
    return window;
}

void log_command(FILE *log, const char *name, double argument)
{
    if (!log)
        return;
    if (argument == 0.0)
        fprintf(log, "CMD %s\n", name);
    else
        fprintf(log, "CMD %s %.3f\n", name, argument);
    fflush(log);
    printf("command: %s %.3f\n", name, argument);
    fflush(stdout);
}

const char *command_name(MediaCommand command)
{
    switch (command)
    {
    case MediaCommand::Play: return "Play";
    case MediaCommand::Pause: return "Pause";
    case MediaCommand::TogglePlayPause: return "TogglePlayPause";
    case MediaCommand::Stop: return "Stop";
    case MediaCommand::Next: return "Next";
    case MediaCommand::Previous: return "Previous";
    case MediaCommand::SeekTo: return "SeekTo";
    case MediaCommand::SeekBy: return "SeekBy";
    }
    return "Unknown";
}

} // namespace

int main(int argc, char **argv)
{
    const Options options = parse_options(argc, argv);

    FILE *log = fopen(options.log_path.c_str(), "w");
    if (!log)
        fprintf(stderr, "could not open command log %s\n", options.log_path.c_str());

    HWND window = nullptr;
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    const bool notify_ok = win_notify::init(kAumid, "Crossfade Smoke Test", exe_path);

    window = create_window();
    if (!window)
    {
        fprintf(stderr, "CreateWindowEx failed: %lu\n", GetLastError());
        return 2;
    }

    WinMediaSession session(window);
    bool playing = true;
    double position = 0.0;
    const double duration = 240.0;

    auto publish = [&]()
    {
        MediaSessionState state;
        state.track_id = "smoke-track-1";
        state.title = "Crossfade Smoke Test";
        state.artist = "Platform Test Artist";
        state.album = "Platform Test Album";
        state.art = options.art;
        state.duration_sec = duration;
        state.position_sec = position;
        state.playing = playing;
        state.shuffle = false;
        state.repeat = 1;
        session.update(state);
    };

    session.set_command_handler([&](MediaCommand command, double argument)
                                {
                                    log_command(log, command_name(command), argument);
                                    if (command == MediaCommand::Play ||
                                        command == MediaCommand::TogglePlayPause)
                                        playing = true;
                                    else if (command == MediaCommand::Pause ||
                                             command == MediaCommand::Stop)
                                        playing = false;
                                    else if (command == MediaCommand::SeekTo)
                                        position = argument;
                                    else if (command == MediaCommand::SeekBy)
                                        position = std::max(0.0, position + argument);
                                    publish();
                                });

    publish();

    win_notify::Toast toast;
    toast.title = "SMOKE-TOAST-TITLE";
    toast.body = "SMOKE-TOAST-BODY";
    toast.image = options.art;
    win_notify::show(toast);

    // Wait for the SMTC session and the toast worker to come up.
    const DWORD deadline = GetTickCount() + 4000;
    while (GetTickCount() < deadline && !(session.valid() && win_notify::ready()))
        Sleep(50);

    const bool media_ok = session.valid();
    const bool toast_ok = notify_ok && win_notify::ready();
    printf("media_session=%s\n", media_ok ? "ready" : "failed");
    printf("media_error=%s\n", session.last_error().c_str());
    printf("notifications=%s\n", toast_ok ? "ready" : "failed");
    printf("notification_error=%s\n", win_notify::last_error().c_str());
    fflush(stdout);

    bool ok = media_ok && toast_ok;
    if (options.self_test)
    {
        // One more publish exercises metadata + timeline on the worker thread.
        position = 42.0;
        publish();
        Sleep(300);

        win_notify::Toast second;
        second.title = "SMOKE-TOAST-TITLE-2";
        second.body = "SMOKE-TOAST-BODY-2";
        win_notify::show(second);
        Sleep(700);
        printf("toasts_shown=%u\n", win_notify::shown_count());

        session.clear();
        win_notify::shutdown();
        DestroyWindow(window);
        if (log)
            fclose(log);
        printf("self_test=%s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    printf("READY\n");
    fflush(stdout);

    // Let the initial toast leave the queue so the count is meaningful.
    Sleep(500);
    printf("toasts_shown=%u\n", win_notify::shown_count());
    fflush(stdout);

    const DWORD stop_at = GetTickCount() + (DWORD)options.seconds * 1000;
    bool running = true;
    while (running && GetTickCount() < stop_at)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (playing)
        {
            position += 0.25;
            if (position >= duration)
                position = 0.0;
        }
        publish();
        Sleep(250);
    }

    session.clear();
    win_notify::shutdown();
    DestroyWindow(window);
    if (log)
        fclose(log);
    printf("done\n");
    return ok ? 0 : 1;
}
