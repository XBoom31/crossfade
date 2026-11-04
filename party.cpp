#include "party.h"

#include "party_link.h"
#include "party_net.h"
#include "party_queue.h"
#include "party_server.h"
#include "party_util.h"
#include "guest_page.h"
#include "texture.h"
#include "ui.h"
#include "ui_notify.h"
#include "ytdlp.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{

// Hard upper bounds so a misbehaving client cannot grow memory without limit.
// Active guests are still evicted after 30 minutes of inactivity in party_tick.
constexpr size_t kMaxGuests = 64;
constexpr size_t kMaxInbox = 64;

using Clock = std::chrono::steady_clock;
double now_sec()
{
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}
double wall_sec()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Guest
{
    std::string id;
    std::string name;
    double last_seen = 0.0;
    party_util::TokenBucket add_bucket{0.2, 1.0, 0.0};
    party_util::TokenBucket search_bucket{1.0, 3.0, 0.0};
};

struct PendingRequest
{
    std::string request_id;
    Track track;
};

struct AddWaiter
{
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    party_api::AddResult result;
};

struct InboxAdd
{
    std::shared_ptr<AddWaiter> waiter;
    Track track;
    std::string guest_id;
    std::string by;
};

struct InboxUndo
{
    std::shared_ptr<AddWaiter> waiter;
    std::string guest_id;
    std::string video_id;
};

struct Snapshot
{
    uint64_t version = 0;
    std::string json;
    std::vector<Track> queue;
    std::vector<PendingRequest> pending;
    std::string now_id;
};

struct MetaWait
{
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    Track track;
    std::string error;
};

// ---- all mutable party state is guarded by g_mu ----
std::mutex g_mu;
std::atomic<PartyRunState> g_state{PartyRunState::Stopped};
PartySettings g_settings;
std::string g_token;
std::string g_url;
std::string g_ip;
std::string g_error;
int g_port = 0;
std::string g_accent_hex = "#6db2d0";
uint64_t g_version = 0;
int g_requests_added = 0;
std::string g_last_sig;
double g_last_build = 0.0;
std::shared_ptr<Snapshot> g_snapshot;
std::unordered_map<std::string, Guest> g_guests;
std::vector<PendingRequest> g_pending;
std::deque<InboxAdd> g_add_inbox;
std::deque<InboxUndo> g_undo_inbox;
std::vector<std::shared_ptr<AddWaiter>> g_waiters;
std::unordered_map<std::string, Track> g_meta_cache;
std::list<std::string> g_meta_lru;
uint64_t g_next_request_id = 1;
std::string g_last_prefetch;
double g_now_duration = 0.0;
double g_last_pos = 0.0;
double g_last_dur = 0.0;
bool g_last_paused = true;
AppState *g_app = nullptr;

std::mutex g_art_mu;
std::unordered_map<std::string, std::string> g_art;

std::mutex g_search_mu;
std::unordered_map<std::string, std::pair<double, std::string>> g_search_cache;

void (*g_wake)() = nullptr;

void wake_loop()
{
    if (g_wake)
        g_wake();
}

std::string lower_ascii(std::string s)
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

std::string accent_hex_locked()
{
    return g_accent_hex;
}

std::string accent_hex_from_ui()
{
    float r = std::max(0.0f, std::min(1.0f, g_accent.x));
    float g = std::max(0.0f, std::min(1.0f, g_accent.y));
    float b = std::max(0.0f, std::min(1.0f, g_accent.z));
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", (int)std::lround(r * 255.0f),
                  (int)std::lround(g * 255.0f), (int)std::lround(b * 255.0f));
    return party_util::safe_accent(buf);
}

void remember_art(const Track &t)
{
    if (t.id.empty() || t.thumbnail_url.empty())
        return;
    std::string path = thumbnail_cache_path(t.thumbnail_url);
    if (path.empty())
        return;
    std::lock_guard<std::mutex> lock(g_art_mu);
    if (g_art.size() > 1500)
        g_art.clear();
    g_art[t.id] = path;
}

void cache_art_async(const std::string &id, const std::string &url)
{
    if (id.empty() || url.empty())
        return;
    std::thread([id, url]() {
        std::string path = get_cached_thumbnail_path(url);
        if (path.empty())
            return;
        std::lock_guard<std::mutex> lock(g_art_mu);
        if (g_art.size() > 1500)
            g_art.clear();
        g_art[id] = path;
    }).detach();
}

void finish_waiter(const std::shared_ptr<AddWaiter> &w, party_api::AddResult r)
{
    std::lock_guard<std::mutex> lock(w->m);
    w->result = std::move(r);
    w->done = true;
    w->cv.notify_all();
}

