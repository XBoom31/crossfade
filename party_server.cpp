#include "party_server.h"

#include "party.h"
#include "party_util.h"
#include "guest_page.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#include "httplib.h"

namespace
{

std::unique_ptr<httplib::Server> g_server;
PartyServerConfig g_cfg;
std::mutex g_server_mutex;
bool g_running = false;
// listen_after_bind() blocks until stop(). Keep the thread joinable so the
// Server object is never destroyed while the listen loop still references it.
std::thread g_listen_thread;

const char *kHostErrorHtml =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>crossfade party</title>"
    "<body style=\"font:16px system-ui,sans-serif;background:#0e1014;color:#f2f4f8;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0\">"
    "<div style='text-align:center;padding:28px'>"
    "<div style='font-size:40px'>🎵</div>"
    "<h1 style='font-size:20px;margin:10px 0 6px'>Open the party link directly</h1>"
    "<p style='color:#a0a7b4;margin:0;max-width:320px'>This device reached crossfade, "
    "but the address did not match the party. Scan the QR code again or use the "
    "exact link shown on the host, including the <b>:port</b>.</p></div></body>";

const char *kScanAgainHtml =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>crossfade party</title>"
    "<body style=\"font:16px system-ui,sans-serif;background:#0e1014;color:#f2f4f8;"
    "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0\">"
    "<div style='text-align:center;padding:28px'>"
    "<div style='font-size:40px'>🎵</div>"
    "<h1 style='font-size:20px;margin:10px 0 6px'>Scan the QR code again</h1>"
    "<p style='color:#a0a7b4;margin:0;max-width:300px'>This party link is no longer "
    "valid. Ask the host for the current QR code.</p></div></body>";

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a]))
        ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

