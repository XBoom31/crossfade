#pragma once
#include <string>

// Minimal Discord Rich Presence client (raw Unix-socket IPC, Linux only).
// The worker connects to a running Discord client, pushes presence updates
// and keeps the 15-second heartbeat; every call is non-blocking and failures
// are silent (Discord may simply not be running).

struct DiscordPresence
{
    bool active = false; // false clears the presence
    std::string title;
    std::string artist;
    std::string album;
    std::string image_url; // album art URL; Discord proxies external images
    long long start_ts = 0; // unix seconds; 0 omits timestamps
    long long end_ts = 0;
};

// Start/stop the worker. An empty client_id disables the integration
// (Discord RPC requires an application client id from the developer portal).
void discord_rpc_start(const std::string &client_id);
void discord_rpc_stop();

// Update the presence shown while playing. Safe from any thread.
void discord_rpc_update(const DiscordPresence &presence);