party_api::AddResult error_result(int http, const std::string &error, const std::string &message,
                                  int retry = 0)
{
    party_api::AddResult r;
    r.http = http;
    r.error = error;
    r.message = message;
    r.retry_after = retry;
    return r;
}

std::string item_json(const Track &t, const std::string &status, int eta, const std::string &by)
{
    std::string out = "{\"id\":\"" + party_util::json_escape(t.id, 16) +
                      "\",\"title\":\"" + party_util::json_escape(t.title, 200) +
                      "\",\"artist\":\"" + party_util::json_escape(t.artist, 200) +
                      "\",\"durationSec\":" + std::to_string(std::max(0, t.duration_sec)) +
                      ",\"by\":\"" + party_util::json_escape(by, 24) +
                      "\",\"mine\":false,\"status\":\"" + party_util::json_escape(status, 16) +
                      "\",\"etaSec\":" + std::to_string(std::max(0, eta)) + "}";
    return out;
}

std::string build_state_json(AppState &state, const std::vector<Track> &queue,
                             const std::vector<PendingRequest> &pending, uint64_t version,
                             double pos, double dur, bool paused)
{
    const double epoch = wall_sec();
    const int guests = (int)g_guests.size();
    std::string j = "{\"v\":" + std::to_string(version) +
                    ",\"serverTime\":" + std::to_string((long long)epoch) +
                    ",\"guests\":" + std::to_string(guests) + ",\"party\":{";
    j += "\"name\":\"" + party_util::json_escape(g_settings.name, 60) + "\",";
    j += "\"open\":true,";
    j += std::string("\"requireApproval\":") + (g_settings.require_approval ? "true" : "false") + ",";
    j += std::string("\"allowSearch\":") + (g_settings.allow_search ? "true" : "false") + ",";
    j += "\"accent\":\"" + accent_hex_locked() + "\"},";

    j += "\"now\":{";
    j += "\"id\":\"" + party_util::json_escape(state.current_track_id, 16) + "\",";
    j += "\"title\":\"" + party_util::json_escape(state.current_title, 200) + "\",";
    j += "\"artist\":\"" + party_util::json_escape(state.current_artist, 200) + "\",";
    j += "\"durationSec\":" + std::to_string((int)std::max(0.0, dur)) + ",";
    j += "\"positionSec\":" + std::to_string((int)std::max(0.0, pos)) + ",";
    j += std::string("\"paused\":") + (paused ? "true" : "false") + "},";

    // Remaining time until the first queue entry starts.
    double eta_cursor = dur > 0.0 ? std::max(0.0, dur - pos) : 0.0;
    if (state.current_track_id.empty())
        eta_cursor = 0.0;

    j += "\"queue\":[";
    bool first = true;
    for (const Track &t : queue)
    {
        if (!first)
            j += ",";
        first = false;
        std::string by = g_settings.show_names ? t.requested_by : "";
        j += item_json(t, "queued", (int)std::lround(eta_cursor), by);
        eta_cursor += std::max(0, t.duration_sec);
    }
    for (const PendingRequest &p : pending)
    {
        if (!first)
            j += ",";
        first = false;
        std::string by = g_settings.show_names ? p.track.requested_by : "";
        j += item_json(p.track, "pending", 0, by);
    }
    j += "],";

    j += "\"limits\":{\"maxPerGuest\":" + std::to_string(g_settings.max_per_guest) +
         ",\"cooldownSec\":" + std::to_string(g_settings.cooldown_sec) +
         ",\"maxVideoMinutes\":" + std::to_string(g_settings.max_video_minutes) + "},";
    // The server patches this marker with the caller's own counters.
    j += "\"you\":{\"queued\":0,\"nextAddInSec\":0,\"ids\":[]}}";
    return j;
}

std::string make_sig(AppState &state, const std::vector<PendingRequest> &pending)
{
    std::string s;
    s += state.current_track_id;
    s += '|';
    s += state.current_title;
    s += '|';
    s += state.current_artist;
    s += '|';
    s += state.playing ? '1' : '0';
    s += '|';
    s += std::to_string((int)g_state.load()) + '|';
    s += std::to_string(g_requests_added) + '|';
    s += std::to_string(g_guests.size()) + '|';
    s += g_settings.name + '|';
    s += g_settings.require_approval ? '1' : '0';
    s += g_settings.show_names ? '1' : '0';
    s += g_settings.allow_search ? '1' : '0';
    s += '|';
    for (const Track &t : state.play_queue)
    {
        s += t.id;
        s += ':';
        s += t.requested_by;
        s += ':';
        s += t.title;
        s += ';';
    }
    s += '|';
    for (const PendingRequest &p : pending)
    {
        s += p.request_id;
        s += ':';
        s += p.track.id;
        s += ';';
    }
    return s;
}

