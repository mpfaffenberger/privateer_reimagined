// -----------------------------------------------------------------------------
// test_world_clock.cpp — headless proof for the Gemini Lives calendar.
// -----------------------------------------------------------------------------

#include "world_clock.h"

#include <cstdio>
#include <string>

namespace {
int failures = 0;

template <typename T>
void expect(const char* name, const T& got, const T& want) {
    const bool ok = got == want;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
} // namespace

int main() {
    using world_clock::quarter;
    using world_clock::stardate_string;

    expect("epoch", stardate_string(0), std::string{"2669.135"});
    expect("Concordia news date", stardate_string(108), std::string{"2669.243"});
    expect("last day of epoch year", stardate_string(230), std::string{"2669.365"});
    expect("year rollover", stardate_string(231), std::string{"2670.001"});
    expect("second year", stardate_string(596), std::string{"2671.001"});
    expect("negative day clamps", stardate_string(-1), std::string{"2669.135"});

    expect("quarter 1 starts", quarter(0), 1);
    expect("quarter 1 ends", quarter(90), 1);
    expect("quarter 2 starts", quarter(91), 2);
    expect("quarter 3 starts", quarter(182), 3);
    expect("quarter 4 starts", quarter(273), 4);
    expect("last day remains Q4", quarter(364), 4);
    expect("quarter wraps with year", quarter(365), 1);

    std::printf("\n=== %s ===\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
