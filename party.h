#pragma once

#include <string>
#include <vector>

struct AppState;

// ----------------------------------------------------------------------
// Party mode — public surface shared by the host UI, the HTTP server and
// the main loop. Everything in here is called on the UI/main thread except
// the party_api::* functions, which are safe from HTTP worker threads.
// ----------------------------------------------------------------------

struct PartySettings
{
    std::string name = "crossfade party";
    int port = 8484;
    std::string interface_ip; // empty = auto (best private IPv4)
    bool require_approval = false;
    int max_per_guest = 3;     // 0 = unlimited
    int cooldown_sec = 5;
    int max_video_minutes = 10; // 0 = unlimited
    int max_queue = 100;
    bool allow_search = true;
    bool allow_duplicates = false;
    bool show_names = true;

    bool operator==(const PartySettings &o) const
    {
        return name == o.name && port == o.port && interface_ip == o.interface_ip &&
               require_approval == o.require_approval && max_per_guest == o.max_per_guest &&
               cooldown_sec == o.cooldown_sec && max_video_minutes == o.max_video_minutes &&
               max_queue == o.max_queue && allow_search == o.allow_search &&
               allow_duplicates == o.allow_duplicates && show_names == o.show_names;
    }
    bool operator!=(const PartySettings &o) const { return !(*this == o); }
};

enum class PartyRunState
{
    Stopped,
    Running,
    Error,
};

struct PartyInterfaceInfo
{
    std::string name;
    std::string ip;
    std::string label;
    bool is_private = false;
};

struct PartyPendingRequest
{
    std::string request_id;
    std::string id;
    std::string title;
    std::string artist;
    std::string by;
    int duration_sec = 0;
};

struct PartyStatus
{
    PartyRunState state = PartyRunState::Stopped;
    std::string url;
    std::string ip;
    int port = 0;
    std::string error;
    int guests = 0;
    int requests_added = 0;
};

// ---- Host-side lifecycle (main thread) ----
void party_init();
void party_set_wake(void (*fn)());
bool party_start(const PartySettings &settings, std::string &error);
void party_stop();
void party_tick(AppState &state, double position_sec, double duration_sec, bool paused);

PartyStatus party_status();
const PartySettings &party_active_settings();
std::string party_url();
std::string party_error();
bool party_running();
void party_set_last_error(const std::string &err);

std::vector<PartyInterfaceInfo> party_interfaces();
std::vector<PartyPendingRequest> party_pending_requests();
void party_approve(const std::string &request_id);
void party_reject(const std::string &request_id);
void party_approve_all();

int party_guest_count();
int party_requests_added();

// ---- HTTP worker-thread surface ----
namespace party_api
{
struct AddResult
{
    int http = 503;
    std::string status;
    std::string error;
    std::string message;
    int retry_after = 0;
    std::string item_json; // one serialized queue item when queued
};

// Join token -> (new) guest id. The token is compared in constant time.
bool join(const std::string &token, std::string &guest_id, bool &is_new);
bool guest_exists(const std::string &guest_id);

// Full state JSON, or a tiny unchanged reply when `since` matches.
std::string state_json(const std::string &guest_id, const std::string &since);

AddResult add(const std::string &guest_id, const std::string &input);
AddResult undo(const std::string &guest_id, const std::string &video_id);
bool set_name(const std::string &guest_id, const std::string &name);

// Search (main backend channel, rate-limited per guest). Returns a JSON
// array string on success.
bool search(const std::string &guest_id, const std::string &query,
            std::string &json, int &http, int &retry_after);

// Local artwork path for the validated 11-char id, or "" when not cached.
std::string art_path(const std::string &video_id);

std::string page_html();
std::string accent_hex();
} // namespace party_api
