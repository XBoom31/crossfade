#include "ytdlp.h"
#include "json_util.h"
#include "paths.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <array>
#include <sstream>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <deque>
#include <thread>
#include <condition_variable>

#ifdef P100_WINDOWS
#include <windows.h>
#include "win_process.h"
#define popen _popen
#define pclose _pclose
#else
#include <poll.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <dirent.h>
#endif

// Directory listing used by the audio-cache scans. dirent on POSIX,
// std::filesystem on Windows; both return plain entry names.
static std::vector<std::string> list_dir_names(const std::string &dir)
{
    std::vector<std::string> names;
#ifdef P100_WINDOWS
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
        names.push_back(entry.path().filename().string());
#else
    DIR *dp = opendir(dir.c_str());
    if (!dp)
        return names;
    struct dirent *entry;
    while ((entry = readdir(dp)) != nullptr)
        names.push_back(entry->d_name);
    closedir(dp);
#endif
    return names;
}

#include <sys/stat.h>
#include <sys/types.h>

// ------------------ Backend process management ------------------
// Three backend child processes run the same script: the main channel serves
// searches / library / lyrics, the play channel serves foreground stream
// resolves, and the prefetch channel serves speculative stream warming. That
// keeps a slow background resolve from delaying a search, and keeps both away
// from a user click. Each keeps its own stream-URL cache and warm-client state.
#ifdef P100_WINDOWS
#define P100_PID_T HANDLE
#else
#define P100_PID_T pid_t
#endif

struct BackendChannel
{
    std::atomic<bool> ready{false}; // set once the {"status":"ready"} line arrives
    std::atomic<uint64_t> commands{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> last_ms{0};
    std::mutex ready_mutex;
    std::condition_variable ready_cv;
#ifndef P100_WINDOWS
    FILE *in = nullptr;
    FILE *out = nullptr;
#endif
    P100_PID_T pid = 0;
    std::mutex mutex;
    std::atomic<bool> dead{false}; // process exited / pipe EOF
#ifdef P100_WINDOWS
    // Named-analogue of the POSIX pair: two anonymous pipes (write end feeds
    // the child's stdin, read end drains its stdout) plus a reader thread,
    // because ReadFile on an anonymous pipe cannot timeout the way poll() can.
    HANDLE h_in_write = NULL;  // our end of the child's stdin
    HANDLE h_out_read = NULL;  // our end of the child's stdout
    std::thread reader;
    std::mutex q_mutex;
    std::condition_variable q_cv;
    std::deque<std::string> lines;
#endif
};
static BackendChannel g_backend;
static BackendChannel g_play_backend;
static BackendChannel g_prefetch_backend;
static std::atomic<bool> g_backend_stopping{false};
static std::atomic<bool> g_optional_backends_enabled{true};
static std::atomic<bool> g_backend_deferred{false};

static std::string g_warm_extractor = "1"; // P100_WARM_EXTRACTOR for children

// Forward decl: JSON helpers are defined below, but start_backend() needs
// one to surface the backend's own startup errors.
static std::string json_get_string(const std::string &json, const std::string &key);

static std::string g_ytdl_browser = "firefox";
static std::string g_auth_mode = "browser";
static BackendError g_backend_error;

std::string backend_error_user_message(const BackendError &err)
{
    switch (err.kind)
    {
    case BackendErrKind::Auth:
        return "You're signed out of YouTube Music. Sign in again from Settings.";
    case BackendErrKind::Network:
        return "No connection to YouTube Music. Check your internet and retry.";
    case BackendErrKind::Backend:
        return "The backend process is not responding. Try restarting the app.";
    default:
    {
        std::string s = err.message;
        size_t nl = s.find('\n');
        if (nl != std::string::npos)
            s = s.substr(0, nl);
        return s;
    }
    }
}

BackendError take_backend_error()
{
    BackendError e = g_backend_error;
    g_backend_error.clear();
    return e;
}

// Map the Python {"kind": "..."} tag to the enum. Unknown/missing kinds fall
// back to Backend so an error is never silently unclassified.
static BackendErrKind parse_error_kind(const std::string &response)
{
    std::string k = json_get_string(response, "kind");
    if (k == "auth")
        return BackendErrKind::Auth;
    if (k == "network")
        return BackendErrKind::Network;
    if (k == "backend")
        return BackendErrKind::Backend;
    if (k == "not_found")
        return BackendErrKind::NotFound;
    // No kind field (older backend script): guess from the message so stale
    // scripts still produce useful UI.
    std::string lower;
    lower.reserve(response.size());
    for (char c : response)
        lower += (char)tolower((unsigned char)c);
    if (lower.find("auth") != std::string::npos || lower.find("cookie") != std::string::npos ||
        lower.find("browser.json") != std::string::npos || lower.find("oauth") != std::string::npos ||
        lower.find("forbidden") != std::string::npos || lower.find("unauthorized") != std::string::npos)
        return BackendErrKind::Auth;
    if (lower.find("name resolution") != std::string::npos ||
        lower.find("name or service not known") != std::string::npos ||
        lower.find("temporary failure") != std::string::npos ||
        lower.find("network is unreachable") != std::string::npos ||
        lower.find("connection") != std::string::npos ||
        lower.find("timed out") != std::string::npos ||
        lower.find("timeout") != std::string::npos)
        return BackendErrKind::Network;
    return BackendErrKind::Backend;
}

static void record_backend_error(const std::string &response)
{
    g_backend_error.kind = parse_error_kind(response);
    g_backend_error.message = json_get_string(response, "error");
    if (g_backend_error.message.empty())
        g_backend_error.message = response;
}

// Backend commands must never hang the UI thread forever: a wedged Python
// process (or a pipe that lost its reader) previously blocked fgets for good.
// Everything up to kReadTimeoutSeconds, then a synthesized error response.
static constexpr int kReadTimeoutSeconds = 45;

void set_ytdl_browser(const std::string &browser)
{
    g_ytdl_browser = browser.empty() ? "firefox" : browser;
}

void set_auth_mode(const std::string &mode)
{
    g_auth_mode = mode == "oauth" ? "oauth" : "browser";
}

// Toggle the backend's in-process stream extraction (faster, experimental).
// Takes effect when the backend processes start.
void set_warm_stream_resolver(bool enabled)
{
    g_warm_extractor = enabled ? "1" : "0";
}

// ==== Execution environment (de-hardcoded) ====
// The binary may live in /bin, /usr/local/bin or the dev tree; find the
// py/ venv and backend script relative to the executable (argv[0] is not
// reliable when launched from $PATH), with env overrides as an escape
// hatch (P100_PYTHON, P100_BACKEND).
static std::string exe_dir()
{
    return p100_exe_dir();
}

// The backend script and venv may live in the installed share dir, the
// project root (dev tree), or next to the binary. base_dirs() asks the
// shared path helper first, then keeps the historical relative lookups.
static std::vector<std::string> base_dirs()
{
    std::vector<std::string> dirs;
    std::string exe = exe_dir();
    dirs.push_back(p100_share_dir());
    dirs.push_back(exe);
    dirs.push_back(exe + "/..");
    dirs.push_back(exe + "/../..");
#ifdef P100_SOURCE_DIR
    dirs.push_back(P100_SOURCE_DIR);
#endif
#ifdef P100_WINDOWS
    {
        char cwd[MAX_PATH];
        if (GetCurrentDirectoryA(MAX_PATH, cwd))
            dirs.push_back(cwd);
    }
#else
    if (char cwd[4096]; getcwd(cwd, sizeof(cwd)))
        dirs.push_back(cwd);
#endif
    return dirs;
}

static std::string resolve_file(const std::string &rel, bool want_dir = false)
{
    for (const auto &base : base_dirs())
    {
        std::string cand = base + "/" + rel;
        struct stat st;
        if (stat(cand.c_str(), &st) == 0)
        {
            if (want_dir ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode))
                return cand;
        }
    }
    return "";
}

static std::string backend_python()
{
    if (const char *env = getenv("P100_PYTHON"))
        return env;
#ifdef P100_WINDOWS
    if (std::string cand = resolve_file("venv/Scripts/python.exe"); !cand.empty())
        return cand;
    if (std::string cand = resolve_file("py/Scripts/python.exe"); !cand.empty())
        return cand;
    return "python"; // fall back to a system interpreter
#else
    // Per-user venv created by install.sh (installed share dir).
    if (std::string cand = resolve_file("venv/bin/python"); !cand.empty())
        return cand;
    // Historical dev layout: <project>/py/bin/python. Kept for existing
    // checkouts; the repo's py/ directory is now the shared backend package.
    if (std::string cand = resolve_file("py/bin/python"); !cand.empty())
        return cand;
    // install.sh puts the venv in the per-user share dir. A dev build in
    // build/ resolves the source tree as its share dir, so look there too.
    {
        std::string base;
        if (const char *xdg = getenv("XDG_DATA_HOME"); xdg && *xdg)
            base = xdg;
        else if (const char *home = getenv("HOME"); home && *home)
            base = std::string(home) + "/.local/share";
        if (!base.empty())
        {
            std::string cand = base + "/crossfade/venv/bin/python";
            struct stat st;
            if (stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                access(cand.c_str(), X_OK) == 0)
                return cand;
        }
    }
    return "python3"; // fall back to a system interpreter
#endif
}

static std::string backend_script()
{
    if (const char *env = getenv("P100_BACKEND"))
        return env;
    const char *name = g_auth_mode == "oauth" ? "p100-backend-oauth.py" : "p100-backend.py";
    if (std::string cand = resolve_file(name); !cand.empty())
        return cand;
    return exe_dir() + "/" + name;
}

static bool is_executable(const std::string &path)
{
#ifdef P100_WINDOWS
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

// Resolve the yt-dlp binary instead of trusting the caller's PATH. This is
// what makes the installed layout work when p100 is launched from a desktop
// menu, where ~/.local/bin and the venv are usually not on PATH.
static std::string ytdlp_bin()
{
    if (const char *env = getenv("P100_YTDLP"))
        return env;

#ifdef P100_WINDOWS
    const char *sibling = "yt-dlp.exe";
    const char *venv_rel = "/venv/Scripts/yt-dlp.exe";
#else
    const char *sibling = "yt-dlp";
    const char *venv_rel = "/venv/bin/yt-dlp";
#endif
    std::string python = backend_python();
    size_t slash = python.find_last_of('/');
    if (slash != std::string::npos)
    {
        std::string candidate = python.substr(0, slash + 1) + sibling;
        if (is_executable(candidate))
            return candidate;
    }

    for (const auto &base : base_dirs())
    {
        std::string candidate = base + venv_rel;
        if (is_executable(candidate))
            return candidate;
    }

    return "yt-dlp";
}

// stderr redirect for popen/system command strings (POSIX vs cmd.exe).
#ifdef P100_WINDOWS
static const char kShellErrRedirect[] = " 2>NUL";
#else
static const char kShellErrRedirect[] = " 2>/dev/null";
#endif

static std::string shell_quote(const std::string &value)
{
#ifdef P100_WINDOWS
    // cmd.exe (used by system/popen) wants double quotes.
    std::string out = "\"";
    for (char c : value)
        out += c;
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : value)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
#endif
}

