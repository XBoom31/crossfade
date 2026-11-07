#include "party_link.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace party_link
{
namespace
{

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a]))
        ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1]))
        --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

bool is_host_char(char c)
{
    return std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':';
}

std::vector<std::string> tokens(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && std::isspace((unsigned char)s[i]))
            ++i;
        size_t start = i;
        while (i < s.size() && !std::isspace((unsigned char)s[i]))
            ++i;
        if (i > start)
        {
            std::string t = s.substr(start, i - start);
            // Strip wrapping punctuation/quotes so "check this: <url>," works.
            size_t a = 0, b = t.size();
            const char *punct = "\"'()[]{}<>,.;!";
            while (a < b && std::strchr(punct, t[a]))
                ++a;
            while (b > a && std::strchr(punct, t[b - 1]))
                --b;
            t = t.substr(a, b - a);
            if (!t.empty())
                out.push_back(std::move(t));
        }
    }
    return out;
}

std::string query_value(const std::string &query, const std::string &key)
{
    size_t pos = 0;
    while (pos < query.size())
    {
        size_t end = query.find('&', pos);
        std::string part = query.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == key)
            return part.substr(eq + 1);
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return "";
}

Parsed make(Result r, const std::string &id, const std::string &msg)
{
    Parsed p;
    p.result = r;
    p.video_id = id;
    p.message = msg;
    return p;
}

Parsed parse_candidate(const std::string &token)
{
    std::string s = token;
    size_t scheme = s.find("://");
    if (scheme != std::string::npos)
        s = s.substr(scheme + 3);
    else if (s.rfind("//", 0) == 0)
        s = s.substr(2);

    size_t host_end = 0;
    while (host_end < s.size() && is_host_char(s[host_end]))
        ++host_end;
    std::string host = lower(s.substr(0, host_end));
    size_t colon = host.find(':');
    if (colon != std::string::npos)
        host = host.substr(0, colon);

    const bool is_short = host == "youtu.be";
    const bool is_youtube = host == "youtube.com" || host == "www.youtube.com" ||
                            host == "m.youtube.com" || host == "music.youtube.com";
    if (!is_short && !is_youtube)
        return make(Result::NotYoutube, "", "That doesn't look like a YouTube link.");

    std::string rest = s.substr(host_end);
    std::string query;
    size_t q = rest.find('?');
    if (q != std::string::npos)
    {
        query = rest.substr(q + 1);
        size_t h = query.find('#');
        if (h != std::string::npos)
            query.resize(h);
        rest.resize(q);
    }
    else
    {
        size_t h = rest.find('#');
        if (h != std::string::npos)
            rest.resize(h);
    }

    std::vector<std::string> segs;
    {
        size_t pos = 0;
        while (pos < rest.size())
        {
            while (pos < rest.size() && rest[pos] == '/')
                ++pos;
            if (pos >= rest.size())
                break;
            size_t end = rest.find('/', pos);
            segs.push_back(rest.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
    }

    std::string vid;
    if (is_short)
    {
        if (!segs.empty())
            vid = segs[0];
        if (vid.empty())
            return make(Result::Channel, "", "That's a channel link. Paste a link to a single song.");
    }
    else
    {
        if (!segs.empty() && segs[0] == "playlist")
            return make(Result::PlaylistOnly, "", "Playlists aren't supported yet — add one song at a time.");
        if (segs.empty() || segs[0] == "watch")
            vid = query_value(query, "v");
        else if (segs[0] == "shorts" || segs[0] == "embed" || segs[0] == "live")
            vid = segs.size() > 1 ? segs[1] : "";
        else if (segs[0] == "channel" || segs[0] == "c" || segs[0] == "user" ||
                 (!segs[0].empty() && segs[0][0] == '@'))
            return make(Result::Channel, "", "That's a channel link. Paste a link to a single song.");
        else
            vid = query_value(query, "v");
        if (vid.empty())
            return make(Result::InvalidId, "", "That link is missing a valid video id.");
    }

    if (!valid_video_id(vid))
        return make(Result::InvalidId, "", "That link is missing a valid video id.");
    return make(Result::Ok, vid, "");
}

} // namespace

bool valid_video_id(const std::string &id)
{
    if (id.size() != 11)
        return false;
    for (char c : id)
    {
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-'))
            return false;
    }
    return true;
}

Parsed parse(const std::string &input)
{
    if (input.size() > 2048)
        return make(Result::TooLong, "", "That input is too long.");
    std::string s = trim(input);
    if (s.empty())
        return make(Result::Empty, "", "Paste a YouTube link first.");
    if (valid_video_id(s))
        return make(Result::Ok, s, "");

    Parsed best = make(Result::NotYoutube, "", "That doesn't look like a YouTube link.");
    bool saw_youtube = false;
    for (const std::string &t : tokens(s))
    {
        std::string lower_t = lower(t);
        if (valid_video_id(t))
            return make(Result::Ok, t, "");
        if (lower_t.find("youtu.be") == std::string::npos &&
            lower_t.find("youtube.com") == std::string::npos)
            continue;
        saw_youtube = true;
        Parsed p = parse_candidate(t);
        if (p.result == Result::Ok)
            return p;
        // Prefer the most specific message over the generic "not a link".
        if (p.result != Result::NotYoutube && best.result == Result::NotYoutube)
            best = p;
    }
    if (!saw_youtube)
    {
        // One last chance: text with an embedded URL glued to punctuation.
        size_t pos = s.find("youtu");
        if (pos != std::string::npos)
        {
            Parsed p = parse_candidate(s.substr(pos));
            if (p.result == Result::Ok)
                return p;
            if (p.result != Result::NotYoutube)
                best = p;
        }
    }
    return best;
}

} // namespace party_link
