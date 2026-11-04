#pragma once

#include <string>

struct PartyServerConfig
{
    std::string bind_ip;   // specific LAN address (or 0.0.0.0 fallback)
    std::string public_ip; // address shown in the QR / allowed in Host
    int port = 0;          // 0 = try the configured range; updated on success
};

bool party_server_start(const PartyServerConfig &config, std::string &error);
void party_server_stop();
bool party_server_running();
int party_server_port();
std::string party_server_bound_ip();
