// Player auto-turret regression (#379): G-key modes gate only fixed forward
// guns; turrets stay auto-armed, the HUD's armed state agrees with firing,
// and authored turret arcs/range decide whether a turret actually fires.
#include "firing.h"

#include "gun.h"
#include "projectile.h"
#include "sfx.h"
#include "ship.h"
#include "ship_registry.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sfx {
void gun_fired(GunType, HMM_Vec3, bool) {}
} // namespace sfx

namespace {
int failures = 0;

constexpr GunType kTurretGun = GunType::IonicPulseCannon;
constexpr float   kRangeM    = 1500.0f;

void check(bool condition, const char* label) {
    std::printf("%-66s %s\n", label, condition ? "PASS" : "FAIL");
    if (!condition) ++failures;
}

void configure_test_gun(GunType type) {
    GunStats& gun = g_gun_stats[(int)type];
    gun = GunStats{};
    gun.damage_cm = 5.0f;
    gun.speed_mps = 1000.0f;
    gun.range_m = kRangeM;
    gun.refire_delay_s = 0.5f;
    gun.energy_cost_gj = 10.0f;
    gun.complete = true;
}

// Rear turret as authored in assets/ships/centurion/ship.json.
GunMount rear_turret(float cone_half_angle_deg = 120.0f) {
    GunMount m;
    m.type = kTurretGun;
    m.is_turret = true;
    m.offset_body = HMM_V3(0.0f, 0.0f, -24.0f);
    m.forward_body = HMM_V3(0.0f, 0.0f, -1.0f);
    m.cone_half_angle_deg = cone_half_angle_deg;
    return m;
}

GunMount fixed_gun(GunType type = kTurretGun) {
    GunMount m;
    m.type = type;
    m.forward_body = HMM_V3(0.0f, 0.0f, 1.0f);
    return m;
}

// Player at the origin with identity orientation. In the player's camera
// basis the nose is -Z, so a rear turret covers +Z.
Ship make_player(std::vector<GunMount> mounts, std::vector<bool> armed) {
    Ship p;
    p.id = 1;
    p.alive = true;
    p.is_player = true;
    p.energy_gj = 0.0f;               // turrets are free; fixed guns go dry
    p.gun_cooldowns.assign(mounts.size(), 0.0f);
    p.mounts = std::move(mounts);
    p.gun_armed = std::move(armed);
    p.perception.nearest_hostile_id = 2;
    return p;
}

// One zero-dt firing tick against a stationary hostile at `target_pos`.
std::vector<Projectile> fire_once(Ship player, HMM_Vec3 target_pos) {
    ShipRegistry ships;
    ships.spawn(std::move(player));
    Ship target;
    target.id = 2;
    target.alive = true;
    target.position = target_pos;
    ships.spawn(std::move(target));
    std::vector<Projectile> projectiles;
    firing::tick(ships, projectiles, 0.0f);
    return projectiles;
}

HMM_Vec3 astern_off_axis(float degrees, float distance_m) {
    const float rad = degrees * 3.14159265358979f / 180.0f;
    return HMM_V3(distance_m * std::sin(rad), 0.0f, distance_m * std::cos(rad));
}

void test_mode_cycle_and_hud() {
    Ship c;  // Centurion-style: 4 fixed Neutron guns + 2 Ionic rear turrets
    const GunMount neutron = fixed_gun(GunType::NeutronGun);
    c.mounts = {neutron, neutron, neutron, neutron, rear_turret(), rear_turret()};

    check(firing::gun_mode_count_for_mounts(c.mounts) == 3,
          "G cycle excludes turret-only Ionic type: {UNARMED, NEUTRON, ALL}");

    firing::apply_gun_mode(c, 0);
    check(firing::gun_mode_armed_count(c) == 2 &&
          firing::mount_armed(c, 4) && firing::mount_armed(c, 5) &&
          !firing::mount_armed(c, 0),
          "UNARMED: HUD shows only the two auto-turrets armed");

    c.gun_armed[4] = false;  // stale/foreign mask bit must not matter
    check(firing::mount_armed(c, 4),
          "turret stays armed even if its mask bit is false");

    firing::apply_gun_mode(c, 1);
    check(firing::gun_mode_armed_count(c) == 6,
          "NEUTRON: four fixed guns plus two auto-turrets armed");

    firing::apply_gun_mode(c, 0);
    firing::arm_all_guns(c);
    const auto& unique = firing::gun_unique_types_cache(c.mounts);
    check(std::strcmp(firing::gun_mode_label(unique, c.gun_mode_idx), "ALL") == 0 &&
          firing::gun_mode_armed_count(c) == 6,
          "arm_all_guns selects the ALL label for a 3-mode loadout");

    firing::apply_gun_mode(c, c.gun_mode_idx);  // armament MFD swap path
    check(firing::gun_mode_armed_count(c) == 6,
          "re-applying the stored mode after a swap keeps ALL armed");

    check(!firing::mount_armed(c, c.mounts.size()),
          "out-of-range mount index is never armed");
}

void test_turret_fires_despite_mask() {
    const auto shots = fire_once(make_player({rear_turret()}, {false}),
                                 HMM_V3(0.0f, 0.0f, 300.0f));
    check(shots.size() == 1,
          "player auto-turret fires despite forward-gun mask=false");
    check(!shots.empty() && shots[0].owner_id == 1,
          "auto-turret projectile belongs to the player");
}

void test_fixed_gun_gating() {
    Ship disarmed = make_player({fixed_gun()}, {false});
    disarmed.energy_gj = 100.0f;
    disarmed.controller.fire_guns = true;
    check(fire_once(disarmed, HMM_V3(0.0f, 0.0f, -300.0f)).empty(),
          "disarmed fixed gun stays silent");

    Ship armed = make_player({fixed_gun()}, {true});
    armed.energy_gj = 100.0f;
    armed.controller.fire_guns = true;
    check(fire_once(armed, HMM_V3(0.0f, 0.0f, -300.0f)).size() == 1,
          "armed fixed gun fires on the trigger");
}

void test_turret_arc_and_range() {
    const HMM_Vec3 off_axis_60 = astern_off_axis(60.0f, 300.0f);
    check(fire_once(make_player({rear_turret(120.0f)}, {true}), off_axis_60).size() == 1,
          "authored 120-degree arc engages a target 60 degrees off astern");
    check(fire_once(make_player({rear_turret(1.0f)}, {true}), off_axis_60).empty(),
          "a 1-degree clamp would have blinded that turret (old loadout bug)");
    check(fire_once(make_player({rear_turret(120.0f)}, {true}),
                    HMM_V3(0.0f, 0.0f, -300.0f)).empty(),
          "rear turret does not fire at a target dead ahead");
    check(fire_once(make_player({rear_turret(120.0f)}, {true}),
                    HMM_V3(0.0f, 0.0f, kRangeM + 500.0f)).empty(),
          "rear turret does not fire beyond gun range");
}

// #145/#510: an empty slot (sold gun, unbought turret) is an inert
// GunType::Count mount -- never fires, never armed, never a G-mode group.
void test_inert_mounts() {
    GunMount inert_turret = rear_turret();
    inert_turret.type = GunType::Count;
    GunMount inert_fixed = fixed_gun();
    inert_fixed.type = GunType::Count;

    Ship c;  // Centurion bought from the dealer: 4 Neutrons, turret not installed
    const GunMount neutron = fixed_gun(GunType::NeutronGun);
    c.mounts = {neutron, neutron, neutron, inert_fixed, inert_turret, inert_turret};
    firing::arm_all_guns(c);
    check(firing::gun_mode_count_for_mounts(c.mounts) == 3,
          "inert mounts add no G-mode group: {UNARMED, NEUTRON, ALL}");
    check(firing::gun_mode_armed_count(c) == 3 && !firing::mount_armed(c, 3) &&
          !firing::mount_armed(c, 4) && !firing::mount_armed(c, 5),
          "ALL arms the 3 real guns; inert fixed + turret mounts stay unarmed");

    check(fire_once(make_player({inert_turret}, {true}), HMM_V3(0.0f, 0.0f, 300.0f)).empty(),
          "unbought turret mount never fires");
    Ship sold = make_player({inert_fixed}, {true});
    sold.energy_gj = 100.0f;
    sold.controller.fire_guns = true;
    check(fire_once(sold, HMM_V3(0.0f, 0.0f, -300.0f)).empty(),
          "sold (empty) fixed mount never fires");
}
} // namespace

int main() {
    configure_test_gun(GunType::IonicPulseCannon);
    configure_test_gun(GunType::NeutronGun);

    test_mode_cycle_and_hud();
    test_turret_fires_despite_mask();
    test_fixed_gun_gating();
    test_turret_arc_and_range();
    test_inert_mounts();

    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
