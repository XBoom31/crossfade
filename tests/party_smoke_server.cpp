// Minimal headless harness for scripts/party-smoke-test.sh.
//
// It links the real party_server.cpp (routes, headers, Host checks, cookie
// parsing, CSRF guard) and replaces only the party_api::* state layer with
// deterministic stubs. That lets the smoke test exercise the HTTP surface
// without starting the GUI, the Python backend or touching auth files.

#include "party.h"
#include "party_server.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
const char *kGuest = "0123456789abcdef0123456789abcdef";
std::atomic<bool> g_run{true};

void on_term(int)
{
    g_run = false;
}

party_api::AddResult ok_add()
{
    party_api::AddResult r;
    r.http = 200;
    r.status = "queued";
    r.item_json = "{\"id\":\"dQw4w9WgXcQ\",\"title\":\"Smoke Song\","
                  "\"artist\":\"Smoke Artist\",\"durationSec\":180,"
                  "\"by\":\"\",\"mine\":false,\"status\":\"queued\",\"etaSec\":0}";
    return r;
}
} // namespace

namespace party_api
{
bool join(const std::string &token, std::string &guest_id, bool &is_new)
{
    if (token != "smoke-token")
        return false;
    guest_id = kGuest;
    is_new = true;
    return true;
}

bool guest_exists(const std::string &guest_id)
{
    return guest_id == kGuest;
}

std::string state_json(const std::string &guest_id, const std::string &since)
{
    (void)guest_id;
    if (!since.empty())
        return "{\"v\":" + since + ",\"unchanged\":true}";
    return "{\"v\":1,\"serverTime\":1,\"guests\":1,\"party\":{\"name\":\"Smoke party\","
           "\"open\":true,\"requireApproval\":false,\"allowSearch\":true,"
           "\"accent\":\"#6db2d0\"},\"now\":{\"id\":\"\",\"title\":\"\","
           "\"artist\":\"\",\"durationSec\":0,\"positionSec\":0,\"paused\":true},"
           "\"queue\":[],\"limits\":{\"maxPerGuest\":3,\"cooldownSec\":5,"
           "\"maxVideoMinutes\":10},\"you\":{\"queued\":0,\"nextAddInSec\":0,"
           "\"ids\":[]}}";
}

AddResult add(const std::string &guest_id, const std::string &input)
{
    (void)guest_id;
    if (input.find("bad") != std::string::npos)
    {
        AddResult r;
        r.http = 400;
        r.error = "invalid_link";
        r.message = "That doesn't look like a YouTube link.";
        return r;
    }
    return ok_add();
}

AddResult undo(const std::string &guest_id, const std::string &video_id)
{
    (void)guest_id;
    (void)video_id;
    AddResult r;
    r.http = 200;
    return r;
}

bool set_name(const std::string &guest_id, const std::string &name)
{
    (void)name;
    return guest_id == kGuest;
}

bool search(const std::string &guest_id, const std::string &query, std::string &json,
            int &http, int &retry_after)
{
    (void)guest_id;
    (void)query;
    (void)retry_after;
    http = 200;
    json = "[]";
    return true;
}

std::string art_path(const std::string &video_id)
{
    (void)video_id;
    return "";
}

std::string page_html()
{
    return "<!doctype html><html><head><title>Smoke party</title></head>"
           "<body><h1>Smoke party</h1></body></html>";
}

std::string accent_hex()
{
    return "#6db2d0";
}
} // namespace party_api

int main(int argc, char **argv)
{
    std::signal(SIGTERM, on_term);
    std::signal(SIGINT, on_term);

    PartyServerConfig cfg;
    cfg.bind_ip = "127.0.0.1";
    cfg.public_ip = "127.0.0.1";
    cfg.port = argc > 1 ? std::atoi(argv[1]) : 18084;
    if (cfg.port < 1024 || cfg.port > 65000)
        cfg.port = 18084;

    std::string error;
    if (!party_server_start(cfg, error))
    {
        std::fprintf(stderr, "party_server_start failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("PORT %d\n", party_server_port());
    std::fflush(stdout);

    while (g_run)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    party_server_stop();
    return 0;
}
