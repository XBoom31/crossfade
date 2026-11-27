#include "ytdlp.h"
#include "backend/native/browse.h"
#include "backend/native/innertube.h"
#include "backend/native/http.h"
#include "tunables.h"
#include "json_util.h"
#include "paths.h"
#include "atomic_file.h"
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

// "local:" ids are files on disk, never YouTube videos. Guarding at this level
// means no caller can accidentally send one to the backend or yt-dlp.
static bool is_local_media_id(const std::string &id)
{
    return id.rfind("local:", 0) == 0;
}

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

// One in-flight id-tagged command on a concurrent channel.
struct BackendReply
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::string response;
};

struct BackendChannel
{
    std::atomic<bool> ready{false}; // set once the {"status":"ready"} line arrives
    std::atomic<uint64_t> commands{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> last_ms{0};
    std::atomic<uint64_t> max_ms{0};
    // Commands currently waiting on this channel (the backend handles one at
    // a time); shown in the debug window so a queue shows up immediately.
    std::atomic<int> pending{0};
    std::mutex ready_mutex;
    std::condition_variable ready_cv;
#ifndef P100_WINDOWS
    FILE *in = nullptr;
    FILE *out = nullptr;
#endif
    P100_PID_T pid = 0;
    std::mutex mutex;
    std::atomic<bool> dead{false}; // process exited / pipe EOF
    // Auto-revive throttle for optional workers (play/prefetch/meta): a crash
    // or a wedged command must not disable the feature for the whole session.
    std::atomic<int64_t> last_revive_ms{0};
    // Concurrent (main) channel: commands carry an id, replies are routed by
    // id by a reader thread, and multiple requests can be in flight.
    bool concurrent = false;
    std::thread reader;
    std::mutex write_mutex;
    std::mutex pending_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<BackendReply>> pending_replies;
    std::atomic<uint64_t> next_req{0};
#ifdef P100_WINDOWS
    // Named-analogue of the POSIX pair: two anonymous pipes (write end feeds
    // the child's stdin, read end drains its stdout) plus a reader thread,
    // because ReadFile on an anonymous pipe cannot timeout the way poll() can.
    HANDLE h_in_write = NULL;  // our end of the child's stdin
    HANDLE h_out_read = NULL;  // our end of the child's stdout
    std::mutex q_mutex;
    std::condition_variable q_cv;
    std::deque<std::string> lines;
#endif
};
static BackendChannel g_backend;
static BackendChannel g_play_backend;
static BackendChannel g_prefetch_backend;
// Background metadata (lyrics today): slow provider cascades must never share
// the interactive main pipe with search/browse. Also hosts speculative
// prefetch batches so the hover/hero channel stays free.
static BackendChannel g_meta_backend;
static std::atomic<bool> g_backend_stopping{false};
// Set from the backend's ready handshake: true when it started without a
// usable sign-in and is limited to public (unauthenticated) browsing.
static std::atomic<bool> g_backend_guest{false};
static std::atomic<bool> g_optional_backends_enabled{true};
static std::atomic<bool> g_backend_deferred{false};

// ---- Live backend command log (for the in-app debug window) ----
static std::mutex g_backend_log_mutex;
static std::deque<BackendLogEntry> g_backend_log;
static uint64_t g_backend_log_seq = 0;

static const char *backend_channel_name(const BackendChannel &ch)
{
    if (&ch == &g_backend)
        return "main";
    if (&ch == &g_play_backend)
        return "play";
    if (&ch == &g_prefetch_backend)
        return "prefetch";
    if (&ch == &g_meta_backend)
        return "meta";
    return "?";
}

static bool channel_usable(const BackendChannel &ch)
{
    return ch.ready.load(std::memory_order_acquire) &&
           !ch.dead.load(std::memory_order_relaxed);
}

// Declaring a channel dead must also clear `ready`, otherwise the send paths
// keep routing to the corpse instead of falling back (this is how lyrics
// stopped working after a stuck prefetch killed the meta channel).
static void channel_mark_dead(BackendChannel &ch)
{
    ch.dead.store(true, std::memory_order_relaxed);
    ch.ready.store(false, std::memory_order_release);
    ch.ready_cv.notify_all();
    // Fail every in-flight id-tagged request; their replies will never come.
    std::unordered_map<uint64_t, std::shared_ptr<BackendReply>> pending;
    {
        std::lock_guard<std::mutex> lock(ch.pending_mutex);
        pending.swap(ch.pending_replies);
    }
    for (auto &entry : pending)
    {
        std::lock_guard<std::mutex> lock(entry.second->mutex);
        entry.second->response = "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
        entry.second->done = true;
        entry.second->cv.notify_all();
    }
}

// One complete reply line from a backend. Id-tagged replies on a concurrent
// channel wake their waiting request; on other channels lines go to the
// serial read queue (the ready handshake uses the same queue).
static void route_backend_line(BackendChannel &ch, std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();
    if (ch.concurrent && !line.empty() && line[0] == '@')
    {
        const size_t space = line.find(' ');
        if (space != std::string::npos && space > 1)
        {
            const uint64_t id = std::strtoull(line.c_str() + 1, nullptr, 10);
            std::shared_ptr<BackendReply> reply;
            {
                std::lock_guard<std::mutex> lock(ch.pending_mutex);
                auto it = ch.pending_replies.find(id);
                if (it != ch.pending_replies.end())
                {
                    reply = it->second;
                    ch.pending_replies.erase(it);
                }
            }
            if (reply)
            {
                {
                    std::lock_guard<std::mutex> lock(reply->mutex);
                    reply->response = line.substr(space + 1);
                    reply->done = true;
                }
                reply->cv.notify_all();
            }
            return;
        }
    }
#ifdef P100_WINDOWS
    {
        std::lock_guard<std::mutex> lock(ch.q_mutex);
        ch.lines.push_back(std::move(line));
    }
    ch.q_cv.notify_all();
#endif
}

// Forward decl: JSON helpers are defined below, but the debug log wants the
// backend's error string when it records a failed command.
static std::string json_get_string(const std::string &json, const std::string &key);

static void record_backend_log(const BackendChannel &ch, const std::string &cmd,
                               uint64_t ms, const std::string &response, bool error,
                               uint64_t queue_ms = 0, size_t pending = 0)
{
    std::lock_guard<std::mutex> lock(g_backend_log_mutex);
    BackendLogEntry e;
    e.seq = ++g_backend_log_seq;
    e.ts_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    e.channel = backend_channel_name(ch);
    const size_t kCmdClip = (size_t)tunable_int("diag.command_clip");
    e.command = cmd.size() > kCmdClip ? cmd.substr(0, kCmdClip) + "…" : cmd;
    e.ms = ms;
    e.queue_ms = queue_ms;
    e.pending = pending;
    e.error = error || response.empty() ||
              response.find("\"error\"") != std::string::npos;
    e.bytes = response.size();
    if (e.error)
    {
        // Keep the actionable part visible in the debug window: the backend's
        // "error" string, or a short label for transport-level failures.
        if (response.find("did not respond in time") != std::string::npos)
            e.detail = "timed out";
        else if (response.find("\"error\"") != std::string::npos)
            e.detail = json_get_string(response, "error");
        else
            e.detail = "empty response";
        const size_t kDetailClip = (size_t)tunable_int("diag.detail_clip");
        if (e.detail.size() > kDetailClip)
            e.detail = e.detail.substr(0, kDetailClip) + "…";
    }
    g_backend_log.push_back(std::move(e));
    while (g_backend_log.size() > (size_t)tunable_int("diag.backend_log_lines"))
        g_backend_log.pop_front();
}

static std::string g_warm_extractor = "1"; // P100_WARM_EXTRACTOR for children

// Forward decl: JSON helpers are defined below, but start_backend() needs
// one to surface the backend's own startup errors.
static std::string json_get_string(const std::string &json, const std::string &key);

static std::string g_ytdl_browser = "firefox";
static std::string g_auth_mode = "browser";
static BackendError g_backend_error;
// Several backend channels (main/play/prefetch) record failures from their
// own worker threads. Without this, two threads could resize/clear the same
// std::string at once (heap corruption or a lost error toast).
static std::mutex g_backend_error_mutex;

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
    std::lock_guard<std::mutex> lock(g_backend_error_mutex);
    BackendError e = g_backend_error;
    g_backend_error.clear();
    return e;
}

// The ready handshake says whether the backend started without a usable
// sign-in. Tolerant of spacing so older/compact JSON both work.
static bool ready_says_guest(const std::string &ready)
{
    size_t pos = ready.find("\"guest\"");
    if (pos == std::string::npos)
        return false;
    pos = ready.find(':', pos);
    if (pos == std::string::npos)
        return false;
    pos = ready.find_first_not_of(" \t", pos + 1);
    return pos != std::string::npos && ready.compare(pos, 4, "true") == 0;
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
    // Tearing the channels down for a sign-in restart makes every in-flight
    // command fail with "Backend stopping"/"Backend not running". Those are
    // expected, not user-facing failures.
    if (g_backend_stopping.load(std::memory_order_relaxed))
        return;
    BackendError err;
    err.kind = parse_error_kind(response);
    err.message = json_get_string(response, "error");
    if (err.message.empty())
        err.message = response;
    std::lock_guard<std::mutex> lock(g_backend_error_mutex);
    g_backend_error = std::move(err);
}

// Backend commands must never hang the UI thread forever: a wedged Python
// process (or a pipe that lost its reader) previously blocked fgets for good.
// Everything up to read_timeout_seconds(), then a synthesized error response.
static int read_timeout_seconds() { return tunable_int("backend.read_timeout_sec"); }
// One backend reply is a single JSON line; anything larger is a bug (or a
// wedged process) and must not grow a buffer without bound on either OS.
static constexpr size_t kMaxBackendLine = 10 * 1024 * 1024;

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

// yt-dlp needs a JavaScript runtime plus the yt-dlp-ejs solver to pass
// YouTube's signature/challenge checks. Without one most clients return no
// audio formats and every resolve fails with "Requested format is not
// available". The installer and the AppImage launcher provision a pinned
// Deno under runtime/; a system deno/node still works for users who have one.
struct JsRuntime
{
    std::string name;
    std::string path;
};

