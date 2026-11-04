#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Small, dependency-free helpers shared by the party server. Kept pure and
// unit-testable (tests/test_party_util.cpp).
namespace party_util
{

bool random_bytes(void *out, size_t n);

std::string base64url(const uint8_t *data, size_t n);
std::string random_token();

// Length-independent comparison for tokens/cookies.
bool constant_time_equal(const std::string &a, const std::string &b);

// Validate/replace UTF-8 and strip control characters. `max_codepoints` = 0
// means no cap.
std::string clean_utf8(const std::string &s, size_t max_codepoints = 0);

// JSON escaping for the already-clean output of clean_utf8(). Invalid UTF-8
// is replaced, so the result is always valid JSON text.
std::string json_escape(const std::string &s, size_t max_codepoints = 0);

std::string html_escape(const std::string &s);

// "#rrggbb" only; returns "#6db2d0" for anything else.
std::string safe_accent(const std::string &s);

struct TokenBucket
{
    double tokens = 0.0;
    double last = 0.0;
    double rate_per_sec = 1.0;
    double burst = 1.0;

    TokenBucket() = default;
    TokenBucket(double rate, double capacity, double now);
    bool take(double now);
    double retry_after(double now) const;
};

} // namespace party_util