void publish_locked(AppState &state, double pos, double dur, bool paused, bool bump)
{
    if (bump)
        ++g_version;
    auto snap = std::make_shared<Snapshot>();
    snap->version = g_version;
    snap->queue = state.play_queue;
    snap->pending = g_pending;
    snap->now_id = state.current_track_id;
    snap->json = build_state_json(state, snap->queue, snap->pending, g_version, pos, dur, paused);
    g_snapshot = std::move(snap);
    g_last_build = now_sec();
    g_last_sig = make_sig(state, g_pending);
}

party_api::AddResult process_add(AppState &state, InboxAdd item)
{
    party_api::AddResult r;
    if (g_state != PartyRunState::Running)
    {
        r = error_result(403, "party_closed", "This party has ended.");
        finish_waiter(item.waiter, std::move(r));
        return r;
    }
    if (party_queue::is_duplicate(state.play_queue, item.track.id, state.current_track_id,
                                  g_settings.allow_duplicates))
    {
        r = error_result(409, "duplicate", "That song is already in the queue.");
        finish_waiter(item.waiter, std::move(r));
        return r;
    }
    for (const PendingRequest &p : g_pending)
    {
        if (p.track.id == item.track.id && !g_settings.allow_duplicates)
        {
            r = error_result(409, "duplicate", "That song is already waiting for the host.");
            finish_waiter(item.waiter, std::move(r));
            return r;
        }
    }
    if (g_settings.max_queue > 0 &&
        (int)(state.play_queue.size() + g_pending.size()) >= g_settings.max_queue)
    {
        r = error_result(503, "busy", "The queue is full right now. Try again in a bit.");
        finish_waiter(item.waiter, std::move(r));
        return r;
    }
    if (g_settings.max_per_guest > 0)
    {
        int mine = party_queue::count_guest_items(state.play_queue, item.guest_id);
        for (const PendingRequest &p : g_pending)
        {
            if (p.track.guest_id == item.guest_id)
                ++mine;
        }
        if (mine >= g_settings.max_per_guest)
        {
            r = error_result(429, "quota",
                             "You already have " + std::to_string(g_settings.max_per_guest) +
                                 " songs in the queue.",
                             std::max(0, g_settings.cooldown_sec));
            finish_waiter(item.waiter, std::move(r));
            return r;
        }
    }
    if (g_settings.max_video_minutes > 0 && item.track.duration_sec > 0 &&
        item.track.duration_sec > g_settings.max_video_minutes * 60)
    {
        r = error_result(422, "too_long",
                         "That video is longer than the " +
                             std::to_string(g_settings.max_video_minutes) + "-minute limit.");
        finish_waiter(item.waiter, std::move(r));
        return r;
    }

    item.track.from_party = true;
    item.track.guest_id = item.guest_id;
    item.track.requested_by = item.by;
    item.track.party_round = party_queue::round_for_guest(state.play_queue, item.guest_id);
    ++g_requests_added;
    state.repaint_requested = true;

    if (g_settings.require_approval)
    {
        PendingRequest p;
        p.request_id = std::to_string(g_next_request_id++);
        p.track = item.track;
        g_pending.push_back(p);
        r.http = 200;
        r.status = "pending";
        r.item_json = item_json(item.track, "pending", 0, item.by);
        state.queue_dirty = true;
        finish_waiter(item.waiter, std::move(r));
    }
    else
    {
        int idx = party_queue::insert_fair(state.play_queue, item.track);
        state.queue_dirty = true;
        // Rough ETA: current track plus every entry before this one.
        double eta = state.current_track_id.empty() ? 0.0 : g_now_duration;
        for (int i = 0; i < idx && i < (int)state.play_queue.size(); ++i)
            eta += std::max(0, state.play_queue[i].duration_sec);
        r.http = 200;
        r.status = "queued";
        r.item_json = item_json(item.track, "queued", (int)std::lround(eta), item.by);
        finish_waiter(item.waiter, std::move(r));
    }
    return r;
}

void process_undo(AppState &state, InboxUndo item)
{
    party_api::AddResult r;
    // Pending requests can be withdrawn too.
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it)
    {
        if (it->track.id == item.video_id && it->track.guest_id == item.guest_id)
        {
            g_pending.erase(it);
            state.queue_dirty = true;
            r.http = 200;
            finish_waiter(item.waiter, std::move(r));
            return;
        }
    }
    for (auto it = state.play_queue.rbegin(); it != state.play_queue.rend(); ++it)
    {
        if (it->id == item.video_id && it->guest_id == item.guest_id)
        {
            state.play_queue.erase(std::next(it).base());
            state.queue_dirty = true;
            state.repaint_requested = true;
            r.http = 200;
            finish_waiter(item.waiter, std::move(r));
            return;
        }
    }
    r = error_result(409, "already_started", "That song already started playing.");
    finish_waiter(item.waiter, std::move(r));
}

