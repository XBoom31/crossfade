#pragma once

#include <string>
#include <vector>

namespace party_net
{
struct Interface
{
    std::string name;
    std::string ip;
    bool is_private = false;
    bool is_virtual = false;
    int score = 0;
};

// All usable IPv4 interfaces (up, non-loopback), best first.
std::vector<Interface> list();

// Best private IPv4 address, or "" when the machine is offline.
std::string best_ip();

std::string interface_label(const Interface &iface);

} // namespace party_net