static std::string find_in_path(const char *name)
{
#ifdef P100_WINDOWS
    const char sep = ';';
    const char *joiner = "\\";
#else
    const char sep = ':';
    const char *joiner = "/";
#endif
    const char *env = getenv("PATH");
    if (!env || !*env)
        return "";
    const std::string path(env);
    size_t start = 0;
    for (;;)
    {
        const size_t end = path.find(sep, start);
        const std::string dir = path.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty())
        {
            const std::string candidate = dir + joiner + name;
            if (is_executable(candidate))
                return candidate;
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return "";
}

static JsRuntime find_js_runtime()
{
    // The runtime never moves while the app runs; detecting it costs a few
    // stat calls per yt-dlp invocation otherwise.
    static const JsRuntime cached = []() {
        if (const char *env = getenv("P100_JS_RUNTIME"); env && *env)
        {
            const std::string spec(env);
            const size_t colon = spec.find(':');
            if (colon != std::string::npos && colon > 0)
                return JsRuntime{spec.substr(0, colon), spec.substr(colon + 1)};
            return JsRuntime{"deno", spec};
        }
#ifdef P100_WINDOWS
        const char *deno_name = "deno.exe";
        const char *node_name = "node.exe";
#else
        const char *deno_name = "deno";
        const char *node_name = "node";
#endif
        std::vector<std::string> candidates;
        // Installed share dir (install.sh) or a bundled AppImage payload.
        candidates.push_back(p100_share_dir() + "/runtime/" + deno_name);
        // Per-user data dir: the AppImage is read-only, so its launcher
        // provisions the runtime next to the venv instead.
        {
            std::string base;
#ifdef P100_WINDOWS
            if (const char *appdata = getenv("LOCALAPPDATA"); appdata && *appdata)
                base = std::string(appdata) + "/crossfade";
#else
            if (const char *xdg = getenv("XDG_DATA_HOME"); xdg && *xdg)
                base = std::string(xdg) + "/crossfade";
            else if (const char *home = getenv("HOME"); home && *home)
                base = std::string(home) + "/.local/share/crossfade";
#endif
            if (!base.empty())
                candidates.push_back(base + "/runtime/" + deno_name);
        }
        // Portable build: runtime next to the executable.
        candidates.push_back(exe_dir() + "/" + deno_name);
        candidates.push_back(exe_dir() + "/runtime/" + deno_name);
        // Windows installer layout: next to yt-dlp.exe in the venv.
        const std::string ytdlp = ytdlp_bin();
        const size_t slash = ytdlp.find_last_of("/\\");
        if (slash != std::string::npos)
            candidates.push_back(ytdlp.substr(0, slash + 1) + deno_name);
        for (const auto &candidate : candidates)
            if (is_executable(candidate))
                return JsRuntime{"deno", candidate};
        if (std::string found = find_in_path(deno_name); !found.empty())
            return JsRuntime{"deno", found};
        if (std::string found = find_in_path(node_name); !found.empty())
            return JsRuntime{"node", found};
        return JsRuntime{};
    }();
    return cached;
}

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
            if (!pending.empty())
                route_backend_line(*ch, pending);
            channel_mark_dead(*ch);
            {
                std::lock_guard<std::mutex> lock(ch->q_mutex);
                ch->lines.push_back(""); // sentinel: dead, nothing more coming
            }
            ch->q_cv.notify_all();
            return;
        }
        pending.append(buf, (size_t)n);
        if (pending.size() > kMaxBackendLine)
        {
            // Same bound as the POSIX reader: a reply that never terminates
            // must not grow the buffer forever. The stream is desynced, so
            // retire the channel after handing over the error.
            fprintf(stderr, "ERROR: Backend response too large ( > %zu MB). Aborting read.\n",
                    kMaxBackendLine / (1024 * 1024));
            channel_mark_dead(*ch);
            {
                std::lock_guard<std::mutex> lock(ch->q_mutex);
                ch->lines.clear();
                ch->lines.push_back("{\"error\":\"Response too large\",\"kind\":\"backend\"}");
                ch->lines.push_back(""); // sentinel: dead, nothing more coming
            }
            ch->q_cv.notify_all();
            return;
        }
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos)
        {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            route_backend_line(*ch, std::move(line));
        }
    }
}

static std::string backend_read_line(BackendChannel &ch, int timeout_sec = -1)
{
    // Same 45 s deadline and error payloads as the POSIX poll() loop below.
    std::unique_lock<std::mutex> lock(ch.q_mutex);
    const int wait_sec = timeout_sec > 0 ? timeout_sec : read_timeout_seconds();
    time_t deadline = time(nullptr) + wait_sec;
    while (ch.lines.empty())
    {
        if (ch.dead.load())
            return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
        if (g_backend_stopping.load(std::memory_order_relaxed))
            return "{\"error\":\"Backend stopping\",\"kind\":\"backend\"}";
        int remaining = (int)(deadline - time(nullptr));
        if (remaining <= 0)
        {
            fprintf(stderr, "ERROR: Backend read timed out after %d s.\n", wait_sec);
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
    const std::string buf = cmd + "\n";
    // WriteFile may write fewer bytes than requested on a pipe. A partial
    // write already reached the backend, so returning false here would leave
    // it waiting on a truncated command; finish the write instead.
    size_t sent = 0;
    while (sent < buf.size())
    {
        DWORD written = 0;
        const DWORD chunk = (DWORD)std::min<size_t>(buf.size() - sent, 0x7fffffff);
        if (!WriteFile(ch.h_in_write, buf.data() + sent, chunk, &written, nullptr) ||
            written == 0)
        {
            // A failed/zero write mid-command desyncs the protocol.
            channel_mark_dead(ch);
            return false;
        }
        sent += written;
    }
    return true;
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
static std::string backend_read_line(BackendChannel &ch, int timeout_sec = -1)
{
    // Raw fd reads (not fgets): stdio buffering hides data from poll(), which
    // would spuriously time out mid-line. One line per command; the backend
    // replies only after a command, so the fd is quiet between reads.

    if (!ch.out)
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    int fd = fileno(ch.out);
    if (fd < 0)
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";

    std::string line;
    char buf[8192];
    size_t total_read = 0;
    const int wait_sec = timeout_sec > 0 ? timeout_sec : read_timeout_seconds();
    time_t deadline = time(nullptr) + wait_sec;

    while (line.find('\n') == std::string::npos)
    {
        if (ch.dead.load())
            return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
        if (g_backend_stopping.load(std::memory_order_relaxed))
            return "{\"error\":\"Backend stopping\",\"kind\":\"backend\"}";

        int remaining = (int)(deadline - time(nullptr));
        if (remaining <= 0)
        {
            fprintf(stderr, "ERROR: Backend read timed out after %d s.\n", wait_sec);
            return "{\"error\":\"Backend did not respond in time\",\"kind\":\"backend\"}";
        }

        struct pollfd pfd = {fd, POLLIN, 0};
        const int wait_ms = std::min(remaining * 1000, 200);
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            channel_mark_dead(ch);
            return "{\"error\":\"Backend pipe error\",\"kind\":\"backend\"}";
        }
        if (pr == 0)
            continue; // poll timed out: loop re-checks the deadline

        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            channel_mark_dead(ch);
            return "{\"error\":\"Backend pipe error\",\"kind\":\"backend\"}";
        }
        if (n == 0)
        {
            // EOF: the backend process exited (crash, killed, bad auth).
            channel_mark_dead(ch);
            return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
        }
        line.append(buf, (size_t)n);
        total_read += (size_t)n;
        if (total_read > kMaxBackendLine)
        {
            fprintf(stderr, "ERROR: Backend response too large ( > %zu MB). Aborting read.\n",
                    kMaxBackendLine / (1024 * 1024));
            return "{\"error\":\"Response too large\",\"kind\":\"backend\"}";
        }
    }

    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    return line;
}
// Reader for id-tagged channels: routes `@id <json>` replies to their waiting
// request. Only the main channel uses this; legacy channels stay request/reply.
static void backend_reader_thread_posix(BackendChannel *ch)
{
    std::string pending;
    char buf[8192];
    for (;;)
    {
        if (ch->dead.load(std::memory_order_relaxed))
            break;
        struct pollfd pfd = {fileno(ch->out), POLLIN, 0};
        int pr = poll(&pfd, 1, 200);
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue; // re-check dead
        const ssize_t n = read(fileno(ch->out), buf, sizeof(buf));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break; // EOF: backend exited
        pending.append(buf, (size_t)n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos)
        {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            route_backend_line(*ch, std::move(line));
        }
    }
    if (!pending.empty())
        route_backend_line(*ch, pending);
    channel_mark_dead(*ch);
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
    // The main channel answers id-tagged commands concurrently; the workers
    // stay serial (one resolve / provider cascade at a time per channel).
    ch.concurrent = (&ch == &g_backend);
#ifdef P100_WINDOWS
    // A stopped backend's reader queues an empty EOF sentinel. If it survives
    // into the new process, the first read returns that stale line and the
    // freshly spawned backend is wrongly declared "not ready" -- which made a
    // sign-in backend restart fail and left the app on "Backend not running".
    {
        std::lock_guard<std::mutex> lock(ch.q_mutex);
        ch.lines.clear();
    }
#endif
#ifdef P100_WINDOWS
    const std::string python = backend_python();
    const std::string script = backend_script();

    // Child env: same contract as the POSIX branch below (setenv before exec).
    _putenv_s("P100_BROWSER", g_ytdl_browser.c_str());
    _putenv_s("P100_AUTH_FILE", backend_auth_file_path().c_str());
    _putenv_s("P100_CONFIG_DIR", p100_config_dir().c_str());
    _putenv_s("P100_YTDLP", ytdlp_bin().c_str());
    _putenv_s("P100_WARM_EXTRACTOR", g_warm_extractor.c_str());
    _putenv_s("P100_BACKEND_ROLE", backend_channel_name(ch));
    _putenv_s("P100_BACKEND_CONCURRENT", ch.concurrent ? "1" : "0");
    _putenv_s("P100_BACKEND_WORKERS", "4");
    {
        // Same contract as POSIX: the backend resolves streams in-process, so
        // it needs the runtime path for yt-dlp's js_runtimes option.
        const JsRuntime js = find_js_runtime();
        const std::string spec = js.path.empty() ? "" : js.name + ":" + js.path;
        _putenv_s("P100_JS_RUNTIME", spec.c_str());
    }
    _putenv_s("PYTHONUNBUFFERED", "1");
    // The command protocol is UTF-8 on both ends; without this, redirected
    // stdio uses the ANSI code page and non-Latin titles/queries break.
    _putenv_s("PYTHONIOENCODING", "utf-8");
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
        channel_mark_dead(ch);
        ch.ready_cv.notify_all();
        return false;
    }
    g_backend_guest.store(ready_says_guest(ready), std::memory_order_relaxed);
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
        setenv("P100_BACKEND_ROLE", backend_channel_name(ch), 1);
        setenv("P100_BACKEND_CONCURRENT", ch.concurrent ? "1" : "0", 1);
        setenv("P100_BACKEND_WORKERS", "4", 1);
        // The backend runs yt-dlp in-process for resolves. Hand it the same
        // JavaScript runtime the C++ side passes on the command line, and put
        // it first on PATH so any yt-dlp invocation that doesn't take the flag
        // still finds it.
        {
            const JsRuntime js = find_js_runtime();
            if (!js.path.empty())
            {
                const std::string spec = js.name + ":" + js.path;
                setenv("P100_JS_RUNTIME", spec.c_str(), 1);
                const size_t slash = js.path.find_last_of('/');
                if (slash != std::string::npos)
                {
                    const std::string runtime_dir = js.path.substr(0, slash);
                    const char *old = getenv("PATH");
                    const std::string path =
                        runtime_dir + (old ? ":" + std::string(old) : "");
                    setenv("PATH", path.c_str(), 1);
                }
            }
        }
        setenv("PYTHONUNBUFFERED", "1", 1);
        // The command protocol is UTF-8 on both ends; keep stdio from
        // falling back to the locale encoding (ASCII under LANG=C).
        setenv("PYTHONIOENCODING", "utf-8", 1);
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
        channel_mark_dead(ch);
        ch.ready_cv.notify_all();
        return false;
    }
    g_backend_guest.store(ready_says_guest(ready), std::memory_order_relaxed);
    ch.ready.store(true, std::memory_order_release);
    ch.ready_cv.notify_all();
    if (ch.concurrent && !ch.reader.joinable())
        ch.reader = std::thread(backend_reader_thread_posix, &ch);
    return true;
#endif // !P100_WINDOWS
}

static std::thread g_optional_backend_thread;

// Tear a channel down completely (both platforms) so it can be spawned again.
// Only used for a channel that is already dead, so no other thread should be
// reading or writing its pipes.
static void teardown_channel(BackendChannel &ch)
{
    // Serialize with any command I/O on this channel before closing its pipes.
    std::lock_guard<std::mutex> io_lock(ch.mutex);
    channel_mark_dead(ch);
#ifdef P100_WINDOWS
    channel_close(ch);
    if (ch.pid)
    {
        TerminateProcess(ch.pid, 0);
        WaitForSingleObject(ch.pid, 2000);
        CloseHandle(ch.pid);
        ch.pid = NULL;
    }
    if (ch.reader.joinable())
        ch.reader.join();
    {
        std::lock_guard<std::mutex> lock(ch.q_mutex);
        ch.lines.clear();
    }
#else
    // Let a concurrent reader leave poll() before its fd is closed.
    if (ch.reader.joinable())
        ch.reader.join();
    if (ch.in)
    {
        fclose(ch.in);
        ch.in = nullptr;
    }
    if (ch.out)
    {
        fclose(ch.out);
        ch.out = nullptr;
    }
    if (ch.pid > 0)
    {
        kill(ch.pid, SIGTERM);
        waitpid(ch.pid, nullptr, 0);
        ch.pid = 0;
    }
#endif
    ch.dead.store(false, std::memory_order_relaxed);
}

