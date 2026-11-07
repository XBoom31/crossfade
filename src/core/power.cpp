#include "power.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef P100_WINDOWS
#include <system_error>
#endif

namespace power
{
namespace
{
std::atomic<bool> g_manual{false};
std::atomic<bool> g_auto_on_battery{true};
std::atomic<bool> g_cli_override{false};
std::atomic<bool> g_on_battery{false};
bool g_battery_checked = false;
std::chrono::steady_clock::time_point g_last_battery_check{};

bool env_truthy()
{
    // Read once: this is consulted from the render path and should not
    // allocate or touch the environment every frame.
    static const bool forced = [] {
        const char *raw = std::getenv("P100_LOW_POWER");
        if (!raw || !*raw)
            return false;
        std::string v(raw);
        for (char &c : v)
            c = (char)std::tolower((unsigned char)c);
        return !(v == "0" || v == "false" || v == "off" || v == "no");
    }();
    return forced;
}

bool read_on_battery()
{
#ifdef P100_WINDOWS
    return false;
#else
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/power_supply", ec))
    {
        std::ifstream type_file(entry.path() / "type");
        std::string type;
        if (!std::getline(type_file, type) || type != "Battery")
            continue;
        std::ifstream status_file(entry.path() / "status");
        std::string status;
        if (!std::getline(status_file, status))
            continue;
        if (status == "Discharging")
            return true;
    }
    return false;
#endif
}
} // namespace

void refresh()
{
    const auto now = std::chrono::steady_clock::now();
    if (g_battery_checked && now - g_last_battery_check < std::chrono::seconds(30))
        return;
    g_battery_checked = true;
    g_last_battery_check = now;
    g_on_battery.store(read_on_battery(), std::memory_order_relaxed);
}

bool on_battery()
{
    return g_on_battery.load(std::memory_order_relaxed);
}

void configure(bool manual, bool auto_on_battery)
{
    g_manual.store(manual, std::memory_order_relaxed);
    g_auto_on_battery.store(auto_on_battery, std::memory_order_relaxed);
}

bool env_forced()
{
    return env_truthy();
}

void set_cli_override(bool on)
{
    g_cli_override.store(on, std::memory_order_relaxed);
}

bool low_power()
{
    if (env_truthy())
        return true;
    if (g_cli_override.load(std::memory_order_relaxed))
        return true;
    if (g_manual.load(std::memory_order_relaxed))
        return true;
    return g_auto_on_battery.load(std::memory_order_relaxed) &&
           g_on_battery.load(std::memory_order_relaxed);
}
} // namespace power
