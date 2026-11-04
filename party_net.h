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
    // True for the address the kernel would use as the source for outbound
    // traffic; this avoids stale DHCP aliases on the same interface.
    bool is_preferred = false;
    // Another address on an interface that also has the preferred address
    // (for example a stale DHCP lease left behind). It still answers locally
    // but may not be reachable from other devices.
    bool is_secondary = false;
    int score = 0;
};

// All usable IPv4 interfaces (up, non-loopback), best first.
std::vector<Interface> list();

// Best private IPv4 address, or "" when the machine is offline.
std::string best_ip();

std::string interface_label(const Interface &iface);

} // namespace party_net
