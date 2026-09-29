// -----------------------------------------------------------------------------
// tools/test_ship_systems.cpp — proof for per-component damage (#141).
//
//   1. Pure model (ship_systems.h): facing -> candidate pick, installed mask,
//      proportional integrity loss, effect curves, snapshot clamping.
//   2. Real damage pipeline (ship.cpp): shields soak without touching
//      components, a penetrating hit chips exactly one, heal_to_full fixes
//      components but keeps the installed mask, a dead shield gen stops
//      regen, a dead radar shrinks perception::radar_range_m.
//   3. Repair desk (repair.cpp): per-component quote, paid repair_system,
//      refusal when broke, and repair_hull leaving components alone.
//
// Hand-built ShipClass/ShieldType, so no asset tables are loaded.
//   cmake --build build --target test_ship_systems && ./build/test_ship_systems
// -----------------------------------------------------------------------------

#include "perception.h"
#include "player.h"
#include "repair.h"
#include "shield.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_systems.h"

#include <cmath>
#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const char* label) {
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", label);
    if (!ok) ++g_fail;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

float total_missing(const ShipSystems& ss) {
    float m = 0.0f;
    for (float v : ss.integrity) m += 1.0f - v;
    return m;
}

void test_pure_model() {
    std::printf("\n--- 1. pure model ---\n");
    using namespace ship_systems;

    ShipSystems ss;
    check(!any_damaged(ss) && ss.integrity == k_pristine, "default is pristine");

    check(pick(ss, HitFacing::Fore, 0.0f)  == ShipSystem::Guns,      "fore low roll -> guns");
    check(pick(ss, HitFacing::Fore, 0.99f) == ShipSystem::Tractor,   "fore high roll -> tractor");
    check(pick(ss, HitFacing::Aft, 0.5f)   == ShipSystem::JumpDrive, "aft mid roll -> jump drive");
    check(pick(ss, HitFacing::Port, 0.0f)  == ShipSystem::Launchers, "flank low roll -> launchers");
    check(pick(ss, HitFacing::Aft, 1.0f)   == ShipSystem::ShieldGen, "roll of exactly 1 clamps to last");

    // Every system must sit behind at least one facing's armor.
    bool reachable[kShipSystemCount] = {};
    for (HitFacing f : { HitFacing::Fore, HitFacing::Aft, HitFacing::Port, HitFacing::Starboard }) {
        const FacingCandidates c = candidates(f);
        for (int i = 0; i < c.n; ++i) reachable[(int)c.list[i]] = true;
    }
    bool all = true;
    for (bool r : reachable) all = all && r;
    check(all, "every system reachable from some facing");

    // Uninstalled hardware can't be hit; the roll spreads over what's left.
    ShipSystems no_jump;
    set_installed(no_jump, ShipSystem::JumpDrive, false);
    check(pick(no_jump, HitFacing::Aft, 0.4f)  == ShipSystem::Engines,   "no drive: aft low -> engines");
    check(pick(no_jump, HitFacing::Aft, 0.99f) == ShipSystem::ShieldGen, "no drive: aft high -> shield gen");

    ShipSystems bare_nose;
    set_installed(bare_nose, ShipSystem::Guns, false);
    set_installed(bare_nose, ShipSystem::Radar, false);
    set_installed(bare_nose, ShipSystem::Tractor, false);
    check(apply_hit(bare_nose, HitFacing::Fore, 5.0f, 0.5f) == ShipSystem::Count &&
          bare_nose.integrity == k_pristine,
          "nothing fitted behind a facing -> pure hull damage");

    ShipSystems hit;
    const ShipSystem s1 = apply_hit(hit, HitFacing::Fore, 5.0f, 0.0f);
    check(s1 == ShipSystem::Guns &&
          near(integrity(hit, ShipSystem::Guns), 1.0f - 5.0f * k_integrity_loss_per_cm),
          "5 cm through -> guns lose 5 * k_integrity_loss_per_cm");
    check(operational(hit, ShipSystem::Guns) && damaged(hit, ShipSystem::Guns),
          "partially damaged guns still operational");
    apply_hit(hit, HitFacing::Fore, 100.0f, 0.0f);
    check(integrity(hit, ShipSystem::Guns) == 0.0f && !operational(hit, ShipSystem::Guns),
          "overkill clamps to exactly 0 -> destroyed");
    check(apply_hit(hit, HitFacing::Fore, 0.0f, 0.5f) == ShipSystem::Count,
          "zero penetration damages nothing");

    // Frame-rate independence: many small hits == one big one.
    ShipSystems drip, lump;
    for (int i = 0; i < 60; ++i) apply_hit(drip, HitFacing::Aft, 0.1f, 0.0f);
    apply_hit(lump, HitFacing::Aft, 6.0f, 0.0f);
    check(std::fabs(integrity(drip, ShipSystem::Engines) -
                    integrity(lump, ShipSystem::Engines)) < 1e-3f,
          "60 x 0.1 cm == 1 x 6 cm");

    ShipSystems fx;
    check(near(speed_mult(fx), 1.0f) && near(turn_mult(fx), 1.0f) &&
          near(radar_mult(fx), 1.0f) && near(shield_regen_mult(fx), 1.0f),
          "pristine effects are all 1.0");
    fx.integrity.fill(0.0f);
    check(near(speed_mult(fx), k_min_speed_mult) && near(turn_mult(fx), k_min_turn_mult) &&
          near(radar_mult(fx), k_min_radar_mult) && shield_regen_mult(fx) == 0.0f,
          "destroyed effects hit their floors (regen 0)");

    ShipSystems removed;
    removed.integrity[(int)ShipSystem::JumpDrive] = 0.2f;
    set_installed(removed, ShipSystem::JumpDrive, false);
    check(integrity(removed, ShipSystem::JumpDrive) == 1.0f && !any_damaged(removed),
          "removing hardware resets it (next one is new)");

    ShipSystems loaded;
    SystemIntegrity junk = k_pristine;
    junk[0] = 7.0f; junk[1] = -2.0f; junk[2] = 0.5f;
    restore(loaded, junk);
    check(loaded.integrity[0] == 1.0f && loaded.integrity[1] == 0.0f &&
          loaded.integrity[2] == 0.5f, "restore clamps snapshot into [0, 1]");
}