#ifdef P100_WINDOWS
// Reader thread: ReadFile on an anonymous pipe has no timeout, so a dedicated
// thread drains the pipe into line-sized chunks and hands them to
// backend_read_line via a small queue (mirrors the POSIX poll() semantics,
// including the 45 s deadline and the same error payloads).
static void backend_reader_thread(BackendChannel *ch)
{
    std::string pending;
    char buf[8192];
    for (;;)
    {
        DWORD n = 0;
        BOOL ok = ReadFile(ch->h_out_read, buf, sizeof(buf), &n, nullptr);
        if (!ok || n == 0)
        {
            // EOF / broken pipe: the backend process is gone.
            ch->dead = true;
            {
                std::lock_guard<std::mutex> lock(ch->q_mutex);
                if (!pending.empty())
                    ch->lines.push_back(pending);
                ch->lines.push_back(""); // sentinel: dead, nothing more coming
            }
            ch->q_cv.notify_all();
            return;
        }
        pending.append(buf, (size_t)n);
        {
            std::lock_guard<std::mutex> lock(ch->q_mutex);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos)
            {
                std::string line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                ch->lines.push_back(std::move(line));
            }
        }
        ch->q_cv.notify_all();
    }
}

static std::string backend_read_line(BackendChannel &ch)
{
    // Same 45 s deadline and error payloads as the POSIX poll() loop below.
    std::unique_lock<std::mutex> lock(ch.q_mutex);
    time_t deadline = time(nullptr) + kReadTimeoutSeconds;
    while (ch.lines.empty())
    {
        if (ch.dead.load())
            return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
        if (g_backend_stopping.load(std::memory_order_relaxed))
            return "{\"error\":\"Backend stopping\",\"kind\":\"backend\"}";
        int remaining = (int)(deadline - time(nullptr));
        if (remaining <= 0)
        {
            fprintf(stderr, "ERROR: Backend read timed out after %d s.\n", kReadTimeoutSeconds);
            return "{\"error\":\"Backend did not respond in time\",\"kind\":\"backend\"}";
        }
        ch.q_cv.wait_for(lock, std::chrono::seconds(1));
    }
    std::string line = std::move(ch.lines.front());
    ch.lines.pop_front();
    if (line.empty())
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    return line;
}

static bool channel_write_line(BackendChannel &ch, const std::string &cmd)
{
    if (ch.h_in_write == NULL)
        return false;
    std::string buf = cmd + "\n";
    DWORD written = 0;
    return WriteFile(ch.h_in_write, buf.data(), (DWORD)buf.size(), &written, nullptr) &&
           written == buf.size();
}

static void channel_close(BackendChannel &ch)
{
    if (ch.h_in_write)
        CloseHandle(ch.h_in_write);
    if (ch.h_out_read)
        CloseHandle(ch.h_out_read);
    ch.h_in_write = ch.h_out_read = NULL;
}
#endif // P100_WINDOWS

#ifndef P100_WINDOWS
static std::string backend_read_line(BackendChannel &ch)
{
    // Raw fd reads (not fgets): stdio buffering hides data from poll(), which
    // would spuriously time out mid-line. One line per command; the backend
    // replies only after a command, so the fd is quiet between reads.
    static const size_t MAX_LINE_SIZE = 10 * 1024 * 1024;

    if (!ch.out)
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    int fd = fileno(ch.out);
    if (fd < 0)
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";

    std::string line;
    char buf[8192];
    size_t total_read = 0;
    time_t deadline = time(nullptr) + kReadTimeoutSeconds;

    while (line.find('\n') == std::string::npos)
    {
        if (ch.dead.load())
            return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
        if (g_backend_stopping.load(std::memory_order_relaxed))
            return "{\"error\":\"Backend stopping\",\"kind\":\"backend\"}";

        int remaining = (int)(deadline - time(nullptr));
        if (remaining <= 0)
        {
            fprintf(stderr, "ERROR: Backend read timed out after %d s.\n", kReadTimeoutSeconds);
            return "{\"error\":\"Backend did not respond in time\",\"kind\":\"backend\"}";
        }

        struct pollfd pfd = {fd, POLLIN, 0};
        const int wait_ms = std::min(remaining * 1000, 200);
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            ch.dead = true;
            return "{\"error\":\"Backend pipe error\",\"kind\":\"backend\"}";
        }
        if (pr == 0)
            continue; // poll timed out: loop re-checks the deadline

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            ch.dead = true;
            return "{\"error\":\"Backend pipe error\",\"kind\":\"backend\"}";
        }
        if (n == 0)
        {
            // EOF: the backend process exited (crash, killed, bad auth).
            ch.dead = true;
            return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
        }
        line.append(buf, (size_t)n);
        total_read += (size_t)n;
        if (total_read > MAX_LINE_SIZE)
        {
            fprintf(stderr, "ERROR: Backend response too large ( > %zu MB). Aborting read.\n",
                    MAX_LINE_SIZE / (1024 * 1024));
            return "{\"error\":\"Response too large\",\"kind\":\"backend\"}";
        }
    }

    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    return line;
}
#endif // !P100_WINDOWS

static void backend_write_line(BackendChannel &ch, const std::string &cmd)
{
    std::lock_guard<std::mutex> lock(ch.mutex);
#ifndef P100_WINDOWS
    if (ch.in)
    {
        fputs((cmd + "\n").c_str(), ch.in);
        fflush(ch.in);
    }
#else
    channel_write_line(ch, cmd);
#endif
}

static bool spawn_backend(BackendChannel &ch)
{
#ifdef P100_WINDOWS
    const std::string python = backend_python();
    const std::string script = backend_script();

    // Child env: same contract as the POSIX branch below (setenv before exec).
    _putenv_s("P100_BROWSER", g_ytdl_browser.c_str());
    _putenv_s("P100_AUTH_FILE", backend_auth_file_path().c_str());
    _putenv_s("P100_CONFIG_DIR", p100_config_dir().c_str());
    _putenv_s("P100_YTDLP", ytdlp_bin().c_str());
    _putenv_s("P100_WARM_EXTRACTOR", g_warm_extractor.c_str());
    _putenv_s("PYTHONUNBUFFERED", "1");
    {
        // venv\Scripts first, so yt-dlp resolves like on POSIX.
        std::string dir = python;
        size_t slash = dir.find_last_of("/\\");
        if (slash != std::string::npos)
        {
            std::string py_dir = dir.substr(0, slash);
            const char *old_path = getenv("PATH");
            std::string new_path = py_dir + ";" + (old_path ? old_path : "");
            _putenv_s("PATH", new_path.c_str());
        }
    }

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE in_read = NULL, out_write = NULL;
    if (!CreatePipe(&in_read, &ch.h_in_write, &sa, 0) ||
        !CreatePipe(&ch.h_out_read, &out_write, &sa, 0))
    {
        fprintf(stderr, "CreatePipe failed (%lu)\n", GetLastError());
        return false;
    }
    SetHandleInformation(ch.h_in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ch.h_out_read, HANDLE_FLAG_INHERIT, 0);

    // Backend stderr -> the same log file the POSIX child uses.
    p100_ensure_config_dir();
    HANDLE err_handle = CreateFileA((p100_config_dir() + "/backend.log").c_str(),
                                    FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    std::string cmdline = "\"" + python + "\" \"" + script + "\"";
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_read;
    si.hStdOutput = out_write;
    si.hStdError = err_handle ? err_handle : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (in_read)
        CloseHandle(in_read);
    if (out_write)
        CloseHandle(out_write);
    if (err_handle && err_handle != INVALID_HANDLE_VALUE)
        CloseHandle(err_handle);
    if (!ok)
    {
        fprintf(stderr, "Failed to launch backend '%s': %lu\n", cmdline.c_str(), GetLastError());
        channel_close(ch);
        return false;
    }
    ch.pid = pi.hProcess;
    ch.dead = false;
    CloseHandle(pi.hThread);
    ch.reader = std::thread(backend_reader_thread, &ch);

    std::string ready = backend_read_line(ch);
    bool ready_ok = ready.find("\"status\"") != std::string::npos &&
                    ready.find("\"ready\"") != std::string::npos;
    if (!ready_ok)
    {
        fprintf(stderr, "Backend not ready: %s\n", ready.c_str());
        if (ready.find("\"error\"") != std::string::npos)
            record_backend_error(ready);
        ch.dead = true;
        ch.ready_cv.notify_all();
        return false;
    }
    ch.ready.store(true, std::memory_order_release);
    ch.ready_cv.notify_all();
    return true;
#else
    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0)
    {
        perror("pipe");
        return false;
    }
    pid_t pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return false;
    }
    if (pid == 0)
    {
        close(to_child[1]);
        close(from_child[0]);
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        // The backend's stderr is near-constant yt-dlp probe noise (client
        // fallback retries on every stream resolve) — send it to a log file
        // instead of spamming whichever terminal p100 was launched from.
        {
            p100_ensure_config_dir();
            std::string log_dir = p100_config_dir();
            FILE *lf = fopen((log_dir + "/backend.log").c_str(), "a");
            if (lf)
            {
                dup2(fileno(lf), STDERR_FILENO);
                fclose(lf);
            }
        }
        // Single cookie source for all yt-dlp invocations.
        setenv("P100_BROWSER", g_ytdl_browser.c_str(), 1);
        const std::string auth_file = backend_auth_file_path();
        setenv("P100_AUTH_FILE", auth_file.c_str(), 1);
        // Keep the Python backend, its yt-dlp subprocesses and the C++ side
        // pointed at the same data directory and interpreter.
        setenv("P100_CONFIG_DIR", p100_config_dir().c_str(), 1);
        setenv("P100_YTDLP", ytdlp_bin().c_str(), 1);
        setenv("P100_WARM_EXTRACTOR", g_warm_extractor.c_str(), 1);
        setenv("PYTHONUNBUFFERED", "1", 1);
        const std::string python = backend_python();
        const std::string script = backend_script();
        size_t python_slash = python.find_last_of('/');
        if (python_slash != std::string::npos)
        {
            std::string python_dir = python.substr(0, python_slash);
            const char *old_path = getenv("PATH");
            std::string new_path = python_dir + (old_path ? ":" + std::string(old_path) : "");
            setenv("PATH", new_path.c_str(), 1);
        }
        execlp(python.c_str(), python.c_str(), script.c_str(), nullptr);
        fprintf(stderr, "Failed to exec backend '%s %s': ", python.c_str(), script.c_str());
        perror("execlp");
        exit(1);
    }
    close(to_child[0]);
    close(from_child[1]);
    ch.in = fdopen(to_child[1], "w");
    ch.out = fdopen(from_child[0], "r");
    if (!ch.in || !ch.out)
    {
        perror("fdopen");
        kill(pid, SIGTERM);
        return false;
    }
    ch.pid = pid;
    ch.dead = false;
    std::string ready = backend_read_line(ch);
    bool ready_ok = ready.find("\"status\"") != std::string::npos &&
                    ready.find("\"ready\"") != std::string::npos;
    if (!ready_ok)
    {
        if (!g_backend_stopping.load(std::memory_order_relaxed))
        {
            fprintf(stderr, "Backend not ready: %s\n", ready.c_str());
            // Surface the backend's own error (usually missing auth file).
            if (ready.find("\"error\"") != std::string::npos)
                record_backend_error(ready);
        }
        ch.dead = true;
        ch.ready_cv.notify_all();
        return false;
    }
    ch.ready.store(true, std::memory_order_release);
    ch.ready_cv.notify_all();
    return true;
