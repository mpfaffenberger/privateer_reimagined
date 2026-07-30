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
    check(centurion.zones.size() == 8,
          "Centurion authors only eight physical weapon hardpoints");

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

    // Editor resizing one zone must propagate to every marker in that category.
    set_category_dimensions(centurion, Kind::Gun, 0.08f, 0.06f);
    bool gun_sizes_match = true;
    for (const Zone& zone : centurion.zones)
        if (zone.kind == Kind::Gun || zone.kind == Kind::Turret)
            gun_sizes_match &= zone.rect[2] == 0.08f && zone.rect[3] == 0.06f;
    check(gun_sizes_match,
          "resizing a gun normalizes forward and turret gun markers");

    centurion.zones[1].rect[2] = 0.22f;
    centurion.zones[1].rect[3] = 0.14f;
    normalize_dimensions(centurion);
    check(centurion.zones[1].rect[2] == centurion.zones[0].rect[2] &&
          centurion.zones[1].rect[3] == centurion.zones[0].rect[3],
          "layout normalization repairs inconsistent authored dimensions");

    Layout fallback_layout;
    check(!load("tarsus", 2, fallback_layout),
          "missing authored Tarsus file reports fallback use");
    check(!fallback_layout.sprite.empty(), "fallback resolves a top-down sprite");
    check(fallback_layout.zones.size() == 4,
          "two-gun fallback contains guns plus left/right launchers");
    bool physical_only = true;
    for (const Zone& zone : fallback_layout.zones)
        physical_only &= is_physical_hardpoint(zone.kind);
    check(physical_only, "fallback exposes physical hardpoints only");

    std::printf("\n=== %s ===\n", failures ? "FAILURES" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
