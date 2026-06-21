#include "firing.h"

#include "gun.h"
#include "projectile.h"
#include "sfx.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"   // for sprite->forward_speed read

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

// Aim direction in world frame for projectile spawning. Two paths:
//
//   * NPCs use the SHIP convention (atlas-authored forward = body +Z)
//     and aim along their CURRENT nose direction — orientation*+Z.
//     Lead prediction happens at the AI layer (where to point the nose),
//     not here.
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
// ship — wing-tip guns fire from the wing tips, not the center.
HMM_Vec3 body_to_world(const HMM_Quat& q, HMM_Vec3 v_body) {
    const HMM_Mat4 R = HMM_QToM4(q);
    const HMM_Vec4 v = HMM_MulM4V4(R, HMM_V4(v_body.X, v_body.Y, v_body.Z, 0));
    return HMM_V3(v.X, v.Y, v.Z);
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
        constexpr float k_player_energy_regen   = 50.0f;
        const float energy_max   = s.klass ? s.klass->energy_max
                                  : (s.is_player ? k_player_energy_max : 0.0f);
        const float energy_regen = s.klass ? s.klass->energy_recharge
                                  : (s.is_player ? k_player_energy_regen : 0.0f);
        s.energy_gj = std::min(s.energy_gj + energy_regen * dt, energy_max);

        if (!s.controller.fire_guns) continue;
        if (s.mounts.empty())        continue;

        const HMM_Vec3 fwd_world = ship_forward_world(s);

        // No gun convergence — every mount fires straight along the
        // ship's aim direction, tracers stay parallel from their muzzle
        // offsets. Parallel is more legible than the toed-in V the
        // earlier convergence math produced; aim accuracy comes from
        // the ITTS reticle telling you where to point.

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
            // gun_armed (np-3dp): G-key cycle gates which mounts the
            // player can actually fire. Skipping means the mount goes
            // cold even if its own cooldown is ready.
            if (i < s.gun_armed.size() && !s.gun_armed[i]) continue;
            if (s.gun_cooldowns[i] > 0.0f)            continue;
            const GunMount& m  = s.mounts[i];
            if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
            const GunStats& gs = g_gun_stats[(int)m.type];
            if (!gs.complete)                          continue;   // null-data gun
            if (s.energy_gj < gs.energy_cost_gj)       continue;   // dry

            Projectile p;
            // Muzzle position: ship pos + rotated mount offset.
            p.position = HMM_AddV3(s.position, body_to_world(s.orientation, m.offset_body));
            // Velocity: shooter's full 3D world velocity + muzzle
            // speed along the aim direction. Inheritance lets the
            // tracer fly with the player's frame so coasting / strafing
            // doesn't make bullets visually drift sideways from the
            // crosshair.
            p.velocity = HMM_AddV3(ship_v, HMM_MulV3F(fwd_world, gs.speed_mps));
            p.damage_cm        = gs.damage_cm;
            p.range_remaining  = gs.range_m;
            p.type             = m.type;
            p.owner_id         = s.id;
            p.alive            = true;
            projectiles.push_back(p);

            s.energy_gj         -= gs.energy_cost_gj;
            s.gun_cooldowns[i]   = gs.refire_delay_s;

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
