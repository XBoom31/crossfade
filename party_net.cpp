#include "party_net.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

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
#include <unistd.h>
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

// Lets the kernel pick the source address it would use for outbound traffic.
// A UDP connect performs only a route lookup (no packet is sent), and catches
// stale secondary DHCP aliases that still answer locally but are unreachable
// from other devices.
std::string route_source_ip()
{
#ifdef _WIN32
    WSADATA wsa;
    const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif
    std::string result;
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
    {
        if (started)
            WSACleanup();
        return result;
    }
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return result;
#endif
    sockaddr_in dst;
    std::memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9);
    // TEST-NET-1: never routed on a normal network, but route lookup is all
    // this needs.
    inet_pton(AF_INET, "192.0.2.1", &dst.sin_addr);
    if (connect(s, (sockaddr *)&dst, sizeof(dst)) == 0)
    {
        sockaddr_in src;
        std::memset(&src, 0, sizeof(src));
#ifdef _WIN32
        int len = (int)sizeof(src);
#else
        socklen_t len = (socklen_t)sizeof(src);
#endif
        if (getsockname(s, (sockaddr *)&src, &len) == 0 && src.sin_family == AF_INET)
        {
            char buf[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &src.sin_addr, buf, sizeof(buf)))
                result = buf;
        }
    }
#ifdef _WIN32
    closesocket(s);
    if (started)
        WSACleanup();
#else
    close(s);
#endif
    return result;
}

std::string preferred_ip()
{
    static std::mutex mu;
    static std::chrono::steady_clock::time_point checked{};
    static std::string cached;
    std::lock_guard<std::mutex> lock(mu);
    const auto now = std::chrono::steady_clock::now();
    if (now - checked < std::chrono::seconds(3))
        return cached;
    cached = route_source_ip();
    checked = now;
    return cached;
}

uint32_t ipv4_key(const std::string &ip)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0xFFFFFFFFu;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

int score_for(const std::string &name, const std::string &ip, bool is_virtual,
              bool is_preferred)
{
    int score = 0;
    if (is_private_ip(ip))
        score += 100;
    if (is_preferred)
        score += 60;
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

Interface make_interface(const std::string &name, const std::string &ip,
                         const std::string &preferred)
{
    Interface i;
    i.name = name;
    i.ip = ip;
    i.is_private = is_private_ip(ip);
    i.is_virtual = virtual_name(name);
    i.is_preferred = !preferred.empty() && ip == preferred;
    i.score = score_for(name, ip, i.is_virtual, i.is_preferred);
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
    const std::string preferred = preferred_ip();
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
            out.push_back(make_interface(name, ip, preferred));
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
        out.push_back(make_interface(ifa->ifa_name ? ifa->ifa_name : "", ip, preferred));
    }
    freeifaddrs(ifaddr);
#endif
    // Score first, then numeric IPv4 order. A plain string comparison puts
    // 10.0.0.100 before 10.0.0.99, which can select a stale DHCP alias.
    std::stable_sort(out.begin(), out.end(), [](const Interface &a, const Interface &b) {
        if (a.score != b.score)
            return a.score > b.score;
        const uint32_t ka = ipv4_key(a.ip);
        const uint32_t kb = ipv4_key(b.ip);
        if (ka != kb)
            return ka < kb;
        return a.name < b.name;
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
