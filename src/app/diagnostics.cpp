#include "diagnostics.h"

#include "library.h"
#include "paths.h"
#include "version.h"
#include "warm_start.h"
#include "ytdlp.h"
#include "tasks.h"

#include <GL/gl.h>
#include <SDL2/SDL.h>
#include <mpv/client.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdarg>
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
#include <sys/utsname.h>
#endif

namespace diagnostics
{
namespace
{
std::mutex g_mu;
std::deque<std::string> g_lines;
constexpr size_t kMaxLines = 2000;
std::atomic<bool> g_history_enabled{true};

std::mutex g_boot_mu;
std::chrono::steady_clock::time_point g_boot_start;
std::vector<std::pair<std::string, long long>> g_marks;

std::mutex g_provider_mu;
std::function<std::string()> g_context_provider;
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


std::string read_proc_value(const char *path, const char *prefix)
{
    std::ifstream in(path);
    if (!in.good())
        return {};
    std::string line;
    const std::string pfx(prefix);
    while (std::getline(in, line))
    {
        if (line.rfind(pfx, 0) == 0)
        {
            size_t start = pfx.size();
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
                ++start;
            std::string value = line.substr(start);
            if (value.rfind(": ", 0) == 0)
                value.erase(0, 2);
            if (value.size() >= 2 &&
                ((value.front() == '\'' && value.back() == '\'') ||
                 (value.front() == '"' && value.back() == '"')))
                value = value.substr(1, value.size() - 2);
            return value;
        }
    }
    return {};
}

std::string tail_file(const std::string &path, size_t max_lines)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return "(missing)\n";
    std::deque<std::string> lines;
    std::string line;
    while (std::getline(in, line))
    {
        lines.push_back(line);
        if (lines.size() > max_lines)
            lines.pop_front();
    }
    std::string out;
    for (const auto &l : lines)
    {
        out += l;
        out += '\n';
    }
    return out.empty() ? "(empty)\n" : out;
}

std::string run_capture(const std::string &cmd)
{
#ifdef _WIN32
    (void)cmd;
    return "n/a\n";
#else
    std::string out;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return "n/a\n";
    char buf[512];
    while (fgets(buf, sizeof(buf), pipe))
        out += buf;
    pclose(pipe);
    return out.empty() ? "n/a\n" : out;
#endif
}

std::string runtime_info()
{
    std::ostringstream out;
    out << "version: " << version() << "\n";
#ifdef _WIN32
    out << "platform: Windows\n";
#elif defined(__APPLE__)
    out << "platform: macOS\n";
#else
    out << "platform: Linux\n";
#endif
#ifndef _WIN32
    struct utsname uts{};
    if (uname(&uts) == 0)
        out << "kernel: " << uts.sysname << " " << uts.release << " (" << uts.machine << ")\n";
#endif
    const std::string distro = read_proc_value("/etc/os-release", "PRETTY_NAME=");
    if (!distro.empty())
        out << "distro: " << distro << "\n";
    const std::string cpu = read_proc_value("/proc/cpuinfo", "model name");
    if (!cpu.empty())
        out << "cpu: " << cpu << "\n";

    out << "memory: RSS " << read_proc_value("/proc/self/status", "VmRSS:")
        << "  peak " << read_proc_value("/proc/self/status", "VmHWM:")
        << "  PSS " << read_proc_value("/proc/self/smaps_rollup", "Pss:")
        << "  threads " << read_proc_value("/proc/self/status", "Threads:") << "\n";
    out << "config: " << p100_config_dir() << "\n";
    out << "library DB: " << library::db_path() << "\n";

    SDL_version sdl{};
    SDL_GetVersion(&sdl);
    out << "SDL: " << (int)sdl.major << "." << (int)sdl.minor << "." << (int)sdl.patch << "\n";

    const GLubyte *glver = glGetString(GL_VERSION);
    const GLubyte *glren = glGetString(GL_RENDERER);
    const GLubyte *glven = glGetString(GL_VENDOR);
    if (glver)
        out << "GL: " << (const char *)glver << " | " << (glren ? (const char *)glren : "?")
            << " | " << (glven ? (const char *)glven : "?") << "\n";

    const unsigned long mpv_api = mpv_client_api_version();
    out << "mpv client API: " << (mpv_api >> 16) << "." << (mpv_api & 0xFFFF) << "\n";

    static std::mutex ver_mu;
    static std::string versions;
    {
        std::lock_guard<std::mutex> lock(ver_mu);
        if (versions.empty())
        {
            const std::string py = backend_python_path();
            versions = run_capture(
                "\"" + py + "\" -c \"import sys; print('python', sys.version.split()[0]);"
                "import yt_dlp; print('yt-dlp', yt_dlp.version.__version__);"
                "import ytmusicapi; print('ytmusicapi', getattr(ytmusicapi, '__version__', '?'))\"");
        }
    }
    out << "backend toolchain:\n" << versions;
    return out.str();
}

std::string boot_marks_text()
{
    std::lock_guard<std::mutex> lock(g_boot_mu);
    if (g_marks.empty())
        return "(none)\n";
    std::ostringstream out;
    for (const auto &m : g_marks)
        out << "  " << m.second << " ms  " << m.first << "\n";
    return out.str();
}

std::string cache_stats()
{
    auto dir_bytes = [](const std::string &dir, size_t &files) {
        files = 0;
        std::error_code ec;
        uintmax_t total = 0;
        for (std::filesystem::recursive_directory_iterator it(dir, ec), end;
             !ec && it != end; it.increment(ec))
        {
            if (it->is_regular_file(ec) && !ec)
            {
                total += it->file_size(ec);
                ++files;
            }
            ec.clear();
        }
        return total;
    };
    size_t audio_files = 0, thumb_files = 0, lyric_files = 0;
    const uintmax_t audio = dir_bytes(p100_config_path("audio_cache"), audio_files);
    const uintmax_t thumbs = dir_bytes(p100_config_path("thumb_cache"), thumb_files);
    const uintmax_t lyrics = dir_bytes(p100_config_path("lyrics"), lyric_files);
    std::ostringstream out;
    out << "audio_cache: " << audio_files << " files, " << (audio / (1024 * 1024)) << " MB\n";
    out << "thumb_cache: " << thumb_files << " files, " << (thumbs / (1024 * 1024)) << " MB\n";
    out << "lyrics: " << lyric_files << " files, " << (lyrics / (1024 * 1024)) << " MB\n";
    return out.str();
}

std::string newest_crash_tail()
{
    std::error_code ec;
    std::string newest;
    std::filesystem::file_time_type newest_time{};
    for (std::filesystem::directory_iterator it(p100_config_dir(), ec), end;
         !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (name.rfind("crash-", 0) != 0 && name.rfind("hang-", 0) != 0)
            continue;
        const auto t = it->last_write_time(ec);
        if (!ec && (newest.empty() || t > newest_time))
        {
            newest = it->path().string();
            newest_time = t;
        }
    }
    if (newest.empty())
        return "(none)\n";
    return newest + "\n" + tail_file(newest, 80);
}

std::string tasks_text()
{
    const auto rows = tasks::snapshot();
    std::ostringstream out;
    out << "workers: active=" << tasks::active_count() << " total=" << tasks::total_count() << "\n";
    const size_t start = rows.size() > 80 ? rows.size() - 80 : 0;
    double base = rows.empty() ? 0.0 : rows.front().start_ms;
    for (size_t i = 1; i < rows.size(); ++i)
        base = std::min(base, rows[i].start_ms);
    for (size_t i = start; i < rows.size(); ++i)
    {
        const auto &r = rows[i];
        out << "  #" << r.id << " " << r.name << " "
            << (r.active ? "running" : "done") << " start=+"
            << (long long)(r.start_ms - base) << "ms";
        if (!r.active)
            out << " dur=" << (long long)r.duration_ms << "ms";
        out << "\n";
    }
    return out.str();
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
    if (!g_history_enabled.load(std::memory_order_relaxed))
        return;
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

void logf(const char *category, const char *fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    log(category, buf);
}

void set_history_enabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_history_enabled.store(enabled, std::memory_order_relaxed);
    if (!enabled)
        g_lines.clear();
}

bool history_enabled()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_history_enabled.load(std::memory_order_relaxed);
}

