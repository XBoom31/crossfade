#include "party_net.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace party_net
{
namespace
{

bool starts_with(const std::string &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

bool is_private_ip(const std::string &ip)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;
    (void)c;
    (void)d;
    if (a == 10)
        return true;
    if (a == 192 && b == 168)
        return true;
    if (a == 172 && b >= 16 && b <= 31)
        return true;
    return false;
}

bool is_link_local(const std::string &ip)
{
    return starts_with(ip, "169.254.");
}

int score_for(const std::string &name, const std::string &ip, bool is_virtual)
{
    int score = 0;
    if (is_private_ip(ip))
        score += 100;
    if (is_link_local(ip))
        score -= 80;
    if (is_virtual)
        score -= 120;
    if (starts_with(name, "wl") || starts_with(name, "wlan") || starts_with(name, "wifi") ||
        starts_with(name, "en") || starts_with(name, "eth") || starts_with(name, "enp") ||
        starts_with(name, "wlp") || starts_with(name, "eno"))
        score += 30;
    if (starts_with(name, "tailscale") || starts_with(name, "tun") ||
        starts_with(name, "utun") || starts_with(name, "wg"))
        score -= 20;
    return score;
}

bool virtual_name(const std::string &name)
{
    static const char *prefixes[] = {
        "docker", "br-", "veth", "virbr", "vmnet", "vboxnet", "tap", "tun",
        "tailscale", "utun", "wg", "vnet", "zt", "ham", "dummy", "lo"};
    for (const char *p : prefixes)
    {
        if (starts_with(name, p))
            return true;
    }
    return false;
}

#ifdef _WIN32
std::string narrow_w(const wchar_t *w)
{
    if (!w)
        return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
#endif

Interface make_interface(const std::string &name, const std::string &ip)
{
    Interface i;
    i.name = name;
    i.ip = ip;
    i.is_private = is_private_ip(ip);
    i.is_virtual = virtual_name(name);
    i.score = score_for(name, ip, i.is_virtual);
    return i;
}

} // namespace

std::string interface_label(const Interface &iface)
{
    if (iface.name.empty())
        return iface.ip;
    return iface.name + " · " + iface.ip;
}

std::vector<Interface> list()
{
    std::vector<Interface> out;
#ifdef _WIN32
    ULONG size = 15000;
    std::vector<unsigned char> buf(size);
    IP_ADAPTER_ADDRESSES *addrs = nullptr;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries)
    {
        buf.resize(size);
        addrs = (IP_ADAPTER_ADDRESSES *)buf.data();
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, addrs, &size);
    }
    if (rc != NO_ERROR)
        return out;
    for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next)
    {
        if (a->OperStatus != IfOperStatusUp)
            continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        std::string name = narrow_w(a->FriendlyName);
        // FriendlyName is wide; fall back to the adapter name when empty.
        if (name.empty() && a->AdapterName)
            name = a->AdapterName;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next)
        {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            sockaddr_in *sin = (sockaddr_in *)u->Address.lpSockaddr;
            char ip[INET_ADDRSTRLEN] = {0};
            if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
                continue;
            out.push_back(make_interface(name, ip));
        }
    }
#else
    struct ifaddrs *ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0)
        return out;
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;
        char ip[INET_ADDRSTRLEN] = {0};
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (!inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)))
            continue;
        out.push_back(make_interface(ifa->ifa_name ? ifa->ifa_name : "", ip));
    }
    freeifaddrs(ifaddr);
#endif
    std::sort(out.begin(), out.end(), [](const Interface &a, const Interface &b) {
        if (a.score != b.score)
            return a.score > b.score;
        return a.ip < b.ip;
    });
    return out;
}

std::string best_ip()
{
    auto all = list();
    for (const Interface &i : all)
    {
        if (i.is_private && !i.is_virtual)
            return i.ip;
    }
    for (const Interface &i : all)
    {
        if (i.is_private)
            return i.ip;
    }
    if (!all.empty())
        return all.front().ip;
    return "";
}

} // namespace party_net
