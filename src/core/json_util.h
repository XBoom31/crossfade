#pragma once

// Small, dependency-free JSON helpers shared by persistence, networking,
// playback and party code. Header-only: callers must not need the UI layer
// just to escape a string or read a file.

#include "ytdlp.h" // Track

#include <cstdio>
#include <fstream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>

namespace jsonutil
{
inline void append_utf8(std::string &out, unsigned cp)
{
    if (cp <= 0x7F)
        out += static_cast<char>(cp);
    else if (cp <= 0x7FF)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0xFFFF)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

inline std::string escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else
                out += c;
        }
    }
    return out;
}

// Find `"key"` and return the parsed string value that follows it. Returns an
// empty string when the key is absent or malformed. Handles the common JSON
// escapes plus \uXXXX sequences.
inline std::string get_string(const std::string &json, const std::string &key,
                              const std::string &fallback = "")
{
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos)
    {
        search = "\"" + key + "\": \"";
        pos = json.find(search);
    }
    if (pos == std::string::npos)
        return fallback;
    pos += search.size();
    std::string out;
    while (pos < json.size())
    {
        char c = json[pos++];
        if (c == '"')
            return out;
        if (c != '\\')
        {
            out += c;
            continue;
        }
        if (pos >= json.size())
            break;
        char e = json[pos++];
        switch (e)
        {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'u':
        {
            if (pos + 4 > json.size())
                return out;
            unsigned cp = 0;
            for (int i = 0; i < 4; ++i)
            {
                char h = json[pos++];
                cp <<= 4;
                if (h >= '0' && h <= '9')
                    cp |= static_cast<unsigned>(h - '0');
                else if (h >= 'a' && h <= 'f')
                    cp |= static_cast<unsigned>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                    cp |= static_cast<unsigned>(h - 'A' + 10);
            }
            append_utf8(out, cp);
            break;
        }
        default: out += e;
        }
    }
    return out;
}

inline int get_int(const std::string &json, const std::string &key, int fallback = 0)
{
    size_t pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos)
        return fallback;
    pos += key.size() + 3;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    size_t end = json.find_first_of(",}", pos);
    try
    {
        return std::stoi(json.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    }
    catch (...)
    {
        return fallback;
    }
}

inline double get_double(const std::string &json, const std::string &key, double fallback = 0.0)
{
    size_t pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos)
        return fallback;
    pos += key.size() + 3;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    size_t end = json.find_first_of(",}", pos);
    try
    {
        return std::stod(json.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
    }
    catch (...)
    {
        return fallback;
    }
}

inline bool get_bool(const std::string &json, const std::string &key, bool fallback = false)
{
    size_t pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos)
        return fallback;
    pos += key.size() + 3;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (json.compare(pos, 4, "true") == 0)
        return true;
    if (json.compare(pos, 5, "false") == 0)
        return false;
    return fallback;
}

inline void write_track(std::ostream &out, const Track &t)
{
    out << "{\"id\":\"" << escape(t.id)
        << "\",\"title\":\"" << escape(t.title)
        << "\",\"artist\":\"" << escape(t.artist)
        << "\",\"artist_id\":\"" << escape(t.artist_id)
        << "\",\"album\":\"" << escape(t.album)
        << "\",\"album_id\":\"" << escape(t.album_id)
        << "\",\"track_number\":" << t.track_number
        << ",\"thumbnail_url\":\"" << escape(t.thumbnail_url)
        << "\",\"duration_sec\":" << t.duration_sec
        << ",\"url\":\"" << escape(t.url) << "\"}";
}

inline std::string track_json(const Track &t)
{
    std::ostringstream out;
    write_track(out, t);
    return out.str();
}

template <typename Path>
inline std::string read_file(const Path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}
} // namespace jsonutil