// Optional workers can die (a crashed backend, a killed process) or be marked
// dead after a wedged reply. Bring one back instead of degrading the whole
// session: play falls back to main, but meta death silently killed lyrics and
// prefetch. Throttled so a persistent fault cannot spawn a process per command.
static bool revive_channel(BackendChannel &ch)
{
    if (&ch == &g_backend)
        return false;
    if (!g_optional_backends_enabled.load(std::memory_order_relaxed) ||
        g_backend_stopping.load(std::memory_order_relaxed))
        return false;
    static std::mutex revive_mutex;
    std::lock_guard<std::mutex> lock(revive_mutex);
    if (channel_usable(ch))
        return true;
    const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const int64_t last = ch.last_revive_ms.load(std::memory_order_relaxed);
    if (last != 0 && now - last < 30000)
        return false;
    ch.last_revive_ms.store(now, std::memory_order_relaxed);
    fprintf(stderr, "Backend worker %s is not usable; restarting it.\n",
            backend_channel_name(ch));
    teardown_channel(ch);
    return spawn_backend(ch);
}

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
    for (BackendChannel *ch : {&g_backend, &g_play_backend, &g_prefetch_backend, &g_meta_backend})
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
        // Meta before prefetch: lyrics and background list warmups should get
        // their own pipe as early as possible so neither touches main.
        if (!spawn_backend(g_meta_backend) &&
            !g_backend_stopping.load(std::memory_order_relaxed))
            fprintf(stderr, "Meta backend worker unavailable; lyrics and background "
                            "prefetch share the main backend channel.\n");
        if (!spawn_backend(g_prefetch_backend) &&
            !g_backend_stopping.load(std::memory_order_relaxed))
            fprintf(stderr, "Prefetch backend worker unavailable; speculative "
                            "warmups share the main backend channel.\n");
    });
    return main_ok;
}

bool backend_is_guest()
{
    return g_backend_guest.load(std::memory_order_relaxed);
}

void cancel_backend_startup()
{
    g_backend_stopping.store(true, std::memory_order_relaxed);
    for (BackendChannel *ch : {&g_backend, &g_play_backend, &g_prefetch_backend, &g_meta_backend})
        ch->ready_cv.notify_all();
}

std::string backend_diagnostics()
{
    auto line = [](const char *name, const BackendChannel &ch) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%-8s ready=%d dead=%d pending=%d pid=%llu commands=%llu errors=%llu "
                      "last=%llums max=%llums",
                      name, (int)ch.ready.load(), (int)ch.dead.load(),
                      (int)std::max(0, ch.pending.load()),
                      (unsigned long long)(uintptr_t)ch.pid,
                      (unsigned long long)ch.commands.load(),
                      (unsigned long long)ch.errors.load(),
                      (unsigned long long)ch.last_ms.load(),
                      (unsigned long long)ch.max_ms.load());
        return std::string(buf);
    };
    std::string out;
    out += line("main", g_backend);
    out += '\n';
    out += line("play", g_play_backend);
    out += '\n';
    out += line("prefetch", g_prefetch_backend);
    out += '\n';
    out += line("meta", g_meta_backend);
    out += '\n';
    return out;
}

BackendDebugSnapshot backend_debug_snapshot()
{
    BackendDebugSnapshot snap;

    auto channel = [&](const char *name, BackendChannel &ch) {
        BackendChannelStats s;
        s.name = name;
        s.ready = ch.ready.load(std::memory_order_relaxed);
        s.dead = ch.dead.load(std::memory_order_relaxed);
        s.pid = (uintptr_t)ch.pid;
        s.commands = ch.commands.load(std::memory_order_relaxed);
        s.errors = ch.errors.load(std::memory_order_relaxed);
        s.last_ms = ch.last_ms.load(std::memory_order_relaxed);
        s.max_ms = ch.max_ms.load(std::memory_order_relaxed);
        s.pending = (size_t)std::max(0, ch.pending.load(std::memory_order_relaxed));
        snap.channels.push_back(std::move(s));
    };
    channel("main", g_backend);
    channel("play", g_play_backend);
    channel("prefetch", g_prefetch_backend);
    channel("meta", g_meta_backend);

    {
        std::lock_guard<std::mutex> lock(g_backend_log_mutex);
        snap.log.reserve(g_backend_log.size());
        for (auto it = g_backend_log.rbegin(); it != g_backend_log.rend(); ++it)
            snap.log.push_back(*it);
    }
    return snap;
}

void backend_debug_clear_log()
{
    std::lock_guard<std::mutex> lock(g_backend_log_mutex);
    g_backend_log.clear();
}

void stop_backend()
{
    // Cancel an in-flight optional-channel ready wait, then join before we
    // close its pipes. Without this a backend spawned while quitting could
    // delay shutdown by the full read timeout.
    g_backend_stopping.store(true, std::memory_order_relaxed);
    if (g_optional_backend_thread.joinable())
        g_optional_backend_thread.join();
    for (BackendChannel *ch : {&g_backend, &g_play_backend, &g_prefetch_backend, &g_meta_backend})
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
        channel_mark_dead(*ch);
        if (ch->reader.joinable())
            ch->reader.join();
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
            if (++depth > tunable_int("backend.json_max_depth"))
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

// A usable thumbnail is an http(s) URL that is not a YouTube page link. The
// watch/youtu.be URLs show up in old caches and in the "thumbnails" fallback
// below (the object's own "url" field); they are never images.
static bool thumbnail_url_is_image(const std::string &url)
{
    if (url.empty())
        return false;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        return false;
    if (url.find("youtube.com/watch") != std::string::npos ||
        url.find("youtu.be/") != std::string::npos)
        return false;
    return true;
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
    // Older album caches stored the track's YouTube watch link here (the
    // thumbnails fallback below used to grab the first "url" in the object).
    // Not an image: clear it so the album-cover fill / video-still fallback
    // can supply real artwork.
    if (!t.thumbnail_url.empty() && !thumbnail_url_is_image(t.thumbnail_url))
        t.thumbnail_url.clear();
    t.duration_sec = json_get_int(obj, "duration_sec");
    if (t.id.empty())
        t.id = json_get_string(obj, "videoId");
    // YouTube Music playlist items store the artist under "artists": [{"name":...}]
    if (t.artist.empty())
        t.artist = json_get_string(obj, "name");
    if (t.thumbnail_url.empty())
    {
        const std::string thumb = json_get_string(obj, "thumbnail");
        if (thumbnail_url_is_image(thumb))
            t.thumbnail_url = thumb;
    }
    if (t.thumbnail_url.empty())
    {
        // thumbnails: [{"url":...}] -- search only inside the thumbnails
        // value: the object also carries the track's own watch URL under
        // "url", which is not an image.
        size_t key = obj.find("\"thumbnails\"");
        if (key == std::string::npos)
            key = obj.find("\"thumbnail\"");
        size_t upos = key == std::string::npos ? std::string::npos
                                               : obj.find("\"url\"", key);
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
                    {
                        const std::string candidate = obj.substr(q + 1, q2 - q - 1);
                        if (thumbnail_url_is_image(candidate))
                            t.thumbnail_url = candidate;
                    }
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
    if (json.size() > (size_t)tunable_int("backend.json_max_input_kb") * 1024u)
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
            if (++depth > tunable_int("backend.json_max_depth"))
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
    if (json.size() > (size_t)tunable_int("backend.json_max_input_kb") * 1024u)
        return false;
    if (json.empty() || json.find("\"error\"") != std::string::npos)
        return false;

    out.ok = true;
    out.version = json_get_int(json, "version");
    if (json_get_bool(json, "none"))
    {
        out.none = true;
        return true;
    }

    out.synced = json_get_bool(json, "synced");
    out.source = json_get_string(json, "source");
    out.language = json_get_string(json, "language");

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
        RichLyricLine rich;
        rich.start_ms = json_get_int(obj, "t") * 1000;
        rich.end_ms = json_get_int(obj, "end") * 1000;
        rich.text = json_get_string(obj, "text");
        normalize_lyric_text(rich.text);
        rich.translation = json_get_string(obj, "translation");
        rich.romanization = json_get_string(obj, "roman");
        const size_t words_key = obj.find("\"words\":[");
        if (words_key != std::string::npos)
        {
            size_t wpos = obj.find('[', words_key) + 1;
            while (wpos < obj.size() && wpos != 0)
            {
                while (wpos < obj.size() &&
                       (isspace((unsigned char)obj[wpos]) || obj[wpos] == ','))
                    ++wpos;
                if (wpos >= obj.size() || obj[wpos] != '{')
                    break;
                const size_t wend = json_object_end(obj, wpos);
                if (wend == std::string::npos)
                    break;
                const std::string wobj = obj.substr(wpos, wend - wpos + 1);
                LyricWord word;
                word.start_ms = json_get_int(wobj, "s");
                word.end_ms = json_get_int(wobj, "e");
                word.text = json_get_string(wobj, "text");
                // Only provider-timed words count. Old caches synthesized
                // words with 0/0 times; treating those as timed made the
                // per-word renderer run on line-only lyrics.
                if (!word.text.empty() && (word.start_ms > 0 || word.end_ms > 0))
                    rich.words.push_back(std::move(word));
                wpos = wend + 1;
            }
        }
        out.times.push_back(rich.start_ms / 1000);
        out.lines.push_back(rich.text);
        out.rich.push_back(std::move(rich));
        pos = end + 1;
    }
    return true;
}

// ------------------ High‑level commands ------------------
// Write one command line while the caller already holds ch.mutex.
static bool write_line_locked(BackendChannel &ch, const std::string &cmd)
{
#ifndef P100_WINDOWS
    if (!ch.in)
        return false;
    fputs((cmd + "\n").c_str(), ch.in);
    return !ferror(ch.in) && fflush(ch.in) == 0;
#else
    return channel_write_line(ch, cmd);
#endif
}

// One command is outstanding per channel. When its reply misses the deadline,
// the backend will still send it later; the next command would then read that
// stale line as its own reply (a prefetch batch decodes as "play this random
// song", a playlist reply as the wrong track list). After a timeout, ping with
// a nonce and discard everything up to the matching reply so the stream is
// aligned again before the next real command.
static bool resync_backend_channel(BackendChannel &ch)
{
    static std::atomic<uint64_t> seq{0};
    const uint64_t nonce = seq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!write_line_locked(ch, "ping " + std::to_string(nonce)))
        return false;

    const std::string needle = "\"nonce\":" + std::to_string(nonce);
    const int wait_sec = std::max(1, tunable_int("backend.resync_timeout_sec"));
    const time_t deadline = time(nullptr) + wait_sec;
    int discarded = 0;
    while (time(nullptr) < deadline && discarded < 32)
    {
        const int remaining = (int)(deadline - time(nullptr));
        if (remaining <= 0)
            break;
        const std::string line = backend_read_line(ch, remaining);
        if (line.find(needle) != std::string::npos)
            return true;
        if (line.find("Backend not running") != std::string::npos ||
            line.find("Backend stopping") != std::string::npos)
            return false;
        ++discarded; // stale reply from the timed-out command
    }
    return false;
}