Track meta_cache_get(const std::string &id)
{
    auto it = g_meta_cache.find(id);
    if (it == g_meta_cache.end())
        return Track();
    g_meta_lru.remove(id);
    g_meta_lru.push_front(id);
    return it->second;
}

void meta_cache_put(const Track &t)
{
    if (t.id.empty())
        return;
    if (g_meta_cache.size() >= 500)
    {
        while (!g_meta_lru.empty() && g_meta_cache.size() >= 500)
        {
            g_meta_cache.erase(g_meta_lru.back());
            g_meta_lru.pop_back();
        }
    }
    g_meta_cache[t.id] = t;
    g_meta_lru.remove(t.id);
    g_meta_lru.push_front(t.id);
}

bool fetch_meta_bounded(const std::string &id, Track &out, std::string &error)
{
    auto w = std::make_shared<MetaWait>();
    std::thread([w, id]() {
        Track t;
        std::string err;
        bool ok = fetch_track_meta(id, t, err);
        std::lock_guard<std::mutex> lock(w->m);
        w->ok = ok;
        w->track = std::move(t);
        w->error = std::move(err);
        w->done = true;
        w->cv.notify_all();
    }).detach();

    std::unique_lock<std::mutex> lock(w->m);
    if (!w->cv.wait_for(lock, std::chrono::seconds(8), [&] { return w->done; }))
    {
        error = "lookup timed out";
        return false;
    }
    if (!w->ok)
    {
        error = w->error;
        return false;
    }
    out = w->track;
    return true;
}

} // namespace

// ----------------------------------------------------------------------
// Host lifecycle
// ----------------------------------------------------------------------
void party_init()
{
    // Intentionally lazy: no threads, sockets or timers before a party starts.
}

void party_set_wake(void (*fn)())
{
    g_wake = fn;
}

bool party_start(const PartySettings &settings, std::string &error)
{
    error.clear();
    if (g_state == PartyRunState::Running)
        party_stop();

    std::string ip = settings.interface_ip;
    if (!ip.empty())
    {
        // A saved interface may be gone (different Wi-Fi, VPN down). Fall
        // back to the best current address instead of binding a dead IP.
        bool present = false;
        for (const party_net::Interface &i : party_net::list())
            if (i.ip == ip)
                present = true;
        if (!present)
            ip.clear();
    }
    if (ip.empty())
        ip = party_net::best_ip();
    if (ip.empty())
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_state = PartyRunState::Error;
        g_error = "No LAN interface found. Connect to Wi-Fi or a hotspot.";
        error = g_error;
        return false;
    }

    std::string token = party_util::random_token();
    if (token.empty())
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_state = PartyRunState::Error;
        g_error = "Could not generate a party token.";
        error = g_error;
        return false;
    }

    PartyServerConfig cfg;
    cfg.bind_ip = ip;
    cfg.public_ip = ip;
    cfg.port = settings.port > 0 ? std::max(1, std::min(65535, settings.port)) : 8484;
    if (!party_server_start(cfg, error))
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_state = PartyRunState::Error;
        g_error = error;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_settings = settings;
        g_settings.port = party_server_port();
        g_token = token;
        g_port = party_server_port();
        g_ip = ip;
        g_url = "http://" + ip + ":" + std::to_string(g_port) + "/j/" + g_token;
        g_error.clear();
        g_state = PartyRunState::Running;
        g_guests.clear();
        g_pending.clear();
        g_add_inbox.clear();
        g_undo_inbox.clear();
        g_waiters.clear();
        g_requests_added = 0;
        g_version = 1;
        g_last_sig.clear();
        g_last_prefetch.clear();
        g_meta_cache.clear();
        g_meta_lru.clear();
        g_accent_hex = accent_hex_from_ui();
        {
            std::lock_guard<std::mutex> art_lock(g_art_mu);
            g_art.clear();
        }
        {
            std::lock_guard<std::mutex> search_lock(g_search_mu);
            g_search_cache.clear();
        }
        if (g_app)
        {
            publish_locked(*g_app, 0.0, 0.0, true, false);
        }
        else
        {
            // The first tick publishes the real state; this keeps /api/state
            // valid for the brief window between start and the next frame.
            AppState empty;
            publish_locked(empty, 0.0, 0.0, true, false);
        }
    }
    return true;
}