void start_boot_clock()
{
    std::lock_guard<std::mutex> lock(g_boot_mu);
    g_boot_start = std::chrono::steady_clock::now();
    g_marks.clear();
    g_marks.emplace_back("boot", 0);
}

void mark(const char *name)
{
    std::lock_guard<std::mutex> lock(g_boot_mu);
    if (g_boot_start == std::chrono::steady_clock::time_point{})
        g_boot_start = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - g_boot_start)
                        .count();
    g_marks.emplace_back(name ? name : "mark", (long long)ms);
}

void set_context_provider(std::function<std::string()> provider)
{
    std::lock_guard<std::mutex> lock(g_provider_mu);
    g_context_provider = std::move(provider);
}

std::string bundle()
{
    std::ostringstream out;
    out << "crossfade diagnostics\n"
        << "====================\n"
        << runtime_info()
        << "\nBoot timings:\n" << boot_marks_text()
        << "\nBackend channels:\n" << backend_diagnostics()
        << "\nBackground workers:\n" << tasks_text()
        << "\nWarm start: active=" << (warm_start::server_active() ? 1 : 0)
        << " socket=" << warm_start::socket_path() << "\n";

    std::function<std::string()> provider;
    {
        std::lock_guard<std::mutex> lock(g_provider_mu);
        provider = g_context_provider;
    }
    if (provider)
    {
        const std::string ctx = provider();
        if (!ctx.empty())
            out << "\nApp state:\n" << ctx;
    }

    out << "\nCache:\n" << cache_stats()
        << "\nLibrary status:\n" << library::status_text()
        << "\nRecent app log:\n" << recent_log()
        << "\nBackend log (tail):\n" << tail_file(p100_config_path("backend.log"), 200)
        << "\nNewest crash log:\n" << newest_crash_tail();
    return out.str();
}

bool save_bundle(std::string &path, std::string &error)
{
    error.clear();
    const long long stamp = (long long)std::time(nullptr);
    char name[64];
    std::snprintf(name, sizeof(name), "diagnostics-%lld.txt", stamp);
    path = p100_config_path(name);
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        error = "Could not create " + path;
        return false;
    }
    const std::string text = bundle();
    out.write(text.data(), (std::streamsize)text.size());
    if (!out.good())
    {
        error = "Failed while writing " + path;
        return false;
    }
    return true;
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
