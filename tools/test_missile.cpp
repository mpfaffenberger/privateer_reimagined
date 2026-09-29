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
// Build + run:
//   cmake --build build --target test_missile && ./build/test_missile
// (the harness chdir's to the repo root so the data tables load by relpath.)
// -----------------------------------------------------------------------------

#include "armor.h"
#include "gun.h"
#include "faction.h"
#include "missile.h"
#include "perception.h"
#include "player.h"
#include "repair.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"
#include "shield.h"

#include <cmath>
#include <cstdio>
#include <string>

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
    faction::init();   // stance matrix for the FF IFF cases (#144)
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
        // Exercise whichever rack the starter loadout fills (heat-seekers
        // today) instead of hard-coding one, so a loadout rebalance can't
        // silently turn this case into a false failure again (#493).
        PlayerState p = player::new_game("troy");
        int rack = -1;
        for (int i = 0; i < k_missile_rack_types && rack < 0; ++i)
            if (player::k_new_game_missiles[i] > 0) rack = i;
        CHECK("starter loadout includes missiles", rack >= 0);
        if (rack < 0) rack = 0;
        const int start = player::missile_count(p, rack);
        std::printf("  new-game rack %d = %d\n", rack, start);
        CHECK("new game starts with ammo in that rack", start > 0);

        int fired = 0;
        while (player::consume_missile(p, rack)) ++fired;
        std::printf("  fired %d until empty; rack now %d\n",
                    fired, player::missile_count(p, rack));
        CHECK("consumed exactly the starting count", fired == start);
        CHECK("empty rack refuses further fire", !player::consume_missile(p, rack));
    }

    // -------------------------------------------------------------------
    // 4. Repair: damage -> pay -> hull restored; broke -> refused.
    // -------------------------------------------------------------------
    std::printf("\n--- 4. paid repair ---\n");
    {
        PlayerState p = player::new_game("troy");
        p.credits = 100000;                  // plenty
        // Player hull, built the way main.cpp builds it. ship::spawn() is the
        // NPC path and applies the enemy armor buff, which heal_to_full
        // (player rules) correctly does not restore (#493).
        Ship s = ship::spawn_player();
        s.klass = k;
        ship::heal_to_full(s);
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

    // -------------------------------------------------------------------
    // 6. Damage immunity + the M23 weapon whitelist (#133/#135): an
    //    immune ship shrugs off everything; opening immune_bypass_gun
    //    lets EXACTLY that gun type through, anonymous sources stay out.
    // -------------------------------------------------------------------
    std::printf("\n--- 6. damage_immune + immune_bypass_gun whitelist ---\n");
    {
        Ship s;
        s.alive          = true;
        s.shield_fore_cm = 10.0f;
        s.armor_fore_cm  = 10.0f;
        s.damage_immune  = true;
        const float h0 = total_health(s);
        ship::take_damage(s, 5.0f, HitFacing::Fore);
        CHECK("immune: anonymous hit bounces", total_health(s) == h0);
        ship::take_damage(s, 5.0f, HitFacing::Fore, GunType::SteltekGun);
        CHECK("immune: steltek hit bounces while whitelist closed",
              total_health(s) == h0);

        s.immune_bypass_gun = GunType::SteltekGun;   // the boost event
        ship::take_damage(s, 5.0f, HitFacing::Fore, GunType::PlasmaGun);
        CHECK("whitelist open: OTHER gun still bounces", total_health(s) == h0);
        ship::take_damage(s, 5.0f, HitFacing::Fore);
        CHECK("whitelist open: anonymous source still bounces",
              total_health(s) == h0);
        ship::take_damage(s, 5.0f, HitFacing::Fore, GunType::SteltekGun);
        CHECK("whitelist open: steltek hit LANDS", total_health(s) < h0);
        ship::take_damage(s, 1.0e6f, HitFacing::Fore, GunType::SteltekGun);
        CHECK("whitelist open: steltek overkill is lethal", !s.alive);
    }

    // -------------------------------------------------------------------
    // 7. FF (#144): no lock, seeks the nearest IFF-hostile, re-acquires
    //    when its mark dies, ignores friendlies/neutrals.
    // -------------------------------------------------------------------
    std::printf("\n--- 7. FF friend-or-foe seeker ---\n");
    {
        CHECK("FF needs no lock", !g_missile_stats[(int)MissileType::FF].needs_lock);
        CHECK("FF is the only auto-acquire type",
              g_missile_stats[(int)MissileType::FF].auto_acquire &&
              !g_missile_stats[(int)MissileType::HS].auto_acquire);
        CHECK("FF name round-trips",
              missile::from_name("FF") == MissileType::FF &&
              std::string(missile::to_name(MissileType::FF)) == "FF");

        ShipRegistry ships;
        ShipSpriteObject spr_owner, spr_near, spr_far, spr_ally;
        // Militia shooter at the origin. Merchant ally is CLOSEST but must
        // be ignored; pirate A (near) is the pick, pirate B the fallback.
        Ship owner = make_target(*k, spr_owner, HMM_V3(0, 0, 0), 7000);
        owner.faction = Faction::Militia;
        Ship ally  = make_target(*k, spr_ally, HMM_V3(-1200, 0, 300), 7001);
        ally.faction = Faction::Merchant;
        Ship near_p = make_target(*k, spr_near, HMM_V3(0, 0, 3000), 7002);
        near_p.faction = Faction::Pirate;
        Ship far_p  = make_target(*k, spr_far, HMM_V3(4000, 0, 0), 7003);
        far_p.faction = Faction::Pirate;
        ships.spawn(std::move(owner));
        ships.spawn(std::move(ally));
        ships.spawn(std::move(near_p));
        ships.spawn(std::move(far_p));
        perception::tick(ships, PlayerReputation{});

        const uint32_t pick = missile::acquire_iff_target(ships, 7000, HMM_V3(0, 0, 0));
        std::printf("  acquire from origin -> %u\n", pick);
        CHECK("FF picks the nearest hostile, skipping a closer ally", pick == 7002);
        CHECK("unknown shooter has no IFF reference",
              missile::acquire_iff_target(ships, 9999, HMM_V3(0, 0, 0)) == 0);

        // Fired straight UP (90 degrees off every bearing) with NO target:
        // must pick A on its first tick without any lock.
        std::vector<Missile> ms;
        ms.push_back(missile::spawn(MissileType::FF, HMM_V3(0, 0, 0),
                                    HMM_V3(0, 1, 0), HMM_V3(0, 0, 0),
                                    /*owner*/ 7000, /*target*/ 0));
        CHECK("FF launches without a target", ms[0].target_id == 0);
        const float dt = 1.0f / 60.0f;
        missile::tick(ms, ships, dt);
        CHECK("FF acquired the nearest hostile on its first tick",
              !ms.empty() && ms[0].target_id == 7002);

        // Kill its mark mid-flight: it should swing onto pirate B.
        ships.find_by_id(7002)->alive = false;
        missile::tick(ms, ships, dt);
        CHECK("FF re-acquires when its first mark dies",
              !ms.empty() && ms[0].target_id == 7003);

        const float b_before = total_health(*ships.find_by_id(7003));
        const float ally_before = total_health(*ships.find_by_id(7001));
        bool detonated = false;
        for (int frame = 0; frame < 1800 && !ms.empty(); ++frame) {
            missile::tick(ms, ships, dt);
            missile::collide_and_damage(ms, ships, dt);
            if (ms.empty()) { detonated = true; break; }
        }
        CHECK("FF homed + detonated on the re-acquired hostile",
              detonated && total_health(*ships.find_by_id(7003)) < b_before);
        CHECK("FF left the friendly alone",
              total_health(*ships.find_by_id(7001)) == ally_before);

        // No hostiles left on the scope: acquire comes back empty.
        ships.find_by_id(7003)->alive = false;
        perception::tick(ships, PlayerReputation{});
        CHECK("no hostile contact -> nothing to acquire",
              missile::acquire_iff_target(ships, 7000, HMM_V3(0, 0, 0)) == 0);
    }

    // -------------------------------------------------------------------
    // 8. FF spoofing (#144): a friendly whose IFF reads hostile to the
    //    shooter (here: one the player provoked) is fair game.
    // -------------------------------------------------------------------
    std::printf("\n--- 8. FF IFF spoof ---\n");
    {
        ShipRegistry ships;
        ShipSpriteObject spr_mer;
        Ship pl = ship::spawn_player();
        pl.id = 8000;
        const uint32_t pl_id = pl.id;
        ships.spawn(std::move(pl));
        Ship mer = make_target(*k, spr_mer, HMM_V3(0, 0, 2000), 8001);
        mer.faction = Faction::Merchant;
        ships.spawn(std::move(mer));

        perception::tick(ships, PlayerReputation{});
        CHECK("a friendly merchant is not an FF target",
              missile::acquire_iff_target(ships, pl_id, HMM_V3(0, 0, 0)) == 0);

        ships.find_by_id(8001)->provoked_by_player = true;
        perception::tick(ships, PlayerReputation{});
        CHECK("a provoked friendly reads hostile and gets targeted",
              missile::acquire_iff_target(ships, pl_id, HMM_V3(0, 0, 0)) == 8001);
    }

    // -------------------------------------------------------------------
    // 9. NPC FF racks (#144): pirates carry them, launch only mid gun-run.
    // -------------------------------------------------------------------
    std::printf("\n--- 9. NPC FF racks ---\n");
    {
        ShipRegistry ships;
        ShipSpriteObject spr_p, spr_m;
        Ship pir = make_target(*k, spr_p, HMM_V3(0, 0, 0), 9001);
        pir.faction = Faction::Pirate;
        missile::arm_npc_rack(pir);
        Ship mer = make_target(*k, spr_m, HMM_V3(0, 0, 5000), 9002);
        mer.faction = Faction::Merchant;
        missile::arm_npc_rack(mer);
        const int rack = pir.ff_missiles;
        CHECK("pirates spawn with an FF rack", rack > 0);
        CHECK("merchants spawn without one", mer.ff_missiles == 0);
        ships.spawn(std::move(pir));
        ships.spawn(std::move(mer));

        std::vector<Missile> ms;
        Ship& p = *ships.find_by_id(9001);
        for (int i = 0; i < 30; ++i) missile::npc_launch(ms, ships, 1.0f);
        CHECK("idle pirate never launches", ms.empty() && p.ff_missiles == rack);

        p.ai.state = AIState::Engage;
        p.controller.fire_guns = true;
        int launched = 0;
        for (int i = 0; i < 120; ++i) launched += missile::npc_launch(ms, ships, 1.0f);
        std::printf("  engaged pirate launched %d of %d\n", launched, rack);
        CHECK("engaged pirate empties its rack, one round at a time",
              launched == rack && p.ff_missiles == 0 && (int)ms.size() == rack);
        CHECK("NPC rounds are FF owned by the pirate",
              !ms.empty() && ms[0].type == MissileType::FF && ms[0].owner_id == 9001);
    }

    std::printf("\n=== %s ===\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}
