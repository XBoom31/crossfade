#include "data_transfer.h"

#include "json_util.h"
#include "library.h"
#include "paths.h"
#include "tasks.h"
#include "ui_notify.h"
#include "version.h"
#include "ytdlp.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifdef P100_WINDOWS
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace data_transfer
{
namespace
{
std::mutex g_mu;
std::string g_export_result;
std::atomic<bool> g_export_running{false};
std::string g_ready_path;

std::string join_sections(const Sections &s)
{
    std::string out;
    auto add = [&](bool on, const char *name) {
        if (!on)
            return;
        if (!out.empty())
            out += ',';
        out += name;
    };
    add(s.settings, "settings");
    add(s.auth, "auth");
    add(s.library, "library");
    add(s.album_cache, "album_cache");
    add(s.artist_cache, "artist_cache");
    add(s.lyrics, "lyrics");
    add(s.thumbs, "thumbs");
    add(s.audio, "audio");
    add(s.logs, "logs");
    return out;
}

std::string python_executable()
{
    std::string py = backend_python_path();
    if (!py.empty())
        return py;
#ifdef P100_WINDOWS
    return "python";
#else
    return "python3";
#endif
}

std::string ready_file_path() { return p100_config_path(".transfer-ready"); }
std::string result_file_path() { return p100_config_path("transfer-result.json"); }

#ifdef P100_WINDOWS
int run_sync(const std::vector<std::string> &args, std::string &output)
{
    output.clear();
    std::vector<const char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);
    return _spawnv(_P_WAIT, argv[0], argv.data());
}

bool spawn_detached(const std::vector<std::string> &args)
{
    std::vector<const char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);
    return _spawnv(_P_DETACH, argv[0], argv.data()) != -1;
}
#else
int run_sync(const std::vector<std::string> &args, std::string &output)
{
    output.clear();
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    const pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0)
    {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        output.append(buf, (size_t)n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool spawn_detached(const std::vector<std::string> &args)
{
    const pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0)
    {
        setsid();
        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (const auto &a : args)
            argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return true;
}
#endif

void set_export_result(const std::string &text)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_export_result = text;
}
} // namespace

std::string helper_script_path()
{
#ifdef P100_SOURCE_DIR
    const std::string from_source = std::string(P100_SOURCE_DIR) + "/py/cf_backend/transfer.py";
    if (std::filesystem::is_regular_file(from_source))
        return from_source;
#endif
    const std::string installed = p100_share_dir() + "/py/cf_backend/transfer.py";
    if (std::filesystem::is_regular_file(installed))
        return installed;
    return installed;
}

std::string default_export_path()
{
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    return p100_config_path(std::string("crossfade-backup-") + stamp + ".zip");
}

std::string default_import_path()
{
    std::error_code ec;
    std::string newest;
    std::filesystem::file_time_type newest_time{};
    for (std::filesystem::directory_iterator it(p100_config_dir(), ec), end;
         !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        if (name.rfind("crossfade-backup-", 0) != 0 || it->path().extension() != ".zip")
            continue;
        const auto t = it->last_write_time(ec);
        if (!ec && (newest.empty() || t > newest_time))
        {
            newest = it->path().string();
            newest_time = t;
        }
    }
    return newest;
}

bool export_async(const std::string &zip_path, const Sections &sections, std::string &error)
{
    error.clear();
    if (zip_path.empty())
    {
        error = "Choose a destination ZIP path.";
        return false;
    }
    if (join_sections(sections).empty())
    {
        error = "Select at least one data section.";
        return false;
    }
    if (g_export_running.exchange(true))
    {
        error = "An export is already running.";
        return false;
    }
    set_export_result("");

    tasks::spawn("data-export", [zip_path, sections]() {
        const std::string snapshot = p100_config_path(".export-library.db");
        std::string snapshot_error;
        bool have_snapshot = false;
        if (library::available())
            have_snapshot = library::backup_to(snapshot, snapshot_error);

        std::vector<std::string> args = {
            python_executable(),
            helper_script_path(),
            "export",
            "--zip", zip_path,
            "--config", p100_config_dir(),
            "--sections", join_sections(sections),
            "--version",
#ifdef CROSSFADE_VERSION
            CROSSFADE_VERSION,
#else
            "unknown",
#endif
        };
        if (have_snapshot)
        {
            args.push_back("--library");
            args.push_back(snapshot);
        }

        std::string output;
        const int rc = run_sync(args, output);
        if (have_snapshot)
        {
            std::error_code ec;
            std::filesystem::remove(snapshot, ec);
        }

        if (rc == 0)
        {
            set_export_result("Backup saved to " + zip_path);
            ui_notify(UiNotifyType::Success, "Backup complete", zip_path, 7.0f);
        }
        else
        {
            std::string msg = "Export failed.";
            if (!output.empty())
                msg += " " + output.substr(0, 400);
            set_export_result(msg);
            ui_notify(UiNotifyType::Error, "Backup failed", msg, 9.0f);
        }
        g_export_running.store(false);
    });
    return true;
}

bool export_running() { return g_export_running.load(); }

std::string take_export_result()
{
    std::lock_guard<std::mutex> lock(g_mu);
    std::string out = std::move(g_export_result);
    g_export_result.clear();
    return out;
}

bool start_import(const std::string &zip_path, std::string &error)
{
    error.clear();
    if (zip_path.empty() || !std::filesystem::is_regular_file(zip_path))
    {
        error = "Choose an existing ZIP backup.";
        return false;
    }
    const std::string ready = ready_file_path();
    const std::string result = result_file_path();
    std::error_code ec;
    std::filesystem::remove(ready, ec);
    std::filesystem::remove(result, ec);

    std::vector<std::string> args = {
        python_executable(),
        helper_script_path(),
        "import",
        "--zip", zip_path,
        "--config", p100_config_dir(),
        "--wait-ready", ready,
        "--result", result,
    };
    if (!spawn_detached(args))
    {
        error = "Could not start the import helper.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_ready_path = ready;
    }
    return true;
}

void mark_shutdown_ready()
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_ready_path.empty())
        return;
    std::ofstream out(g_ready_path, std::ios::binary);
    if (out)
        out << "ready";
    g_ready_path.clear();
}

std::string take_result_message()
{
    const std::string path = result_file_path();
    if (!std::filesystem::is_regular_file(path))
        return {};
    const std::string json = jsonutil::read_file(path);
    std::error_code ec;
    std::filesystem::remove(path, ec);
    if (json.empty())
        return {};
    const bool ok = jsonutil::get_bool(json, "ok", false);
    std::string message = jsonutil::get_string(json, "message", "");
    const std::string backup = jsonutil::get_string(json, "backup", "");
    if (message.empty())
        message = ok ? "Data imported." : "Import failed.";
    if (ok && !backup.empty())
        message += " Previous data: " + backup;
    return message;
}
} // namespace data_transfer