// Id-tagged command for a concurrent channel. The write is serialized, the
// wait is per request, and the reader thread routes the reply back. A late
// reply after a timeout finds no pending entry and is dropped, so no resync is
// needed.
static std::string send_command_job(BackendChannel &ch, const std::string &cmd)
{
    const uint64_t id = ch.next_req.fetch_add(1, std::memory_order_relaxed) + 1;
    auto reply = std::make_shared<BackendReply>();
    {
        std::lock_guard<std::mutex> lock(ch.pending_mutex);
        ch.pending_replies[id] = reply;
    }
    const int pending_ahead = ch.pending.fetch_add(1, std::memory_order_relaxed);
    const auto cmd_start = std::chrono::steady_clock::now();
    uint64_t queue_ms = 0;
    bool written = false;
    {
        std::lock_guard<std::mutex> wl(ch.write_mutex);
        queue_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - cmd_start)
                        .count();
        if (!ch.dead.load(std::memory_order_relaxed))
            written = write_line_locked(
                ch, "@" + std::to_string(id) + " " + cmd);
    }

    std::string response;
    if (!written)
    {
        channel_mark_dead(ch);
        response = "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    }
    else
    {
        std::unique_lock<std::mutex> lk(reply->mutex);
        const int wait_sec = read_timeout_seconds();
        if (!reply->cv.wait_for(lk, std::chrono::seconds(wait_sec),
                                [&] { return reply->done; }))
        {
            fprintf(stderr, "ERROR: Backend command timed out after %d s: %.80s\n",
                    wait_sec, cmd.c_str());
            response = "{\"error\":\"Backend did not respond in time\",\"kind\":\"backend\"}";
        }
        else
        {
            response = reply->response;
        }
    }
    {
        std::lock_guard<std::mutex> lock(ch.pending_mutex);
        ch.pending_replies.erase(id);
    }
    ch.pending.fetch_sub(1, std::memory_order_relaxed);

    const uint64_t elapsed_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cmd_start)
                                    .count();
    ch.commands.fetch_add(1, std::memory_order_relaxed);
    ch.last_ms.store(elapsed_ms, std::memory_order_relaxed);
    if (elapsed_ms > ch.max_ms.load(std::memory_order_relaxed))
        ch.max_ms.store(elapsed_ms, std::memory_order_relaxed);
    if (response.find("\"error\"") != std::string::npos)
    {
        ch.errors.fetch_add(1, std::memory_order_relaxed);
        record_backend_error(response);
    }
    else
    {
        std::lock_guard<std::mutex> err_lock(g_backend_error_mutex);
        if (g_backend_error.ok())
            g_backend_error.clear();
    }
    record_backend_log(ch, cmd, elapsed_ms, response, false, queue_ms,
                       (size_t)std::max(0, pending_ahead));
    return response;
}

// Handled entirely in C++ when it succeeds; false means "ask Python".
static bool native_handle_command(const std::string &cmd, std::string &out);

static std::string send_command_on(BackendChannel &ch, const std::string &cmd)
{
    {
        std::string native_out;
        if (native_handle_command(cmd, native_out))
            return native_out;
    }
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
    if (ch.concurrent)
        return send_command_job(ch, cmd);
    const auto cmd_start = std::chrono::steady_clock::now();
    ch.pending.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(ch.mutex);
    if (ch.dead.load(std::memory_order_relaxed))
    {
        // Another command's timeout/EOF marked the channel dead while this one
        // waited in the queue; never write into closed pipes.
        ch.pending.fetch_sub(1, std::memory_order_relaxed);
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    }
    const uint64_t queue_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cmd_start)
                                  .count();
    const size_t pending_ahead = (size_t)std::max(0, ch.pending.load(std::memory_order_relaxed) - 1);
    if (!write_line_locked(ch, cmd))
    {
        ch.pending.fetch_sub(1, std::memory_order_relaxed);
        channel_mark_dead(ch);
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    }
    std::string response = backend_read_line(ch);
    ch.pending.fetch_sub(1, std::memory_order_relaxed);
    const uint64_t elapsed_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cmd_start)
                                    .count();
    // Remember the failure reason (with its classified kind) so the UI can
    // tell the user what to do (stale cookies → re-run `ytmusicapi browser`,
    // offline → check connection, dead backend → restart).
    if (response.empty())
    {
        // A dead/piped-closed backend previously looked like "empty data";
        // make it an explicit error instead.
        {
            std::lock_guard<std::mutex> err_lock(g_backend_error_mutex);
            g_backend_error.kind = BackendErrKind::Backend;
            g_backend_error.message = "Backend not running";
        }
        record_backend_log(ch, cmd, elapsed_ms, response, true, queue_ms, pending_ahead);
        return "{\"error\":\"Backend not running\",\"kind\":\"backend\"}";
    }
    ch.commands.fetch_add(1, std::memory_order_relaxed);
    ch.last_ms.store(elapsed_ms, std::memory_order_relaxed);
    if (elapsed_ms > ch.max_ms.load(std::memory_order_relaxed))
        ch.max_ms.store(elapsed_ms, std::memory_order_relaxed);
    const bool timed_out =
        response.find("Backend did not respond in time") != std::string::npos;
    if (response.find("\"error\"") != std::string::npos)
    {
        ch.errors.fetch_add(1, std::memory_order_relaxed);
        record_backend_error(response);
    }
    else
    {
        std::lock_guard<std::mutex> err_lock(g_backend_error_mutex);
        if (g_backend_error.ok())
            g_backend_error.clear();
    }
    record_backend_log(ch, cmd, elapsed_ms, response, false, queue_ms, pending_ahead);
    if (timed_out)
    {
        const auto resync_start = std::chrono::steady_clock::now();
        const bool synced = resync_backend_channel(ch);
        const uint64_t resync_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - resync_start)
                                       .count();
        record_backend_log(ch, "resync after timeout", resync_ms,
                           synced ? "{\"ok\":true}"
                                  : "{\"error\":\"Backend resync failed\"}",
                           !synced);
        if (!synced)
            ch.dead.store(true, std::memory_order_relaxed);
    }
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
// A reply that means "this channel's process/pipe is gone". Callers use it to
// fall through to the next worker instead of failing the command outright.
static bool response_channel_dead(const std::string &response)
{
    return response.find("Backend not running") != std::string::npos ||
           response.find("Backend not responding") != std::string::npos ||
           response.find("Backend did not respond in time") != std::string::npos;
}

static std::string send_play_command(const std::string &cmd)
{
    if (channel_usable(g_play_backend))
    {
        std::string response = send_command_on(g_play_backend, cmd);
        if (!response_channel_dead(response))
            return response;
        if (revive_channel(g_play_backend))
        {
            response = send_command_on(g_play_backend, cmd);
            if (!response_channel_dead(response))
                return response;
        }
    }
    else if (g_play_backend.dead.load() && revive_channel(g_play_backend))
    {
        std::string response = send_command_on(g_play_backend, cmd);
        if (!response_channel_dead(response))
            return response;
    }
    return send_command_on(g_backend, cmd);
}

// Speculative prefetches get their own channel. They can take tens of seconds
// (a full yt-dlp resolve), so sharing the main channel starved searches and
// metadata lookups; sharing the play channel would make a click wait instead.
static std::string send_prefetch_command(const std::string &cmd)
{
    if (channel_usable(g_prefetch_backend))
    {
        std::string response = send_command_on(g_prefetch_backend, cmd);
        if (!response_channel_dead(response))
            return response;
        if (revive_channel(g_prefetch_backend))
        {
            response = send_command_on(g_prefetch_backend, cmd);
            if (!response_channel_dead(response))
                return response;
        }
    }
    else if (g_prefetch_backend.dead.load() && revive_channel(g_prefetch_backend))
    {
        std::string response = send_command_on(g_prefetch_backend, cmd);
        if (!response_channel_dead(response))
            return response;
    }
    return send_command_on(g_backend, cmd);
}

// Background metadata and speculative batches: slow provider cascades (lyrics)
// and list warmups must never sit in front of an interactive search. The meta
// worker falls back to prefetch and then main when it could not start.
static std::string send_meta_command(const std::string &cmd)
{
    if (channel_usable(g_meta_backend))
    {
        std::string response = send_command_on(g_meta_backend, cmd);
        if (!response_channel_dead(response))
            return response;
        // The worker died between our check and its reply: retry once on the
        // next channel so the first lyrics request after a crash still works,
        // then let the revive path bring meta back for the next one.
        if (revive_channel(g_meta_backend))
        {
            response = send_command_on(g_meta_backend, cmd);
            if (!response_channel_dead(response))
                return response;
        }
    }
    else if (g_meta_backend.dead.load() && revive_channel(g_meta_backend))
    {
        std::string response = send_command_on(g_meta_backend, cmd);
        if (!response_channel_dead(response))
            return response;
    }
    if (channel_usable(g_prefetch_backend))
        return send_command_on(g_prefetch_backend, cmd);
    if (g_prefetch_backend.dead.load() && revive_channel(g_prefetch_backend))
        return send_command_on(g_prefetch_backend, cmd);
    return send_command_on(g_backend, cmd);
}

