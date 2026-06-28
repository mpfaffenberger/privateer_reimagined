#include "firing.h"

#include "aim.h"
#include "gun.h"
#include "perception.h"
#include "projectile.h"
#include "sfx.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"   // for sprite->forward_speed read

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

// Nose-forward aim (world frame) for FIXED guns. Turret mounts do NOT
// use this — they compute their own per-mount lead aim in firing::tick.
// Two paths:
//
//   * NPCs use the SHIP convention (atlas-authored forward = body +Z)
//     and aim along their CURRENT nose direction — orientation*+Z.
//     Lead prediction for fixed guns happens at the AI layer (where to
//     point the nose), not here.
//
//   * Player uses controller.desired_forward as the aim vector. main.cpp
//     sets this each frame from camera-forward + mouse-driven gimbal
//     offset (Freelancer-style: guns track the mouse cursor up to ±15°
//     off the ship's nose). When zero (uninitialised), falls back to the
//     camera-convention forward = orientation * -Z so default behaviour
//     stays "shoot straight ahead".
HMM_Vec3 ship_forward_world(const Ship& s) {
    if (s.is_player) {
        const HMM_Vec3& aim = s.controller.desired_forward;
        const float l2 = HMM_DotV3(aim, aim);
        if (l2 > 1e-6f) return HMM_DivV3F(aim, std::sqrt(l2));
        // Fallback: camera-forward.
        const HMM_Mat4 R = HMM_QToM4(s.orientation);
        const HMM_Vec4 f = HMM_MulM4V4(R, HMM_V4(0, 0, -1, 0));
        return HMM_V3(f.X, f.Y, f.Z);
    }
    const HMM_Mat4 R = HMM_QToM4(s.orientation);
    const HMM_Vec4 f = HMM_MulM4V4(R, HMM_V4(0, 0, 1, 0));
    return HMM_V3(f.X, f.Y, f.Z);
}

// Transform a body-frame offset into world space, given the ship's
// orientation. Used to put the muzzle at the actual mount point on the
// ship — wing-tip guns fire from the wing tips, not the center. With a
// w=0 vector this also rotates a body-frame DIRECTION into world space
// (turret forward_body cones reuse it).
HMM_Vec3 body_to_world(const HMM_Quat& q, HMM_Vec3 v_body) {
    const HMM_Mat4 R = HMM_QToM4(q);
    const HMM_Vec4 v = HMM_MulM4V4(R, HMM_V4(v_body.X, v_body.Y, v_body.Z, 0));
    return HMM_V3(v.X, v.Y, v.Z);
}

// Resolve an NPC turret's current target from the ship's perception.
// Prefer the pre-computed nearest hostile; otherwise scan the visible
// list for the closest Hostile contact whose bearing already lies inside
// this mount's firing cone. Returns nullptr when nothing shootable is in
// view. The cone test here uses the contact's observer->target bearing;
// the precise lead-aim cone test happens at the call site once we know
// the predicted intercept direction.
Ship* pick_turret_target(const Ship& s, ShipRegistry& ships,
                         HMM_Vec3 base_dir_world, float cos_cone) {
    if (s.perception.nearest_hostile_id != 0) {
        Ship* t = ships.find_by_id(s.perception.nearest_hostile_id);
        if (t && t->alive) return t;
    }
    Ship* best      = nullptr;
    float best_dist = 1e30f;
    for (const PerceivedContact& c : s.perception.visible) {
        if (c.stance != Stance::Hostile)                     continue;
        if (c.distance_m >= best_dist)                       continue;
        if (HMM_DotV3(c.to_unit, base_dir_world) < cos_cone) continue;
        Ship* t = ships.find_by_id(c.ship_id);
        if (!t || !t->alive)                                 continue;
        best      = t;
        best_dist = c.distance_m;
    }
    return best;
}

} // namespace