#endif // !P100_WINDOWS
}

static std::thread g_optional_backend_thread;

void set_optional_backends_enabled(bool enabled)
{
    g_optional_backends_enabled.store(enabled, std::memory_order_relaxed);
}

void set_backend_deferred(bool deferred)
{
    g_backend_deferred.store(deferred, std::memory_order_relaxed);
}

bool start_backend()
{
    g_backend_stopping.store(false, std::memory_order_relaxed);
    for (BackendChannel *ch : {&g_backend, &g_play_backend, &g_prefetch_backend})
    {
        ch->ready.store(false, std::memory_order_relaxed);
        ch->dead.store(false, std::memory_order_relaxed);
    }
    // Only the main channel blocks startup. The play/prefetch workers are
    // optimizations: spawning them takes ~2.5 s of Python startup, so they
    // come up in the background and stay on the main channel until ready.
    bool main_ok = spawn_backend(g_backend);

    // A backend restart during the session must not assign over a still
    // joinable thread (std::terminate).
    if (g_optional_backend_thread.joinable())
        g_optional_backend_thread.join();
    if (!g_optional_backends_enabled.load(std::memory_order_relaxed))
        return main_ok;
    g_optional_backend_thread = std::thread([]() {
        if (!spawn_backend(g_play_backend) &&
            !g_backend_stopping.load(std::memory_order_relaxed))
            fprintf(stderr, "Play backend worker unavailable; stream resolves "
                            "share the main backend channel.\n");
        if (!spawn_backend(g_prefetch_backend) &&
            !g_backend_stopping.load(std::memory_order_relaxed))
            fprintf(stderr, "Prefetch backend worker unavailable; speculative "
                            "warmups share the main backend channel.\n");
    });
    return main_ok;
}

void cancel_backend_startup()
{
    g_backend_stopping.store(true, std::memory_order_relaxed);
    for (BackendChannel *ch : {&g_backend, &g_play_backend, &g_prefetch_backend})
        ch->ready_cv.notify_all();
}

std::string backend_diagnostics()
{
    auto line = [](const char *name, const BackendChannel &ch) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%-8s ready=%d dead=%d pid=%llu commands=%llu errors=%llu last=%llums",
                      name, (int)ch.ready.load(), (int)ch.dead.load(),
                      (unsigned long long)(uintptr_t)ch.pid,
                      (unsigned long long)ch.commands.load(),
                      (unsigned long long)ch.errors.load(),
                      (unsigned long long)ch.last_ms.load());
        return std::string(buf);
    };
    std::string out;
    out += line("main", g_backend);
    out += '\n';
    out += line("play", g_play_backend);
    out += '\n';
    out += line("prefetch", g_prefetch_backend);
    out += '\n';
    return out;
}

void stop_backend()
{
    // Cancel an in-flight optional-channel ready wait, then join before we
    // close its pipes. Without this a backend spawned while quitting could
    // delay shutdown by the full read timeout.
    g_backend_stopping.store(true, std::memory_order_relaxed);
    if (g_optional_backend_thread.joinable())
        g_optional_backend_thread.join();
    for (BackendChannel *ch : {&g_backend, &g_play_backend, &g_prefetch_backend})
    {
        backend_write_line(*ch, "quit");
#ifdef P100_WINDOWS
        channel_close(*ch);
        if (ch->pid)
        {
            TerminateProcess(ch->pid, 0);
            WaitForSingleObject(ch->pid, 5000);
            CloseHandle(ch->pid);
            ch->pid = NULL;
        }
        if (ch->reader.joinable())
            ch->reader.join();
#else
        if (ch->in)
            fclose(ch->in);
        if (ch->out)
            fclose(ch->out);
        if (ch->pid > 0)
        {
            kill(ch->pid, SIGTERM);
            waitpid(ch->pid, nullptr, 0);
        }
        ch->in = nullptr;
        ch->out = nullptr;
        ch->pid = 0;
#endif
    }
}

// ------------------ JSON parsing with Unicode support ------------------

static std::string json_get_string(const std::string &json, const std::string &key)
{
    return jsonutil::get_string(json, key);
}


static int json_get_int(const std::string &json, const std::string &key)
{
    return jsonutil::get_int(json, key);
}

static constexpr int kJsonMaxDepth = 256;
static constexpr size_t kJsonMaxInput = 16u * 1024u * 1024u;

static std::string json_find_object(const std::string &json, size_t key_pos)
{
    size_t obj_start = json.rfind('{', key_pos);
    if (obj_start == std::string::npos)
        return "";
    // Use a forward scan since we don't have json_object_end visible here
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t i = obj_start; i < json.size(); ++i)
    {
        char c = json[i];
        if (in_str)
        {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                in_str = false;
        }
        else if (c == '"')
            in_str = true;
        else if (c == '{')
        {
            if (++depth > kJsonMaxDepth)
                return "";
        }
        else if (c == '}')
        {
            if (--depth == 0)
                return json.substr(obj_start, i - obj_start + 1);
        }
    }
    return "";
}

// Exposed for liked songs persistence
static void parse_track_object(const std::string &obj, Track &t)
{
    t.id = json_get_string(obj, "videoId");
    if (t.id.empty())
        t.id = json_get_string(obj, "id");
    t.title = json_get_string(obj, "title");
    t.artist = json_get_string(obj, "artist");
    t.artist_id = json_get_string(obj, "artist_id");
    t.album = json_get_string(obj, "album");
    t.album_id = json_get_string(obj, "album_id");
    t.track_number = json_get_int(obj, "track_number");
    t.thumbnail_url = json_get_string(obj, "thumbnail_url");
    t.duration_sec = json_get_int(obj, "duration_sec");
    if (t.id.empty())
        t.id = json_get_string(obj, "videoId");
    // YouTube Music playlist items store the artist under "artists": [{"name":...}]
    if (t.artist.empty())
        t.artist = json_get_string(obj, "name");
    if (t.thumbnail_url.empty())
        t.thumbnail_url = json_get_string(obj, "thumbnail");
    if (t.thumbnail_url.empty())
    {
        // thumbnails: [{"url":...}]
        size_t upos = obj.find("\"url\"");
        if (upos != std::string::npos)
        {
            size_t c = obj.find(':', upos);
            if (c != std::string::npos)
            {
                size_t q = obj.find('"', c + 1);
                if (q != std::string::npos)
                {
                    size_t q2 = obj.find('"', q + 1);
                    if (q2 != std::string::npos)
                        t.thumbnail_url = obj.substr(q + 1, q2 - q - 1);
                }
            }
        }
    }
    if (t.duration_sec == 0)
        t.duration_sec = json_get_int(obj, "duration_seconds");
    if (t.duration_sec == 0)
        t.duration_sec = json_get_int(obj, "duration");
    // Optional party provenance. Missing in older files, hence the defaults.
    t.requested_by = json_get_string(obj, "requested_by");
    t.guest_id = json_get_string(obj, "guest_id");
    t.from_party = json_get_string(obj, "from_party") == "true" ||
                   obj.find("\"from_party\":true") != std::string::npos;
    t.party_round = json_get_int(obj, "party_round");
    if (!t.id.empty())
        t.url = "https://www.youtube.com/watch?v=" + t.id;
}

std::vector<Track> parse_track_array(const std::string &json)
{
    if (json.size() > kJsonMaxInput)
        return {};
    std::vector<Track> tracks;
    size_t pos = 0;
    int count = 0;
    while ((pos = json.find("\"id\":", pos)) != std::string::npos && count < 500)
    {
        std::string obj = json_find_object(json, pos);
        if (obj.empty())
        {
            pos += 5;
            continue;
        }
        Track t;
        parse_track_object(obj, t);
        if (!t.id.empty() && !t.title.empty())
            tracks.push_back(std::move(t));
        pos = json.find('}', pos);
        if (pos == std::string::npos)
            break;
        pos += 1;
        count++;
    }
    if (tracks.empty())
    {
        // Some backends may emit videoId instead of id
        pos = 0;
        count = 0;
        while ((pos = json.find("\"videoId\":", pos)) != std::string::npos && count < 500)
        {
            std::string obj = json_find_object(json, pos);
            if (obj.empty())
            {
                pos += 10;
                continue;
            }
            Track t;
            parse_track_object(obj, t);
            if (!t.id.empty() && !t.title.empty())
                tracks.push_back(std::move(t));
            pos = json.find('}', pos);
            if (pos == std::string::npos)
                break;
            pos += 1;
            count++;
        }
    }
    return tracks;
}


static bool json_get_bool(const std::string &json, const std::string &key, bool def = false)
{
    return jsonutil::get_bool(json, key, def);
}