// Speculative work (list/queue warmups) must never fall back to main: a large
// prefetch batch there is exactly what starves a search. Wait for one of the
// background workers instead (they start a few seconds after main).
static std::string send_background_command(const std::string &cmd)
{
    // Low-memory mode runs only the main worker; it is concurrent, so
    // speculative work can share it without blocking anything else.
    if (!g_optional_backends_enabled.load(std::memory_order_relaxed))
        return send_command_on(g_backend, cmd);
    for (int i = 0; i < 50; ++i)
    {
        if (channel_usable(g_meta_backend))
        {
            std::string response = send_command_on(g_meta_backend, cmd);
            if (!response_channel_dead(response))
                return response;
        }
        else if (channel_usable(g_prefetch_backend))
        {
            std::string response = send_command_on(g_prefetch_backend, cmd);
            if (!response_channel_dead(response))
                return response;
        }
        if (g_meta_backend.dead.load())
            revive_channel(g_meta_backend);
        if (g_prefetch_backend.dead.load())
            revive_channel(g_prefetch_backend);
        if (g_backend_stopping.load(std::memory_order_relaxed))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return "{\"error\":\"Background backend unavailable\",\"kind\":\"backend\"}";
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

// The runtime path comes from find_js_runtime() above; pass it explicitly so
// downloads work even when the runtime directory is not on PATH.
static std::string js_runtime_arg()
{
    const JsRuntime runtime = find_js_runtime();
    if (runtime.name.empty() || runtime.path.empty())
        return "";
    return " --js-runtimes \"" + runtime.name + ":" + runtime.path + "\"";
}

// Run a shell command and capture combined stdout/stderr so a yt-dlp failure
// can be reported instead of disappearing into 2>NUL.
static int run_capture(const std::string &cmd, std::string &out)
{
    out.clear();
#ifdef P100_WINDOWS
    return winproc::direct_capture(cmd, out);
#else
    FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        return -1;
    // yt-dlp's -j JSON for a music video can exceed 100 KB (formats +
    // thumbnails). The old 64 KB cap silently cut off the tail, which is where
    // `artist` and `thumbnail` live, so sidecars were written with a title
    // only. Keep a generous safety cap instead.
    const size_t kCaptureCap = (size_t)tunable_int("backend.capture_max_kb") * 1024u;
    char buffer[1024];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
    {
        if (out.size() < kCaptureCap)
        {
            const size_t room = kCaptureCap - out.size();
            out.append(buffer, n < room ? n : room);
        }
    }
    const int status = pclose(pipe);
    return (status >= 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif
}

static void log_download_failure(const std::string &video_id, const char *attempt,
                                 int rc, const std::string &output,
                                 const std::string &command)
{
    std::string tail = output;
    if (tail.size() > 2000)
        tail = tail.substr(tail.size() - 2000);
    const std::string path = p100_config_path("downloads.log");
    std::error_code ec;
    if (std::filesystem::exists(path, ec) &&
        std::filesystem::file_size(path, ec) > 1024 * 1024)
        std::filesystem::remove(path, ec);
    std::ofstream log(path, std::ios::app);
    if (log)
    {
        log << "[" << std::time(nullptr) << "] " << video_id
            << " attempt=" << attempt << " rc=" << rc << "\n"
            << "cmd: " << command << "\n"
            << tail << "\n\n";
    }
    // Console: one actionable line. The full yt-dlp output (including the
    // Python traceback from a failed JS challenge) goes to downloads.log.
    std::string summary;
    const size_t err_at = tail.rfind("ERROR:");
    if (err_at != std::string::npos)
    {
        const size_t eol = tail.find('\n', err_at);
        summary = tail.substr(err_at, eol == std::string::npos ? std::string::npos
                                                               : eol - err_at);
    }
    else
    {
        summary = "rc=" + std::to_string(rc);
    }
    if (summary.size() > 220)
        summary = summary.substr(0, 217) + "...";
    fprintf(stderr, "[yt-dlp] %s (%s) failed: %s\n", video_id.c_str(), attempt,
            summary.c_str());
}

static Track fetch_track_metadata(const std::string &video_id)
{
    Track t;
    t.id = video_id;
    t.url = "https://www.youtube.com/watch?v=" + video_id;

    const std::string base =
        shell_quote(ytdlp_bin()) + " -j --skip-download" + js_runtime_arg();
    const std::string url = "https://www.youtube.com/watch?v=" + video_id;
    const std::string attempts[] = {
        base + cookie_arg() + " " + url,
        base + " " + url,
        base + " --extractor-args \"youtube:player_client=visionos\" " + url,
    };

    std::string result;
    for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); ++i)
    {
        std::string output;
        const int rc = run_capture(attempts[i], output);
        const size_t brace = output.find('{');
        if (rc == 0 && brace != std::string::npos)
        {
            result = output.substr(brace);
            break;
        }
        if (i + 1 == sizeof(attempts) / sizeof(attempts[0]))
            log_download_failure(video_id, "metadata", rc, output, attempts[i]);
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
    std::ostringstream out;
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
    atomic_file::replace_text(meta_path, out.str());
}

std::string authoritative_thumbnail_url(const std::string &video_id,
                                        const std::string &fallback)
{
    if (video_id.empty() || video_id.rfind("local:", 0) == 0)
        return fallback;

    // Sidecars are named from the cached audio file, so they are identity-
    // scoped. Only trust the URL when it also carries the video id; a channel
    // avatar copied from another row must never become this track's cover.
    const std::string dir = cache_dir() + "/audio_cache";
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec))
    {
        const std::string prefix = video_id + ".";
        for (std::filesystem::directory_iterator it(dir, ec), end;
             !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.rfind(prefix, 0) != 0 || name.size() < 10 ||
                name.compare(name.size() - 10, 10, ".meta.json") != 0)
                continue;
            std::ifstream in(it->path());
            if (!in)
                continue;
            std::ostringstream buf;
            buf << in.rdbuf();
            const std::string url = jsonutil::get_string(buf.str(), "thumbnail_url");
            if (!url.empty() && url.find(video_id) != std::string::npos)
                return url;
        }
    }

    // id-derived YouTube art is always correct, even if lower resolution.
    return "https://img.youtube.com/vi/" + video_id + "/hqdefault.jpg";
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

void save_cached_track_metadata(const Track &track, const std::string &audio_path)
{
    if (audio_path.empty() || track.title.empty())
        return;
    std::lock_guard<std::mutex> lock(g_meta_mutex);
    // A delete may have run while the download was finishing; never write a
    // sidecar that would resurrect the track in the Downloads list.
    if (g_meta_writes_blocked)
        return;
    if (!track.id.empty() && g_meta_cancelled.count(track.id) != 0)
        return;
    std::error_code ec;
    if (!std::filesystem::exists(audio_path, ec))
        return;
    save_metadata(track, audio_path);
    g_downloads_dirty = true;
}

// ------------------ Public functions ------------------
// The personalized Home feed is paginated by a continuation token. Keep the
// newest token here so the UI can append more rows without re-fetching the
// first page. The token chain is per-session; it is replaced on refresh and
// dropped when the native continuation is exhausted or fails.
static std::mutex g_home_continuation_mutex;
static std::string g_home_continuation;

static void store_home_continuation(const std::string &continuation)
{
    std::lock_guard<std::mutex> lock(g_home_continuation_mutex);
    g_home_continuation = continuation;
}

static bool parse_home_feed(const std::string &json, std::vector<Track> &out,
                            std::string &continuation)
{
    out.clear();
    continuation.clear();
    const std::size_t first = json.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return false;
    if (json[first] == '[')
    {
        // Python fallback: a plain track array, no pagination.
        out = parse_track_array(json);
        return true;
    }
    for_each_json_object(json, "tracks", [&](const std::string &obj) {
        Track t;
        parse_track_object(obj, t);
        if (!t.id.empty() && !t.title.empty())
            out.push_back(std::move(t));
    });
    continuation = json_get_string(json, "continuation");
    return true;
}