void firing::tick(ShipRegistry& ships,
                  std::vector<Projectile>& projectiles,
                  float dt) {
    for (Ship& s : ships) {
        if (!s.alive) continue;

        // Cooldowns + energy regen run for EVERY ship every frame —
        // not gated on fire_guns. Otherwise a ship that toggles fire
        // off would freeze its cooldowns mid-cycle.
        for (float& cd : s.gun_cooldowns) {
            cd -= dt;
            if (cd < 0.0f) cd = 0.0f;
        }
        // Energy pool. Player has no klass (until they pick a ship in
        // the upgrade flow); use a generous v1 default so the player
        // can always shoot. NPCs use class numbers.
        constexpr float k_player_energy_max     = 200.0f;
        // 75 GJ/s leaves ~15–30 GJ/s net once a stock shield gen (45–60
        // GJ/s drain) is running, so the player's guns/AB always recover.
        constexpr float k_player_energy_regen   = 75.0f;
        const float energy_max   = s.klass ? s.klass->energy_max
                                  : (s.is_player ? k_player_energy_max : 0.0f);
        const float energy_regen = s.klass ? s.klass->energy_recharge
                                  : (s.is_player ? k_player_energy_regen : 0.0f);
        // Engine upgrade ADDS GJ/s; shield gen DRAINS GJ/s from the budget.
        // Both are absolute (not multipliers) per np-3dp.27 — when the
        // drain exceeds the base + bonus, the shield pulls energy from
        // whatever else is using it (guns/AB).
        const float regen = energy_regen + s.engine_recharge_add_gj - s.shield_recharge_drain_gj;
        s.energy_gj = std::min(s.energy_gj + std::max(0.0f, regen) * dt, energy_max);

        if (s.mounts.empty()) continue;

        // Shared nose-forward aim for FIXED guns (player gimbal handled
        // inside ship_forward_world). No convergence — mounts fire
        // parallel from their muzzle offsets; aim accuracy comes from
        // the ITTS reticle. TURRET mounts ignore this and compute their
        // own per-mount lead aim below, so we no longer gate the whole
        // ship on controller.fire_guns: a fleeing merchant's tail
        // turret still bites.
        const HMM_Vec3 fwd_world = ship_forward_world(s);

        // Inherit shooter velocity. ship.world_velocity is populated
        // by sync_from_sprite (NPCs: orientation*+Z*forward_speed) or
        // main.cpp's player block (camera.velocity for the player).
        // Without this, projectiles fly at exactly muzzle speed in the
        // ship's forward direction — fine when the shooter is at
        // rest, but a coasting/strafing player sees tracers drift in
        // their reference frame. Adding the shooter's full 3D motion
        // matches real-world ballistics + every other space sim.
        const HMM_Vec3 ship_v = s.world_velocity;

        for (size_t i = 0; i < s.mounts.size(); ++i) {
            const GunMount& m  = s.mounts[i];
            if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
            const GunStats& gs = g_gun_stats[(int)m.type];
            if (!gs.complete)                          continue;   // null-data gun

            // Per-mount rarity mods (#90). Parallel to mounts; absent /
            // default = no change (1.0/1.0), so NPCs (empty mount_mods)
            // and Basic player guns behave exactly as before. fire_rate
            // shortens the cooldown; energy scales the per-shot cost.
            const inventory::WeaponMods wm =
                (i < s.mount_mods.size()) ? s.mount_mods[i] : inventory::WeaponMods{};
            const float fire_rate_mult = (wm.fire_rate_mult > 0.0f) ? wm.fire_rate_mult : 1.0f;
            const float shot_energy_cost = gs.energy_cost_gj * wm.energy_mult;

            // gun_armed (np-3dp): G-key cycle gates which mounts fire.
            // NPCs leave gun_armed empty (treated as armed). Cooldown
            // applies to every mount, fixed and turret alike.
            if (i < s.gun_armed.size() && !s.gun_armed[i]) continue;
            if (s.gun_cooldowns[i] > 0.0f)                 continue;

            // Muzzle position: ship pos + rotated mount offset.
            const HMM_Vec3 muzzle =
                HMM_AddV3(s.position, body_to_world(s.orientation, m.offset_body));

            // Per-mount aim direction. Two firing models below.
            HMM_Vec3 aim_dir;

            if (m.is_turret) {
                // ---- NPC auto-turret (lead-predicting, fires FREE) ----
                // Player turrets are OUT OF SCOPE.
                if (s.is_player) continue;

                // Cone geometry for this mount, world frame.
                HMM_Vec3 base_dir = body_to_world(s.orientation, m.forward_body);
                const float bl2 = HMM_DotV3(base_dir, base_dir);
                if (bl2 < 1e-9f) continue;
                base_dir = HMM_DivV3F(base_dir, std::sqrt(bl2));
                constexpr float k_deg2rad = 3.14159265358979f / 180.0f;
                const float cos_cone = std::cos(m.cone_half_angle_deg * k_deg2rad);

                // Target: nearest hostile, else nearest in-cone hostile.
                Ship* target = pick_turret_target(s, ships, base_dir, cos_cone);
                if (!target) continue;

                // Range gate against this gun's actual reach.
                const HMM_Vec3 to_t = HMM_SubV3(target->position, muzzle);
                if (HMM_DotV3(to_t, to_t) > gs.range_m * gs.range_m) continue;

                // Lead prediction (ITTS) on the target's world velocity.
                const HMM_Vec3 lead = aim::lead_point(
                    muzzle, target->position, target->world_velocity, gs.speed_mps);
                HMM_Vec3 d = HMM_SubV3(lead, muzzle);
                const float dl2 = HMM_DotV3(d, d);
                if (dl2 < 1e-9f) continue;
                aim_dir = HMM_DivV3F(d, std::sqrt(dl2));

                // Reject if the predicted aim leaves the mount's cone.
                if (HMM_DotV3(aim_dir, base_dir) < cos_cone) continue;

                // No fire_guns gate, no energy gate, no energy drain —
                // turrets fire free, gated only by cooldown + arc.
            } else {
                // ---- Fixed forward gun (legacy path, unchanged) -------
                if (!s.controller.fire_guns)         continue;
                if (s.energy_gj < shot_energy_cost)  continue;   // dry
                aim_dir = fwd_world;
            }

            Projectile p;
            p.position = muzzle;
            // Velocity: shooter's full 3D world velocity + muzzle speed
            // along the (per-mount) aim direction.
            p.velocity = HMM_AddV3(ship_v, HMM_MulV3F(aim_dir, gs.speed_mps));
            p.damage_cm        = gs.damage_cm;
            p.range_remaining  = gs.range_m;
            p.type             = m.type;
            p.owner_id         = s.id;
            p.alive            = true;
            projectiles.push_back(p);

            // Fixed guns drain the shared pool (scaled by the mount's
            // rarity energy mult); turrets fire free.
            if (!m.is_turret) s.energy_gj -= shot_energy_cost;
            // Faster fire rate => shorter cooldown (guarded mult>0 above).
            s.gun_cooldowns[i] = gs.refire_delay_s / fire_rate_mult;

            // One shot fired -> one sound. The gun type selects the
            // sample (per-gun originals, laser_fire fallback); player
            // shots are 2D (always audible), NPC shots positional --
            // policy in sfx.cpp.
            sfx::gun_fired(m.type, p.position, s.is_player);
        }
    }
}

