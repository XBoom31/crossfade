#pragma once

#include "ui.h"

#include <cctype>
#include <cstdio>
#include <ostream>
#include <string>

// Small JSON helpers shared by the store and playlist persistence layers.
// Header-only so the split files don't need a new link dependency.
namespace persist
{
inline std::string json_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
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
            if ((unsigned char)c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += c;
        }
    }
    return out;
}

inline void track_to_json(std::ostream &out, const Track &t)
{
    out << "{\"id\":\"" << json_escape(t.id)
        << "\",\"title\":\"" << json_escape(t.title)
        << "\",\"artist\":\"" << json_escape(t.artist)
        << "\",\"artist_id\":\"" << json_escape(t.artist_id)
        << "\",\"album\":\"" << json_escape(t.album)
        << "\",\"album_id\":\"" << json_escape(t.album_id)
        << "\",\"track_number\":" << t.track_number
        << ",\"thumbnail_url\":\"" << json_escape(t.thumbnail_url)
        << "\",\"duration_sec\":" << t.duration_sec
        << ",\"url\":\"" << json_escape(t.url) << "\"}";
}

inline std::string sanitize_filename(const std::string &name)
{
    std::string safe = name;
    for (char &c : safe)
    {
        if (!std::isalnum((unsigned char)c) && c != ' ' && c != '_' && c != '-')
            c = '_';
    }
    return safe;
}

inline std::string extract_str_field(const std::string &json, const std::string &key)
{
    size_t pos = json.find("\"" + key + "\":\"");
    if (pos == std::string::npos)
        return "";
    pos += key.size() + 4;
    std::string out;
    bool escaped = false;
    while (pos < json.size())
    {
        char c = json[pos++];
        if (escaped)
        {
            switch (c)
            {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default: out += c;
            }
            escaped = false;
        }
        else if (c == '\\')
            escaped = true;
        else if (c == '"')
            break;
        else
            out += c;
    }
    return out;
}

inline int extract_int_field(const std::string &json, const std::string &key)
{
    size_t pos = json.find("\"" + key + "\":");
    if (pos == std::string::npos)
        return 0;
    pos += key.size() + 3;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    size_t end = json.find_first_of(",}", pos);
    std::string val = json.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    try
    {
        return std::stoi(val);
    }
    catch (...)
    {
        return 0;
    }
}
} // namespace persist