std::vector<Track> fetch_recommendations(bool force_fresh, int limit)
{
    if (limit <= 0)
        limit = 40;
    std::string path = cache_dir() + "/recommendations_cache.json";
    if (!force_fresh)
    {
        // A feed cached before the current auth file was installed belongs to
        // a different (or anonymous) session. Serving it after a sign-in is
        // exactly how a personalized Home could look like guest content for an
        // hour, even across a relaunch.
        bool auth_newer = false;
        {
            std::error_code cache_ec, auth_ec;
            const auto cache_time = std::filesystem::last_write_time(path, cache_ec);
            const auto auth_time =
                std::filesystem::last_write_time(backend_auth_file_path(), auth_ec);
            auth_newer = !cache_ec && !auth_ec && auth_time > cache_time;
        }
        struct stat st;
        if (!auth_newer && stat(path.c_str(), &st) == 0 && st.st_size > 0)
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
                    {
                        std::vector<Track> tracks;
                        std::string continuation;
                        if (parse_home_feed(json, tracks, continuation))
                        {
                            store_home_continuation(continuation);
                            return tracks;
                        }
                    }
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
    std::vector<Track> tracks;
    std::string continuation;
    if (!parse_home_feed(response, tracks, continuation))
        return {};
    store_home_continuation(continuation);
    return tracks;
}

std::vector<Track> fetch_recommendations_more(bool &has_more)
{
    has_more = false;
    if (!native_backend_enabled() || !native::http_available())
        return {};
    std::string token;
    {
        std::lock_guard<std::mutex> lock(g_home_continuation_mutex);
        token = g_home_continuation;
    }
    if (token.empty())
        return {};
    std::string response = send_command("home_more " + token);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
    {
        if (!response.empty())
            fprintf(stderr, "Home feed error: %s\n", response.c_str());
        store_home_continuation("");
        return {};
    }
    std::vector<Track> tracks;
    std::string continuation;
    if (!parse_home_feed(response, tracks, continuation))
    {
        store_home_continuation("");
        return {};
    }
    store_home_continuation(continuation);
    has_more = !continuation.empty();
    return tracks;
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
    if (is_local_media_id(video_id))
    {
        error = "local file";
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
    if (is_local_media_id(video_id))
        return {};
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

// Tolerant decimal parse for backend-provided counts. Never throws, stops at
// the first non-digit and saturates instead of overflowing (std::stoi used to
// throw on malformed/huge input, and an exception in a parser thread kills
// the process).
static int parse_bounded_count(const std::string &s, size_t begin, size_t end)
{
    int value = 0;
    for (size_t i = begin; i < end && i < s.size() &&
                       isdigit((unsigned char)s[i]); ++i)
    {
        if (value > 100000000)
        {
            value = 1000000000;
            break;
        }
        value = value * 10 + (s[i] - '0');
    }
    return value;
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
                if (q != std::string::npos && q + 1 < obj.size() &&
                    isdigit((unsigned char)obj[q + 1]))
                {
                    size_t q2 = obj.find('"', q + 1);
                    if (q2 != std::string::npos)
                        p.count = parse_bounded_count(obj, q + 1, q2);
                }
                else
                {
                    size_t d = c + 1;
                    while (d < obj.size() && isspace((unsigned char)obj[d])) d++;
                    p.count = parse_bounded_count(obj, d, obj.size());
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
    while ((pos = response.find("\"videoId\"", pos)) != std::string::npos && guard < 1000)
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
    if (tracks.empty())
    {
        // The native backend emits ytmusicapi-shaped tracks but names the
        // video field "id" (it has no ytmusicapi to copy "videoId" from).
        // Skip the playlist's own top-level id so the root object is not
        // mistaken for a track.
        const std::string playlist_id = json_get_string(response, "id");
        pos = 0;
        guard = 0;
        while ((pos = response.find("\"id\":", pos)) != std::string::npos && guard < 2000)
        {
            std::string obj = json_find_object(response, pos);
            if (obj.empty())
            {
                pos += 5;
                continue;
            }
            Track t;
            parse_track_object(obj, t);
            if (!t.id.empty() && t.id != playlist_id && !t.title.empty())
                tracks.push_back(std::move(t));
            pos += 5;
            guard++;
        }
    }
    return tracks;
}

// ytmusicapi's get_playlist reply puts the playlist browse id in the top-level
// "id" key. Auto playlists (Liked Music, "LM") have "id": null, and the first
// *quoted* "id" after it is the author's channel id — so json_get_string() is
// the wrong tool for this check. Inspect only the first (top-level) id value:
// null means "no id, accept"; a quoted value must match the request.
static bool playlist_reply_matches_id(const std::string &json, const std::string &want)
{
    const size_t key = json.find("\"id\"");
    if (key == std::string::npos)
        return true;
    const size_t colon = json.find(':', key + 4);
    if (colon == std::string::npos)
        return true;
    const size_t value = json.find_first_not_of(" \t\r\n", colon + 1);
    if (value == std::string::npos)
        return true;
    if (json.compare(value, 4, "null") == 0)
        return true; // Liked Music and other browse-less auto playlists
    if (json[value] != '"')
        return true;
    std::string id;
    for (size_t i = value + 1; i < json.size(); ++i)
    {
        const char c = json[i];
        if (c == '"')
            break;
        if (c == '\\' && i + 1 < json.size())
            ++i; // playlist ids never need unescaping, but don't misread one
        id += c;
    }
    return id == want;
}

// Shared tail of the two playlist fetchers: validate, parse, cache. A rejected
// or unparseable reply records a backend error so the UI offers a Retry instead
// of silently marking the playlist as an empty one.
static std::vector<Track> accept_youtube_playlist_reply(const std::string &playlist_id,
                                                         const std::string &response)
{
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return {};
    // get_playlist() always carries trackCount; requiring it rejects a stale
    // reply from a different command (library_playlists, prefetch, search).
    if (response.find("\"trackCount\"") == std::string::npos)
    {
        record_backend_error(
            "{\"error\":\"Unexpected reply while loading the playlist\",\"kind\":\"backend\"}");
        return {};
    }
    if (!playlist_reply_matches_id(response, playlist_id))
    {
        record_backend_error(
            "{\"error\":\"The playlist reply did not match the request\",\"kind\":\"backend\"}");
        return {};
    }
    const int expected = json_get_int(response, "trackCount");
    auto tracks = parse_youtube_playlist_tracks_response(response);
    if (tracks.empty() && expected > 0)
    {
        record_backend_error(
            "{\"error\":\"Could not parse the playlist tracks\",\"kind\":\"backend\"}");
        return {};
    }
    write_text_file(youtube_playlist_cache_path(playlist_id), response);
    return tracks;
}

std::vector<Track> fetch_youtube_playlist(const std::string &playlist_id)
{
    if (playlist_id.empty())
        return {};
    const std::string response = send_command("playlist " + playlist_id);
    return accept_youtube_playlist_reply(playlist_id, response);
}

// Background variant for the offline pre-cache sweep. It runs on the prefetch
// channel so warming playlist contents can never queue in front of an
// interactive pick, radio station or Home refresh on the main channel.
std::vector<Track> fetch_youtube_playlist_prefetch(const std::string &playlist_id)
{
    if (playlist_id.empty())
        return {};
    const std::string response = send_prefetch_command("playlist " + playlist_id);
    return accept_youtube_playlist_reply(playlist_id, response);
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

std::vector<std::pair<std::string, std::string>> parse_playlist_item_pairs(
    const std::string &response)
{
    std::vector<std::pair<std::string, std::string>> out;
    // ytmusicapi appends "setVideoId" to each playlist item *after* its
    // nested artists/album/thumbnails objects, so the enclosing object can't
    // be found by scanning backwards for the nearest '{' (that lands inside
    // the last nested object, which carries no videoId). Walk the "tracks"
    // array and parse each top-level element instead. Order of appearance is
    // the server order.
    const size_t key = response.find("\"tracks\"");
    if (key == std::string::npos)
        return out;
    const size_t colon = response.find(':', key + 8);
    if (colon == std::string::npos)
        return out;
    size_t arr = colon + 1;
    while (arr < response.size() &&
           (response[arr] == ' ' || response[arr] == '\t' ||
            response[arr] == '\r' || response[arr] == '\n'))
        ++arr;
    if (arr >= response.size() || response[arr] != '[')
        return out;

    int depth = 0;
    bool in_str = false, esc = false;
    size_t obj_start = std::string::npos;
    for (size_t i = arr + 1; i < response.size(); ++i)
    {
        const char c = response[i];
        if (in_str)
        {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                in_str = false;
            continue;
        }
        if (c == '"')
        {
            in_str = true;
        }
        else if (c == '{')
        {
            if (depth == 0)
                obj_start = i;
            if (++depth > tunable_int("backend.json_max_depth"))
                return out;
        }
        else if (c == '}')
        {
            if (depth <= 0)
                continue; // stray brace outside an element
            if (--depth == 0 && obj_start != std::string::npos)
            {
                const std::string obj =
                    response.substr(obj_start, i - obj_start + 1);
                const std::string vid = json_get_string(obj, "videoId");
                const std::string svid = json_get_string(obj, "setVideoId");
                if (!vid.empty() && !svid.empty())
                    out.emplace_back(vid, svid);
                obj_start = std::string::npos;
            }
        }
        else if (c == ']' && depth == 0)
        {
            break; // end of the tracks array
        }
    }
    return out;
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
    return parse_playlist_item_pairs(response);
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
    for_each_json_object(response, "unmatched", [&](const std::string &obj) {
        const std::string title = json_get_string(obj, "title");
        const std::string artist = json_get_string(obj, "artist");
        if (title.empty())
            return;
        out.unmatched.push_back(artist.empty() ? title : title + " - " + artist);
    });
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
    if (video_id.empty() || is_local_media_id(video_id))
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
    if (video_id.empty() || is_local_media_id(video_id))
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

SearchSuggestions fetch_search_suggestions(const std::string &query)
{
    SearchSuggestions out;
    std::string response = send_command("suggest " + query);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return out;
    for_each_json_object(response, "queries", [&](const std::string &obj) {
        SearchSuggestionQuery q;
        q.text = json_get_string(obj, "text");
        q.history = json_get_bool(obj, "history", false);
        if (!q.text.empty())
            out.queries.push_back(std::move(q));
    });
    for_each_json_object(response, "items", [&](const std::string &obj) {
        SearchSuggestionItem it;
        it.kind = json_get_string(obj, "kind");
        it.id = json_get_string(obj, "id");
        it.title = json_get_string(obj, "title");
        it.artist = json_get_string(obj, "artist");
        it.album = json_get_string(obj, "album");
        it.subtitle = json_get_string(obj, "subtitle");
        it.thumbnail_url = json_get_string(obj, "thumbnail_url");
        if (!it.id.empty() && !it.title.empty())
            out.items.push_back(std::move(it));
    });
    return out;
}

std::vector<AlbumSummary> fetch_search_albums(const std::string &query)
{
    std::vector<AlbumSummary> albums;
    if (query.empty())
        return albums;
    std::string response = send_command("album_search " + query);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return albums;
    for_each_json_array_object(response, [&](const std::string &obj) {
        AlbumSummary a;
        a.browse_id = json_get_string(obj, "browse_id");
        a.title = json_get_string(obj, "title");
        a.artist = json_get_string(obj, "artist");
        a.year = json_get_string(obj, "year");
        a.thumbnail_url = json_get_string(obj, "thumbnail_url");
        a.track_count = json_get_int(obj, "track_count");
        if (!a.browse_id.empty() && !a.title.empty())
            albums.push_back(std::move(a));
    });
    return albums;
}

bool fetch_search_all(const std::string &query, SearchAllResult &out)
{
    out = SearchAllResult{};
    if (query.empty())
        return false;
    std::string response = send_command("search_all " + query);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return false;

    // "top": {...} or null -- YouTube Music's own intent match. The backend
    // only reshapes it; src/app/search_top.cpp decides whether to trust it.
    const size_t top_key = response.find("\"top\":");
    if (top_key != std::string::npos)
    {
        const size_t brace = response.find_first_not_of(" \t\r\n", top_key + 6);
        if (brace != std::string::npos && response[brace] == '{')
        {
            const size_t end = json_object_end(response, brace);
            if (end != std::string::npos)
            {
                const std::string obj = response.substr(brace, end - brace + 1);
                const std::string kind = json_get_string(obj, "kind");
                if (kind == "song" || kind == "video")
                    out.top.kind = TopKind::Song;
                else if (kind == "album")
                    out.top.kind = TopKind::Album;
                else if (kind == "artist")
                    out.top.kind = TopKind::Artist;
                out.top.video = kind == "video";
                out.top.id = json_get_string(obj, "id");
                out.top.title = json_get_string(obj, "title");
                out.top.artist = json_get_string(obj, "artist");
                out.top.artist_id = json_get_string(obj, "artist_id");
                out.top.album = json_get_string(obj, "album");
                out.top.album_id = json_get_string(obj, "album_id");
                out.top.year = json_get_string(obj, "year");
                out.top.duration = json_get_string(obj, "duration");
                out.top.duration_sec = json_get_int(obj, "duration_sec");
                out.top.subscribers = json_get_string(obj, "subscribers");
                out.top.thumbnail_url = json_get_string(obj, "thumbnail_url");
                if (out.top.id.empty() || out.top.title.empty())
                    out.top = TopResult{};
            }
        }
    }

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
    return out.top.kind != TopKind::None || !out.songs.empty() || !out.albums.empty() ||
           !out.artists.empty();
}

bool fetch_lyrics(const std::string &video_id, Lyrics &out)
{
    if (is_local_media_id(video_id))
    {
        // A local file has no YouTube lyrics lookup. Return a definitive
        // "none" so the panel stops retrying and caches the result.
        out = Lyrics{};
        out.ok = true;
        out.none = true;
        out.source = "Local file";
        return true;
    }
    // Lyrics are a slow provider cascade and are never needed on the
    // interactive path: keep them off the main channel (meta, then prefetch).
    std::string response = send_meta_command("lyrics " + video_id);
    return parse_lyrics_json(response, out);
}

// ---- Studio: waveform + beat analysis ---------------------------------
// The backend decodes with ffmpeg and writes a compact text pyramid so the
// C++ side never needs a JSON array parser. Format:
//   CFWAVE 1
//   duration <sec>
//   bpm <bpm>
//   confidence <0..1>
//   beats <t,t,...>
//   downbeats <t,t,...>
//   wave <buckets>   (repeated, coarse -> fine)
//   <min>,<max>      (buckets lines per wave)
static void parse_num_list(const std::string &text, std::vector<double> &out)
{
    out.clear();
    size_t start = 0;
    while (start < text.size())
    {
        const size_t comma = text.find(',', start);
        const std::string tok =
            text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!tok.empty())
        {
            try
            {
                out.push_back(std::stod(tok));
            }
            catch (...)
            {
            }
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
}

bool load_studio_analysis_file(const std::string &path, StudioAnalysis &out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return false;
    std::string line;
    if (!std::getline(in, line))
        return false;
    if (line.rfind("CFWAVE", 0) != 0)
        return false;

    out = StudioAnalysis{};
    StudioWaveLevel level;
    int wave_left = 0;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        if (wave_left > 0)
        {
            const size_t comma = line.find(',');
            float lo = 0.0f, hi = 0.0f;
            if (comma != std::string::npos)
            {
                try
                {
                    lo = std::stof(line.substr(0, comma));
                    hi = std::stof(line.substr(comma + 1));
                }
                catch (...)
                {
                }
            }
            level.mins.push_back(lo);
            level.maxs.push_back(hi);
            if (--wave_left == 0)
            {
                out.waves.push_back(std::move(level));
                level = StudioWaveLevel{};
            }
            continue;
        }

        const size_t sp = line.find(' ');
        const std::string key = line.substr(0, sp);
        const std::string val = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (key == "duration")
            out.duration = std::atof(val.c_str());
        else if (key == "bpm")
            out.bpm = std::atof(val.c_str());
        else if (key == "confidence")
            out.confidence = (float)std::atof(val.c_str());
        else if (key == "beats")
            parse_num_list(val, out.beats);
        else if (key == "downbeats")
            parse_num_list(val, out.downbeats);
        else if (key == "wave")
        {
            level = StudioWaveLevel{};
            level.buckets = std::atoi(val.c_str());
            wave_left = std::max(0, level.buckets);
        }
    }
    out.ok = out.duration > 0.0 && !out.waves.empty();
    return out.ok;
}

bool fetch_studio_analysis(const std::string &id, const std::string &audio_path,
                           const std::string &cache_path, StudioAnalysis &out)
{
    out = StudioAnalysis{};
    if (load_studio_analysis_file(cache_path, out))
    {
        out.id = id;
        return true;
    }

    // A cache file that failed to parse is stale/partial: remove it so the
    // backend re-analyses instead of returning the same unreadable file.
    std::remove(cache_path.c_str());

    const std::string payload =
        "{\"id\":\"" + jsonutil::escape(id) + "\",\"path\":\"" +
        jsonutil::escape(audio_path) + "\",\"cache\":\"" +
        jsonutil::escape(cache_path) + "\"}";
    // Analysis is CPU-heavy: run it on the prefetch channel so a search or a
    // user click on the main channel never waits behind it.
    send_prefetch_command("analyze " + payload);
    if (load_studio_analysis_file(cache_path, out))
    {
        out.id = id;
        return true;
    }
    return false;
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
    if (id.rfind("local:", 0) == 0)
    {
        // A local file path (or empty when missing). Never synthesise a
        // YouTube URL for a local id.
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

// Download the audio stream the native resolver already picked. This avoids
// the yt-dlp/deno signature dance entirely for the common case (and keeps
// working when the account is in the SABR experiment, where yt-dlp's web
// clients fail). ffmpeg only remuxes; no re-encode.
static std::string native_download_audio(const std::string &video_id,
                                         const std::string &dir)
{
    if (!native_backend_enabled() || !native::http_available())
        return "";
    const native::StreamResult stream = native::resolve_audio(video_id);
    if (!stream.ok || stream.url.empty())
        return "";
    std::string ext = "webm";
    if (stream.mime.find("mp4") != std::string::npos)
        ext = "m4a";
    else if (stream.mime.find("ogg") != std::string::npos)
        ext = "ogg";
    const std::string path = dir + "/" + video_id + "." + ext;
    const std::string tmp = path + ".part";
    const char *ffmpeg_env = getenv("P100_FFMPEG");
    const std::string ffmpeg = ffmpeg_env && *ffmpeg_env ? ffmpeg_env : "ffmpeg";
    const std::string cmd = shell_quote(ffmpeg) + " -v error -y -user_agent " +
                            shell_quote(native::web_user_agent()) + " -i " +
                            shell_quote(stream.url) + " -vn -c:a copy " +
                            shell_quote(tmp);
    std::string output;
    const int rc = run_capture(cmd, output);
    std::error_code ec;
    const auto size = std::filesystem::file_size(tmp, ec);
    if (rc != 0 || ec || size == 0)
    {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return "";
    }
    std::error_code rename_ec;
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec)
    {
        std::error_code rm_ec;
        std::filesystem::remove(tmp, rm_ec);
        return "";
    }
    Track t = fetch_track_metadata(video_id);
    if (!t.title.empty())
        save_metadata(t, path);
    return path;
}

std::string get_cached_audio_path(const std::string &video_id)
{
    if (video_id.empty())
        return "";
    // Local files are already on disk. Never hand a "local:" id to yt-dlp:
    // it would try https://www.youtube.com/watch?v=local:... and spam errors.
    if (is_local_media_id(video_id))
        return "";
    std::string dir = cache_dir() + "/audio_cache";
    if (!ensure_dir(dir))
        return "";

    std::string cached = find_audio_in(dir, video_id);
    if (!cached.empty())
        return cached;

    if (std::string native_path = native_download_audio(video_id, dir);
        !native_path.empty())
        return native_path;

    const std::string url = "https://www.youtube.com/watch?v=" + video_id;
    const std::string output = " -o \"" + dir + "/" + video_id + ".%(ext)s\" ";
    const std::string base = shell_quote(ytdlp_bin()) +
                             " -f bestaudio --no-progress --socket-timeout 15 --retries 2" +
                             js_runtime_arg();

    // Try authenticated first (private/age-restricted tracks), then public,
    // then the visionos client which currently needs no JS challenge or
    // cookies. All failures are logged: predownload used to fail silently.
    struct Attempt
    {
        const char *name;
        std::string command;
    };
    // Anonymous first: a signed-in account can be in YouTube's SABR-only
    // experiment, where cookie requests fail the JS challenge while the
    // anonymous one resolves (and downloads) normally.
    const Attempt attempts[] = {
        {"public", base + output + url},
        {"cookies", base + cookie_arg() + output + url},
        {"visionos", base + " --extractor-args \"youtube:player_client=visionos\"" + output + url},
    };

    for (const Attempt &attempt : attempts)
    {
        std::string output_text;
        const int rc = run_capture(attempt.command, output_text);
        std::string full_path = find_audio_in(dir, video_id);
        if (rc == 0 && !full_path.empty())
        {
            // Only write a sidecar when the fetch produced a title: an empty
            // one would hide the song from the Downloads list (which skips
            // empty-title entries) while find_audio_in still plays it.
            Track t = fetch_track_metadata(video_id);
            if (!t.title.empty())
                save_metadata(t, full_path);
            return full_path;
        }
        log_download_failure(video_id, attempt.name, rc, output_text, attempt.command);
    }
    return "";
}

// Resolve a direct streaming URL (yt-dlp -g) so playback can start
// almost immediately instead of waiting for a full file download.
static std::atomic<int> g_native_backend{1};

void set_native_backend_enabled(bool enabled)
{
    g_native_backend.store(enabled ? 1 : 0);
}

bool native_backend_enabled()
{
    // P100_BACKEND=python forces the old path; anything else uses the switch.
    const char *env = getenv("P100_BACKEND");
    if (env && *env)
        return std::string(env) != "python";
    return g_native_backend.load() != 0;
}

static bool native_handle_command(const std::string &cmd, std::string &out)
{
    if (!native_backend_enabled() || !native::http_available())
        return false;
    const std::size_t sp = cmd.find(' ');
    const std::string word = cmd.substr(0, sp);
    const std::string arg =
        sp == std::string::npos ? std::string() : cmd.substr(sp + 1);
    if (word == "meta" && arg.size() == 11)
    {
        std::string error;
        if (native::fetch_meta(arg, out, error))
            return true;
    }
    if (word == "search" && !arg.empty())
    {
        std::string error;
        if (native::search(arg, 20, out, error))
            return true;
    }
    if (word == "album_search" && !arg.empty())
    {
        std::string error;
        if (native::search_albums(arg, 20, out, error))
            return true;
    }
    if (word == "artist_search" && !arg.empty())
    {
        std::string error;
        if (native::search_artists(arg, 20, out, error))
            return true;
    }
    if (word == "suggest")
    {
        std::string error;
        if (native::suggest(arg, out, error))
            return true;
    }
    if (word == "album" && !arg.empty())
    {
        std::string error;
        if (native::album(arg, out, error))
            return true;
    }
    if (word == "album_of" && arg.size() == 11)
    {
        std::string error;
        if (native::album_of(arg, out, error))
            return true;
    }
    if (word == "artist" && !arg.empty())
    {
        std::string error;
        if (native::artist(arg, out, error))
            return true;
    }
    if (word == "artist_of" && arg.size() == 11)
    {
        std::string error;
        if (native::artist_of(arg, out, error))
            return true;
    }
    if (word == "artist_songs")
    {
        std::string id, off, cnt;
        std::size_t a = arg.find(' ');
        std::size_t b = a == std::string::npos ? std::string::npos : arg.find(' ', a + 1);
        if (a != std::string::npos && b != std::string::npos)
        {
            id = arg.substr(0, a);
            off = arg.substr(a + 1, b - a - 1);
            cnt = arg.substr(b + 1);
            std::string error;
            if (!id.empty() &&
                native::artist_songs(id, std::atoi(off.c_str()), std::atoi(cnt.c_str()), out,
                                     error))
                return true;
        }
    }
    if (word == "playlist" && !arg.empty())
    {
        std::string error;
        if (native::playlist(arg, out, error))
            return true;
    }
    if (word == "radio" && arg.size() == 11)
    {
        std::string error;
        if (native::radio(arg, 50, out, error))
            return true;
    }
    if (word == "recommendations")
    {
        std::string error;
        const int limit = arg.empty() ? 40 : std::atoi(arg.c_str());
        if (native::home(limit, out, error))
            return true;
    }
    if (word == "home_more" && !arg.empty())
    {
        std::string error;
        if (native::home_more(arg, 40, out, error))
            return true;
        // Python has no home_more command. Treat an exhausted/broken token as
        // a definitive "no more" instead of letting it fall through and log an
        // unknown-command error on every scroll.
        out = "{\"tracks\":[],\"continuation\":\"\"}";
        return true;
    }
    if (word == "search_all" && !arg.empty())
    {
        std::string error;
        if (native::search_all(arg, 20, out, error))
            return true;
    }
    if (word == "library_playlists")
    {
        std::string error;
        if (native::library_playlists(out, error))
            return true;
    }
    if (word == "podcasts_library")
    {
        std::string error;
        if (native::podcasts_library(out, error))
            return true;
    }
    if (word == "podcasts_search" && !arg.empty())
    {
        std::string error;
        if (native::podcasts_search(arg, out, error))
            return true;
    }
    if (word == "podcast" && !arg.empty())
    {
        std::string error;
        if (native::podcast(arg, out, error))
            return true;
    }
    if (word == "episodes_later")
    {
        std::string error;
        if (native::episodes_later(out, error))
            return true;
    }
    if (word == "prefetch")
    {
        std::vector<std::string> ids;
        const std::string list = arg;
        std::size_t pos = 0;
        while (pos < list.size())
        {
            std::size_t end = list.find(' ', pos);
            if (end == std::string::npos)
                end = list.size();
            if (end > pos)
                ids.push_back(list.substr(pos, end - pos));
            pos = end + 1;
        }
        std::string error;
        if (!ids.empty() && native::prefetch_urls(ids, out, error))
            return true;
    }
    if (word == "musicvideo" && arg.size() == 11)
    {
        std::string error;
        if (native::musicvideo(arg, out, error))
            return true;
    }
    if (word == "lyrics" && arg.size() == 11)
    {
        std::string error;
        if (native::lyrics(arg, out, error))
            return true;
    }
    if (word == "rate_song")
    {
        std::string error;
        if (native::rate_song(arg, out, error))
            return true;
    }
    if (word == "create_playlist")
    {
        std::string error;
        if (native::create_playlist(arg, out, error))
            return true;
    }
    if (word == "delete_playlist")
    {
        std::string error;
        if (native::delete_playlist(arg, out, error))
            return true;
    }
    if (word == "edit_playlist")
    {
        std::string error;
        if (native::edit_playlist(arg, out, error))
            return true;
    }
    if (word == "add_playlist_items")
    {
        std::string error;
        if (native::add_playlist_items(arg, out, error))
            return true;
    }
    if (word == "remove_playlist_items")
    {
        std::string error;
        if (native::remove_playlist_items(arg, out, error))
            return true;
    }
    if (word == "play_video" || word == "play_video_fast" ||
        word == "play_video_fresh")
    {
        std::string id = arg;
        int height = 720;
        const std::size_t sp2 = arg.find(' ');
        if (sp2 != std::string::npos)
        {
            id = arg.substr(0, sp2);
            height = std::atoi(arg.c_str() + sp2 + 1);
        }
        std::string error;
        if (id.size() == 11 &&
            native::resolve_video(id, height, word == "play_video_fresh", out, error))
            return true;
    }
    return false;
}

static std::string try_native_stream(const std::string &video_id)
{
    if (!native_backend_enabled() || !native::http_available())
        return "";
    native::StreamResult r = native::resolve_audio(video_id);
    return r.ok ? r.url : "";
}

std::string fetch_stream_url(const std::string &video_id)
{
    if (is_local_media_id(video_id))
        return ""; // a file, not a stream
    if (std::string native_url = try_native_stream(video_id); !native_url.empty())
        return native_url;
    std::string response = send_play_command("play " + video_id);
    if (response.find("\"error\"") != std::string::npos)
        return "";
    // Guard against a stale reply from the previous command (see the timeout
    // resync in send_command_on): never hand another track's URL to mpv.
    const std::string id = json_get_string(response, "id");
    if (!id.empty() && id != video_id)
        return "";
    return json_get_string(response, "url");
}

// Speculative prefetch variant: goes to the dedicated prefetch backend
// channel, leaving searches/metadata and the foreground play channel free.
// The channel processes commands serially, which also self-throttles the
// prefetch rate.
std::string fetch_stream_url_prefetch(const std::string &video_id)
{
    if (is_local_media_id(video_id))
        return "";
    std::string response = send_prefetch_command("play " + video_id);
    if (response.find("\"error\"") != std::string::npos)
        return "";
    const std::string id = json_get_string(response, "id");
    if (!id.empty() && id != video_id)
        return "";
    return json_get_string(response, "url");
}

std::string fetch_stream_url_fresh(const std::string &video_id)
{
    if (is_local_media_id(video_id))
        return "";
    if (std::string native_url = try_native_stream(video_id); !native_url.empty())
        return native_url;
    std::string response = send_play_command("play_fresh " + video_id);
    if (response.find("\"error\"") != std::string::npos)
        return "";
    const std::string id = json_get_string(response, "id");
    if (!id.empty() && id != video_id)
        return "";
    return json_get_string(response, "url");
}

VideoStreamUrls fetch_video_stream_urls(const std::string &video_id, bool fresh, int max_height,
                                        bool fast)
{
    VideoStreamUrls out;
    if (video_id.empty() || is_local_media_id(video_id))
        return out;
    if (max_height <= 0)
        max_height = 720;
    std::string cmd;
    if (fast && !fresh)
        cmd = "play_video_fast ";
    else
        cmd = fresh ? "play_video_fresh " : "play_video ";
    std::string response =
        send_play_command(cmd + video_id + " " + std::to_string(max_height));
    if (response.empty())
    {
        out.error = "backend unavailable";
        return out;
    }
    out.video = json_get_string(response, "video");
    out.audio = json_get_string(response, "audio");
    out.error = json_get_string(response, "error");
    return out;
}

VideoSyncResult fetch_video_sync_cached(const std::string &song_id, const std::string &video_id)
{
    VideoSyncResult out;
    if (song_id.empty() || video_id.empty() || is_local_media_id(song_id))
        return out;
    // Play channel: already booted for the match/resolve, so the cache hit
    // does not wait for a cold prefetch process on the critical path.
    const std::string response =
        send_play_command("videosync_cached " + song_id + " " + video_id);
    if (response.empty())
    {
        out.error = "backend unavailable";
        return out;
    }
    if (response.find("\"error\"") != std::string::npos)
    {
        out.error = jsonutil::get_string(response, "error");
        return out;
    }
    // `ok` is the command status (true even when the pair is not cached yet).
    out.ok = jsonutil::get_bool(response, "ok");
    out.offset = jsonutil::get_double(response, "offset");
    out.confidence = jsonutil::get_double(response, "confidence");
    out.confident = jsonutil::get_bool(response, "confident");
    return out;
}

VideoSyncResult fetch_video_sync(const std::string &song_id, const std::string &video_id,
                                 int song_duration_sec)
{
    VideoSyncResult out;
    if (song_id.empty() || video_id.empty() || is_local_media_id(song_id))
        return out;
    const std::string cmd = "videosync " + song_id + " " + video_id + " " +
                            std::to_string(std::max(0, song_duration_sec));
    const std::string response = send_prefetch_command(cmd);
    if (response.empty())
    {
        out.error = "backend unavailable";
        return out;
    }
    if (response.find("\"error\"") != std::string::npos)
    {
        out.error = jsonutil::get_string(response, "error");
        return out;
    }
    out.ok = true;
    out.offset = jsonutil::get_double(response, "offset");
    out.confidence = jsonutil::get_double(response, "confidence");
    out.confident = jsonutil::get_bool(response, "confident");
    return out;
}

MusicVideoMatch fetch_music_video_prefetch(const std::string &video_id)
{
    MusicVideoMatch out;
    if (video_id.empty() || is_local_media_id(video_id))
        return out;
    std::string response = send_prefetch_command("musicvideo " + video_id);
    if (response.empty())
    {
        out.error = "backend unavailable";
        return out;
    }
    out.error = json_get_string(response, "error");
    if (!out.error.empty() || response.find("\"error\"") != std::string::npos)
        return out;
    out.ok = true;
    out.id = json_get_string(response, "videoId");
    out.title = json_get_string(response, "title");
    out.artist = json_get_string(response, "artist");
    out.duration_sec = json_get_int(response, "durationSec");
    out.matched = jsonutil::get_bool(response, "matched");
    out.self_video = jsonutil::get_bool(response, "self");
    return out;
}

MusicVideoMatch fetch_music_video(const std::string &video_id)
{
    MusicVideoMatch out;
    if (video_id.empty() || is_local_media_id(video_id))
        return out;
    std::string response = send_play_command("musicvideo " + video_id);
    if (response.empty())
    {
        out.error = "backend unavailable";
        return out;
    }
    out.error = json_get_string(response, "error");
    if (!out.error.empty() || response.find("\"error\"") != std::string::npos)
        return out;
    out.ok = true;
    out.id = json_get_string(response, "videoId");
    out.title = json_get_string(response, "title");
    out.artist = json_get_string(response, "artist");
    out.duration_sec = json_get_int(response, "durationSec");
    out.matched = jsonutil::get_bool(response, "matched");
    out.self_video = jsonutil::get_bool(response, "self");
    return out;
}

std::unordered_map<std::string, std::string> fetch_stream_urls_prefetch(
    const std::vector<std::string> &video_ids, bool background)
{
    std::unordered_map<std::string, std::string> urls;
    std::string cmd = "prefetch";
    for (const std::string &id : video_ids)
    {
        if (id.empty() || is_local_media_id(id))
            continue;
        cmd += " " + id;
    }
    if (cmd == "prefetch")
        return urls;
    // Interactive warmups (hover/hero) use the dedicated prefetch worker;
    // page/queue warmups go to the background meta worker so they never sit
    // in front of a click. A background batch waits for its worker rather than
    // ever landing on main.
    std::string response =
        background ? send_background_command(cmd) : send_prefetch_command(cmd);
    if (response.empty() || response.find("\"error\"") != std::string::npos)
        return urls;
    for_each_json_array_object(response, [&](const std::string &obj) {
        const std::string id = json_get_string(obj, "id");
        const std::string url = json_get_string(obj, "url");
        if (!id.empty() && !url.empty())
            urls[id] = url;
    });
    return urls;
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

        // Old sidecars written while the yt-dlp capture was truncated have no
        // thumbnail at all. The video id determines YouTube's standard grid
        // thumbnail, so every YouTube-backed download gets a cover without a
        // metadata lookup (and the cover sweep can cache it).
        if (t.thumbnail_url.empty() && !t.id.empty() && !is_local_media_id(t.id))
            t.thumbnail_url = "https://img.youtube.com/vi/" + t.id + "/hqdefault.jpg";

        if (!t.title.empty())
            tracks.push_back(t);
    }

    return tracks;
}

// ---------------------------------------------------------------------------
// Offline video cache (Downloads tab)
// ---------------------------------------------------------------------------
static std::string video_cache_dir()
{
    std::string dir = cache_dir() + "/video_cache";
    if (!ensure_dir(dir))
        return "";
    return dir;
}

std::string find_cached_video_path(const std::string &video_id)
{
    if (video_id.empty() || is_local_media_id(video_id))
        return "";
    std::string dir = video_cache_dir();
    if (dir.empty())
        return "";
    for (const char *ext : {"mp4", "mkv", "webm"})
    {
        const std::string path = dir + "/" + video_id + "." + ext;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
            return path;
    }
    return "";
}

std::string get_cached_video_path(const std::string &video_id, int max_height)
{
    if (video_id.empty() || is_local_media_id(video_id))
        return "";
    std::string cached = find_cached_video_path(video_id);
    if (!cached.empty())
        return cached;
    std::string dir = video_cache_dir();
    if (dir.empty())
        return "";

    const int h = std::clamp(max_height > 0 ? max_height : 720, 240, 2160);
    const std::string hs = std::to_string(h);
    const std::string format =
        "bv*[vcodec^=avc1][height<=" + hs + "]+ba/"
        "bv*[height<=" + hs + "]+ba/b[height<=" + hs + "]";
    const std::string url = "https://www.youtube.com/watch?v=" + video_id;
    const std::string output = " -o \"" + dir + "/" + video_id + ".%(ext)s\" ";
    const std::string base = shell_quote(ytdlp_bin()) + " -f \"" + format +
                             "\" --merge-output-format mp4 --no-progress "
                             "--socket-timeout 15 --retries 2" + js_runtime_arg();

    struct Attempt
    {
        const char *name;
        std::string command;
    };
    // Anonymous first: a signed-in account can be in YouTube's SABR-only
    // experiment, where cookie requests fail the JS challenge while the
    // anonymous one resolves (and downloads) normally.
    const Attempt attempts[] = {
        {"public", base + output + url},
        {"cookies", base + cookie_arg() + output + url},
        {"visionos", base + " --extractor-args \"youtube:player_client=visionos\"" + output + url},
    };
    for (const Attempt &attempt : attempts)
    {
        std::string output_text;
        const int rc = run_capture(attempt.command, output_text);
        std::string full_path = find_cached_video_path(video_id);
        if (rc == 0 && !full_path.empty())
        {
            Track t = fetch_track_metadata(video_id);
            if (!t.title.empty())
                save_metadata(t, full_path);
            return full_path;
        }
        log_download_failure(video_id, attempt.name, rc, output_text, attempt.command);
    }
    return "";
}

std::vector<Track> get_cached_videos()
{
    std::vector<Track> tracks;
    std::string dir = video_cache_dir();
    if (dir.empty())
        return tracks;

    for (const std::string &name : list_dir_names(dir))
    {
        if (name == "." || name == "..")
            continue;
        size_t dot = name.find_last_of('.');
        if (dot == std::string::npos)
            continue;
        if (name.size() > 10 && name.compare(name.size() - 10, 10, ".meta.json") == 0)
            continue;
        if (name.find('.') != dot)
            continue; // fragments / parts

        const std::string ext = name.substr(dot + 1);
        if (!(ext == "mp4" || ext == "mkv" || ext == "webm"))
            continue;
        const std::string full_path = dir + "/" + name;
        struct stat st;
        if (stat(full_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        Track t;
        t.is_video = true;
        t.url = full_path;
        t.id = name.substr(0, dot);
        const std::string meta_path = full_path + ".meta.json";
        if (stat(meta_path.c_str(), &st) == 0)
        {
            std::ifstream in(meta_path);
            if (in.good())
            {
                std::string json((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                const std::string stem = name.substr(0, dot);
                t.id = json_get_string(json, "id");
                if (t.id.empty())
                    t.id = stem;
                t.title = json_get_string(json, "title");
                if (t.title.empty())
                    t.title = stem;
                t.artist = json_get_string(json, "artist");
                if (t.artist.empty())
                    t.artist = "Unknown";
                t.album = json_get_string(json, "album");
                t.album_id = json_get_string(json, "album_id");
                t.artist_id = json_get_string(json, "artist_id");
                t.thumbnail_url = json_get_string(json, "thumbnail_url");
                t.duration_sec = json_get_int(json, "duration_sec");
            }
        }
        else
        {
            t.title = name.substr(0, dot);
            t.artist = "Unknown";
        }
        if (t.thumbnail_url.empty() && !t.id.empty() && !is_local_media_id(t.id))
            t.thumbnail_url = "https://img.youtube.com/vi/" + t.id + "/hqdefault.jpg";
        if (!t.title.empty())
            tracks.push_back(t);
    }
    return tracks;
}

void delete_cached_video(const std::string &video_id)
{
    const std::string path = find_cached_video_path(video_id);
    if (path.empty())
        return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".meta.json", ec);
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
    std::lock_guard<std::mutex> lock(g_backend_error_mutex);
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

// The recommendation feed and the account playlists belong to the signed-in
// user. After a sign-in (or an account switch) the old files must not be served
// again, or Home keeps showing an anonymous/generic feed for up to an hour and
// the playlist pane shows the previous account's lists.
void clear_recommendations_cache()
{
    std::error_code ec;
    std::filesystem::remove(cache_dir() + "/recommendations_cache.json", ec);
}

void clear_youtube_playlist_caches()
{
    std::error_code ec;
    std::filesystem::remove(youtube_playlists_cache_path(), ec);
    const std::string dir = cache_dir();
    for (const std::string &name : list_dir_names(dir))
    {
        if (name.rfind("youtube_playlist_", 0) != 0)
            continue;
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0)
            continue;
        std::filesystem::remove(dir + "/" + name, ec);
    }
}

// True when `path` was written before the auth file was last replaced. A cache
// in that state belongs to a previous (or anonymous) session and must not be
// shown as if it were the current account's data.
bool cache_older_than_auth(const std::string &path)
{
    std::error_code cache_ec, auth_ec;
    const auto cache_time = std::filesystem::last_write_time(path, cache_ec);
    if (cache_ec)
        return false;
    const auto auth_time =
        std::filesystem::last_write_time(backend_auth_file_path(), auth_ec);
    return !auth_ec && auth_time > cache_time;
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
