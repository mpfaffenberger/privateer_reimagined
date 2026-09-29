#include "gun_modes.h"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* label) {
    std::printf("%-58s %s\n", label, condition ? "PASS" : "FAIL");
    if (!condition) ++failures;
}

} // namespace

int main() {
    constexpr int unique_types = 3;
    check(gun_modes::count(unique_types) == 5,
          "three gun types produce unarmed + 3 types + all");
    check(gun_modes::normalize(4, unique_types) == 4,
          "final ALL mode is reachable");
    check(gun_modes::is_all(4, unique_types),
          "final mode is identified as ALL");
    check(gun_modes::normalize(5, unique_types) == 0,
          "cycle wraps from ALL to UNARMED");
    check(gun_modes::type_index(1, unique_types) == 0 &&
          gun_modes::type_index(3, unique_types) == 2,
          "per-type modes map to every unique gun type");
    check(gun_modes::count(0) == 1 && gun_modes::is_unarmed(27, 0),
          "empty loadout has only an UNARMED mode");
    check(gun_modes::all_mode(unique_types) == 4 &&
          gun_modes::is_all(gun_modes::all_mode(1), 1),
          "all_mode names the final ALL slot for any loadout");
    check(gun_modes::all_mode(0) == 0 && gun_modes::is_unarmed(gun_modes::all_mode(0), 0),
          "empty loadout's all_mode collapses to UNARMED");

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
