#include "armament_loadout.h"
#include "gun_modes.h"
#include "ship_class.h"

#include <cstdio>

// firing.cpp (arm_all_guns) links the shot SFX hook; nothing fires here.
namespace sfx {
void gun_fired(GunType, HMM_Vec3, bool) {}
} // namespace sfx

namespace {

int failures = 0;
void check(bool condition, const char* label) {
    std::printf("%-58s %s\n", label, condition ? "PASS" : "FAIL");
    if (!condition) ++failures;
}

// Two nose guns + a rear turret gated by the "rear" TurretSlot, shaped like
// assets/ships/centurion/ship.json (authored turret cone = 120 degrees).
ShipClass centurion_like_hull() {
    ShipClass hull;
    GunMount left;
    left.type = GunType::Laser;
    left.offset_body = HMM_V3(-3.0f, 0.0f, 6.0f);
    GunMount right = left;
    right.offset_body.X = 3.0f;
    GunMount rear;
    rear.type = GunType::IonicPulseCannon;
    rear.is_turret = true;
    rear.offset_body = HMM_V3(0.0f, 0.0f, -24.0f);
    rear.forward_body = HMM_V3(0.0f, 0.0f, -1.0f);
    rear.cone_half_angle_deg = 120.0f;
    hull.default_guns = {left, right, rear};
    hull.turret_slots = {TurretSlot{"rear", "Rear Turret", {2}}};
    return hull;
}

bool all_armed(const Ship& ship) {
    if (ship.gun_armed.size() != ship.mounts.size()) return false;
    for (bool armed : ship.gun_armed) if (!armed) return false;
    return true;
}

bool cooldowns_reset(const Ship& ship) {
    if (ship.gun_cooldowns.size() != ship.mounts.size()) return false;
    for (float cd : ship.gun_cooldowns) if (cd != 0.0f) return false;
    return true;
}

// #514: the loadout -> live mounts build that main.cpp apply_player_loadout
// runs on every launch/refit.
void test_fit_player_mounts() {
    const ShipClass hull = centurion_like_hull();

    PlayerState owner;
    owner.turrets = {"rear"};
    owner.gun_mounts = {
        MountSlot{"mass_driver", inventory::Rarity::Rare},
        MountSlot{"not_a_real_gun"},
        MountSlot{"tachyon_cannon", inventory::Rarity::Legendary},
    };
    Ship ship;
    ship.klass = &hull;
    ship.mounts = {GunMount{}};           // stale state from a previous hull
    ship.gun_cooldowns = {9.0f, 9.0f, 9.0f, 9.0f};
    armament_loadout::fit_player_mounts(ship, owner);
    check(ship.mounts.size() == 3, "one live mount per filled hardpoint");
    check(ship.mounts[0].type == GunType::MassDriver &&
          ship.mounts[2].type == GunType::TachyonCannon,
          "player gun overrides the hardpoint's default type");
    check(ship.mounts[1].type == GunType::Laser,
          "unknown gun name falls back to a Laser");
    check(ship.mounts[0].offset_body.X == -3.0f && ship.mounts[1].offset_body.X == 3.0f &&
          ship.mounts[2].offset_body.Z == -24.0f && ship.mounts[2].is_turret &&
          !ship.mounts[0].is_turret,
          "hardpoint geometry and turret flag come from the hull");
    check(ship.mounts[2].cone_half_angle_deg == 120.0f,
          "authored turret cone is preserved, not clamped (#379)");
    check(cooldowns_reset(ship), "gun_cooldowns sized to mounts and zeroed");
    // Two forward types (mass driver + laser); the turret isn't a G-key
    // group (#379), so ALL is the 2-type ALL mode.
    check(all_armed(ship) &&
          ship.gun_mode_idx == gun_modes::all_mode(2),
          "fresh loadout is armed in ALL mode");
    check(ship.mount_mods.size() == 3 &&
          ship.mount_mods[0].fire_rate_mult == owner.gun_mounts[0].mods.fire_rate_mult &&
          ship.mount_mods[1].fire_rate_mult == 1.0f &&
          ship.mount_mods[2].energy_mult == owner.gun_mounts[2].mods.energy_mult,
          "per-mount weapon mods follow the fitted items");

    PlayerState no_turret = owner;
    no_turret.turrets.clear();
    no_turret.gun_mounts[1].gun_id.clear();
    armament_loadout::fit_player_mounts(ship, no_turret);
    check(ship.mounts.size() == 3 && ship.mounts[1].type == GunType::Count &&
          ship.mounts[2].type == GunType::Count,
          "empty slot and unbought turret stay as inert mounts");
    check(ship.mounts[2].is_turret && ship.mounts[2].cone_half_angle_deg == 120.0f,
          "inert mounts keep their hardpoint geometry");
    check(ship.gun_mode_idx == gun_modes::all_mode(1),
          "ALL mode counts only fitted gun types");

    PlayerState starter;
    starter.gun_mounts = {MountSlot{"laser"}, MountSlot{"laser"}};
    armament_loadout::fit_player_mounts(ship, starter);
    check(ship.mounts.size() == 2 && cooldowns_reset(ship) && all_armed(ship) &&
          ship.mount_mods.size() == 2,
          "short loadout fills only its slots, parallel arrays match");

    PlayerState oversized = owner;
    oversized.gun_mounts.push_back(MountSlot{"laser"});
    armament_loadout::fit_player_mounts(ship, oversized);
    check(ship.mounts.size() == 3 && ship.gun_cooldowns.size() == 3,
          "loadout longer than the hull is capped at its hardpoints");

    armament_loadout::fit_player_mounts(ship, PlayerState{});
    check(ship.mounts.size() == 3 && ship.mounts[0].type == GunType::Laser &&
          ship.mounts[2].type == GunType::IonicPulseCannon &&
          ship.mount_mods.size() == 3 && ship.mount_mods[2].fire_rate_mult == 1.0f,
          "no loadout (--ship override) fills every default gun");

    ship.klass = nullptr;
    armament_loadout::fit_player_mounts(ship, owner);
    check(ship.mounts.empty() && ship.gun_cooldowns.empty() && ship.mount_mods.empty(),
          "no hull class means no mounts");
}

} // namespace

