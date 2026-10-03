// -----------------------------------------------------------------------------
// tools/test_credits_format.cpp -- format_credits (src/credits_format.h, #747).
//
// Build (from repo root):
//   clang++ -std=c++20 -Isrc tools/test_credits_format.cpp -o /tmp/test_credits_format
// -----------------------------------------------------------------------------

#include "credits_format.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

static int g_fail = 0;

static void expect(int64_t in, const char* want) {
    const std::string got = format_credits(in);
    const bool ok = got == want;
    std::printf("  [%s] %lld -> \"%s\"%s%s\n", ok ? "PASS" : "FAIL", (long long)in, got.c_str(),
                ok ? "" : "  want ", ok ? "" : want);
    if (!ok) ++g_fail;
}

int main() {
    std::printf("== format_credits ==\n");
    expect(0, "0");
    expect(7, "7");
    expect(999, "999");
    expect(1000, "1,000");
    expect(2640, "2,640");
    expect(80000, "80,000");
    expect(100000, "100,000");
    expect(1234567, "1,234,567");
    expect(-1500, "-1,500");
    expect(-999, "-999");
    expect(std::numeric_limits<int64_t>::max(), "9,223,372,036,854,775,807");
    expect(std::numeric_limits<int64_t>::min(), "-9,223,372,036,854,775,808");
    std::printf("%s\n", g_fail == 0 ? "ALL CHECKS PASS" : "CHECK FAILURES");
    return g_fail == 0 ? 0 : 1;
}
