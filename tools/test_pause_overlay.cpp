#include "pause_overlay.h"

#include <cstdio>

int main() {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* label) {
        std::printf("%-55s %s\n", label, condition ? "PASS" : "FAIL");
        if (!condition) ++failures;
    };

    check(pause_overlay::visible(true, true, false, false),
          "paused Flight shows instructions");
    check(!pause_overlay::visible(false, true, false, false),
          "unpaused Flight hides instructions");
    check(!pause_overlay::visible(true, false, false, false),
          "non-Flight mode hides instructions");
    check(!pause_overlay::visible(true, true, true, false),
          "title screen hides instructions");
    check(!pause_overlay::visible(true, true, false, true),
          "cinematic hides instructions");

    std::printf("\n%s\n", failures ? "FAIL" : "ALL PASS");
    return failures ? 1 : 0;
}