bool split_header_token(const std::string &header, const std::string &name, std::string &out)
{
    size_t pos = 0;
    while (pos <= header.size())
    {
        size_t end = header.find(';', pos);
        std::string part = trim(header.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == name)
        {
            out = part.substr(eq + 1);
            if (out.size() >= 2 && out.front() == '"' && out.back() == '"')
                out = out.substr(1, out.size() - 2);
            return true;
        }
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return false;
}

std::string cookie_value(const httplib::Request &req, const std::string &name)
{
    std::string out;
    if (split_header_token(req.get_header_value("Cookie"), name, out))
        return out;
    return "";
}

bool host_ok(const std::string &host)
{
    std::string h = lower(trim(host));
    if (h.empty())
        return false;
    std::string port = ":" + std::to_string(g_cfg.port);
    if (h == lower(g_cfg.public_ip) + port)
        return true;
    if (h == "localhost" + port || h == "127.0.0.1" + port)
        return true;
    // Some clients omit the port on the default port; keep that working only
    // for loopback, never for a LAN hostname.
    if (g_cfg.port == 80 && (h == "localhost" || h == "127.0.0.1"))
        return true;
    return false;
}

void security_headers(const httplib::Request &req, httplib::Response &res)
{
    // Cover art is immutable once cached, but a miss must not be cached: the
    // guest page retries the same URL as thumbnails finish downloading.
    if (req.path.rfind("/api/art/", 0) == 0 && res.status == 200)
        res.set_header("Cache-Control", "public, max-age=86400");
    else
        res.set_header("Cache-Control", "no-store");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("Referrer-Policy", "no-referrer");
    res.set_header("X-Frame-Options", "DENY");
    res.set_header("Cross-Origin-Resource-Policy", "same-origin");
}

void json_reply(httplib::Response &res, int status, const std::string &body)
{
    res.status = status;
    res.set_content(body, "application/json; charset=utf-8");
}

std::string error_json(const std::string &error, const std::string &message, int retry = 0)
{
    std::string out = "{\"ok\":false,\"error\":\"" + party_util::json_escape(error) +
                      "\",\"message\":\"" + party_util::json_escape(message) + "\"";
    if (retry > 0)
        out += ",\"retryAfterSec\":" + std::to_string(retry);
    out += "}";
    return out;
}

bool parse_json_string_field(const std::string &body, const std::string &key, std::string &out)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < body.size() && std::isspace((unsigned char)body[pos]))
        ++pos;
    if (pos >= body.size() || body[pos] != '"')
        return false;
    ++pos;
    std::string result;
    bool esc = false;
    while (pos < body.size())
    {
        char c = body[pos++];
        if (esc)
        {
            switch (c)
            {
            case '"': result += '"'; break;
            case '\\': result += '\\'; break;
            case '/': result += '/'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'u':
            {
                if (pos + 4 > body.size())
                    return false;
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char h = body[pos + i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')
                        cp += (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        cp += (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        cp += (unsigned)(h - 'A' + 10);
                    else
                        return false;
                }
                pos += 4;
                if (cp < 0x80)
                    result += (char)cp;
                else if (cp < 0x800)
                {
                    result += (char)(0xC0 | (cp >> 6));
                    result += (char)(0x80 | (cp & 0x3F));
                }
                else
                {
                    result += (char)(0xE0 | (cp >> 12));
                    result += (char)(0x80 | ((cp >> 6) & 0x3F));
                    result += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: result += c; break;
            }
            esc = false;
        }
        else if (c == '\\')
            esc = true;
        else if (c == '"')
        {
            out = std::move(result);
            return true;
        }
        else
            result += c;
    }
    return false;
}

bool wants_json(const httplib::Request &req)
{
    std::string ct = lower(req.get_header_value("Content-Type"));
    return ct.find("application/json") != std::string::npos;
}

bool csrf_header(const httplib::Request &req)
{
    return req.get_header_value("X-CF") == "1";
}

std::string etag_for(const std::string &body)
{
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : body)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "\"%016llx\"", (unsigned long long)h);
    return buf;
}

void serve_page(httplib::Response &res, const std::string &html, int status = 200)
{
    std::string tag = etag_for(html);
    res.status = status;
    res.set_header("ETag", tag);
    res.set_header("Content-Security-Policy",
                   "default-src 'none'; script-src 'self' 'unsafe-inline'; "
                   "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                   "connect-src 'self'; base-uri 'none'; form-action 'none'; "
                   "frame-ancestors 'none'");
    res.set_content(html, "text/html; charset=utf-8");
}

std::string read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string image_type(const std::string &data)
{
    if (data.size() >= 8 && (unsigned char)data[0] == 0x89 && data[1] == 'P' &&
        data[2] == 'N' && data[3] == 'G')
        return "image/png";
    if (data.size() >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' &&
        data[3] == 'F')
        return "image/webp";
    if (data.size() >= 3 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xD8)
        return "image/jpeg";
    return "application/octet-stream";
}

} // namespace

