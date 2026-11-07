#include "discord_rpc.h"

#include "json_util.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <condition_variable>
#include <thread>

// Discord IPC frames: 4-byte little-endian opcode, 4-byte little-endian
// payload length, then the JSON payload. Op 0 = handshake, op 1 = frame
// (SET_ACTIVITY), op 2 = close.
namespace
{
constexpr int OP_HANDSHAKE = 0;
constexpr int OP_FRAME = 1;

std::mutex g_mutex;
std::condition_variable g_cv;
DiscordPresence g_presence;
bool g_presence_dirty = false;
std::atomic<bool> g_running{false};
std::string g_client_id;

std::string json_escape(const std::string &s)
{
    return jsonutil::escape(s);
}

bool read_full(int fd, void *buf, size_t n)
{
    auto *p = (uint8_t *)buf;
    while (n > 0)
    {
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

bool write_full(int fd, const void *buf, size_t n)
{
    auto *p = (const uint8_t *)buf;
    while (n > 0)
    {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

bool send_frame(int fd, int op, const std::string &payload)
{
    uint8_t header[8];
    uint32_t op_le = (uint32_t)op;
    uint32_t len_le = (uint32_t)payload.size();
    memcpy(header, &op_le, 4);
    memcpy(header + 4, &len_le, 4);
    if (!write_full(fd, header, 8))
        return false;
    if (!payload.empty() && !write_full(fd, payload.data(), payload.size()))
        return false;
    return true;
}

// Consume one frame (used for the READY reply); false on EOF.
bool skip_frame(int fd)
{
    uint8_t header[8];
    if (!read_full(fd, header, 8))
        return false;
    uint32_t len;
    memcpy(&len, header + 4, 4);
    while (len > 0)
    {
        uint8_t scratch[512];
        size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
        if (!read_full(fd, scratch, chunk))
            return false;
        len -= (uint32_t)chunk;
    }
    return true;
}

int connect_discord_socket()
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    std::string base;
    if (runtime && *runtime)
        base = runtime;
    else
    {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "/run/user/%d", (int)getuid());
        base = tmp;
    }
    for (int i = 0; i < 10; ++i)
    {
        std::string path = base + "/discord-ipc-" + std::to_string(i);
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path))
        {
            close(fd);
            return -1;
        }
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, (const sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;
        close(fd);
    }
    return -1;
}

std::string build_activity_payload(const DiscordPresence &p, long nonce)
{
    char nonce_buf[32];
    snprintf(nonce_buf, sizeof(nonce_buf), "p100-%ld", nonce);
    std::string payload = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
                          std::to_string((int)getpid()) + ",\"activity\":";
    if (!p.active || p.title.empty())
    {
        payload += "null";
    }
    else
    {
        payload += "{\"details\":\"" + json_escape(p.title) + "\"";
        if (!p.artist.empty())
            payload += ",\"state\":\"by " + json_escape(p.artist) + "\"";
        if (p.start_ts > 0 && p.end_ts > p.start_ts)
            payload += ",\"timestamps\":{\"start\":" + std::to_string(p.start_ts) +
                       ",\"end\":" + std::to_string(p.end_ts) + "}";
        if (!p.image_url.empty())
        {
            payload += ",\"assets\":{\"large_image\":\"" + json_escape(p.image_url) + "\"";
            if (!p.album.empty())
                payload += ",\"large_text\":\"" + json_escape(p.album) + "\"";
            payload += "}";
        }
        payload += ",\"instance\":true}";
    }
    payload += "},\"nonce\":\"" + std::string(nonce_buf) + "\"}";
    return payload;
}

void discord_worker()
{
    long nonce = 0;
    int fail_streak = 0;
    for (;;)
    {
        std::string client_id;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            client_id = g_client_id;
        }
        // An empty client id disables the integration; keep polling in case
        // the user pastes one in Settings while we're running.
        if (client_id.empty())
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait_for(lock, std::chrono::seconds(10),
                          [] { return !g_client_id.empty(); });
            if (g_client_id.empty())
            {
                g_presence_dirty = false;
                continue;
            }
            std::lock_guard<std::mutex> lock2(g_mutex);
            client_id = g_client_id;
        }
        int fd = connect_discord_socket();
        if (fd < 0)
        {
            // Discord isn't running (or not installed): back off and retry,
            // so it picks up automatically when the user starts it.
            if (++fail_streak > 6)
                fail_streak = 6;
            int backoff_s = (1 << fail_streak); // 2,4,8,16,32,64
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait_for(lock, std::chrono::seconds(backoff_s),
                          [] { return g_presence_dirty; });
            g_presence_dirty = false;
            continue;
        }
        fail_streak = 0;

        std::string handshake = "{\"v\":1,\"client_id\":\"" + json_escape(client_id) + "\"}";
        if (!send_frame(fd, OP_HANDSHAKE, handshake) || !skip_frame(fd))
        {
            close(fd);
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait_for(lock, std::chrono::seconds(2),
                          [] { return g_presence_dirty; });
            g_presence_dirty = false;
            continue;
        }

        // Connected: send the current presence, then heartbeat re-sends while
        // idle (Discord clears the presence after ~60 s without a beat).
        long long last_beat = 0;
        for (;;)
        {
            DiscordPresence p;
            bool beat = false;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                long long now = (long long)time(nullptr);
                if (!g_presence_dirty &&
                    !g_cv.wait_for(lock, std::chrono::seconds(15),
                                   [] { return g_presence_dirty; }))
                {
                    beat = true;
                }
                g_presence_dirty = false;
                p = g_presence;
                (void)now;
            }
            long long now = (long long)time(nullptr);
            if (beat && now - last_beat < 15)
                continue;
            std::string payload = build_activity_payload(p, ++nonce);
            if (!send_frame(fd, OP_FRAME, payload) || !skip_frame(fd))
                break; // socket broke: reconnect
            last_beat = now;
        }
        close(fd);
    }
}

} // namespace

void discord_rpc_start(const std::string &client_id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running.load() && g_client_id == client_id)
        return;
    if (g_running.load())
    {
        // Restart with a different client id: the old worker keeps polling
        // g_client_id on its next handshake, so just swap the id.
        g_client_id = client_id;
        g_cv.notify_all();
        return;
    }
    g_client_id = client_id;
    if (client_id.empty())
        return;
    g_running.store(true);
    std::thread(discord_worker).detach();
}

void discord_rpc_stop()
{
    DiscordPresence p;
    discord_rpc_update(p); // clears the presence; process exit closes the socket
}

void discord_rpc_update(const DiscordPresence &presence)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_presence = presence;
        g_presence_dirty = true;
    }
    g_cv.notify_all();
}
