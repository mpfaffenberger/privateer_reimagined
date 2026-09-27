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
    check(!gun_modes::participates_in_forward_cycle(true) &&
          gun_modes::participates_in_forward_cycle(false),
          "turrets are excluded from the forward-gun mode cycle");
    check(gun_modes::mount_is_armed(true, 6, false, -1),
          "auto-turret remains armed in UNARMED mode");
    check(gun_modes::mount_is_armed(true, 6, false, 3),
          "auto-turret remains armed when another gun type is selected");
    check(!gun_modes::mount_is_armed(false, 6, false, 3) &&
          gun_modes::mount_is_armed(false, 3, false, 3),
          "fixed mounts still obey selected forward-gun type");

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