bool party_server_start(const PartyServerConfig &config, std::string &error)
{
    error.clear();
    std::lock_guard<std::mutex> lock(g_server_mutex);
    if (g_running)
        return true;

    g_cfg = config;
    auto svr = std::unique_ptr<httplib::Server>(new httplib::Server());
    svr->set_payload_max_length(4096);
    svr->set_read_timeout(5, 0);
    svr->set_write_timeout(5, 0);
    svr->set_keep_alive_max_count(64);
    svr->set_keep_alive_timeout(3);

    svr->set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
        if (!host_ok(req.get_header_value("Host")))
        {
            // An empty body can look like a broken blank page on a phone.
            // Explain the mismatch instead; this still reveals nothing.
            serve_page(res, kHostErrorHtml, 421);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    svr->set_post_routing_handler([](const httplib::Request &req, httplib::Response &res) {
        security_headers(req, res);
    });
    svr->set_error_handler([](const httplib::Request &, httplib::Response &res) {
        // httplib calls this for every 4xx/5xx response. Only supply an empty
        // default when the route did not set a body; overwriting it would turn
        // the friendly scan-again page and every JSON error into a blank page.
        if (res.body.empty())
            res.set_content("", "text/plain");
    });

    svr->Get("/healthz", [](const httplib::Request &, httplib::Response &res) {
        res.status = 200;
        res.set_content("ok", "text/plain; charset=utf-8");
    });
    svr->Get(R"(/j/([A-Za-z0-9_-]+))", [](const httplib::Request &req, httplib::Response &res) {
        std::string guest_id;
        bool is_new = false;
        if (!party_api::join(req.matches[1], guest_id, is_new))
        {
            serve_page(res, kScanAgainHtml, 404);
            return;
        }
        // Serve the page directly instead of redirecting to "/". Cookie
        // handling during a cross-site QR navigation is the fragile part on
        // phones; keeping both the Set-Cookie and the document in one 200
        // response avoids relying on a cookie being replayed for the redirect.
        std::string cookie = "cf_guest=" + guest_id +
                             "; HttpOnly; SameSite=Strict; Path=/; Max-Age=43200";
        res.set_header("Set-Cookie", cookie);
        serve_page(res, party_api::page_html());
    });

    svr->Get("/", [](const httplib::Request &req, httplib::Response &res) {
        std::string guest = cookie_value(req, "cf_guest");
        if (guest.empty() || !party_api::guest_exists(guest))
        {
            serve_page(res, kScanAgainHtml, 403);
            return;
        }
        if (req.get_header_value("If-None-Match") == etag_for(party_api::page_html()))
        {
            res.status = 304;
            return;
        }
        serve_page(res, party_api::page_html());
    });

    svr->Get("/api/state", [](const httplib::Request &req, httplib::Response &res) {
        std::string guest = cookie_value(req, "cf_guest");
        if (guest.empty() || !party_api::guest_exists(guest))
        {
            json_reply(res, 401, error_json("no_session", "Scan the QR code again."));
            return;
        }
        std::string since;
        if (req.has_param("v"))
            since = req.get_param_value("v");
        json_reply(res, 200, party_api::state_json(guest, since));
    });

    svr->Post("/api/add", [](const httplib::Request &req, httplib::Response &res) {
        std::string guest = cookie_value(req, "cf_guest");
        if (guest.empty() || !party_api::guest_exists(guest))
        {
            json_reply(res, 401, error_json("no_session", "Scan the QR code again."));
            return;
        }
        if (!wants_json(req) || !csrf_header(req))
        {
            json_reply(res, 415, error_json("bad_request", "Couldn't read that request."));
            return;
        }
        std::string input;
        if (!parse_json_string_field(req.body, "input", input))
        {
            json_reply(res, 400, error_json("invalid_link", "That doesn't look like a YouTube link."));
            return;
        }
        party_api::AddResult r = party_api::add(guest, input);
        if (r.http == 200)
        {
            json_reply(res, 200,
                       "{\"ok\":true,\"status\":\"" + party_util::json_escape(r.status) +
                           "\",\"item\":" + r.item_json + "}");
        }
        else
        {
            json_reply(res, r.http, error_json(r.error, r.message, r.retry_after));
        }
    });

    svr->Delete(R"(/api/add/([A-Za-z0-9_-]{11}))",
                [](const httplib::Request &req, httplib::Response &res) {
                    std::string guest = cookie_value(req, "cf_guest");
                    if (guest.empty() || !party_api::guest_exists(guest))
                    {
                        json_reply(res, 401, error_json("no_session", "Scan the QR code again."));
                        return;
                    }
                    if (!csrf_header(req))
                    {
                        json_reply(res, 415, error_json("bad_request", "Couldn't read that request."));
                        return;
                    }
                    party_api::AddResult r = party_api::undo(guest, req.matches[1]);
                    if (r.http == 200)
                        json_reply(res, 200, "{\"ok\":true}");
                    else
                        json_reply(res, r.http, error_json(r.error, r.message, r.retry_after));
                });

    svr->Get("/api/search", [](const httplib::Request &req, httplib::Response &res) {
        std::string guest = cookie_value(req, "cf_guest");
        if (guest.empty() || !party_api::guest_exists(guest))
        {
            json_reply(res, 401, error_json("no_session", "Scan the QR code again."));
            return;
        }
        std::string q = req.has_param("q") ? req.get_param_value("q") : "";
        std::string json;
        int http = 200, retry = 0;
        if (!party_api::search(guest, q, json, http, retry))
        {
            if (http == 429)
                json_reply(res, 429, error_json("rate_limited", "One search at a time, please.", retry));
            else
                json_reply(res, http, error_json("search_failed",
                                                 "Couldn't search right now. Try again."));
            return;
        }
        json_reply(res, 200, json);
    });

    svr->Post("/api/name", [](const httplib::Request &req, httplib::Response &res) {
        std::string guest = cookie_value(req, "cf_guest");
        if (guest.empty() || !party_api::guest_exists(guest))
        {
            json_reply(res, 401, error_json("no_session", "Scan the QR code again."));
            return;
        }
        if (!wants_json(req) || !csrf_header(req))
        {
            json_reply(res, 415, error_json("bad_request", "Couldn't read that request."));
            return;
        }
        std::string name;
        if (!parse_json_string_field(req.body, "name", name))
        {
            json_reply(res, 400, error_json("bad_request", "Couldn't read that request."));
            return;
        }
        party_api::set_name(guest, name);
        json_reply(res, 200, "{\"ok\":true}");
    });

    svr->Get(R"(/api/art/([A-Za-z0-9_-]{11}))",
             [](const httplib::Request &req, httplib::Response &res) {
                 std::string path = party_api::art_path(req.matches[1]);
                 if (path.empty())
                 {
                     res.status = 404;
                     res.set_content("", "text/plain");
                     return;
                 }
                 std::string data = read_file(path);
                 if (data.empty())
                 {
                     res.status = 404;
                     res.set_content("", "text/plain");
                     return;
                 }
                 res.status = 200;
                 res.set_content(data, image_type(data));
             });

    svr->Options(R"(.*)", [](const httplib::Request &, httplib::Response &res) {
        res.status = 404;
        res.set_content("", "text/plain");
    });

    int port = config.port > 0 ? config.port : 8484;
    std::string bind_ip = config.bind_ip.empty() ? "0.0.0.0" : config.bind_ip;
    bool bound = false;
    for (int i = 0; i < 10; ++i)
    {
        int try_port = port + i;
        if (svr->bind_to_port(bind_ip, try_port))
        {
            g_cfg.port = try_port;
            port = try_port;
            bound = true;
            break;
        }
    }
    if (!bound && bind_ip != "0.0.0.0")
    {
        for (int i = 0; i < 10; ++i)
        {
            int try_port = config.port + i;
            if (svr->bind_to_port("0.0.0.0", try_port))
            {
                g_cfg.bind_ip = "0.0.0.0";
                g_cfg.port = try_port;
                port = try_port;
                bound = true;
                break;
            }
        }
    }
    if (!bound)
    {
        error = "Could not open the party port. It may already be in use.";
        return false;
    }

    httplib::Server *raw = svr.get();
    g_server = std::move(svr);
    g_running = true;
    // listen() blocks until stop(); run it on a dedicated thread. httplib
    // itself owns the request worker pool. The thread uses the raw pointer,
    // which stays valid until stop() has joined it.
    g_listen_thread = std::thread([raw] { raw->listen_after_bind(); });
    return true;
}

void party_server_stop()
{
    std::unique_ptr<httplib::Server> old;
    std::thread thread;
    {
        std::lock_guard<std::mutex> lock(g_server_mutex);
        if (!g_running)
            return;
        g_running = false;
        old = std::move(g_server);
        thread = std::move(g_listen_thread);
    }
    if (old)
        old->stop();
    if (thread.joinable())
        thread.join();
}

bool party_server_running()
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_running;
}

int party_server_port()
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_cfg.port;
}

std::string party_server_bound_ip()
{
    std::lock_guard<std::mutex> lock(g_server_mutex);
    return g_cfg.bind_ip;
}