// =============================================================================
// Gun arm-mode helpers (np-3dp). All inline so firing.cpp's I-cache stays
// compact and the helpers stay cheap to call from main/HUD paths.
// =============================================================================
namespace {

// Thread-local cache of "unique GunTypes in first-occurrence order" for
// the most-recently-seen mounts pointer. Lets the G-press and HUD paths
// share the result without recomputing each frame. Keyed by the
// std::vector* address; matches the typical "1 player ship" reality so
// misses on first call and re-fills after are fine.
struct UniqueCache {
    const std::vector<GunMount>* key = nullptr;
    std::vector<int>             types;
};
UniqueCache& ucache() {
    static UniqueCache c;
    return c;
}

} // namespace

int firing::gun_mode_count_for_mounts(const std::vector<GunMount>& mounts) {
    UniqueCache& uc = ucache();
    if (uc.key != &mounts || uc.types.empty()) {
        uc.key = &mounts;
        uc.types.clear();
        for (const GunMount& m : mounts) {
            const int t = (int)m.type;
            if (std::find(uc.types.begin(), uc.types.end(), t) == uc.types.end()) {
                uc.types.push_back(t);
            }
        }
    }
    return (int)uc.types.size() + 2;   // +2: mode 0 unarmed, last mode all
}

const std::vector<int>& firing::gun_unique_types_cache(
    const std::vector<GunMount>&mounts) {
    UniqueCache& uc = ucache();
    if (uc.key != &mounts || uc.types.empty()) {
        uc.key = &mounts;
        uc.types.clear();
        for (const GunMount& m : mounts) {
            const int t = (int)m.type;
            if (std::find(uc.types.begin(), uc.types.end(), t) == uc.types.end()) {
                uc.types.push_back(t);
            }
        }
    }
    return uc.types;
}

void firing::apply_gun_mode(Ship& s, uint8_t mode_idx) {
    const std::vector<int>& u = gun_unique_types_cache(s.mounts);
    if (u.empty()) return;
    // Mode 0 = unarmed, last = all, in-between = one type per mode.
    const int N    = (int)u.size();
    const int last = N + 1;
    int m = (int)mode_idx;
    if (N > 0) m = ((m % last) + last) % last;   // safe mod for any input
    // Compute the "type filter" for this mode.
    int target_type = -1;   // -1 = all, 0..N-1 = specific type, -2 = unarmed
    if      (m == 0)   target_type = -2;
    else if (m == last) target_type = -1;
    else                target_type = u[m - 1];
    for (size_t i = 0; i < s.mounts.size(); ++i) {
        if (i >= s.gun_armed.size()) break;
        const int type = (int)s.mounts[i].type;
        bool arm;
        if      (target_type == -2) arm = false;        // unarmed
        else if (target_type == -1) arm = true;         // all
        else                          arm = (type == target_type);
        s.gun_armed[i] = arm;
    }
    s.gun_mode_idx = (uint8_t)m;
}

const char* firing::gun_mode_label(const std::vector<int>& unique_types,
                                  uint8_t mode_idx) {
    static const char* k_unarmed = "UNARMED";
    static const char* k_all     = "ALL";
    const int N    = (int)unique_types.size();
    const int last = N + 1;
    if (N == 0) return k_unarmed;
    int m = (int)mode_idx;
    m = ((m % last) + last) % last;
    if (m == 0)   return k_unarmed;
    if (m == last) return k_all;
    // Single-type mode: use the gun's canonical name uppercased.
    const int t = unique_types[m - 1];
    const char* name = gun::to_name((GunType)t);
    static thread_local char buf[40];
    std::snprintf(buf, sizeof(buf), "%s", name);
    for (char* c = buf; *c; ++c) *c = (char)std::toupper((unsigned char)*c);
    return buf;
}

int firing::gun_mode_armed_count(const Ship& s) {
    int n = 0;
    for (bool a : s.gun_armed) if (a) ++n;
    return n;
}
