#include "diagnostics.h"

#include "library.h"
#include "paths.h"
#include "version.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <shellapi.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
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

// Hang watchdog. The main loop stamps a heartbeat; if it stops advancing for
// a minute, the watchdog records the stall and signals the stuck main thread
// so the normal crash handler can dump its stack. sig_atomic_t is safe to read
// from the signal handler.
std::atomic<int64_t> g_last_heartbeat_ms{0};
std::atomic<bool> g_watchdog_stop{false};
volatile sig_atomic_t g_hang_pending = 0;
#ifndef _WIN32
pthread_t g_main_thread;
#endif

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void write_hang_note(int64_t stalled_ms)
{
    char path[1200];
    const long long stamp = (long long)std::time(nullptr);
    std::snprintf(path, sizeof(path), "%s/hang-%lld.log", g_crash_dir_buf, stamp);
    std::FILE *f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f,
                 "crossfade hang: main loop stalled for %lld ms\n"
                 "A crash log with the stalled thread's stack follows.\n",
                 (long long)stalled_ms);
    std::fclose(f);
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
    // Keep the signal path free of allocation and locale/tz calls. An epoch
    // suffix is enough to distinguish dumps and std::time() is safe here.
    const long long now = (long long)std::time(nullptr);
    char path[1200];
    std::snprintf(path, sizeof(path), "%s/crash-%lld.log", g_crash_dir_buf, now);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        char header[256];
        int n;
        if (g_hang_pending)
            n = std::snprintf(header, sizeof(header),
                              "crossfade hang: main loop unresponsive\nversion %s\n",
#ifdef CROSSFADE_VERSION
                              CROSSFADE_VERSION
#else
                              "unknown"
#endif
            );
        else
            n = std::snprintf(header, sizeof(header),
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

    g_last_heartbeat_ms.store(now_ms());
#ifndef _WIN32
    g_main_thread = pthread_self();
#endif
}

void start_watchdog()
{
#ifndef _WIN32
    g_last_heartbeat_ms.store(now_ms());
    g_watchdog_stop.store(false);
    std::thread([]() {
        constexpr int64_t kHangMs = 60000;
        while (!g_watchdog_stop.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (g_watchdog_stop.load(std::memory_order_relaxed))
                return;
            const int64_t stalled = now_ms() - g_last_heartbeat_ms.load();
            if (stalled < kHangMs || g_hang_pending)
                continue;
            g_hang_pending = 1;
            write_hang_note(stalled);
            // Deliver SIGABRT to the blocked main thread, not this watchdog,
            // so the crash handler records the stack that is actually stuck.
            pthread_kill(g_main_thread, SIGABRT);
            return;
        }
    }).detach();
#endif
}

void heartbeat()
{
    g_last_heartbeat_ms.store(now_ms());
}

void stop_watchdog()
{
    g_watchdog_stop.store(true);
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
