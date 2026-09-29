#include "armament_loadout.h"
#include "ship_class.h"

#include <cstdio>

namespace {

int failures = 0;
void check(bool condition, const char* label) {
    std::printf("%-58s %s\n", label, condition ? "PASS" : "FAIL");
    if (!condition) ++failures;
}

} // namespace

int main() {
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
