#pragma once

// Small helper for writes that must never leave a half-written file behind.
// A crash between fopen() and fclose() otherwise leaves session.json or a
// thumbnail with a valid-looking name but truncated bytes, which can make the
// next launch paint the wrong cover. Write to a unique sibling temp file and
// rename it over the final path only after the full payload is on disk.

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#ifdef P100_WINDOWS
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace atomic_file
{
inline bool replace_bytes(const std::string &path, const void *data, size_t size)
{
    if (path.empty())
        return false;

    static std::atomic<unsigned long long> seq{0};
#ifdef P100_WINDOWS
    const long pid = _getpid();
#else
    const long pid = (long)::getpid();
#endif
    const std::string temp = path + ".tmp." + std::to_string(pid) + "." +
                             std::to_string(seq.fetch_add(1) + 1);

    FILE *f = std::fopen(temp.c_str(), "wb");
    if (!f)
        return false;
    const bool wrote = size == 0 || std::fwrite(data, 1, size, f) == size;
    if (!wrote || std::fflush(f) != 0)
    {
        std::fclose(f);
        std::remove(temp.c_str());
        return false;
    }
#ifndef P100_WINDOWS
    // Best effort: filesystems that do not support fsync simply ignore it.
    ::fsync(::fileno(f));
#endif
    if (std::fclose(f) != 0)
    {
        std::remove(temp.c_str());
        return false;
    }

#ifdef P100_WINDOWS
    const BOOL ok = MoveFileExA(temp.c_str(), path.c_str(),
                                MOVEFILE_REPLACE_EXISTING |
                                    MOVEFILE_WRITE_THROUGH);
    if (!ok)
    {
        std::remove(temp.c_str());
        return false;
    }
#else
    if (::rename(temp.c_str(), path.c_str()) != 0)
    {
        std::remove(temp.c_str());
        return false;
    }
#endif
    return true;
}

inline bool replace_text(const std::string &path, const std::string &text)
{
    return replace_bytes(path, text.data(), text.size());
}

inline bool replace_bytes(const std::string &path,
                          const std::vector<unsigned char> &data)
{
    return replace_bytes(path, data.data(), data.size());
}
} // namespace atomic_file