int main() {
    test_fit_player_mounts();

    PlayerState player;
    player.gun_mounts = {
        MountSlot{"plasma_gun", inventory::Rarity::Rare},
        MountSlot{"neutron_gun", inventory::Rarity::Legendary},
    };

    Ship ship;
    GunMount forward;
    forward.type = GunType::PlasmaGun;
    forward.offset_body = HMM_V3(-4.0f, 0.0f, 8.0f);
    GunMount turret;
    turret.type = GunType::NeutronGun;
    turret.offset_body = HMM_V3(0.0f, 2.0f, -7.0f);
    turret.is_turret = true;
    ship.mounts = {forward, turret};
    ship.mount_mods = {player.gun_mounts[0].mods, player.gun_mounts[1].mods};
    ship.gun_cooldowns = {0.25f, 0.75f};

    check(armament_loadout::swap_mounted_weapons(player, ship, 0, 1),
          "valid drag swaps mounted weapons");
    check(player.gun_mounts[0].gun_id == "neutron_gun" &&
          player.gun_mounts[1].gun_id == "plasma_gun",
          "persistent fitted items swap slots");
    check(ship.mounts[0].type == GunType::NeutronGun &&
          ship.mounts[1].type == GunType::PlasmaGun,
          "live gun types follow persistent items");
    check(ship.mounts[0].offset_body.X == -4.0f &&
          ship.mounts[1].offset_body.Z == -7.0f && ship.mounts[1].is_turret,
          "physical geometry and turret behavior stay on hardpoints");
    check(ship.gun_cooldowns[0] == 0.75f && ship.gun_cooldowns[1] == 0.25f,
          "gun-owned cooldown state follows the moved weapon");
    check(!armament_loadout::swap_mounted_weapons(player, ship, 0, 8),
          "invalid destination is rejected without mutation");

    // #145: a gun can't be dragged into a turret whose hardware isn't owned.
    ShipClass hull;
    hull.default_guns = {forward, turret};
    hull.turret_slots = {TurretSlot{"rear", "Rear Turret", {1}}};
    PlayerState buyer;
    buyer.gun_mounts = {MountSlot{"plasma_gun"}, MountSlot{}};
    Ship live;
    live.klass = &hull;
    live.mounts = {forward, turret};
    live.mounts[1].type = GunType::Count;   // inert: nothing fitted
    check(!armament_loadout::swap_mounted_weapons(buyer, live, 0, 1) &&
          buyer.gun_mounts[0].gun_id == "plasma_gun" && buyer.gun_mounts[1].gun_id.empty(),
          "drag into an unbought turret is refused (#145)");
    buyer.turrets = {"rear"};
    check(armament_loadout::swap_mounted_weapons(buyer, live, 0, 1) &&
          buyer.gun_mounts[1].gun_id == "plasma_gun" &&
          live.mounts[1].type == GunType::PlasmaGun && live.mounts[0].type == GunType::Count,
          "drag into an owned turret moves the gun, source goes inert");

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
