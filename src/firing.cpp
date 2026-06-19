#include "firing.h"

#include "gun.h"
#include "gunnery_probe.h"
#include "projectile.h"
#include "sfx.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"   // for sprite->forward_speed read

#include <algorithm>
#include <cmath>
#include <random>

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
    // NPC gunnery aim override: the combat AI writes a skill-scaled
    // intercept solution into controller.fire_aim_world so bolts lead the
    // target instead of flying off the imperfectly-tracking nose. Zero =
    // fall through to the nose (idle/None-behaviour ships).
    {
        const HMM_Vec3& a = s.controller.fire_aim_world;
        const float al2 = HMM_DotV3(a, a);
        if (al2 > 1e-6f) return HMM_DivV3F(a, std::sqrt(al2));
    }
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

        // --- AI skill-based to-hit -------------------------------------
        // Ballistic aiming alone can't hit a jinking target often enough
        // (perfect first-order lead tops out ~15% over the ~1.5s bolt
        // flight). So NPC gunnery uses a per-shot to-hit ROLL gated by the
        // AI already deciding to fire (in arc + range, via should_fire):
        // evasion still works by staying out of the gun arc, but a target
        // caught in front is hit at the pilot's skill rate. On a hit we
        // apply damage directly and the bolt is cosmetic (damage_cm=0);
        // on a miss the cosmetic bolt sprays wide. Player bolts are
        // unchanged real ballistics (damage on collision).
        Ship* npc_target = nullptr;
        float npc_hit_rate = 0.0f;
        if (!s.is_player && s.ai.target_id != 0) {
            npc_target = ships.find_by_id(s.ai.target_id);
            if (npc_target && !npc_target->alive) npc_target = nullptr;
            const float f2 = s.skill_f2;
            const float tt = std::clamp((f2 - 40.0f) / 20.0f, 0.0f, 1.0f);
            npc_hit_rate = 0.17f + 0.20f * std::pow(tt, 1.6f);
        }
        static std::mt19937 s_hit_rng(0xB0117E5u);
        std::uniform_real_distribution<float> s_hit_U(0.0f, 1.0f);

        for (size_t i = 0; i < s.mounts.size(); ++i) {
            if (s.gun_cooldowns[i] > 0.0f)            continue;
            const GunMount& m  = s.mounts[i];
            if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
            const GunStats& gs = g_gun_stats[(int)m.type];
            if (!gs.complete)                          continue;   // null-data gun
            if (s.energy_gj < gs.energy_cost_gj)       continue;   // dry

            Projectile p;
            // Muzzle position: ship pos + rotated mount offset.
            p.position = HMM_AddV3(s.position, body_to_world(s.orientation, m.offset_body));

            HMM_Vec3 shot_dir = fwd_world;
            float    shot_damage = gs.damage_cm;

            if (npc_target) {
                // Roll this shot against the pilot's skill hit rate.
                const bool will_hit = (s_hit_U(s_hit_rng) < npc_hit_rate);
                const HMM_Vec3 tpos = npc_target->sprite ? npc_target->sprite->position
                                                         : npc_target->position;
                const HMM_Vec3 muzzle_to_t = HMM_SubV3(tpos, p.position);
                const float    mt_l2 = HMM_DotV3(muzzle_to_t, muzzle_to_t);
                const HMM_Vec3 to_t_u = (mt_l2 > 1e-6f)
                    ? HMM_DivV3F(muzzle_to_t, std::sqrt(mt_l2)) : fwd_world;
                if (will_hit) {
                    // Apply damage directly (instant, reliable) and send a
                    // cosmetic tracer straight at the target so it reads as
                    // a hit. facing = the side facing the shooter.
                    const HMM_Vec3 hit_pos = HMM_SubV3(tpos,
                        HMM_MulV3F(to_t_u, ship::hit_radius_m(*npc_target)));
                    const HitFacing facing = ship::facing_of_hit(*npc_target, hit_pos);
                    ship::take_damage(*npc_target, gs.damage_cm, facing);
                    if (!npc_target->alive) npc_target->killed_by_id = s.id;
                    gunnery_probe::hit(s.skill_f2);
                    shot_dir = to_t_u;          // tracer converges on target
                } else {
                    // Deliberate miss: spray a few degrees off the target.
                    const HMM_Vec3 ref = (std::fabs(to_t_u.Y) < 0.99f)
                        ? HMM_V3(0,1,0) : HMM_V3(1,0,0);
                    const HMM_Vec3 ux = HMM_NormV3(HMM_Cross(ref, to_t_u));
                    const HMM_Vec3 vx = HMM_Cross(to_t_u, ux);
                    const float ph = s_hit_U(s_hit_rng) * 6.2831853f;
                    const float mag = 0.06f + 0.05f * s_hit_U(s_hit_rng);   // ~3-6 deg
                    shot_dir = HMM_NormV3(HMM_AddV3(to_t_u,
                        HMM_MulV3F(HMM_AddV3(HMM_MulV3F(ux, std::cos(ph)),
                                             HMM_MulV3F(vx, std::sin(ph))), mag)));
                }
                shot_damage = 0.0f;             // NPC bolts are cosmetic; damage via roll
            }

            // Velocity: shooter's full 3D world velocity + muzzle
            // speed along the aim direction.
            p.velocity = HMM_AddV3(ship_v, HMM_MulV3F(shot_dir, gs.speed_mps));
            p.damage_cm        = shot_damage;
            p.range_remaining  = gs.range_m;
            p.type             = m.type;
            p.owner_id         = s.id;
            p.alive            = true;
            projectiles.push_back(p);

            // Gunnery hit-rate probe: count NPC shots by shooter skill.
            if (!s.is_player) {
                gunnery_probe::shot(s.skill_f2);
            }

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