void party_stop()
{
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state == PartyRunState::Stopped && !party_server_running())
        {
            g_token.clear();
            g_url.clear();
            return;
        }
        g_state = PartyRunState::Stopped;
        std::vector<std::shared_ptr<AddWaiter>> waiters;
        waiters.swap(g_waiters);
        for (auto &w : waiters)
        {
            party_api::AddResult r = error_result(403, "party_closed", "This party has ended.");
            finish_waiter(w, std::move(r));
        }
        g_add_inbox.clear();
        g_undo_inbox.clear();
        g_guests.clear();
        g_pending.clear();
        g_snapshot.reset();
        g_token.clear();
        g_url.clear();
        g_ip.clear();
        g_port = 0;
        g_version = 0;
        g_requests_added = 0;
        g_last_sig.clear();
        g_last_prefetch.clear();
        {
            std::lock_guard<std::mutex> art_lock(g_art_mu);
            g_art.clear();
        }
    }
    party_server_stop();
}

void party_set_last_error(const std::string &err)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_error = err;
}

bool party_running()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_state == PartyRunState::Running;
}

PartyStatus party_status()
{
    PartyStatus s;
    std::lock_guard<std::mutex> lock(g_mu);
    s.state = g_state;
    s.url = g_url;
    s.ip = g_ip;
    s.port = g_port;
    s.error = g_error;
    s.guests = (int)g_guests.size();
    s.requests_added = g_requests_added;
    return s;
}

const PartySettings &party_active_settings()
{
    return g_settings;
}

std::string party_url()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_url;
}

std::string party_error()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_error;
}

std::vector<PartyInterfaceInfo> party_interfaces()
{
    std::vector<PartyInterfaceInfo> out;
    for (const party_net::Interface &i : party_net::list())
    {
        PartyInterfaceInfo p;
        p.name = i.name;
        p.ip = i.ip;
        p.label = party_net::interface_label(i);
        p.is_private = i.is_private;
        out.push_back(p);
    }
    return out;
}

std::vector<PartyPendingRequest> party_pending_requests()
{
    std::vector<PartyPendingRequest> out;
    std::lock_guard<std::mutex> lock(g_mu);
    out.reserve(g_pending.size());
    for (const PendingRequest &p : g_pending)
    {
        PartyPendingRequest r;
        r.request_id = p.request_id;
        r.id = p.track.id;
        r.title = p.track.title;
        r.artist = p.track.artist;
        r.by = p.track.requested_by;
        r.duration_sec = p.track.duration_sec;
        out.push_back(std::move(r));
    }
    return out;
}

void party_approve(const std::string &request_id)
{
    if (!g_app)
        return;
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it)
    {
        if (it->request_id != request_id)
            continue;
        Track t = it->track;
        g_pending.erase(it);
        if (!party_queue::is_duplicate(g_app->play_queue, t.id,
                                       g_app->current_track_id,
                                       g_settings.allow_duplicates))
            party_queue::insert_fair(g_app->play_queue, t);
        g_app->queue_dirty = true;
        g_app->repaint_requested = true;
        publish_locked(*g_app, g_last_pos, g_last_dur, g_last_paused, true);
        return;
    }
}

void party_reject(const std::string &request_id)
{
    std::lock_guard<std::mutex> lock(g_mu);
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it)
    {
        if (it->request_id != request_id)
            continue;
        g_pending.erase(it);
        if (g_app)
        {
            g_app->queue_dirty = true;
            g_app->repaint_requested = true;
        }
        return;
    }
}

void party_approve_all()
{
    if (!g_app)
        return;
    std::lock_guard<std::mutex> lock(g_mu);
    for (const PendingRequest &p : g_pending)
    {
        Track t = p.track;
        if (!party_queue::is_duplicate(g_app->play_queue, t.id,
                                       g_app->current_track_id,
                                       g_settings.allow_duplicates))
            party_queue::insert_fair(g_app->play_queue, t);
    }
    g_pending.clear();
    g_app->queue_dirty = true;
    g_app->repaint_requested = true;
    publish_locked(*g_app, g_last_pos, g_last_dur, g_last_paused, true);
}

int party_guest_count()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return (int)g_guests.size();
}

int party_requests_added()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_requests_added;
}

