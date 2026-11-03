#include "accent.h"
#include "ui.h"
#include "daily_accent.h"

#ifndef P100_WINDOWS
#include <gio/gio.h>
#endif
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

ImVec4 g_accent(0.43f, 0.70f, 0.81f, 1.0f);
ImVec4 g_accent_hover(0.56f, 0.78f, 0.88f, 1.0f);
ImVec4 g_accent_active(0.70f, 0.85f, 0.93f, 1.0f);
ImVec4 g_accent_text(0.70f, 0.85f, 0.93f, 1.0f);

static ImVec4 accent_mix_white(const ImVec4 &c, float t)
{
    return ImVec4(c.x + (1.0f - c.x) * t,
                  c.y + (1.0f - c.y) * t,
                  c.z + (1.0f - c.z) * t,
                  1.0f);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (char)std::tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

bool accent_parse_hex(const char *text, ImVec4 &out)
{
    if (!text || text[0] != '#' || strlen(text) != 7)
        return false;
    int values[6];
    for (int i = 0; i < 6; ++i)
    {
        values[i] = hex_digit(text[i + 1]);
        if (values[i] < 0)
            return false;
    }
    out = ImVec4((values[0] * 16 + values[1]) / 255.0f,
                 (values[2] * 16 + values[3]) / 255.0f,
                 (values[4] * 16 + values[5]) / 255.0f,
                 1.0f);
    return true;
}

static int color_channel_255(float value)
{
    int v = (int)(value * 255.0f + 0.5f);
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

std::string accent_to_hex(const ImVec4 &color)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x", color_channel_255(color.x),
             color_channel_255(color.y), color_channel_255(color.z));
    return std::string(buf);
}

// Read the desktop accent colour from the XDG desktop portal. Plasma and
// recent GNOME expose it as (ddd) under org.freedesktop.appearance.
#ifndef P100_WINDOWS
static bool read_portal_accent(ImVec4 &out)
{
    GError *error = nullptr;
    GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!connection)
    {
        if (error)
            g_error_free(error);
        return false;
    }

    GVariant *reply = g_dbus_connection_call_sync(
        connection,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.Settings",
        "Read",
        g_variant_new("(ss)", "org.freedesktop.appearance", "accent-color"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        250,
        nullptr,
        &error);

    g_object_unref(connection);

    if (!reply)
    {
        if (error)
            g_error_free(error);
        return false;
    }

    bool ok = false;
    GVariant *variant = g_variant_get_child_value(reply, 0);
    GVariant *inner = g_variant_get_variant(variant);
    if (g_variant_is_of_type(inner, G_VARIANT_TYPE("(ddd)")))
    {
        double r = 0.0, g = 0.0, b = 0.0;
        g_variant_get(inner, "(ddd)", &r, &g, &b);
        out = ImVec4((float)r, (float)g, (float)b, 1.0f);
        ok = true;
    }

    g_variant_unref(inner);
    g_variant_unref(variant);
    g_variant_unref(reply);
    return ok;
}
#endif // !P100_WINDOWS

void update_accent(const Settings &settings)
{
    static bool portal_available = true;
    ImVec4 base;
    bool have_color = false;

    if (const char *override_color = getenv("P100_ACCENT"))
        have_color = accent_parse_hex(override_color, base);

    // Explicit choice from Settings takes precedence over the desktop theme.
    if (!have_color && !settings.accent_color.empty())
        have_color = accent_parse_hex(settings.accent_color.c_str(), base);

#ifndef P100_WINDOWS
    if (!have_color && portal_available)
    {
        if (read_portal_accent(base))
            have_color = true;
        else
            portal_available = false;
    }
#endif

    if (!have_color)
    {
        time_t now = time(nullptr);
        struct tm tmv;
        int r = 110, g = 178, b = 255;
#ifdef P100_WINDOWS
        bool have_tm = now != (time_t)-1 && localtime_s(&tmv, &now) == 0;
#else
        bool have_tm = now != (time_t)-1 && localtime_r(&now, &tmv) != nullptr;
#endif
        if (have_tm)
            accent_rgb(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, &r, &g, &b);
        base = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    }

    g_accent = base;
    g_accent_hover = accent_mix_white(base, 0.18f);
    g_accent_active = accent_mix_white(base, 0.32f);
    g_accent_text = accent_mix_white(base, 0.45f);
}
