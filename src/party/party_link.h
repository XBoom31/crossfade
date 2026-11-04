#pragma once

#include <string>

// Pure YouTube link/ID parsing for party mode. No I/O, no globals: all of
// this is unit-tested from tests/test_party_link.cpp.
namespace party_link
{
enum class Result
{
    Ok,
    Empty,
    TooLong,
    NotYoutube,
    PlaylistOnly,
    Channel,
    InvalidId,
};

struct Parsed
{
    Result result = Result::Empty;
    std::string video_id;
    // Short, guest-facing message. Never contains internals.
    std::string message;
};

// Extract an 11-character YouTube video id from a paste. Accepts full URLs,
// scheme-less URLs, a bare id, and text containing a URL (first valid one).
Parsed parse(const std::string &input);

// Strict ^[A-Za-z0-9_-]{11}$ check. This is the only shape ever forwarded
// to the backend or used to build a URL.
bool valid_video_id(const std::string &id);

} // namespace party_link