void party_tick(AppState &state, double position_sec, double duration_sec, bool paused)
{
    g_app = &state;
    if (g_state != PartyRunState::Running)
        return;

    std::vector<std::pair<std::string, std::string>> notifications; // title, by
    std::string prefetch_ids;

    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_now_duration = duration_sec;
        g_last_pos = position_sec;
        g_last_dur = duration_sec;
        g_last_paused = paused;

        // Drain worker requests on the main thread, in arrival order.
        while (!g_add_inbox.empty())
        {
            InboxAdd item = std::move(g_add_inbox.front());
            g_add_inbox.pop_front();
            // Keep the guest display name current (it may have changed
            // between the HTTP request and the drain).
            auto git = g_guests.find(item.guest_id);
            if (git != g_guests.end() && !git->second.name.empty())
                item.by = git->second.name;
            const std::string title = item.track.title;
            const std::string by = item.by;
            party_api::AddResult r = process_add(state, std::move(item));
            if (r.http == 200 && !title.empty())
                notifications.emplace_back(title, by);
        }
        while (!g_undo_inbox.empty())
        {
            InboxUndo item = std::move(g_undo_inbox.front());
            g_undo_inbox.pop_front();
            process_undo(state, std::move(item));
        }

        // Evict guests idle for more than 30 minutes.
        const double t = now_sec();
        for (auto it = g_guests.begin(); it != g_guests.end();)
        {
            if (t - it->second.last_seen > 1800.0)
                it = g_guests.erase(it);
            else
                ++it;
        }

        g_accent_hex = accent_hex_from_ui();
        remember_art(Track{state.current_track_id, state.current_title, state.current_artist,
                           state.current_artist_id, "", state.current_album_id, 0,
                           state.current_thumbnail_url, (int)duration_sec, "",
                           "", "", false, 0});

        std::string sig = make_sig(state, g_pending);
        bool bump = sig != g_last_sig;
        if (bump || (!g_guests.empty() && t - g_last_build >= 1.0))
        {
            publish_locked(state, position_sec, duration_sec, paused, bump);
        }

        // Prefetch warm stream URLs for guest songs at the head of Up Next.
        std::string ids;
        int n = 0;
        for (const Track &tr : state.play_queue)
        {
            if (n >= 2)
                break;
            if (tr.from_party)
            {
                ids += tr.id;
                ids += ',';
                ++n;
            }
        }
        if (!ids.empty() && ids != g_last_prefetch)
        {
            g_last_prefetch = ids;
            prefetch_ids = ids;
        }
    }

    if (!notifications.empty() && state.settings.show_notifications)
    {
        for (const auto &n : notifications)
        {
            std::string msg = n.first;
            if (!n.second.empty())
                msg += " · " + n.second;
            ui_notify(UiNotifyType::Success, "Song added", msg, 5.0f);
        }
    }

    if (!prefetch_ids.empty())
    {
        // Warm the first guest entries so crossfade into them is as smooth as
        // for host songs. prefetch_urls_for() runs on a background thread.
        std::vector<Track> warm;
        int count = 0;
        for (const Track &tr : state.play_queue)
        {
            if (count >= 2)
                break;
            if (tr.from_party)
            {
                warm.push_back(tr);
                ++count;
            }
        }
        if (!warm.empty())
            prefetch_urls_for(warm, warm.size());
    }
}

// ----------------------------------------------------------------------
// HTTP worker-thread surface
// ----------------------------------------------------------------------
namespace party_api
{

bool join(const std::string &token, std::string &guest_id, bool &is_new)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state != PartyRunState::Running)
        return false;
    if (!party_util::constant_time_equal(token, g_token))
        return false;

    const double t = now_sec();
    if (g_guests.size() >= kMaxGuests)
    {
        // Make room for a real guest by first dropping sessions that have
        // already gone quiet well past the normal 30-minute idle window.
        for (auto it = g_guests.begin(); it != g_guests.end();)
        {
            if (t - it->second.last_seen > 1800.0)
                it = g_guests.erase(it);
            else
                ++it;
        }
        if (g_guests.size() >= kMaxGuests)
            return false;
    }

    unsigned char raw[16];
    if (!party_util::random_bytes(raw, sizeof(raw)))
        return false;
    static const char *hex = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (unsigned char b : raw)
    {
        id += hex[b >> 4];
        id += hex[b & 15];
    }
    Guest g;
    g.id = id;
    g.last_seen = t;
    g.add_bucket = party_util::TokenBucket(
        g_settings.cooldown_sec > 0 ? 1.0 / g_settings.cooldown_sec : 100.0, 1.0, t);
    g.search_bucket = party_util::TokenBucket(1.0, 3.0, t);
    g_guests[id] = std::move(g);
    guest_id = id;
    is_new = true;
    return true;
}

