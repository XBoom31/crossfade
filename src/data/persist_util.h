#pragma once

// Legacy names kept for the persistence call sites. The implementation lives
// in json_util.h so it can be shared without pulling in the UI layer.

#include "json_util.h"

#include <cctype>
#include <ostream>
#include <string>

namespace persist
{
inline std::string json_escape(const std::string &s) { return jsonutil::escape(s); }

inline void track_to_json(std::ostream &out, const Track &t) { jsonutil::write_track(out, t); }

inline std::string sanitize_filename(const std::string &name)
{
    std::string safe = name;
    for (char &c : safe)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '_' && c != '-')
            c = '_';
    }
    return safe;
}

inline std::string extract_str_field(const std::string &json, const std::string &key)
{
    return jsonutil::get_string(json, key);
}

inline int extract_int_field(const std::string &json, const std::string &key)
{
    return jsonutil::get_int(json, key);
}
} // namespace persist
