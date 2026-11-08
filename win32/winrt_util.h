#pragma once

// Minimal WinRT plumbing shared by the Windows desktop-integration modules
// (System Media Transport Controls and toast notifications).
//
// The project deliberately avoids external C++ dependencies, so this uses the
// raw WinRT ABI headers that ship with mingw-w64 plus a tiny ComPtr/HString
// pair instead of C++/WinRT or WRL. Everything here is header-only and links
// against Windows' runtimeobject.lib (RoInitialize / RoGetActivationFactory /
// HSTRING helpers).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winstring.h>
#include <inspectable.h>
#include <roapi.h>

#include <cstdio>
#include <string>
#include <utility>

namespace cfwin
{

// UTF-8 <-> UTF-16. The app keeps every string in UTF-8, WinRT wants UTF-16.
inline std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string narrow(const wchar_t *s, int len = -1)
{
    if (!s)
        return {};
    if (len < 0)
        len = (int)wcslen(s);
    if (len == 0)
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
        return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    return out;
}

inline std::string hstring_to_utf8(HSTRING s)
{
    if (!s)
        return {};
    UINT32 len = 0;
    const wchar_t *raw = WindowsGetStringRawBuffer(s, &len);
    return narrow(raw, (int)len);
}

// RAII wrapper around a WinRT HSTRING.
class HString
{
public:
    HString() = default;

    explicit HString(const std::wstring &w)
    {
        WindowsCreateString(w.c_str(), (UINT32)w.size(), &m_str);
    }

    explicit HString(const std::string &utf8)
        : HString(widen(utf8))
    {
    }

    HString(const HString &) = delete;
    HString &operator=(const HString &) = delete;

    HString(HString &&other) noexcept : m_str(other.m_str) { other.m_str = nullptr; }

    HString &operator=(HString &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_str = other.m_str;
            other.m_str = nullptr;
        }
        return *this;
    }

    ~HString() { reset(); }

    void reset()
    {
        if (m_str)
            WindowsDeleteString(m_str);
        m_str = nullptr;
    }

    HSTRING get() const { return m_str; }
    bool valid() const { return m_str != nullptr; }

private:
    HSTRING m_str = nullptr;
};

// Minimal intrusive COM smart pointer (no WRL dependency).
template <class T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    ComPtr(ComPtr &&other) noexcept : m_p(other.m_p) { other.m_p = nullptr; }

    ComPtr &operator=(ComPtr &&other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_p = other.m_p;
            other.m_p = nullptr;
        }
        return *this;
    }

    ~ComPtr() { reset(); }

    T *get() const { return m_p; }
    T *operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }

    // Out-parameter helper: releases the current object and returns its slot.
    T **put()
    {
        reset();
        return &m_p;
    }

    void **put_void() { return reinterpret_cast<void **>(put()); }

    void attach(T *p)
    {
        reset();
        m_p = p;
    }

    T *detach()
    {
        T *p = m_p;
        m_p = nullptr;
        return p;
    }

    void reset()
    {
        if (m_p)
            m_p->Release();
        m_p = nullptr;
    }

private:
    T *m_p = nullptr;
};

inline const char *hr_hex(HRESULT hr, char *buf, size_t size)
{
    snprintf(buf, size, "0x%08lx", (unsigned long)hr);
    return buf;
}

inline void log_failure(const char *what, HRESULT hr)
{
    char buf[16];
    fprintf(stderr, "[crossfade/winrt] %s failed: %s\n", what, hr_hex(hr, buf, sizeof(buf)));
}

// Turn a local path into a file:/// URI, pass through http(s)/file URIs and
// percent-encode the characters Windows' toast/SMTC parsers refuse raw.
inline std::string to_uri(const std::string &path_or_url)
{
    if (path_or_url.empty())
        return {};

    if (path_or_url.rfind("http://", 0) == 0 ||
        path_or_url.rfind("https://", 0) == 0 ||
        path_or_url.rfind("file://", 0) == 0)
        return path_or_url;

    std::string p = path_or_url;
    for (char &c : p)
        if (c == '\\')
            c = '/';

    std::string out;
    if (p.size() >= 2 && p[1] == ':')
        out = "file:///";
    else if (!p.empty() && p[0] == '/')
        out = "file://";
    else
        out = "file:///";

    static const char *hex = "0123456789ABCDEF";
    for (unsigned char c : p)
    {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                           c == '_' || c == '~' || c == '/' || c == ':';
        if (plain)
        {
            out += (char)c;
        }
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// "audio-x-generic" and friends are freedesktop icon names, not files. The
// Windows toast layer must ignore them instead of trying to load a URI.
inline bool looks_like_icon_name(const std::string &s)
{
    return !s.empty() && s.find('/') == std::string::npos &&
           s.find('\\') == std::string::npos && s.find('.') == std::string::npos &&
           s.find(':') == std::string::npos;
}

inline std::string xml_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c; break;
        }
    }
    return out;
}

} // namespace cfwin