bool guest_exists(const std::string &guest_id)
{
    if (guest_id.empty() || guest_id.size() != 32)
        return false;
    for (char c : guest_id)
    {
        if (!std::isxdigit((unsigned char)c))
            return false;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_guests.find(guest_id);
    if (it == g_guests.end())
        return false;
    it->second.last_seen = now_sec();
    return g_state == PartyRunState::Running;
}

std::string state_json(const std::string &guest_id, const std::string &since)
{
    std::shared_ptr<Snapshot> snap;
    int queued = 0;
    int next_add = 0;
    std::string ids;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state != PartyRunState::Running || !g_snapshot)
            return "{\"v\":0,\"closed\":true}";
        snap = g_snapshot;
        if (!since.empty() && since == std::to_string(snap->version))
            return "{\"v\":" + since + ",\"unchanged\":true}";
        auto git = g_guests.find(guest_id);
        if (git != g_guests.end())
        {
            next_add = (int)std::ceil(git->second.add_bucket.retry_after(now_sec()));
            bool first = true;
            for (const Track &t : snap->queue)
            {
                if (t.from_party && t.guest_id == guest_id)
                {
                    ++queued;
                    if (!first)
                        ids += ",";
                    first = false;
                    ids += "\"" + party_util::json_escape(t.id, 16) + "\"";
                }
            }
            for (const PendingRequest &p : snap->pending)
            {
                if (p.track.guest_id == guest_id)
                {
                    ++queued;
                    if (!first)
                        ids += ",";
                    first = false;
                    ids += "\"" + party_util::json_escape(p.track.id, 16) + "\"";
                }
            }
        }
    }
    std::string json = snap->json;
    const std::string marker = "\"you\":{\"queued\":0,\"nextAddInSec\":0,\"ids\":[]}";
    std::string repl = "\"you\":{\"queued\":" + std::to_string(queued) +
                       ",\"nextAddInSec\":" + std::to_string(std::max(0, next_add)) +
                       ",\"ids\":[" + ids + "]}";
    size_t pos = json.rfind(marker);
    if (pos != std::string::npos)
        json.replace(pos, marker.size(), repl);
    return json;
}

AddResult add(const std::string &guest_id, const std::string &input)
{
    if (g_state != PartyRunState::Running)
        return error_result(403, "party_closed", "This party has ended.");

    party_link::Parsed parsed = party_link::parse(input);
    if (parsed.result != party_link::Result::Ok)
        return error_result(400, "invalid_link", parsed.message);

    const double t = now_sec();
    int max_video_minutes = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto git = g_guests.find(guest_id);
        if (git == g_guests.end())
            return error_result(401, "no_session", "Scan the QR code again.");
        Guest &g = git->second;
        g.last_seen = t;
        max_video_minutes = g_settings.max_video_minutes;
        if (g_settings.cooldown_sec > 0)
        {
            if (!g.add_bucket.take(t))
            {
                int retry = (int)std::ceil(g.add_bucket.retry_after(t));
                return error_result(429, "rate_limited", "Slow down a little.",
                                    std::max(1, retry));
            }
        }
        // Cheap duplicate check before paying for a metadata lookup.
        if (!g_settings.allow_duplicates && g_snapshot)
        {
            if (g_snapshot->now_id == parsed.video_id)
                return error_result(409, "duplicate", "That song is already playing.");
            for (const Track &tr : g_snapshot->queue)
                if (tr.id == parsed.video_id)
                    return error_result(409, "duplicate", "That song is already in the queue.");
            for (const PendingRequest &p : g_snapshot->pending)
                if (p.track.id == parsed.video_id)
                    return error_result(409, "duplicate", "That song is already waiting for the host.");
        }
    }

    Track meta;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        meta = meta_cache_get(parsed.video_id);
        have = !meta.id.empty();
        if (have && !g_settings.allow_duplicates)
        {
            if (g_snapshot)
            {
                if (g_snapshot->now_id == meta.id)
                    return error_result(409, "duplicate", "That song is already playing.");
                for (const Track &tr : g_snapshot->queue)
                    if (tr.id == meta.id)
                        return error_result(409, "duplicate", "That song is already in the queue.");
            }
        }
    }
    if (!have)
    {
        std::string err;
        if (!fetch_meta_bounded(parsed.video_id, meta, err))
            return error_result(503, "lookup_failed",
                                "Couldn't look that song up. Try again.", 3);
        std::lock_guard<std::mutex> lock(g_mu);
        meta_cache_put(meta);
    }
    if (meta.duration_sec > 0 && max_video_minutes > 0 &&
        meta.duration_sec > max_video_minutes * 60)
    {
        return error_result(422, "too_long",
                            "That video is longer than the " +
                                std::to_string(max_video_minutes) + "-minute limit.");
    }
    cache_art_async(meta.id, meta.thumbnail_url);

    auto waiter = std::make_shared<AddWaiter>();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto git = g_guests.find(guest_id);
        if (git == g_guests.end())
            return error_result(401, "no_session", "Scan the QR code again.");
        if (g_add_inbox.size() >= kMaxInbox)
            return error_result(503, "busy", "The app is busy right now. Try again.", 2);
        InboxAdd item;
        item.waiter = waiter;
        item.track = meta;
        item.guest_id = guest_id;
        item.by = git->second.name;
        g_add_inbox.push_back(std::move(item));
        g_waiters.push_back(waiter);
        if (g_waiters.size() > 256)
            g_waiters.erase(g_waiters.begin(), g_waiters.begin() + 128);
    }
    wake_loop();

    std::unique_lock<std::mutex> lock(waiter->m);
    if (!waiter->cv.wait_for(lock, std::chrono::seconds(9), [&] { return waiter->done; }))
        return error_result(503, "busy", "The app is busy right now. Try again.");
    return waiter->result;
}

