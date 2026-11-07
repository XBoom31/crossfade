#include "party_util.h"

#include "json_util.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#endif

namespace party_util
{

bool random_bytes(void *out, size_t n)
{
    if (n == 0)
        return true;
#ifdef _WIN32
    return BCryptGenRandom(nullptr, (PUCHAR)out, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    uint8_t *p = (uint8_t *)out;
    size_t done = 0;
#ifdef __linux__
    while (done < n)
    {
        ssize_t got = getrandom(p + done, n - done, 0);
        if (got > 0)
        {
            done += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        break;
    }
#endif
    if (done < n)
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0)
            return false;
        while (done < n)
        {
            ssize_t got = read(fd, p + done, n - done);
            if (got > 0)
            {
                done += (size_t)got;
                continue;
            }
            if (got < 0 && errno == EINTR)
                continue;
            break;
        }
        close(fd);
    }
    return done == n;
#endif
}

std::string base64url(const uint8_t *data, size_t n)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((n * 4 + 2) / 3);
    size_t i = 0;
    while (i + 2 < n)
    {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == n)
    {
        uint32_t v = (uint32_t)data[i] << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
    }
    else if (i + 2 == n)
    {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
    }
    return out;
}

std::string random_token()
{
    uint8_t buf[16];
    if (!random_bytes(buf, sizeof(buf)))
        return "";
    return base64url(buf, sizeof(buf)); // 22 chars for 128 bits
}

bool constant_time_equal(const std::string &a, const std::string &b)
{
    // Compare a fixed-length digest-like representation: length difference
    // still leaks only the length, never the content.
    unsigned char diff = (unsigned char)(a.size() ^ b.size());
    size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        unsigned char ca = i < a.size() ? (unsigned char)a[i] : 0;
        unsigned char cb = i < b.size() ? (unsigned char)b[i] : 0;
        diff |= (unsigned char)(ca ^ cb);
    }
    return diff == 0;
}

std::string clean_utf8(const std::string &s, size_t max_codepoints)
{
    std::string out;
    out.reserve(s.size());
    size_t cp = 0;
    for (size_t i = 0; i < s.size();)
    {
        unsigned char c = (unsigned char)s[i];
        size_t len = 0;
        uint32_t v = 0;
        if (c < 0x80)
        {
            len = 1;
            v = c;
        }
        else if ((c & 0xE0) == 0xC0)
        {
            len = 2;
            v = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            len = 3;
            v = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            len = 4;
            v = c & 0x07;
        }
        else
        {
            len = 0;
        }
        bool ok = len > 0 && i + len <= s.size();
        if (ok)
        {
            for (size_t k = 1; k < len; ++k)
            {
                unsigned char cc = (unsigned char)s[i + k];
                if ((cc & 0xC0) != 0x80)
                {
                    ok = false;
                    break;
                }
                v = (v << 6) | (cc & 0x3F);
            }
        }
        if (ok)
        {
            if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800) ||
                (len == 4 && v < 0x10000) || v > 0x10FFFF ||
                (v >= 0xD800 && v <= 0xDFFF))
                ok = false;
        }
        if (!ok)
        {
            if (max_codepoints && cp >= max_codepoints)
                break;
            out += "\xEF\xBF\xBD"; // U+FFFD
            ++cp;
            ++i;
            continue;
        }
        if (max_codepoints && cp >= max_codepoints)
            break;
        if (v < 0x20 || v == 0x7F)
            out += ' ';
        else
            out.append(s, i, len);
        ++cp;
        i += len;
    }
    return out;
}

std::string json_escape(const std::string &s, size_t max_codepoints)
{
    return jsonutil::escape(clean_utf8(s, max_codepoints));
}

std::string html_escape(const std::string &s)
{
    std::string clean = clean_utf8(s, 200);
    std::string out;
    out.reserve(clean.size() + 8);
    for (char c : clean)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string safe_accent(const std::string &s)
{
    if (s.size() != 7 || s[0] != '#')
        return "#6db2d0";
    for (size_t i = 1; i < s.size(); ++i)
    {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return "#6db2d0";
    }
    return s;
}

TokenBucket::TokenBucket(double rate, double capacity, double now)
    : tokens(capacity), last(now), rate_per_sec(rate), burst(capacity)
{
}

bool TokenBucket::take(double now)
{
    if (now < last)
        last = now;
    if (rate_per_sec > 0.0)
        tokens = std::min(burst, tokens + (now - last) * rate_per_sec);
    last = now;
    if (tokens >= 1.0)
    {
        tokens -= 1.0;
        return true;
    }
    return false;
}

double TokenBucket::retry_after(double now) const
{
    if (tokens >= 1.0 || rate_per_sec <= 0.0)
        return 0.0;
    double elapsed = std::max(0.0, now - last);
    double have = std::min(burst, tokens + elapsed * rate_per_sec);
    if (have >= 1.0)
        return 0.0;
    return (1.0 - have) / rate_per_sec;
}

} // namespace party_util
