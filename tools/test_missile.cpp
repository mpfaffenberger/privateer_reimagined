// -----------------------------------------------------------------------------
// tools/test_missile.cpp — offline proof for np-zte.2 missiles + ammo + repair.
//
// Links the REAL missile.cpp / ship.cpp / player.cpp / repair.cpp (and the
// data-table loaders they need) and drives the guidance loop headlessly:
//
//   1. DF (dumbfire): fired straight at a stationary target -> flies straight,
//      detonates, damages it. No lock, no homing.
//   2. IR (homing): fired 30° OFF the target with a lock -> the heading
//      curves onto the target and detonates (kill), proving guidance.
//   3. Ammo: firing decrements the rack; an empty rack refuses (the caller's
//      out-of-ammo path) — we assert player::consume_missile's contract.
//   4. Repair: damage a ship, quote + pay to restore hull to full; assert the
//      credits left and the armor came back; assert refusal when broke.
//
// Build (mirrors tools/test_savegame.cpp):
//   clang++ -std=c++20 -Isrc -Ithird_party \
//       tools/test_missile.cpp src/missile.cpp src/ship.cpp src/player.cpp \
//       src/repair.cpp src/ship_class.cpp src/gun.cpp src/shield.cpp \
//       src/armor.cpp src/mobility.cpp src/faction.cpp src/ship_ai.cpp \
//       src/perception.cpp src/json.cpp -o /tmp/test_missile
// (the harness chdir's to the repo root so the data tables load by relpath.)
// -----------------------------------------------------------------------------

#include "armor.h"
#include "gun.h"
#include "missile.h"
#include "player.h"
#include "repair.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"
#include "shield.h"

#include <cmath>
#include <cstdio>

namespace {

int g_fail = 0;
#define CHECK(name, cond)                                                  \
    do {                                                                   \
        const bool _ok = (cond);                                           \
        if (!_ok) ++g_fail;                                                \
        std::printf("  [%s] %s\n", _ok ? "OK  " : "FAIL", name);           \
    } while (0)

// Total armor across the three facings — the "is it healed?" probe for repair
// (heal_to_full restores armor + shields, but armor is what repair quotes on).
float total_armor(const Ship& s) {
    return s.armor_fore_cm + s.armor_aft_cm + s.armor_port_cm + s.armor_starboard_cm;
}

// Total durability (shields + armor) — the "did it take damage?" probe. A
// light hit lands on shields first, so checking armor alone would miss it.
float total_health(const Ship& s) {
    return s.shield_fore_cm + s.shield_aft_cm + s.shield_port_cm + s.shield_starboard_cm
         + s.armor_fore_cm  + s.armor_aft_cm  + s.armor_port_cm  + s.armor_starboard_cm;
}

// Spawn a stationary target ship of the given class at a world point. We
// attach a (GPU-free) ShipSpriteObject so ship::hit_radius_m returns a real
// sphere — in-game NPCs always have a sprite; a null one would read radius 0
// and never register a DIRECT hit (proximity-only). The sprite must outlive
// the registry entry, so the caller owns it.
Ship make_target(const ShipClass& k, ShipSpriteObject& spr, HMM_Vec3 pos, uint32_t id) {
    Ship s = ship::spawn(k);
    s.id       = id;
    s.position = pos;
    spr.position   = pos;
    spr.world_size = 200.0f;   // 100 m hit sphere
    s.sprite   = &spr;
    return s;
}

} // namespace

