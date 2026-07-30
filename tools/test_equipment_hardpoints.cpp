#include "equipment_hardpoints.h"

#include <cstdio>
#include <string>

int main() {
    using namespace equipment_hardpoints;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };

    Layout centurion;
    check(load("centurion", 6, centurion), "authored Centurion layout loads");
    check(centurion.ship == "centurion", "layout keeps ship identity");
    check(centurion.sprite.find("el+090") != std::string::npos,
          "layout uses the exact +90-degree sprite");
    check(centurion.zones.size() == 14, "Centurion exposes fourteen authored zones");

    int guns = 0, turrets = 0, launchers = 0;
    for (const Zone& zone : centurion.zones) {
        guns += zone.kind == Kind::Gun;
        turrets += zone.kind == Kind::Turret;
        launchers += zone.kind == Kind::Launcher;
        check(zone.rect[0] >= 0.0f && zone.rect[1] >= 0.0f &&
              zone.rect[0] + zone.rect[2] <= 1.0f &&
              zone.rect[1] + zone.rect[3] <= 1.0f,
              "zone remains inside normalized ship image");
    }
    check(guns == 4, "Centurion has four forward-gun zones");
    check(turrets == 2, "Centurion has two turret-gun zones");
    check(launchers == 2, "Centurion has left and right launcher zones");

    Layout fallback_layout;
    check(!load("tarsus", 2, fallback_layout),
          "missing authored Tarsus file reports fallback use");
    check(!fallback_layout.sprite.empty(), "fallback resolves a top-down sprite");
    check(fallback_layout.zones.size() >= 10, "fallback remains fully usable/editable");

    std::printf("\n=== %s ===\n", failures ? "FAILURES" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
