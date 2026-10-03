#pragma once
// -----------------------------------------------------------------------------
// credits_format.h -- player-facing credit amounts (#747).
//
// One spelling for money everywhere a screen shows it: thousands grouped with
// commas ("80,000"), so a 5-digit price can't be misread as a 4-digit one.
// Pure + header-only so headless tests and any UI can share it.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>

// 80000 -> "80,000"; -1500 -> "-1,500". Safe for INT64_MIN.
inline std::string format_credits(int64_t amount) {
    const bool negative = amount < 0;
    const uint64_t magnitude = negative ? 0ull - (uint64_t)amount : (uint64_t)amount;
    const std::string digits = std::to_string(magnitude);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3 + 1);
    if (negative) out.push_back('-');
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}