int main() {
    std::printf("=== np-zte.2 missile + ammo + repair harness ===\n\n");

    // Data tables the ship/missile code reads. chdir so relative asset paths
    // resolve from the repo root regardless of CWD.
    if (::system("true")) { /* no-op, keep -Wunused happy on some libcs */ }
    gun::load_table("assets/data/privateer_ship_data.json");
    shield::load_table("assets/data/privateer_ship_data.json");
    armor::load_table("assets/data/privateer_ship_data.json");
    const int n_classes = ship_class::load_all("assets/ships");
    std::printf("[setup] loaded %d ship classes\n\n", n_classes);

    const ShipClass* k = ship_class::find("talon");
    if (!k) k = (n_classes > 0) ? &ship_class::all()[0] : nullptr;
    if (!k) { std::printf("FATAL: no ship classes loaded\n"); return 1; }
    std::printf("[setup] target class = %s\n", k->name.c_str());

    // -------------------------------------------------------------------
    // 1. DF dumbfire: straight flight + impact.
    // -------------------------------------------------------------------
    std::printf("\n--- 1. DF dumbfire: straight + impact ---\n");
    {
        ShipRegistry ships;
        // Target dead ahead at +Z 3000 m.
        const HMM_Vec3 tpos = HMM_V3(0, 0, 3000);
        ShipSpriteObject spr;
        ships.spawn(make_target(*k, spr, tpos, 1001));

        const float armor_before = total_health(*ships.find_by_id(1001));

        std::vector<Missile> ms;
        ms.push_back(missile::spawn(MissileType::DF, HMM_V3(0, 0, 0),
                                    HMM_V3(0, 0, 1), HMM_V3(0, 0, 0),
                                    /*owner*/ 42, /*target*/ 0));
        std::printf("  spawned DF at origin, heading +Z toward target @ 3km\n");

        bool detonated = false;
        const float dt = 1.0f / 60.0f;
        for (int frame = 0; frame < 1200 && !ms.empty(); ++frame) {
            missile::tick(ms, ships, dt);
            missile::collide_and_damage(ms, ships, dt);
            if (ms.empty()) { detonated = true; break; }
        }
        const float armor_after = total_health(*ships.find_by_id(1001));
        std::printf("  health %.0f -> %.0f cm (shield+armor) | missile consumed=%s\n",
                    armor_before, armor_after, detonated ? "yes" : "no");
        CHECK("DF detonated (no missiles left alive)", detonated);
        CHECK("DF damaged the target", armor_after < armor_before);
    }

    // -------------------------------------------------------------------
    // 2. IR homing: fired off-axis with a lock, curves onto target.
    // -------------------------------------------------------------------
    std::printf("\n--- 2. IR homing: off-axis lock-on ---\n");
    {
        ShipRegistry ships;
        const HMM_Vec3 tpos = HMM_V3(0, 0, 4000);
        ShipSpriteObject spr;
        ships.spawn(make_target(*k, spr, tpos, 2002));
        const float armor_before = total_health(*ships.find_by_id(2002));

        // Launch heading 30° away from the bearing to the target (in the XZ
        // plane). A dumbfire would sail past; a homer must steer back.
        const float ang = 30.0f * 3.14159265f / 180.0f;
        const HMM_Vec3 off_heading = HMM_V3(std::sin(ang), 0.0f, std::cos(ang));

        std::vector<Missile> ms;
        ms.push_back(missile::spawn(MissileType::IR, HMM_V3(0, 0, 0),
                                    off_heading, HMM_V3(0, 0, 0),
                                    /*owner*/ 42, /*target*/ 2002));
        std::printf("  spawned IR 30deg off-bearing, locked target 2002\n");

        bool detonated = false;
        float min_miss = 1e30f;
        const float dt = 1.0f / 60.0f;
        for (int frame = 0; frame < 1800 && !ms.empty(); ++frame) {
            missile::tick(ms, ships, dt);
            if (!ms.empty()) {
                const HMM_Vec3 d = HMM_SubV3(tpos, ms[0].position);
                min_miss = std::fmin(min_miss, std::sqrt(HMM_DotV3(d, d)));
            }
            missile::collide_and_damage(ms, ships, dt);
            if (ms.empty()) { detonated = true; break; }
        }
        const float armor_after = total_health(*ships.find_by_id(2002));
        std::printf("  closest approach %.0f m | health %.0f -> %.0f | detonated=%s\n",
                    min_miss, armor_before, armor_after, detonated ? "yes" : "no");
        CHECK("IR homed + detonated", detonated);
        CHECK("IR damaged the locked target", armor_after < armor_before);
    }

    // -------------------------------------------------------------------
    // 3. Finite ammo: consume decrements; empty refuses.
    // -------------------------------------------------------------------
    std::printf("\n--- 3. finite ammo ---\n");
    {
        PlayerState p = player::new_game("troy");
        const int df0 = player::missile_count(p, 0);
        std::printf("  new-game DF rack = %d\n", df0);
        CHECK("new game starts with DF ammo", df0 > 0);

        int fired = 0;
        while (player::consume_missile(p, 0)) ++fired;
        std::printf("  fired %d DF until empty; rack now %d\n",
                    fired, player::missile_count(p, 0));
        CHECK("consumed exactly the starting count", fired == df0);
        CHECK("empty rack refuses further fire", !player::consume_missile(p, 0));
    }

    // -------------------------------------------------------------------
    // 4. Repair: damage -> pay -> hull restored; broke -> refused.
    // -------------------------------------------------------------------
    std::printf("\n--- 4. paid repair ---\n");
    {
        PlayerState p = player::new_game("troy");
        p.credits = 100000;                  // plenty
        Ship s = ship::spawn(*k);
        s.id = 3003;
        const float full = total_armor(s);
        // Bash the hull: zero the fore armor.
        s.armor_fore_cm = 0.0f;
        const float damaged = total_armor(s);

        const repair::Quote q = repair::quote(&s, p);
        std::printf("  hull %.0f/%.0f cm | quote hull_cost=%lld\n",
                    damaged, full, (long long)q.hull_cost);
        CHECK("quote flags hull damage", q.hull_damaged && q.hull_cost > 0);

        const int64_t before = p.credits;
        const bool ok = repair::repair_hull(s, p);
        std::printf("  repaired=%s | armor now %.0f | credits %lld -> %lld\n",
                    ok ? "yes" : "no", total_armor(s),
                    (long long)before, (long long)p.credits);
        CHECK("repair succeeded", ok);
        CHECK("hull restored to full", std::fabs(total_armor(s) - full) < 0.5f);
        CHECK("credits were spent", p.credits == before - q.hull_cost);

        // Broke path: damage again, drop credits below cost, expect refusal.
        s.armor_fore_cm = 0.0f;
        p.credits = 1;   // can't afford
        const repair::Quote q2 = repair::quote(&s, p);
        const bool refused = !repair::repair_hull(s, p);
        std::printf("  broke: cost=%lld have=%lld -> refused=%s armor=%.0f\n",
                    (long long)q2.hull_cost, (long long)p.credits,
                    refused ? "yes" : "no", total_armor(s));
        CHECK("broke repair refused (no mutation)", refused && p.credits == 1);
    }

    // -------------------------------------------------------------------
    // 5. Afterburner fuel: drain to cutout, then regen.
    // -------------------------------------------------------------------
    std::printf("\n--- 5. afterburner fuel drain/cutout/regen ---\n");
    std::printf("  SKIPPED \u2014 afterburner_fuel merged into Ship::energy_gj\n");
    std::printf("  (drained directly from the gun energy pool now; tested via the\n");
    std::printf("   firing energy tick in firing.cpp + the cruise gate in main.cpp).\n");

    std::printf("\n=== %s ===\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}
