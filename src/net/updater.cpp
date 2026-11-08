#include "tasks.h"
#include "updater.h"
#include "version.h"

#ifdef _WIN32
#include "ui_notify.h"
#include "win_process.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#endif

namespace updater
{
namespace
{
#ifdef _WIN32

constexpr const char *kVersionUrl =
    "https://github.com/XBoom31/crossfade/releases/latest/download/version.txt";

std::mutex g_mu;
Status g_status;
void (*g_wake)() = nullptr;
bool g_notified_available = false;
bool g_launch_failed = false;
bool g_notified_launch_failure = false;

std::string current_version()
{
#ifdef CROSSFADE_VERSION
    return CROSSFADE_VERSION;
#else
    return "0.0.0";
#endif
}

std::string trim_version(std::string s)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c) && c != '"'; };
    while (!s.empty() && !not_space((unsigned char)s.front()))
        s.erase(s.begin());
    while (!s.empty() && !not_space((unsigned char)s.back()))
        s.pop_back();
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V'))
        s.erase(s.begin());
    return s;
}

std::vector<int> parse_version(const std::string &v)
{
    std::vector<int> out;
    int n = 0;
    bool have = false;
    for (char c : v)
    {
        if (c >= '0' && c <= '9')
        {
            n = n * 10 + (c - '0');
            have = true;
        }
        else if (c == '.')
        {
            out.push_back(have ? n : 0);
            n = 0;
            have = false;
        }
        else
        {
            break;
        }
    }
    if (have)
        out.push_back(n);
    return out;
}

bool version_newer(const std::string &latest, const std::string &current)
{
    const auto a = parse_version(latest);
    const auto b = parse_version(current);
    if (a.empty() || b.empty())
        return false;
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av != bv)
            return av > bv;
    }
    return false;
}

void set_status(State state, std::string latest, std::string message)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_status.state = state;
    g_status.latest = std::move(latest);
    g_status.message = std::move(message);
}

std::filesystem::path installed_updater()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
        return {};
    std::filesystem::path p(buf);
    return p.parent_path() / L"install-crossfade.cmd";
}

#endif // _WIN32
} // namespace

void init(void (*wake)())
{
#ifdef _WIN32
    g_wake = wake;
    check_async();
#else
    (void)wake;
#endif
}

void check_async()
{
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_status.state == State::Checking || g_status.state == State::Updating)
            return;
        g_status.state = State::Checking;
        g_status.current = current_version();
        g_status.message.clear();
    }

    tasks::spawn("update-check", []()
                {
        std::string out;
        const std::string cmd =
            std::string("curl -fsSL --connect-timeout 8 --max-time 20 \"") + kVersionUrl +
            "\" 2>nul";
        FILE *pipe = winproc::popen_hidden(cmd, "rb");
        if (pipe)
        {
            char buf[256];
            size_t n = fread(buf, 1, sizeof(buf) - 1, pipe);
            if (n > 0)
                out.assign(buf, n);
            winproc::pclose_hidden(pipe);
        }

        const std::string latest = trim_version(out);
        const std::string current = current_version();
        if (latest.empty())
            set_status(State::Failed, "", "Couldn't reach the update server.");
        else if (version_newer(latest, current))
            set_status(State::Available, latest, "");
        else
            set_status(State::UpToDate, latest, "");

        void (*wake)() = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            wake = g_wake;
        }
        if (wake)
            wake(); });
#endif
}

Status status()
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_mu);
    return g_status;
#else
    Status s;
    s.state = State::Disabled;
    s.current = "0.0.0";
    return s;
#endif
}

void poll()
{
#ifdef _WIN32
    Status s = status();
    if (s.state == State::Available && !g_notified_available)
    {
        g_notified_available = true;
        ui_notify(UiNotifyType::Info, "crossfade update available",
                  "Version " + s.latest +
                      " is ready. The updater opens in its own window and will offer to "
                      "restart crossfade when it finishes.",
                  0.0f, "Update now", &start_update);
    }

    bool launch_failed = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        launch_failed = g_launch_failed;
    }
    if (launch_failed && !g_notified_launch_failure)
    {
        g_notified_launch_failure = true;
        ui_notify(UiNotifyType::Error, "Couldn't start the updater",
                  "Run install-crossfade.cmd from the crossfade release page manually.", 0.0f);
    }
#endif
}

void start_update()
{
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::Updating;
    }

    const std::filesystem::path local_cmd = installed_updater();

    std::wstring file = L"cmd.exe";
    std::wstring params;
    if (!local_cmd.empty() && std::filesystem::exists(local_cmd))
    {
        params = L"/c \"" + local_cmd.wstring() + L"\"";
    }
    else
    {
        // Older installs don't ship the updater next to the exe. Download the
        // current one-click cmd and let it do the normal release install.
        file = L"powershell.exe";
        params =
            L"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command \""
            L"$u='https://github.com/XBoom31/crossfade/releases/latest/download/install-crossfade.cmd';"
            L"$p=Join-Path $env:TEMP 'crossfade-update.cmd';"
            L"Invoke-WebRequest -UseBasicParsing $u -OutFile $p;"
            L"Start-Process cmd.exe -ArgumentList '/c',$p\"";
    }

    const HINSTANCE r = ShellExecuteW(nullptr, L"open", file.c_str(), params.c_str(),
                                     nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_status.state = State::Failed;
        g_status.message = "The updater process could not be started.";
        g_launch_failed = true;
    }
#endif
}

} // namespace updater
