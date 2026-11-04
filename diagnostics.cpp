#include "diagnostics.h"

#include "library.h"
#include "paths.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace diagnostics
{
namespace
{
std::mutex g_mu;
std::deque<std::string> g_lines;
constexpr size_t kMaxLines = 400;
std::string g_crash_dir;
char g_crash_dir_buf[1024] = {0};

std::string stamp()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string version()
{
#ifdef CROSSFADE_VERSION
    return CROSSFADE_VERSION;
#else
    return "unknown";
#endif
}

void append_line(const std::string &line)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_lines.push_back(line);
    while (g_lines.size() > kMaxLines)
        g_lines.pop_front();
}

#ifdef _WIN32
LONG WINAPI crash_filter(EXCEPTION_POINTERS *info)
{
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_crash_dir.empty())
            return EXCEPTION_EXECUTE_HANDLER;
        dir.assign(g_crash_dir.begin(), g_crash_dir.end());
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    swprintf(name, 128, L"\\crash-%04d%02d%02d-%02d%02d%02d.dmp", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring path = dir + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal,
                          &mei, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void posix_crash_handler(int sig)
{
    char ts[32] = "unknown";
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &now) == 0)
#else
    if (localtime_r(&now, &tm) != nullptr)
#endif
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    char path[1200];
    std::snprintf(path, sizeof(path), "%s/crash-%s.log", g_crash_dir_buf, ts);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        char header[256];
        const int n = std::snprintf(header, sizeof(header),
                                    "crossfade crash signal %d\nversion %s\n", sig,
#ifdef CROSSFADE_VERSION
                                    CROSSFADE_VERSION
#else
                                    "unknown"
#endif
        );
        (void)!::write(fd, header, (size_t)n);
        void *frames[48];
        const int count = backtrace(frames, 48);
        backtrace_symbols_fd(frames, count, fd);
        ::close(fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif
} // namespace

void init()
{
    g_crash_dir = p100_config_dir();
    std::snprintf(g_crash_dir_buf, sizeof(g_crash_dir_buf), "%s", g_crash_dir.c_str());
    std::error_code ec;
    std::filesystem::create_directories(g_crash_dir, ec);
    log("app", std::string("crossfade ") + version() + " starting");
    log("app", std::string("config: ") + p100_config_dir());
    install_crash_handler();
}

void install_crash_handler()
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#else
    signal(SIGSEGV, posix_crash_handler);
    signal(SIGABRT, posix_crash_handler);
    signal(SIGILL, posix_crash_handler);
    signal(SIGFPE, posix_crash_handler);
#endif
}

void log(const char *category, const std::string &message)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    std::ostringstream line;
    line << ts << " [" << (category ? category : "app") << "] " << message;
    append_line(line.str());
}

std::string recent_log()
{
    std::lock_guard<std::mutex> lock(g_mu);
    std::string out;
    for (const auto &line : g_lines)
    {
        out += line;
        out += '\n';
    }
    return out;
}

std::string bundle()
{
    std::ostringstream out;
    out << "crossfade diagnostics\n"
        << "version: " << version() << "\n"
#ifdef _WIN32
        << "platform: Windows\n"
#elif defined(__APPLE__)
        << "platform: macOS\n"
#else
        << "platform: Linux\n"
#endif
        << "config: " << p100_config_dir() << "\n"
        << "library DB: " << library::db_path() << "\n"
        << "library backup: " << library::backup_dir() << "\n\n"
        << "Library status:\n"
        << library::status_text() << "\n\n"
        << "Recent log:\n"
        << recent_log();
    return out.str();
}

bool safe_mode_requested()
{
    const char *env = std::getenv("P100_SAFE_MODE");
    return env && *env && std::strcmp(env, "0") != 0;
}

void open_folder(const std::string &path)
{
#ifdef _WIN32
    std::wstring wide(path.begin(), path.end());
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    const std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
    const int rc = std::system(cmd.c_str());
    (void)rc;
#endif
}

} // namespace diagnostics