// End offset (inclusive) of the JSON object starting at obj_start ('{').
// Quote-aware so braces inside a lyric string don't confuse the scan.
static size_t json_object_end(const std::string &json, size_t obj_start)
{
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t i = obj_start; i < json.size(); ++i)
    {
        char c = json[i];
        if (in_str)
        {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                in_str = false;
        }
        else if (c == '"')
            in_str = true;
        else if (c == '{')
        {
            if (++depth > kJsonMaxDepth)
                return std::string::npos;
        }
        else if (c == '}')
        {
            if (--depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

// Walk the objects in a top-level JSON array (no key) and invoke `each`.
template <typename Fn>
static void for_each_json_array_object(const std::string &json, Fn &&each)
{
    const size_t arr = json.find('[');
    if (arr == std::string::npos)
        return;
    size_t pos = arr + 1;
    int guard = 0;
    while (pos < json.size() && guard++ < 1000)
    {
        while (pos < json.size() &&
               (isspace((unsigned char)json[pos]) || json[pos] == ','))
            ++pos;
        if (pos >= json.size() || json[pos] == ']')
            break;
        if (json[pos] != '{')
            break;
        const size_t end = json_object_end(json, pos);
        if (end == std::string::npos)
            break;
        each(json.substr(pos, end - pos + 1));
        pos = end + 1;
    }
}

// Walk the objects in a JSON array field and invoke `each` with each raw
// object substring. Shared by the artist/search-all/podcast/Spotify parsers.
template <typename Fn>
static void for_each_json_object(const std::string &json, const std::string &key, Fn &&each)
{
    const size_t key_pos = json.find("\"" + key + "\":");
    if (key_pos == std::string::npos)
        return;
    const size_t arr = json.find('[', key_pos);
    if (arr == std::string::npos)
        return;
    size_t pos = arr + 1;
    while (pos < json.size())
    {
        while (pos < json.size() &&
               (isspace((unsigned char)json[pos]) || json[pos] == ','))
            ++pos;
        if (pos >= json.size() || json[pos] == ']')
            break;
        if (json[pos] != '{')
            break;
        const size_t end = json_object_end(json, pos);
        if (end == std::string::npos)
            break;
        each(json.substr(pos, end - pos + 1));
        pos = end + 1;
    }
}

static void replace_all_utf8(std::string &s, const char *from, const char *to)
{
    const std::string needle(from);
    const std::string replacement(to);
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos)
    {
        s.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

// Some lyric providers emit full-width CJK punctuation, which the UI font
// has no glyphs for (it would render as "?"). Map the common marks to ASCII.
static void normalize_lyric_text(std::string &s)
{
    static const char *const pairs[][2] = {
        {"\xEF\xBC\x9A", ": "},  // full-width colon
        {"\xEF\xBC\x8C", ", "},  // full-width comma
        {"\xEF\xBC\x9B", "; "},  // full-width semicolon
        {"\xEF\xBC\x81", "!"},   // full-width exclamation mark
        {"\xEF\xBC\x9F", "?"},   // full-width question mark
        {"\xEF\xBC\x88", "("},   // full-width left parenthesis
        {"\xEF\xBC\x89", ")"},   // full-width right parenthesis
        {"\xEF\xBC\x8E", "."},   // full-width full stop
        {"\xE3\x80\x81", ", "},  // ideographic comma
        {"\xE3\x80\x82", ". "},  // ideographic full stop
        {"\xE3\x80\x80", " "},   // ideographic space
    };
    for (const auto &p : pairs)
        replace_all_utf8(s, p[0], p[1]);
}

bool parse_lyrics_json(const std::string &json, Lyrics &out)
{
    if (json.size() > kJsonMaxInput)
        return false;
    if (json.empty() || json.find("\"error\"") != std::string::npos)
        return false;

    out.ok = true;
    if (json_get_bool(json, "none"))
    {
        out.none = true;
        return true;
    }

    out.synced = json_get_bool(json, "synced");
    out.source = json_get_string(json, "source");

    size_t lines_key = json.find("\"lines\":");
    if (lines_key == std::string::npos)
        return true;
    size_t arr = json.find('[', lines_key);
    if (arr == std::string::npos)
        return true;

    size_t pos = arr + 1;
    while (pos < json.size())
    {
        while (pos < json.size() && (isspace((unsigned char)json[pos]) || json[pos] == ','))
            pos++;
        if (pos >= json.size() || json[pos] == ']')
            break;
        if (json[pos] != '{')
            break;
        size_t end = json_object_end(json, pos);
        if (end == std::string::npos)
            break;
        std::string obj = json.substr(pos, end - pos + 1);
        out.times.push_back(json_get_int(obj, "t"));
        std::string text = json_get_string(obj, "text");
        normalize_lyric_text(text);
        out.lines.push_back(std::move(text));
        pos = end + 1;
    }
    return true;
}

// ------------------ High‑level commands ------------------
static std::string send_command_on(BackendChannel &ch, const std::string &cmd)
{
    // A command can arrive while the main worker is still importing Python
    // (session restore does exactly this). Wait on the caller's thread rather
    // than making the UI wait during startup. Optional channels are only
    // selected once ready, so this normally only affects the main channel.
    if (!ch.ready.load(std::memory_order_acquire))
    {
        if (g_backend_deferred.load(std::memory_order_relaxed))
            return "{\"error\":\"Backend parked\",\"kind\":\"backend\"}";
        std::unique_lock<std::mutex> rl(ch.ready_mutex);
        ch.ready_cv.wait_for(rl, std::chrono::milliseconds(15000), [&] {
            return ch.ready.load(std::memory_order_acquire) || ch.dead.load() ||
                   g_backend_stopping.load(std::memory_order_relaxed);
        });
        if (!ch.ready.load(std::memory_order_acquire))
            return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    }
    const auto cmd_start = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(ch.mutex);
#ifndef P100_WINDOWS
    if (!ch.in)
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    fputs((cmd + "\n").c_str(), ch.in);
    if (ferror(ch.in) || fflush(ch.in) != 0)
    {
        // The pipe broke (backend died): the read below will hit EOF and
        // return the "not running" error, but short-circuit for clarity.
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    }
#else
    if (!channel_write_line(ch, cmd))
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
#endif
    std::string response = backend_read_line(ch);
    // Remember the failure reason (with its classified kind) so the UI can
    // tell the user what to do (stale cookies → re-run `ytmusicapi browser`,
    // offline → check connection, dead backend → restart).
    if (response.empty())
    {
        // A dead/piped-closed backend previously looked like "empty data";
        // make it an explicit error instead.
        g_backend_error.kind = BackendErrKind::Backend;
        g_backend_error.message = "Backend not running";
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    }
    ch.commands.fetch_add(1, std::memory_order_relaxed);
    ch.last_ms.store((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - cmd_start)
                         .count(),
                     std::memory_order_relaxed);
    if (response.find("\"error\"") != std::string::npos)
    {
        ch.errors.fetch_add(1, std::memory_order_relaxed);
        record_backend_error(response);
    }
    else if (g_backend_error.ok())
        g_backend_error.clear();
    return response;
}

static std::string send_command(const std::string &cmd)
{
    return send_command_on(g_backend, cmd);
}

// Foreground stream resolves (play/play_fresh) go to the dedicated play
// worker so they never queue behind searches/lyrics on the main channel.
// Speculative prefetches use send_prefetch_command() below. Both fall back to
// the main channel if their worker is down.
static std::string send_play_command(const std::string &cmd)
{
    if (g_play_backend.ready.load(std::memory_order_acquire))
        return send_command_on(g_play_backend, cmd);
    return send_command_on(g_backend, cmd);
}

// Speculative prefetches get their own channel. They can take tens of seconds
// (a full yt-dlp resolve), so sharing the main channel starved searches and
// metadata lookups; sharing the play channel would make a click wait instead.
static std::string send_prefetch_command(const std::string &cmd)
{
    if (g_prefetch_backend.ready.load(std::memory_order_acquire))
        return send_command_on(g_prefetch_backend, cmd);
    return send_command_on(g_backend, cmd);
}

// ------------------ Cache directory / cookie source ------------------
static std::string cache_dir()
{
    return p100_config_dir();
}

// Create dir (and parents) if needed. Returns false when it still isn't a
// directory, so callers can skip writing into a path that can't exist.
static bool ensure_dir(const std::string &dir)
{
    if (dir.empty())
        return false;
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec))
        return true;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

// Set whenever cached audio changes on disk; consumed by the main loop to
// refresh the Downloads list in-session.
std::atomic<bool> g_downloads_dirty{false};

static std::string cookie_arg()
{
    return " --cookies-from-browser " + g_ytdl_browser;
}

static Track fetch_track_metadata(const std::string &video_id)
{
    Track t;
    t.id = video_id;
    t.url = "https://www.youtube.com/watch?v=" + video_id;

    std::string cmd = shell_quote(ytdlp_bin()) + " -j --skip-download " + cookie_arg() + " " + t.url + kShellErrRedirect;
    std::array<char, 128> buffer;
    std::string result;
#ifdef P100_WINDOWS
    std::unique_ptr<FILE, decltype(&winproc::pclose_hidden)> pipe(
        winproc::popen_hidden(cmd, "r"), winproc::pclose_hidden);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif

    if (pipe)
    {
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        {
            result += buffer.data();
        }
    }
    if (result.empty())
    {
        // Some tracks are only fetchable without cookie access (public
        // content); don't fail the whole cache write because of that.
        cmd = shell_quote(ytdlp_bin()) + " -j --skip-download " + t.url + kShellErrRedirect;
#ifdef P100_WINDOWS
        std::unique_ptr<FILE, decltype(&winproc::pclose_hidden)> p2(
            winproc::popen_hidden(cmd, "r"), winproc::pclose_hidden);
#else
        std::unique_ptr<FILE, decltype(&pclose)> p2(popen(cmd.c_str(), "r"), pclose);
#endif
        if (p2)
        {
            while (fgets(buffer.data(), buffer.size(), p2.get()) != nullptr)
            {
                result += buffer.data();
            }
        }
    }

    t.title = json_get_string(result, "title");
    t.artist = json_get_string(result, "artist");
    if (t.artist.empty())
        t.artist = json_get_string(result, "uploader");
    t.thumbnail_url = json_get_string(result, "thumbnail");
    t.duration_sec = json_get_int(result, "duration");
    return t;
}

// Minimal JSON string escaping for the metadata sidecar.
static std::string json_escape(const std::string &s)
{
    return jsonutil::escape(s);
}

static void save_metadata(const Track &t, const std::string &audio_path)
{
    std::string meta_path = audio_path + ".meta.json";
    std::ofstream out(meta_path);
    if (!out)
        return;

    out << "{"
        << "\"id\":\"" << json_escape(t.id) << "\","
        << "\"title\":\"" << json_escape(t.title) << "\","
        << "\"artist\":\"" << json_escape(t.artist) << "\","
        << "\"artist_id\":\"" << json_escape(t.artist_id) << "\","
        << "\"album\":\"" << json_escape(t.album) << "\","
        << "\"album_id\":\"" << json_escape(t.album_id) << "\","
        << "\"track_number\":" << t.track_number << ","
        << "\"thumbnail_url\":\"" << json_escape(t.thumbnail_url) << "\","
        << "\"duration_sec\":" << t.duration_sec
        << "}";
    out.close();
}

// ------------------ Metadata sidecar coordination ------------------
// A delete racing the detached finalize thread (which fetches metadata over
// the network before writing) must not leave an orphan .meta.json behind:
// deletes register the id first, bulk deletes block all writes for their
// scope, and finalize re-checks under the same lock before writing.
static std::mutex g_meta_mutex;
static std::unordered_set<std::string> g_meta_cancelled;
static bool g_meta_writes_blocked = false;

void cancel_pending_metadata(const std::string &video_id)
{
    if (video_id.empty())
        return;
    std::lock_guard<std::mutex> lock(g_meta_mutex);
    g_meta_cancelled.insert(video_id);
}

void with_metadata_writes_blocked(const std::function<void()> &fn)
{
    std::lock_guard<std::mutex> lock(g_meta_mutex);
    const bool prev = g_meta_writes_blocked;
    g_meta_writes_blocked = true;
    fn();
    g_meta_writes_blocked = prev;
}

// ------------------ Public functions ------------------
std::vector<Track> fetch_recommendations(bool force_fresh, int limit)
{
    if (limit <= 0)
        limit = 40;
    std::string path = cache_dir() + "/recommendations_cache.json";
    if (!force_fresh)
    {
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && st.st_size > 0)
        {
            time_t now = time(nullptr);
            if (now - st.st_mtime < 3600)
            {
                std::ifstream file(path);
                if (file.good())
                {
                    std::string json((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
                    file.close();
                    if (!json.empty())
                        return parse_track_array(json);
                }
            }
        }
    }

    std::string cmd = "recommendations";
    if (limit != 40)
        cmd += " " + std::to_string(limit);
    std::string response = send_command(cmd);
    // An empty response means the backend wasn't reachable; don't treat it
    // as success or we'd cache an empty list for an hour.
    if (response.empty() || response.find("\"error\"") != std::string::npos)
    {
        if (!response.empty())
            fprintf(stderr, "Backend error: %s\n", response.c_str());
        return {};
    }

    mkdir(cache_dir().c_str(), 0755);
    std::ofstream out(path);
    if (out.good())
    {
        out << response;
        out.close();
    }
    return parse_track_array(response);
}

std::vector<Track> fetch_search(const std::string &query, bool *failed)
{
    if (failed)
        *failed = false;
    std::string response = send_command("search " + query);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
    {
        if (failed)
            *failed = true;
        return {};
    }
    return parse_track_array(response);
}

bool fetch_track_meta(const std::string &video_id, Track &out, std::string &error)
{
    error.clear();
    if (video_id.empty())
    {
        error = "missing video id";
        return false;
    }
    std::string response = send_command("meta " + video_id);
    if (response.empty())
    {
        error = "backend not running";
        return false;
    }
    if (response.find("\"error\"") != std::string::npos)
    {
        error = json_get_string(response, "error");
        if (error.empty())
            error = "lookup failed";
        return false;
    }
    Track t;
    t.id = json_get_string(response, "id");
    if (t.id.empty())
        t.id = video_id;
    t.title = json_get_string(response, "title");
    t.artist = json_get_string(response, "artist");
    t.thumbnail_url = json_get_string(response, "thumbUrl");
    if (t.thumbnail_url.empty())
        t.thumbnail_url = json_get_string(response, "thumbnail_url");
    t.duration_sec = json_get_int(response, "durationSec");
    if (t.duration_sec == 0)
        t.duration_sec = json_get_int(response, "duration_sec");
    t.url = "https://www.youtube.com/watch?v=" + t.id;
    if (t.title.empty())
    {
        error = "no metadata";
        return false;
    }
    out = std::move(t);
    return true;
}

std::vector<Track> fetch_radio(const std::string &video_id)
{
    std::string response = send_command("radio " + video_id);
    if (response.find("\"error\"") != std::string::npos)
        return {};
    auto tracks = parse_track_array(response);
    if (tracks.size() > 1)
        tracks.erase(tracks.begin());
    if (tracks.size() > 100)
        tracks.resize(100);
    return tracks;
}

std::vector<YouTubePlaylist> parse_youtube_playlists_response(const std::string &response)
{
    std::vector<YouTubePlaylist> out;
    size_t pos = 0;
    int guard = 0;
    while ((pos = response.find("\"playlistId\"", pos)) != std::string::npos && guard < 200)
    {
        std::string obj = json_find_object(response, pos);
        if (obj.empty())
        {
            pos += 12;
            continue;
        }
        YouTubePlaylist p;
        p.id = json_get_string(obj, "playlistId");
        p.title = json_get_string(obj, "title");
        size_t upos = obj.find("\"url\"");
        if (upos != std::string::npos)
        {
            size_t c = obj.find(':', upos);
            if (c != std::string::npos)
            {
                size_t q = obj.find('"', c + 1);
                if (q != std::string::npos)
                {
                    size_t q2 = obj.find('"', q + 1);
                    if (q2 != std::string::npos)
                        p.thumbnail_url = obj.substr(q + 1, q2 - q - 1);
                }
            }
        }
        size_t cpos = obj.find("\"count\"");
        if (cpos != std::string::npos)
        {
            size_t c = obj.find(':', cpos);
            if (c != std::string::npos)
            {
                size_t q = obj.find('"', c + 1);
                if (q != std::string::npos && q < obj.size() - 1 && isdigit((unsigned char)obj[q + 1]))
                {
                    size_t q2 = obj.find('"', q + 1);
                    if (q2 != std::string::npos)
                        p.count = std::stoi(obj.substr(q + 1, q2 - q - 1));
                }
                else
                {
                    size_t d = c + 1;
                    while (d < obj.size() && isspace((unsigned char)obj[d])) d++;
                    int val = 0;
                    while (d < obj.size() && isdigit((unsigned char)obj[d]))
                    {
                        val = val * 10 + (obj[d] - '0');
                        d++;
                    }
                    p.count = val;
                }
            }
        }
        if (!p.id.empty())
            out.push_back(std::move(p));
        pos = response.find('}', pos);
        if (pos == std::string::npos)
            break;
        pos += 1;
        guard++;
    }
    return out;
}

std::vector<YouTubePlaylist> fetch_youtube_playlists()
{
    std::vector<YouTubePlaylist> out;
    std::string response = send_command("library_playlists");
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return out;
    write_text_file(youtube_playlists_cache_path(), response);
    return parse_youtube_playlists_response(response);
}

std::vector<Track> parse_youtube_playlist_tracks_response(const std::string &response)
{
    std::vector<Track> tracks;
    size_t pos = 0;
    int guard = 0;
    while ((pos = response.find("\"videoId\"", pos)) != std::string::npos && guard < 500)
    {
        std::string obj = json_find_object(response, pos);
        if (obj.empty())
        {
            pos += 9;
            continue;
        }
        Track t;
        parse_track_object(obj, t);
        if (!t.id.empty() && !t.title.empty())
            tracks.push_back(std::move(t));
        pos = response.find('}', pos);
        if (pos == std::string::npos)
            break;
        pos += 1;
        guard++;
    }
    return tracks;
}

std::vector<Track> fetch_youtube_playlist(const std::string &playlist_id)
{
    if (playlist_id.empty())
        return {};
    std::string response = send_command("playlist " + playlist_id);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return {};
    write_text_file(youtube_playlist_cache_path(playlist_id), response);
    return parse_youtube_playlist_tracks_response(response);
}


// ------------------ Server write-back ------------------
// Mutations of the signed-in YouTube Music account. All are blocking and
// network-bound: call them from the write-back worker (writeback.cpp), never
// on the UI thread. Failures are recorded via take_backend_error() by
// send_command, so callers report them with ui_notify_backend_error().

static bool writeback_ok(const std::string &response)
{
    return !response.empty() && response.find("\"error\"") == std::string::npos;
}

bool backend_rate_song(const std::string &video_id, const char *rating)
{
    if (video_id.empty() || !rating)
        return false;
    std::string payload = "{\"videoId\":\"" + json_escape(video_id) +
                          "\",\"rating\":\"" + rating + "\"}";
    return writeback_ok(send_command("rate_song " + payload));
}

std::string backend_create_playlist(const std::string &title,
                                    const std::vector<std::string> &video_ids)
{
    if (title.empty())
        return "";
    std::string payload = "{\"title\":\"" + json_escape(title) + "\"";
    if (!video_ids.empty())
    {
        payload += ",\"videoIds\":[";
        for (size_t i = 0; i < video_ids.size(); ++i)
        {
            if (i)
                payload += ",";
            payload += "\"" + json_escape(video_ids[i]) + "\"";
        }
        payload += "]";
    }
    payload += "}";
    std::string response = send_command("create_playlist " + payload);
    if (!writeback_ok(response))
        return "";
    std::string id = json_get_string(response, "playlistId");
    return id;
}

bool backend_delete_playlist(const std::string &playlist_id)
{
    if (playlist_id.empty())
        return false;
    std::string payload = "{\"playlistId\":\"" + json_escape(playlist_id) + "\"}";
    return writeback_ok(send_command("delete_playlist " + payload));
}

bool backend_rename_playlist(const std::string &playlist_id, const std::string &title)
{
    if (playlist_id.empty() || title.empty())
        return false;
    std::string payload = "{\"playlistId\":\"" + json_escape(playlist_id) +
                          "\",\"title\":\"" + json_escape(title) + "\"}";
    return writeback_ok(send_command("edit_playlist " + payload));
}

bool backend_add_playlist_items(const std::string &playlist_id,
                                const std::vector<std::string> &video_ids)
{
    if (playlist_id.empty() || video_ids.empty())
        return false;
    std::string payload = "{\"playlistId\":\"" + json_escape(playlist_id) +
                          "\",\"videoIds\":[";
    for (size_t i = 0; i < video_ids.size(); ++i)
    {
        if (i)
            payload += ",";
        payload += "\"" + json_escape(video_ids[i]) + "\"";
    }
    payload += "]}";
    return writeback_ok(send_command("add_playlist_items " + payload));
}

bool backend_remove_playlist_items(
    const std::string &playlist_id,
    const std::vector<std::pair<std::string, std::string>> &items)
{
    if (playlist_id.empty() || items.empty())
        return false;
    std::string payload = "{\"playlistId\":\"" + json_escape(playlist_id) +
                          "\",\"videos\":[";
    bool first = true;
    for (const auto &item : items)
    {
        if (item.second.empty())
            continue; // setVideoId is required by the server
        if (!first)
            payload += ",";
        first = false;
        payload += "{\"videoId\":\"" + json_escape(item.first) +
                   "\",\"setVideoId\":\"" + json_escape(item.second) + "\"}";
    }
    payload += "]}";
    if (first)
        return false; // nothing actually removable
    return writeback_ok(send_command("remove_playlist_items " + payload));
}

bool backend_move_playlist_item(const std::string &playlist_id,
                                const std::string &set_video_id,
                                const std::string &after_item_id)
{
    if (playlist_id.empty() || set_video_id.empty())
        return false;
    std::string payload = "{\"playlistId\":\"" + json_escape(playlist_id) +
                          "\",\"moveItem\":{\"setVideoId\":\"" +
                          json_escape(set_video_id) + "\"";
    if (after_item_id.empty())
        payload += ",\"afterItemId\":null}";
    else
        payload += ",\"afterItemId\":\"" + json_escape(after_item_id) + "\"}";
    payload += "}";
    return writeback_ok(send_command("edit_playlist " + payload));
}

std::vector<std::pair<std::string, std::string>> backend_playlist_items(
    const std::string &playlist_id)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (playlist_id.empty())
        return out;
    std::string response = send_command("playlist " + playlist_id);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return out;
    // Tracks are the objects that carry both videoId and setVideoId; the
    // playlist header object has neither, so scanning for setVideoId keys
    // skips it. Order of appearance is the server order.
    size_t pos = 0;
    int guard = 0;
    while ((pos = response.find("\"setVideoId\"", pos)) != std::string::npos && guard < 2000)
    {
        std::string obj = json_find_object(response, pos);
        if (!obj.empty())
        {
            std::string vid = json_get_string(obj, "videoId");
            std::string svid = json_get_string(obj, "setVideoId");
            if (!vid.empty() && !svid.empty())
                out.emplace_back(std::move(vid), std::move(svid));
        }
        pos += 12;
        guard++;
    }
    return out;
}

static PodcastShow parse_podcast_show(const std::string &obj)
{
    PodcastShow p;
    p.browse_id = json_get_string(obj, "browse_id");
    p.title = json_get_string(obj, "title");
    p.author = json_get_string(obj, "author");
    p.thumbnail_url = json_get_string(obj, "thumbnail_url");
    p.description = json_get_string(obj, "description");
    p.saved = json_get_bool(obj, "saved");
    return p;
}

static PodcastEpisode parse_podcast_episode(const std::string &obj)
{
    PodcastEpisode e;
    e.video_id = json_get_string(obj, "video_id");
    e.title = json_get_string(obj, "title");
    e.author = json_get_string(obj, "author");
    e.thumbnail_url = json_get_string(obj, "thumbnail_url");
    e.date = json_get_string(obj, "date");
    e.description = json_get_string(obj, "description");
    e.duration_sec = json_get_int(obj, "duration_sec");
    return e;
}

static std::vector<PodcastShow> parse_podcast_shows(const std::string &json)
{
    std::vector<PodcastShow> out;
    size_t arr = json.find('[');
    if (arr == std::string::npos)
        return out;
    size_t pos = arr + 1;
    int guard = 0;
    while (pos < json.size() && guard++ < 500)
    {
        while (pos < json.size() && (isspace((unsigned char)json[pos]) || json[pos] == ','))
            ++pos;
        if (pos >= json.size() || json[pos] == ']')
            break;
        if (json[pos] != '{')
            break;
        const size_t end = json_object_end(json, pos);
        if (end == std::string::npos)
            break;
        PodcastShow p = parse_podcast_show(json.substr(pos, end - pos + 1));
        if (!p.browse_id.empty() || !p.title.empty())
            out.push_back(std::move(p));
        pos = end + 1;
    }
    return out;
}

std::vector<PodcastShow> fetch_podcast_library()
{
    const std::string response = send_command("podcasts_library");
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return {};
    return parse_podcast_shows(response);
}

std::vector<PodcastShow> fetch_podcast_search(const std::string &query)
{
    if (query.empty())
        return {};
    const std::string response = send_command("podcasts_search " + query);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return {};
    return parse_podcast_shows(response);
}

std::vector<PodcastEpisode> fetch_episodes_later()
{
    const std::string response = send_command("episodes_later");
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return {};
    std::vector<PodcastEpisode> out;
    for_each_json_array_object(response, [&](const std::string &obj) {
        out.push_back(parse_podcast_episode(obj));
    });
    return out;
}

std::vector<SpotifyPlaylistSummary> fetch_spotify_library(std::string &error)
{
    error.clear();
    std::vector<SpotifyPlaylistSummary> out;
    const std::string response = send_command("spotify_library");
    if (response.empty())
    {
        error = "Backend not running";
        return out;
    }
    if (response.find("\"error\"") != std::string::npos)
    {
        error = json_get_string(response, "error");
        if (error.empty())
            error = "Could not load your Spotify playlists";
        return out;
    }
    for_each_json_array_object(response, [&](const std::string &obj) {
        SpotifyPlaylistSummary item;
        item.id = json_get_string(obj, "id");
        item.kind = json_get_string(obj, "kind");
        item.name = json_get_string(obj, "name");
        item.owner = json_get_string(obj, "owner");
        item.art = json_get_string(obj, "art");
        item.count = json_get_int(obj, "count");
        if (!item.id.empty() && !item.name.empty())
            out.push_back(std::move(item));
    });
    return out;
}

SpotifyImportResult fetch_spotify_import(const std::string &link)
{
    SpotifyImportResult out;
    if (link.empty())
    {
        out.error = "Paste a Spotify playlist link or ID";
        return out;
    }
    const std::string response = send_command("spotify_import " + link);
    if (response.empty())
    {
        out.error = "Backend not running";
        return out;
    }
    if (response.find("\"error\"") != std::string::npos)
    {
        out.error = json_get_string(response, "error");
        if (out.error.empty())
            out.error = "Spotify import failed";
        return out;
    }
    out.name = json_get_string(response, "name");
    out.skipped = json_get_int(response, "skipped");
    out.total = json_get_int(response, "total");
    size_t key = response.find("\"tracks\":");
    if (key != std::string::npos)
        out.tracks = parse_track_array(response.substr(key));
    if (out.tracks.empty())
        out.error = "No Spotify tracks could be matched on YouTube Music";
    return out;
}

PodcastDetails fetch_podcast_details(const std::string &browse_id)
{
    PodcastDetails details;
    if (browse_id.empty())
        return details;
    const std::string response = send_command("podcast " + browse_id);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return details;

    size_t show_key = response.find("\"show\":");
    if (show_key != std::string::npos)
    {
        size_t obj = response.find('{', show_key);
        if (obj != std::string::npos)
        {
            size_t end = json_object_end(response, obj);
            if (end != std::string::npos)
                details.show = parse_podcast_show(response.substr(obj, end - obj + 1));
        }
    }
    if (details.show.browse_id.empty())
        details.show.browse_id = browse_id;

    size_t eps_key = response.find("\"episodes\":");
    if (eps_key != std::string::npos)
    {
        for_each_json_object(response, "episodes", [&](const std::string &obj) {
            details.episodes.push_back(parse_podcast_episode(obj));
        });
    }
    return details;
}

bool fetch_album(const std::string &browse_id, Album &out)
{
    if (browse_id.empty())
        return false;
    std::string response = send_command("album " + browse_id);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
    {
        if (!response.empty())
            fprintf(stderr, "Album error: %s\n", response.c_str());
        return false;
    }

    out = Album{};
    out.browse_id = json_get_string(response, "browse_id");
    if (out.browse_id.empty())
        out.browse_id = browse_id;
    out.title = json_get_string(response, "title");
    out.artist = json_get_string(response, "artist");
    out.year = json_get_string(response, "year");
    out.duration = json_get_string(response, "duration");
    out.description = json_get_string(response, "description");
    out.thumbnail_url = json_get_string(response, "thumbnail_url");
    out.audio_playlist_id = json_get_string(response, "audio_playlist_id");
    out.track_count = json_get_int(response, "track_count");

    size_t tracks_key = response.find("\"tracks\":");
    if (tracks_key == std::string::npos)
        return !out.title.empty() || !out.tracks.empty();
    for_each_json_object(response, "tracks", [&](const std::string &obj) {
        Track t;
        parse_track_object(obj, t);
        if (t.album.empty())
            t.album = out.title;
        if (t.album_id.empty())
            t.album_id = out.browse_id;
        if (!t.id.empty() && !t.title.empty())
            out.tracks.push_back(std::move(t));
    });
    if (out.track_count == 0)
        out.track_count = (int)out.tracks.size();
    return !out.tracks.empty() || !out.title.empty();
}

bool fetch_album_for_track(const std::string &video_id, std::string &browse_id, std::string &album_name)
{
    if (video_id.empty())
        return false;
    std::string response = send_command("album_of " + video_id);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return false;
    browse_id = json_get_string(response, "album_id");
    if (browse_id.empty())
        browse_id = json_get_string(response, "browse_id");
    album_name = json_get_string(response, "album_name");
    if (album_name.empty())
        album_name = json_get_string(response, "album");
    return !browse_id.empty();
}

bool fetch_artist(const std::string &browse_id, ArtistInfo &out)
{
    if (browse_id.empty())
        return false;
    std::string response = send_command("artist " + browse_id);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
    {
        if (!response.empty())
            fprintf(stderr, "Artist error: %s\n", response.c_str());
        return false;
    }

    out = ArtistInfo{};
    out.browse_id = json_get_string(response, "browse_id");
    if (out.browse_id.empty())
        out.browse_id = browse_id;
    out.name = json_get_string(response, "name");
    out.description = json_get_string(response, "description");
    out.subscribers = json_get_string(response, "subscribers");
    out.monthly_listeners = json_get_string(response, "monthly_listeners");
    out.thumbnail_url = json_get_string(response, "thumbnail_url");
    out.songs_browse_id = json_get_string(response, "songs_browse_id");

    for_each_json_object(response, "songs", [&](const std::string &obj) {
        Track t;
        parse_track_object(obj, t);
        if (t.artist_id.empty())
            t.artist_id = browse_id;
        if (t.artist.empty())
            t.artist = out.name;
        if (!t.id.empty() && !t.title.empty())
            out.songs.push_back(std::move(t));
    });

    for_each_json_object(response, "albums", [&](const std::string &obj) {
        ArtistAlbum a;
        a.browse_id = json_get_string(obj, "browse_id");
        a.title = json_get_string(obj, "title");
        a.year = json_get_string(obj, "year");
        a.thumbnail_url = json_get_string(obj, "thumbnail_url");
        a.type = json_get_string(obj, "type");
        if (!a.browse_id.empty() && !a.title.empty())
            out.albums.push_back(std::move(a));
    });

    return !out.name.empty() || !out.songs.empty();
}

bool fetch_artist_songs(const std::string &songs_browse_id, int offset, int count,
                        std::vector<Track> &out)
{
    out.clear();
    if (songs_browse_id.empty() || count <= 0)
        return false;
    const std::string response = send_command(
        "artist_songs " + songs_browse_id + " " + std::to_string(std::max(0, offset)) + " " +
        std::to_string(count));
    if (response.empty() || response.find("\"error\"") != std::string::npos)
    {
        if (!response.empty())
            fprintf(stderr, "Artist songs error: %s\n", response.c_str());
        return false;
    }
    out = parse_track_array(response);
    return true;
}

bool fetch_artist_for_track(const std::string &video_id, std::string &artist_id, std::string &artist_name)
{
    if (video_id.empty())
        return false;
    std::string response = send_command("artist_of " + video_id);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return false;
    artist_id = json_get_string(response, "artist_id");
    artist_name = json_get_string(response, "artist_name");
    return !artist_id.empty();
}

std::vector<ArtistSummary> fetch_artist_search(const std::string &query)
{
    std::vector<ArtistSummary> artists;
    if (query.empty())
        return artists;
    std::string response = send_command("artist_search " + query);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return artists;

    size_t pos = response.find('{');
    while (pos != std::string::npos && artists.size() < 50)
    {
        size_t end = json_object_end(response, pos);
        if (end == std::string::npos)
            break;
        std::string obj = response.substr(pos, end - pos + 1);
        ArtistSummary a;
        a.browse_id = json_get_string(obj, "browse_id");
        a.name = json_get_string(obj, "name");
        a.thumbnail_url = json_get_string(obj, "thumbnail_url");
        a.subscribers = json_get_string(obj, "subscribers");
        if (!a.browse_id.empty() && !a.name.empty())
            artists.push_back(std::move(a));
        pos = response.find('{', end + 1);
    }
    return artists;
}

bool fetch_search_all(const std::string &query, SearchAllResult &out)
{
    out = SearchAllResult{};
    if (query.empty())
        return false;
    std::string response = send_command("search_all " + query);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return false;

    for_each_json_object(response, "songs", [&](const std::string &obj) {
        Track t;
        parse_track_object(obj, t);
        if (!t.id.empty() && !t.title.empty())
            out.songs.push_back(std::move(t));
    });
    for_each_json_object(response, "albums", [&](const std::string &obj) {
        AlbumSummary a;
        a.browse_id = json_get_string(obj, "browse_id");
        a.title = json_get_string(obj, "title");
        a.artist = json_get_string(obj, "artist");
        a.year = json_get_string(obj, "year");
        a.thumbnail_url = json_get_string(obj, "thumbnail_url");
        a.track_count = json_get_int(obj, "track_count");
        if (!a.browse_id.empty() && !a.title.empty())
            out.albums.push_back(std::move(a));
    });
    for_each_json_object(response, "artists", [&](const std::string &obj) {
        ArtistSummary a;
        a.browse_id = json_get_string(obj, "browse_id");
        a.name = json_get_string(obj, "name");
        a.thumbnail_url = json_get_string(obj, "thumbnail_url");
        a.subscribers = json_get_string(obj, "subscribers");
        if (!a.browse_id.empty() && !a.name.empty())
            out.artists.push_back(std::move(a));
    });
    return !out.songs.empty() || !out.albums.empty() || !out.artists.empty();
}

bool fetch_lyrics(const std::string &video_id, Lyrics &out)
{
    std::string response = send_command("lyrics " + video_id);
    return parse_lyrics_json(response, out);
}

// Look up an already-cached audio file for a video id (no download).
static std::string find_audio_in(const std::string &dir, const std::string &video_id)
{
    if (video_id.empty())
        return "";
    // Cached songs are always "<id>.<ext>". Probe the known extensions
    // directly instead of listing the whole directory: the audio cache can
    // hold thousands of files, and this is also called per visible row.
    static const char *const exts[] = {"opus", "m4a", "mp3", "webm",
                                       "flac", "aac", "ogg"};
    for (const char *ext : exts)
    {
        const std::string path = dir + "/" + video_id + "." + ext;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            return path;
    }
    return "";
}

std::string find_cached_audio_path(const std::string &video_id)
{
    std::string dir = cache_dir() + "/audio_cache";
    return find_audio_in(dir, video_id);
}

std::string playable_track_url(const std::string &id, const std::string &url)
{
    if (!url.empty() && url.find("://") != std::string::npos)
        return url; // remote stream / YouTube URL
    if (!url.empty())
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(url, ec))
            return url;
    }
    if (!id.empty())
    {
        const std::string cached = find_cached_audio_path(id);
        if (!cached.empty())
            return cached;
        return "https://www.youtube.com/watch?v=" + id;
    }
    return url;
}

std::string get_cached_audio_path(const std::string &video_id)
{
    if (video_id.empty())
        return "";
    std::string dir = cache_dir() + "/audio_cache";
    if (!ensure_dir(dir))
        return "";

    std::string cached = find_audio_in(dir, video_id);
    if (!cached.empty())
        return cached;

    std::string output_template = dir + "/" + video_id + ".%(ext)s";
    std::string cmd = shell_quote(ytdlp_bin()) + " -f bestaudio" + cookie_arg() + " -o \"" + output_template + "\" "
                                                                        "https://www.youtube.com/watch?v=" +
                      video_id + kShellErrRedirect;
#ifdef P100_WINDOWS
    int ret = winproc::system_hidden(cmd);
#else
    int ret = system(cmd.c_str());
#endif
    if (ret != 0)
    {
        // Cookie-locked profiles can break the signed download; retry
        // without explicit cookies (works for public tracks).
        cmd = shell_quote(ytdlp_bin()) + " -f bestaudio -o \"" + output_template + "\" "
                                                               "https://www.youtube.com/watch?v=" +
              video_id + kShellErrRedirect;
#ifdef P100_WINDOWS
        ret = winproc::system_hidden(cmd);
#else
        ret = system(cmd.c_str());
#endif
    }
    if (ret != 0)
        return "";

    std::string full_path = find_audio_in(dir, video_id);
    if (!full_path.empty())
    {
        // Only write a sidecar when the fetch produced a title: an empty
        // one would hide the song from the Downloads list (which skips
        // empty-title entries) while find_audio_in still plays it.
        Track t = fetch_track_metadata(video_id);
        if (!t.title.empty())
            save_metadata(t, full_path);
    }
    return full_path;
}

// Resolve a direct streaming URL (yt-dlp -g) so playback can start
// almost immediately instead of waiting for a full file download.
std::string fetch_stream_url(const std::string &video_id)
{
    std::string response = send_play_command("play " + video_id);
    if (response.find("\"error\"") != std::string::npos)
        return "";
    return json_get_string(response, "url");
}

// Speculative prefetch variant: goes to the dedicated prefetch backend
// channel, leaving searches/metadata and the foreground play channel free.
// The channel processes commands serially, which also self-throttles the
// prefetch rate.
std::string fetch_stream_url_prefetch(const std::string &video_id)
{
    std::string response = send_prefetch_command("play " + video_id);
    if (response.find("\"error\"") != std::string::npos)
        return "";
    return json_get_string(response, "url");
}

std::string fetch_stream_url_fresh(const std::string &video_id)
{
    std::string response = send_play_command("play_fresh " + video_id);
    if (response.find("\"error\"") != std::string::npos)
        return "";
    return json_get_string(response, "url");
}

std::vector<Track> get_cached_tracks()
{
    std::vector<Track> tracks;
    std::string dir = cache_dir() + "/audio_cache";

    for (const std::string &name : list_dir_names(dir))
    {
        if (name == "." || name == "..")
            continue;
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos)
            continue;

        // Sweep orphaned sidecars: a delete/clear that raced a finalize
        // thread can leave <id>.<ext>.meta.json with no audio next to it.
        if (name.size() > 10 && name.compare(name.size() - 10, 10, ".meta.json") == 0)
        {
            struct stat ast;
            const std::string audio = dir + "/" + name.substr(0, name.size() - 10);
            if (stat(audio.c_str(), &ast) != 0 || !S_ISREG(ast.st_mode))
            {
                std::error_code ec;
                std::filesystem::remove(dir + "/" + name, ec);
            }
            continue;
        }

        // Whole cached songs are exactly "<id>.<ext>". A second dot means a
        // leftover yt-dlp fragment ("<id>.f140.webm"), not a playable track.
        if (name.find('.') != dot)
            continue;

        std::string ext = name.substr(dot + 1);
        if (!(ext == "opus" || ext == "m4a" || ext == "mp3" || ext == "webm" ||
              ext == "flac" || ext == "aac" || ext == "ogg"))
            continue;

        std::string full_path = dir + "/" + name;
        struct stat st;
        if (stat(full_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        Track t;
        t.url = full_path;
        t.id = name.substr(0, dot);

        std::string meta_path = full_path + ".meta.json";
        if (stat(meta_path.c_str(), &st) == 0)
        {
            std::ifstream in(meta_path);
            if (in.good())
            {
                std::string json((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                in.close();
                const std::string stem = name.substr(0, dot);
                t.id = json_get_string(json, "id");
                if (t.id.empty())
                    t.id = stem;
                // Never let a corrupt/empty sidecar title hide the song:
                // fall back to the filename, like the no-sidecar path does.
                t.title = json_get_string(json, "title");
                if (t.title.empty())
                    t.title = stem;
                t.artist = json_get_string(json, "artist");
                if (t.artist.empty())
                    t.artist = "Unknown";
                t.artist_id = json_get_string(json, "artist_id");
                t.album = json_get_string(json, "album");
                t.album_id = json_get_string(json, "album_id");
                t.track_number = json_get_int(json, "track_number");
                t.thumbnail_url = json_get_string(json, "thumbnail_url");
                t.duration_sec = json_get_int(json, "duration_sec");
            }
        }
        else
        {
            t.title = name.substr(0, dot);
            t.artist = "Unknown";
            t.thumbnail_url = "";
            t.duration_sec = 0;
        }

        if (!t.title.empty())
            tracks.push_back(t);
    }

    return tracks;
}

std::string recorded_audio_path(const std::string &video_id, const std::string &ext)
{
    if (video_id.empty() || ext.empty())
        return "";
    std::string dir = cache_dir() + "/audio_cache";
    if (!ensure_dir(dir))
        return "";
    return dir + "/" + video_id + "." + ext;
}

std::string recorded_audio_temp_path(const std::string &video_id, const std::string &ext)
{
    if (video_id.empty() || ext.empty())
        return "";
    std::string base = cache_dir() + "/audio_cache";
    if (!ensure_dir(base))
        return "";
    std::string dir = base + "/.tmp";
    if (!ensure_dir(dir))
        return "";
    // A fresh recording supersedes any stale delete mark for this id (the
    // user re-played the track they had deleted mid-recording).
    {
        std::lock_guard<std::mutex> lock(g_meta_mutex);
        g_meta_cancelled.erase(video_id);
    }
    // Unique per recording (the extension stays last, so mpv still picks the
    // right muxer): replaying a track while its previous recording is still
    // being finalized must not have the two share one temp file.
    static std::atomic<unsigned long long> s_temp_seq{0};
    return dir + "/" + std::to_string(s_temp_seq++) + "_" + video_id + "." + ext;
}

// Drop recordings left behind in .tmp by a crash/quit mid-track: they are
// never finalized and would otherwise accumulate forever. Startup only.
void cleanup_incomplete_recordings()
{
    std::string dir = cache_dir() + "/audio_cache/.tmp";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
    {
        std::error_code rec;
        if (entry.is_regular_file(rec))
            std::filesystem::remove(entry.path(), rec);
    }
}


// ------------------ Download cache size cap ------------------
// 0 = unlimited (the default). Enforced after each finished download by
// evicting oldest-written recordings until the cache fits again.
static std::atomic<double> g_audio_cache_cap{0.0};
static int (*g_audio_cache_rank)(const std::string &video_id) = nullptr;

void set_audio_cache_rank(int (*rank_fn)(const std::string &video_id))
{
    g_audio_cache_rank = rank_fn;
}

void set_audio_cache_cap(double bytes)
{
    g_audio_cache_cap.store(bytes > 0.0 ? bytes : 0.0);
}

struct AudioCacheEntry
{
    std::filesystem::path path;
    double size = 0.0;
    std::filesystem::file_time_type mtime;
};

// Collect the regular files under the audio cache and their total size.
static std::vector<AudioCacheEntry> collect_audio_cache(double &total)
{
    std::vector<AudioCacheEntry> out;
    total = 0.0;
    const std::string dir = cache_dir() + "/audio_cache";
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(dir, ec), end;
    while (!ec && it != end)
    {
        if (it->is_regular_file(ec) && !ec)
        {
            const double sz = (double)it->file_size(ec);
            const auto mt = it->last_write_time(ec);
            if (!ec)
            {
                out.push_back({it->path(), sz, mt});
                total += sz;
            }
        }
        ec.clear();
        it.increment(ec);
    }
    return out;
}

double audio_cache_usage_bytes()
{
    double total = 0.0;
    collect_audio_cache(total);
    return total;
}

void audio_cache_clear(const std::string &keep_id)
{
    const std::string dir = cache_dir() + "/audio_cache";
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
            continue;
        ec.clear();
        const std::string name = it->path().filename().string();
        // Keep the track currently playing (its file may be re-opened on seek).
        if (!keep_id.empty() && name.rfind(keep_id + ".", 0) == 0)
            continue;
        std::filesystem::remove(it->path(), ec);
        ec.clear();
    }
    g_downloads_dirty = true;
}

static void audio_cache_enforce_cap(const std::string &keep_id)
{
    const double cap = g_audio_cache_cap.load();
    if (cap <= 0.0)
        return;
    double total = 0.0;
    std::vector<AudioCacheEntry> ents = collect_audio_cache(total);
    if (total <= cap)
        return;
    std::error_code ec;

    // Evict the least-listened songs first (when a ranking callback is
    // installed); oldest file breaks ties.
    std::sort(ents.begin(), ents.end(), [](const AudioCacheEntry &a, const AudioCacheEntry &b) {
        const int ra = g_audio_cache_rank ? g_audio_cache_rank(a.path.stem().string()) : 0;
        const int rb = g_audio_cache_rank ? g_audio_cache_rank(b.path.stem().string()) : 0;
        if (ra != rb)
            return ra < rb;
        return a.mtime < b.mtime;
    });
    for (const auto &e : ents)
    {
        if (total <= cap)
            break;
        // Keep the track currently playing.
        if (!keep_id.empty() && e.path.stem().string() == keep_id)
            continue;
        std::filesystem::remove(e.path, ec);
        ec.clear();
        std::filesystem::remove(e.path.string() + ".meta.json", ec);
        ec.clear();
        total -= e.size;
    }
    g_downloads_dirty = true;
}

void audio_cache_enforce_cap_now(const std::string &keep_id)
{
    audio_cache_enforce_cap(keep_id);
}

void finalize_recorded_audio(const std::string &video_id, const std::string &audio_path)
{
    // Network-bound fetch first, outside the lock.
    Track t = fetch_track_metadata(video_id);

    std::lock_guard<std::mutex> lock(g_meta_mutex);
    if (g_meta_writes_blocked)
        return;
    if (g_meta_cancelled.erase(video_id) > 0)
    {
        // The track was deleted while this finalize was still fetching: the
        // EOF rename already resurrected the file the delete couldn't see,
        // so drop it (and any sidecar) again instead of leaving orphans.
        std::error_code ec;
        std::filesystem::remove(audio_path, ec);
        std::filesystem::remove(audio_path + ".meta.json", ec);
        g_downloads_dirty = true;
        return;
    }
    if (!std::filesystem::exists(audio_path))
        return;
    if (!t.title.empty())
    {
        save_metadata(t, audio_path);
        g_downloads_dirty = true;
    }
    audio_cache_enforce_cap(video_id);
}

// Sniff the real container of a finished recording. mpv picks its muxer from
// the extension we handed it, and stream_container_ext() only guesses from
// mime/itag params -- a wrong guess must not reach the cache under a lying
// extension.
std::string sniff_audio_container_ext(const std::string &path, const std::string &fallback)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return fallback;
    unsigned char b[16] = {0};
    in.read(reinterpret_cast<char *>(b), sizeof(b));
    const size_t n = (size_t)in.gcount();
    if (n >= 4 && b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3)
        return "webm"; // EBML / Matroska
    if (n >= 8 && b[4] == 'f' && b[5] == 't' && b[6] == 'y' && b[7] == 'p')
        return "m4a"; // ISO base media (MP4)
    if (n >= 4 && b[0] == 'I' && b[1] == 'D' && b[2] == '3')
        return "mp3";
    if (n >= 4 && b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S')
        return "ogg";
    if (n >= 4 && b[0] == 'f' && b[1] == 'L' && b[2] == 'a' && b[3] == 'C')
        return "flac";
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0)
        return ((b[1] & 0xF6) == 0xF0) ? "aac" : "mp3"; // ADTS vs MPEG sync
    return fallback;
}

// ------------------ Backend/auth diagnostics ------------------
bool backend_running()
{
#ifdef P100_WINDOWS
    return g_backend.h_in_write != NULL && g_backend.pid != NULL &&
           !g_backend.dead.load();
#else
    return g_backend.in != nullptr && g_backend.pid > 0 && !g_backend.dead.load();
#endif
}

BackendError backend_error_snapshot()
{
    return g_backend_error;
}

std::string backend_script_path()
{
    return backend_script();
}

std::string backend_python_path()
{
    return backend_python();
}

std::string backend_pythonw_path()
{
    const std::string py = backend_python();
    static const std::string exe = "python.exe";
    if (py.size() > exe.size() &&
        py.compare(py.size() - exe.size(), exe.size(), exe) == 0)
    {
        const std::string pyw = py.substr(0, py.size() - exe.size()) + "pythonw.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(pyw, ec))
            return pyw;
    }
    return py;
}

std::string backend_auth_file_path()
{
    if (const char *env = getenv("P100_AUTH_FILE"))
        return env;
    struct stat st;
    if (g_auth_mode == "oauth")
        return p100_config_path("browseroauth.json");
    std::string preferred = p100_config_path("browser.json");
    if (stat(preferred.c_str(), &st) == 0)
        return preferred;
    if (std::string local = resolve_file("browser.json"); !local.empty())
        return local;
    return preferred;
}

std::string backend_reauth_command()
{
    if (g_auth_mode == "oauth")
        return "\"" + backend_python() + "\" \"" + backend_script() + "\" --login";
    std::string python = backend_python();
    std::string cli;
    size_t slash = python.find_last_of('/');
    if (slash != std::string::npos)
    {
        std::string candidate = python.substr(0, slash + 1) + "ytmusicapi";
        struct stat st;
        if (stat(candidate.c_str(), &st) == 0)
            cli = candidate;
    }
    if (cli.empty())
        cli = "ytmusicapi";
    return cli + " browser --file \"" + backend_auth_file_path() + "\"";
}

std::string youtube_playlists_cache_path()
{
    return cache_dir() + "/youtube_playlists_cache.json";
}

std::string youtube_playlist_cache_path(const std::string &playlist_id)
{
    std::string safe = playlist_id;
    for (char &c : safe)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-')
            c = '_';
    return cache_dir() + "/youtube_playlist_" + safe + ".json";
}

bool read_text_file(const std::string &path, std::string &out)
{
    std::ifstream in(path);
    if (!in.good())
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return !out.empty();
}

void write_text_file(const std::string &path, const std::string &data)
{
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
    {
        std::string dir = path.substr(0, slash);
        mkdir(dir.c_str(), 0755);
    }
    std::ofstream out(path);
    if (out.good())
        out << data;
}
