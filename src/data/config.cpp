#include "config.h"
#include "paths.h"
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <string>

static std::string g_browser = "firefox";
static std::string g_config_path;

void load_config()
{
    p100_ensure_config_dir();
    g_config_path = p100_config_path("config.ini");
    std::ifstream file(g_config_path);
    if (file.is_open())
    {
        std::string line;
        while (std::getline(file, line))
        {
            if (line.rfind("browser=", 0) == 0)
            {
                g_browser = line.substr(8);
            }
        }
    }
}

void save_config()
{
    p100_ensure_config_dir();
    std::ofstream file(g_config_path);
    if (file.is_open())
    {
        file << "browser=" << g_browser << "\n";
    }
}

std::string get_browser()
{
    return g_browser;
}

void set_browser(const std::string &browser)
{
    g_browser = browser;
}