AddResult undo(const std::string &guest_id, const std::string &video_id)
{
    if (!party_link::valid_video_id(video_id))
        return error_result(400, "invalid_link", "That doesn't look like a YouTube link.");
    auto waiter = std::make_shared<AddWaiter>();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state != PartyRunState::Running)
            return error_result(403, "party_closed", "This party has ended.");
        if (g_guests.find(guest_id) == g_guests.end())
            return error_result(401, "no_session", "Scan the QR code again.");
        if (g_undo_inbox.size() >= kMaxInbox)
            return error_result(503, "busy", "The app is busy right now. Try again.", 2);
        InboxUndo item;
        item.waiter = waiter;
        item.guest_id = guest_id;
        item.video_id = video_id;
        g_undo_inbox.push_back(std::move(item));
        g_waiters.push_back(waiter);
    }
    wake_loop();
    std::unique_lock<std::mutex> lock(waiter->m);
    if (!waiter->cv.wait_for(lock, std::chrono::seconds(5), [&] { return waiter->done; }))
        return error_result(503, "busy", "The app is busy right now. Try again.");
    return waiter->result;
}

bool set_name(const std::string &guest_id, const std::string &name)
{
    std::string clean = party_util::clean_utf8(name, 24);
    clean = trim(clean);
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_guests.find(guest_id);
    if (it == g_guests.end())
        return false;
    it->second.name = clean;
    return true;
}

bool search(const std::string &guest_id, const std::string &query, std::string &json,
            int &http, int &retry_after)
{
    http = 200;
    retry_after = 0;
    std::string q = trim(party_util::clean_utf8(query, 200));
    bool allow_search = false;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        allow_search = g_state == PartyRunState::Running && g_settings.allow_search;
    }
    if (!allow_search)
    {
        http = 403;
        return false;
    }
    if (q.empty())
    {
        json = "[]";
        return true;
    }
    const double t = now_sec();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto git = g_guests.find(guest_id);
        if (git == g_guests.end())
        {
            http = 401;
            return false;
        }
        git->second.last_seen = t;
        if (!git->second.search_bucket.take(t))
        {
            http = 429;
            retry_after = std::max(1, (int)std::ceil(git->second.search_bucket.retry_after(t)));
            return false;
        }
    }

    std::string key = lower_ascii(q);
    {
        std::lock_guard<std::mutex> lock(g_search_mu);
        auto it = g_search_cache.find(key);
        if (it != g_search_cache.end() && t - it->second.first < 15.0)
        {
            json = it->second.second;
            return true;
        }
    }

    std::vector<Track> tracks = fetch_search(q);
    std::string out = "[";
    bool first = true;
    int n = 0;
    std::vector<Track> to_cache;
    for (const Track &tr : tracks)
    {
        if (n >= 10)
            break;
        if (!party_link::valid_video_id(tr.id) || tr.title.empty())
            continue;
        if (!first)
            out += ",";
        first = false;
        out += "{\"id\":\"" + party_util::json_escape(tr.id, 16) +
               "\",\"title\":\"" + party_util::json_escape(tr.title, 200) +
               "\",\"artist\":\"" + party_util::json_escape(tr.artist, 200) +
               "\",\"durationSec\":" + std::to_string(std::max(0, tr.duration_sec)) + "}";
        to_cache.push_back(tr);
        ++n;
    }
    out += "]";
    {
        std::lock_guard<std::mutex> lock(g_search_mu);
        if (g_search_cache.size() > 100)
            g_search_cache.clear();
        g_search_cache[key] = {t, out};
    }
    for (const Track &tr : to_cache)
        cache_art_async(tr.id, tr.thumbnail_url);
    json = std::move(out);
    return true;
}

std::string art_path(const std::string &video_id)
{
    if (!party_link::valid_video_id(video_id))
        return "";
    std::lock_guard<std::mutex> lock(g_art_mu);
    auto it = g_art.find(video_id);
    return it == g_art.end() ? "" : it->second;
}

std::string page_html()
{
    std::string name;
    std::string accent;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        name = party_util::html_escape(g_settings.name);
        accent = g_accent_hex;
    }
    std::string html = kGuestPageHtml;
    auto replace_all = [](std::string &s, const std::string &from, const std::string &to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos)
        {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all(html, "__PARTY_NAME__", name);
    replace_all(html, "__ACCENT__", party_util::safe_accent(accent));
    return html;
}

std::string accent_hex()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_accent_hex;
}

} // namespace party_api
