#include "firing.h"

#include "gun.h"
#include "projectile.h"
#include "sfx.h"
#include "ship.h"
#include "ship_registry.h"

#include <cstdio>
#include <vector>

namespace sfx {
void gun_fired(GunType, HMM_Vec3, bool) {}
} // namespace sfx

namespace {
int failures = 0;

void check(bool condition, const char* label) {
    std::printf("%-64s %s\n", label, condition ? "PASS" : "FAIL");
    if (!condition) ++failures;
}

void configure_test_gun(GunType type) {
    GunStats& gun = g_gun_stats[(int)type];
    gun = GunStats{};
    gun.damage_cm = 5.0f;
    gun.speed_mps = 1000.0f;
    gun.range_m = 1500.0f;
    gun.refire_delay_s = 0.5f;
    gun.energy_cost_gj = 10.0f;
    gun.complete = true;
}

Ship make_target() {
    Ship target;
    target.id = 2;
    target.alive = true;
    target.position = HMM_V3(0.0f, 0.0f, 300.0f);
    return target;
}
} // namespace

int main() {
    configure_test_gun(GunType::IonicPulseCannon);

    Ship centurion_modes;
    GunMount neutron;
    neutron.type = GunType::NeutronGun;
    GunMount ionic_turret;
    ionic_turret.type = GunType::IonicPulseCannon;
    ionic_turret.is_turret = true;
    centurion_modes.mounts = {
        neutron, neutron, neutron, neutron, ionic_turret, ionic_turret
    };
    check(firing::gun_mode_count_for_mounts(centurion_modes.mounts) == 3,
          "Centurion G cycle excludes turret-only Ionic weapon type");
    firing::apply_gun_mode(centurion_modes, 0);
    check(firing::gun_mode_armed_count(centurion_modes) == 2 &&
          centurion_modes.gun_armed[4] && centurion_modes.gun_armed[5],
          "UNARMED mode leaves only two autonomous turrets active");
    firing::apply_gun_mode(centurion_modes, 1);
    check(firing::gun_mode_armed_count(centurion_modes) == 6,
          "Neutron mode arms four fixed guns plus two autonomous turrets");

    ShipRegistry ships;
    Ship player;
    player.id = 1;
    player.alive = true;
    player.is_player = true;
    player.position = HMM_V3(0.0f, 0.0f, 0.0f);
    player.energy_gj = 0.0f; // turrets are free
    GunMount turret;
    turret.type = GunType::IonicPulseCannon;
    turret.is_turret = true;
    turret.offset_body = HMM_V3(0.0f, 0.0f, -24.0f);
    turret.forward_body = HMM_V3(0.0f, 0.0f, -1.0f);
    turret.cone_half_angle_deg = 120.0f;
    player.mounts = {turret};
    player.gun_cooldowns = {0.0f};
    player.gun_armed = {false}; // regression: G-key mode disarmed this bit
    player.perception.nearest_hostile_id = 2;
    ships.spawn(std::move(player));
    ships.spawn(make_target());

    std::vector<Projectile> projectiles;
    firing::tick(ships, projectiles, 0.0f);
    check(projectiles.size() == 1,
          "player auto-turret fires despite forward-gun armed mask=false");
    check(!projectiles.empty() && projectiles[0].owner_id == 1,
          "auto-turret projectile belongs to player");
    check(ships.player() && ships.player()->energy_gj == 0.0f,
          "auto-turret remains independent of player energy pool");

    ShipRegistry fixed_ships;
    Ship fixed_player;
    fixed_player.id = 1;
    fixed_player.alive = true;
    fixed_player.is_player = true;
    fixed_player.energy_gj = 100.0f;
    GunMount fixed = turret;
    fixed.is_turret = false;
    fixed.forward_body = HMM_V3(0.0f, 0.0f, 1.0f);
    fixed_player.mounts = {fixed};
    fixed_player.gun_cooldowns = {0.0f};
    fixed_player.gun_armed = {false};
    fixed_player.controller.fire_guns = true;
    fixed_ships.spawn(std::move(fixed_player));
    fixed_ships.spawn(make_target());
    projectiles.clear();
    firing::tick(fixed_ships, projectiles, 0.0f);
    check(projectiles.empty(), "disarmed fixed gun remains safely disabled");

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
