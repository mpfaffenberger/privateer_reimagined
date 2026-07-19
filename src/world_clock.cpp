// -----------------------------------------------------------------------------
// world_clock.cpp — Confed-stardate arithmetic. See world_clock.h.
// -----------------------------------------------------------------------------

#include "world_clock.h"

#include <algorithm>
#include <cstdio>

namespace world_clock {

namespace {
constexpr int k_days_per_year = 365;
constexpr int k_days_per_quarter = 91;
}

std::string stardate_string(int day) {
    const int elapsed = std::max(0, day);
    const int absolute_day = (k_epoch_doy - 1) + elapsed;
    const int year = k_epoch_year + absolute_day / k_days_per_year;
    const int day_of_year = absolute_day % k_days_per_year + 1;

    char out[16];
    std::snprintf(out, sizeof(out), "%d.%03d", year, day_of_year);
    return out;
}

int quarter(int day) {
    const int elapsed = std::max(0, day);
    const int day_in_year = elapsed % k_days_per_year;
    return std::min(4, day_in_year / k_days_per_quarter + 1);
}

} // namespace world_clock