// A tiny hull: 50 cm armor + 10 cm shields per facing, 10 km radar.
struct TestHull {
    ShieldType sh;
    ShipClass  k;
    TestHull() {
        sh.name = "test_shield";
        sh.front_cm = sh.back_cm = sh.port_cm = sh.starboard_cm = 10.0f;
        k.name = "testhull";
        k.armor_fore_cm = k.armor_aft_cm = 50.0f;
        k.armor_port_cm = k.armor_starboard_cm = 50.0f;
        k.default_shield = &sh;
        k.radar_range = 10000.0f;
    }
    Ship spawn() const {
        Ship s = ship::spawn_player();
        s.klass = &k;
        ship::heal_to_full(s);
        return s;
    }
};

void test_damage_pipeline(const TestHull& hull) {
    std::printf("\n--- 2. ship damage pipeline ---\n");

    Ship s = hull.spawn();
    ship::take_damage(s, 8.0f, HitFacing::Fore);   // 10 cm shield soaks it
    check(!ship_systems::any_damaged(s.systems), "shield-soaked hit leaves components alone");

    ship::take_damage(s, 7.0f, HitFacing::Fore);   // 2 shield left -> 5 cm through
    int hurt = 0;
    for (int i = 0; i < kShipSystemCount; ++i)
        if (ship_systems::damaged(s.systems, ship_systems::at(i))) ++hurt;
    check(hurt == 1, "penetrating hit damages exactly one component");
    check(near(total_missing(s.systems), 5.0f * ship_systems::k_integrity_loss_per_cm),
          "integrity lost == penetrating cm * k_integrity_loss_per_cm");

    ship_systems::set_installed(s.systems, ShipSystem::JumpDrive, false);
    ship::heal_to_full(s);
    check(!ship_systems::any_damaged(s.systems), "heal_to_full repairs components");
    check(!ship_systems::installed(s.systems, ShipSystem::JumpDrive),
          "heal_to_full keeps the installed mask");

    // Shield regen scales with generator integrity.
    Ship r = hull.spawn();
    r.shield_fore_cm = 0.0f;
    r.systems.integrity[(int)ShipSystem::ShieldGen] = 0.0f;
    ship::regen_shields(r, 1.0f);
    check(r.shield_fore_cm == 0.0f, "destroyed shield gen: no regen");
    ship_systems::repair(r.systems, ShipSystem::ShieldGen);
    ship::regen_shields(r, 1.0f);
    check(r.shield_fore_cm > 0.0f, "repaired shield gen: regen resumes");

    Ship radar = hull.spawn();
    const float full_r = perception::radar_range_m(radar);
    radar.systems.integrity[(int)ShipSystem::Radar] = 0.0f;
    check(near(full_r, 10000.0f) &&
          near(perception::radar_range_m(radar), 10000.0f * ship_systems::k_min_radar_mult),
          "destroyed radar shrinks the sensor sphere to its floor");
}

