#include "warm_start.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

#ifdef P100_WINDOWS
// Windows warm start uses a per-user, per-session named pipe. The running
// instance owns the pipe; later launches write "show"/"toggle"/"quit" and
// exit, so reopening from the Start Menu is instant. (A real Windows service
// cannot play audio or show UI, so "background at sign-in" is a per-user
// hidden launch triggered by the installer's Run entry.)
#include <windows.h>

#include <cctype>

namespace warm_start
{
namespace
{
std::atomic<bool> g_stopping{false};
HANDLE g_thread = nullptr;
std::atomic<HANDLE> g_pipe{INVALID_HANDLE_VALUE};

const char *command_name(Command cmd)
{
    switch (cmd)
    {
    case Command::Show: return "show";
    case Command::Hide: return "hide";
    case Command::Toggle: return "toggle";
    case Command::Quit: return "quit";
    }
    return "show";
}

bool parse_command(const std::string &line, Command &out)
{
    if (line == "show")
        out = Command::Show;
    else if (line == "hide")
        out = Command::Hide;
    else if (line == "toggle")
        out = Command::Toggle;
    else if (line == "quit")
        out = Command::Quit;
    else
        return false;
    return true;
}

std::string make_pipe_name()
{
    char user[256] = {};
    DWORD user_len = sizeof(user);
    if (!GetUserNameA(user, &user_len) || user_len == 0)
        std::snprintf(user, sizeof(user), "user");
    for (char *p = user; *p; ++p)
    {
        if (!std::isalnum((unsigned char)*p) && *p != '-' && *p != '_')
            *p = '_';
    }
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return "\\\\.\\pipe\\crossfade-warm-" + std::string(user) + "-" +
           std::to_string((unsigned long)session);
}

HANDLE open_client(DWORD wait_ms)
{
    const std::string name = make_pipe_name();
    if (wait_ms > 0 && !WaitNamedPipeA(name.c_str(), wait_ms))
        return INVALID_HANDLE_VALUE;
    return CreateFileA(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}

void server_loop(HANDLE pipe, std::function<void(Command)> on_command)
{
    while (!g_stopping.load(std::memory_order_relaxed))
    {
        const BOOL connected = ConnectNamedPipe(pipe, nullptr);
        const DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
        if (!connected && connect_error != ERROR_PIPE_CONNECTED)
        {
            if (g_stopping.load(std::memory_order_relaxed))
                break;
            if (connect_error == ERROR_INVALID_HANDLE)
                break;
            Sleep(20);
            continue;
        }

        char buf[64] = {};
        DWORD read = 0;
        const BOOL ok = ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr);
        DisconnectNamedPipe(pipe);
        if (!ok || read == 0)
            continue;

        std::string line(buf, read);
        while (!line.empty() &&
               (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        Command cmd;
        if (parse_command(line, cmd) && on_command)
            on_command(cmd);
    }

    g_pipe.store(INVALID_HANDLE_VALUE, std::memory_order_release);
    CloseHandle(pipe);
}

struct ServerContext
{
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::function<void(Command)> callback;
};

DWORD WINAPI server_entry(LPVOID param)
{
    auto *context = static_cast<ServerContext *>(param);
    server_loop(context->pipe, std::move(context->callback));
    delete context;
    return 0;
}
} // namespace

std::string socket_path() { return make_pipe_name(); }

bool send(Command cmd)
{
    HANDLE pipe = open_client(0);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        // A connected client may be holding the single instance; retry
        // briefly rather than failing a fast repeat launch.
        pipe = open_client(400);
        if (pipe == INVALID_HANDLE_VALUE)
            return false;
    }

    const std::string line = std::string(command_name(cmd)) + "\n";
    DWORD written = 0;
    const BOOL ok = WriteFile(pipe, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(pipe);
    return ok && written == line.size();
}

bool start_server(std::function<void(Command)> on_command, std::string &error)
{
    if (g_pipe.load(std::memory_order_acquire) != INVALID_HANDLE_VALUE)
        return true;

    const std::string name = make_pipe_name();
    HANDLE pipe = CreateNamedPipeA(
        name.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        error = (err == ERROR_ACCESS_DENIED)
                    ? "another crossfade instance is already running"
                    : "could not create the warm-start pipe (" +
                          std::to_string((unsigned long)err) + ")";
        return false;
    }

    g_stopping.store(false, std::memory_order_relaxed);
    g_pipe.store(pipe, std::memory_order_release);

    auto *context = new ServerContext{pipe, std::move(on_command)};
    g_thread = CreateThread(nullptr, 0, server_entry, context, 0, nullptr);
    if (!g_thread)
    {
        delete context;
        g_pipe.store(INVALID_HANDLE_VALUE, std::memory_order_release);
        CloseHandle(pipe);
        error = "could not start the warm-start thread";
        return false;
    }
    return true;
}

void stop_server()
{
    if (g_pipe.load(std::memory_order_acquire) == INVALID_HANDLE_VALUE && !g_thread)
        return;

    g_stopping.store(true, std::memory_order_relaxed);

    // Unblock ConnectNamedPipe by connecting ourselves. If another client is
    // holding the pipe, cancel the pending I/O instead so shutdown cannot
    // hang on the join.
    if (g_pipe.load(std::memory_order_acquire) != INVALID_HANDLE_VALUE)
    {
        HANDLE wake = open_client(0);
        if (wake != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(wake, "stop\n", 5, &written, nullptr);
            CloseHandle(wake);
        }
        else if (g_thread)
        {
            CancelSynchronousIo(g_thread);
        }
    }

    if (g_thread)
    {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    g_pipe.store(INVALID_HANDLE_VALUE, std::memory_order_release);
}

bool server_active()
{
    return g_pipe.load(std::memory_order_acquire) != INVALID_HANDLE_VALUE;
}
} // namespace warm_start
#else

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace warm_start
{
namespace
{
std::atomic<bool> g_stopping{false};
std::thread g_accept_thread;
int g_listen_fd = -1;
std::string g_path;

const char *command_name(Command cmd)
{
    switch (cmd)
    {
    case Command::Show: return "show";
    case Command::Hide: return "hide";
    case Command::Toggle: return "toggle";
    case Command::Quit: return "quit";
    }
    return "show";
}

bool parse_command(const std::string &line, Command &out)
{
    if (line == "show")
        out = Command::Show;
    else if (line == "hide")
        out = Command::Hide;
    else if (line == "toggle")
        out = Command::Toggle;
    else if (line == "quit")
        out = Command::Quit;
    else
        return false;
    return true;
}

std::string make_socket_path()
{
    if (const char *rt = getenv("XDG_RUNTIME_DIR"); rt && *rt)
        return std::string(rt) + "/crossfade.sock";
    return "/tmp/crossfade-" + std::to_string((int)getuid()) + ".sock";
}

bool fill_addr(sockaddr_un &addr, const std::string &path)
{
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path))
        return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}
} // namespace

std::string socket_path() { return make_socket_path(); }

bool send(Command cmd)
{
    const std::string path = make_socket_path();
    sockaddr_un addr;
    if (!fill_addr(addr, path))
        return false;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return false;
    }
    std::string line = std::string(command_name(cmd)) + "\n";
    const ssize_t written = write(fd, line.data(), line.size());
    close(fd);
    return written == (ssize_t)line.size();
}

bool start_server(std::function<void(Command)> on_command, std::string &error)
{
    if (g_listen_fd >= 0)
        return true;

    g_path = make_socket_path();
    sockaddr_un addr;
    if (!fill_addr(addr, g_path))
    {
        error = "socket path is too long";
        return false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        error = std::strerror(errno);
        return false;
    }

    // A leftover socket from a crashed instance would make bind() fail; the
    // caller only reaches here after a failed connect(), so it is stale.
    unlink(g_path.c_str());
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
    {
        error = std::strerror(errno);
        close(fd);
        return false;
    }
    chmod(g_path.c_str(), 0600);
    if (listen(fd, 4) != 0)
    {
        error = std::strerror(errno);
        close(fd);
        unlink(g_path.c_str());
        return false;
    }

    g_listen_fd = fd;
    g_stopping.store(false, std::memory_order_relaxed);
    const int listen_fd = fd;
    g_accept_thread = std::thread([listen_fd, on_command = std::move(on_command)]() {
        while (!g_stopping.load(std::memory_order_relaxed))
        {
            const int client = accept(listen_fd, nullptr, nullptr);
            if (client < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            // A client that connects but never sends must not wedge the
            // accept loop.
            struct timeval tv{};
            tv.tv_sec = 2;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[64] = {0};
            const ssize_t n = read(client, buf, sizeof(buf) - 1);
            close(client);
            if (n <= 0)
                continue;
            std::string line(buf, (size_t)n);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            Command cmd;
            if (parse_command(line, cmd) && on_command)
                on_command(cmd);
        }
    });
    return true;
}

void stop_server()
{
    if (g_listen_fd < 0)
        return;
    g_stopping.store(true, std::memory_order_relaxed);
    shutdown(g_listen_fd, SHUT_RDWR);
    close(g_listen_fd);
    g_listen_fd = -1;
    if (g_accept_thread.joinable())
        g_accept_thread.join();
    if (!g_path.empty())
        unlink(g_path.c_str());
    g_path.clear();
}

bool server_active() { return g_listen_fd >= 0; }
} // namespace warm_start
#endif
