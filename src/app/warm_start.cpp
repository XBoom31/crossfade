#include "warm_start.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef P100_WINDOWS
// Windows keeps the previous behaviour (multiple instances) for now.
namespace warm_start
{
bool send(Command) { return false; }
bool start_server(std::function<void(Command)>, std::string &error)
{
    error = "warm start is not supported on Windows yet";
    return false;
}
void stop_server() {}
bool server_active() { return false; }
std::string socket_path() { return {}; }
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