void test_repair(const TestHull& hull) {
    std::printf("\n--- 3. repair desk ---\n");

    PlayerState p = player::new_game("troy");
    p.credits = 100000;
    Ship s = hull.spawn();
    s.systems.integrity[(int)ShipSystem::Radar]   = 0.5f;
    s.systems.integrity[(int)ShipSystem::Engines] = 0.0f;

    const repair::Quote q = repair::quote(&s, p);
    const int64_t radar_cost   = q.system_cost[(int)ShipSystem::Radar];
    const int64_t engines_cost = q.system_cost[(int)ShipSystem::Engines];
    std::printf("  radar 50%% -> %lld cr | engines 0%% -> %lld cr\n",
                (long long)radar_cost, (long long)engines_cost);
    check(q.systems_damaged && radar_cost > 0 && engines_cost > radar_cost,
          "quote prices each damaged component, worse damage costs more");
    check(q.system_cost[(int)ShipSystem::Guns] == 0, "pristine component quotes 0");
    check(q.systems_cost == radar_cost + engines_cost && q.total >= q.systems_cost,
          "systems_cost sums components and rolls into total");

    const int64_t before = p.credits;
    check(repair::repair_system(s, p, ShipSystem::Radar), "radar repair succeeds");
    check(ship_systems::integrity(s.systems, ShipSystem::Radar) == 1.0f &&
          p.credits == before - radar_cost, "radar restored and paid for exactly");
    check(p.hp_valid && p.hp_systems[(int)ShipSystem::Radar] == 1.0f &&
          p.hp_systems[(int)ShipSystem::Engines] == 0.0f,
          "landed repair updates the save snapshot");
    check(!repair::repair_system(s, p, ShipSystem::Radar), "repairing a pristine part refuses");

    p.credits = 1;
    check(!repair::repair_system(s, p, ShipSystem::Engines) && p.credits == 1 &&
          ship_systems::integrity(s.systems, ShipSystem::Engines) == 0.0f,
          "broke: engine repair refused, nothing mutated");

    p.credits = 100000;
    s.armor_fore_cm = 1.0f;
    check(repair::repair_hull(s, p), "hull repair succeeds");
    check(ship_systems::integrity(s.systems, ShipSystem::Engines) == 0.0f,
          "hull repair leaves components broken (sold separately)");
}

} // namespace

int main() {
    std::printf("=== #141 per-component damage harness ===\n");
    const TestHull hull;
    test_pure_model();
    test_damage_pipeline(hull);
    test_repair(hull);
    std::printf("\n=== %s ===\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}
