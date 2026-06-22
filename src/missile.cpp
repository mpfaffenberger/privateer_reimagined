// -----------------------------------------------------------------------------
// missile.cpp — guided-projectile guidance, lifetime, and detonation.
//
// See missile.h for the why (parallel-struct design, reused damage path).
// The hot loops mirror projectile.cpp deliberately so the two read the same.
// -----------------------------------------------------------------------------

#include "missile.h"

#include "ship.h"
#include "ship_registry.h"
#include "ship_sprite.h"
#include "world_scale.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ---- per-type stats ---------------------------------------------------------
// Tuned for "combat feel", not realism. Damage well above gun-per-shot
// (missiles are scarce); IR turns hardest (any-aspect tracker), HS softer
// (chasing the engine plume), DF doesn't turn at all. prox_radius gives the
// homing types a forgiving "close enough" so a near-miss still detonates.
const MissileStats g_missile_stats[kMissileTypeCount] = {
    //                short long          dmg   speed  turn   life  range   prox  lock  build
    // Damage = canonical Privateer cm-of-durasteel (Wing Commander CIC
    // reference): DF 13.0, HS 16.0, IR 17.5. The old 45/55/70 numbers hit
    // ~3-5x too hard (a single HS one-shot a Tarsus through armour+shield);
    // these line the missile up with the gun damage scale (1.8-10 cm/shot)
    // so a missile reads as a heavy single hit, not an instant kill.
    // Canonical Privateer (gamefaq): DF 13cm @1000kps, HS 16cm @800kps,
    // IR 17cm @850kps. Our muzzle speeds run hotter than canon's kps for
    // demo-scale readability, but the armour-penetration (damage) + lock
    // technique match the source exactly.
    /* DF */ { "DF", "Dumbfire",       13.0f, 1400.0f, 0.0f, 6.0f, 8000.0f, 60.0f, false, false },
    /* HS */ { "HS", "Heat-Seeker",    16.0f, 1200.0f, 1.2f, 8.0f, 9000.0f, 90.0f, true,  false },
    /* IR */ { "IR", "Image-Rec",      17.0f, 1100.0f, 2.4f, 9.0f,10000.0f,100.0f, true,  true  },
};

namespace missile {

MissileType from_name(const char* s) {
    if (!s) return MissileType::Count;
    for (int i = 0; i < kMissileTypeCount; ++i)
        if (std::strcmp(s, g_missile_stats[i].short_name) == 0) return (MissileType)i;
    return MissileType::Count;
}

const char* to_name(MissileType t) {
    const int i = (int)t;
    return (i >= 0 && i < kMissileTypeCount) ? g_missile_stats[i].short_name : "?";
}

Missile spawn(MissileType type, const HMM_Vec3& muzzle_pos,
              const HMM_Vec3& forward, const HMM_Vec3& shooter_vel,
              uint32_t owner_id, uint32_t target_id) {
    const MissileStats& ms = g_missile_stats[(int)type];

    // Normalize the launch direction; fall back to +Z if a degenerate
    // forward sneaks in (shouldn't, but a zero heading would NaN the
    // guidance math downstream).
    HMM_Vec3 fwd = forward;
    const float l2 = HMM_DotV3(fwd, fwd);
    fwd = (l2 > 1e-6f) ? HMM_DivV3F(fwd, std::sqrt(l2)) : HMM_V3(0, 0, 1);

    Missile m;
    m.position        = muzzle_pos;
    m.heading         = fwd;
    m.velocity        = HMM_AddV3(shooter_vel, HMM_MulV3F(fwd, ms.speed_mps));
    m.damage_cm       = ms.damage_cm;
    m.speed_mps       = ms.speed_mps;
    m.turn_rate_radps = ms.turn_rate_radps;
    m.life_remaining  = ms.lifetime_s;
    m.range_remaining = ms.range_m;
    m.prox_radius_m   = ms.prox_radius_m;
    m.owner_id        = owner_id;
    // Unguided types ignore the target id entirely.
    m.target_id       = ms.needs_lock ? target_id : 0;
    m.type            = type;
    m.alive           = true;
    return m;
}

namespace {

// Latest world pose of a ship (sprite if present, else canonical pose) —
// same freshness reasoning as projectile.cpp's collide pass.
HMM_Vec3 ship_world_pos(const Ship& s) {
    return s.sprite ? s.sprite->position : s.position;
}

} // namespace

void tick(std::vector<Missile>& missiles, ShipRegistry& ships, float dt) {
    const float scaled_dt = dt * world_scale::k_world_velocity_scale;

    for (Missile& m : missiles) {
        if (!m.alive) continue;

        // ---- guidance: steer heading toward the target ----------------
        // Only homing missiles with a still-alive target steer. A DF (or a
        // homer whose target died / fell out of the registry) coasts on its
        // last heading — exactly what you want when a lock breaks.
        if (m.turn_rate_radps > 0.0f && m.target_id != 0) {
            const Ship* tgt = ships.find_by_id(m.target_id);
            if (tgt && tgt->alive) {
                const HMM_Vec3 to_t = HMM_SubV3(ship_world_pos(*tgt), m.position);
                const float    d2   = HMM_DotV3(to_t, to_t);
                if (d2 > 1e-6f) {
                    const HMM_Vec3 want = HMM_DivV3F(to_t, std::sqrt(d2));
                    // Angle between current heading and desired bearing.
                    const float dot   = std::clamp(HMM_DotV3(m.heading, want), -1.0f, 1.0f);
                    const float angle = std::acos(dot);
                    const float max_step = m.turn_rate_radps * scaled_dt;
                    if (angle <= max_step || angle < 1e-4f) {
                        m.heading = want;                 // can snap to bearing
                    } else {
                        // Rotate `heading` toward `want` by max_step via a
                        // normalized slerp-ish lerp: blend then renormalize.
                        // Cheap and stable for the small per-frame steps here.
                        const float t = max_step / angle;
                        HMM_Vec3 h = HMM_AddV3(HMM_MulV3F(m.heading, 1.0f - t),
                                               HMM_MulV3F(want, t));
                        const float hl2 = HMM_DotV3(h, h);
                        if (hl2 > 1e-6f) m.heading = HMM_DivV3F(h, std::sqrt(hl2));
                    }
                    // Rebuild velocity along the steered heading at the
                    // missile's own speed (drop the inherited drift once
                    // it's guiding — it should fly where it points).
                    m.velocity = HMM_MulV3F(m.heading, m.speed_mps);
                }
            } else {
                // Target gone — forget it so we stop trying to find it.
                m.target_id = 0;
            }
        }

        // ---- integrate + expire ---------------------------------------
        const HMM_Vec3 step = HMM_MulV3F(m.velocity, scaled_dt);
        m.position = HMM_AddV3(m.position, step);
        const float traveled = std::sqrt(HMM_DotV3(step, step));
        m.range_remaining -= traveled;
        m.life_remaining  -= dt;
        if (m.range_remaining <= 0.0f || m.life_remaining <= 0.0f)
            m.alive = false;
    }

    // Swap-compact dead entries (matches projectile::tick).
    size_t write = 0;
    for (size_t read = 0; read < missiles.size(); ++read) {
        if (missiles[read].alive) {
            if (write != read) missiles[write] = missiles[read];
            ++write;
        }
    }
    missiles.resize(write);
}

void collide_and_damage(std::vector<Missile>& missiles, ShipRegistry& ships,
                        float dt) {
    const float scaled_dt = dt * world_scale::k_world_velocity_scale;

    for (Missile& m : missiles) {
        if (!m.alive) continue;

        const HMM_Vec3 prev = HMM_SubV3(m.position, HMM_MulV3F(m.velocity, scaled_dt));

        // --- proximity detonation against the homing target -------------
        // A guided missile that gets within prox_radius of its target
        // detonates even without a dead-center swept hit — the "close
        // enough" splash that makes homing missiles feel lethal.
        if (m.target_id != 0) {
            // One lookup, reused for the proximity test AND the damage
            // apply (#12) — take_damage wants a mutable Ship* anyway.
            Ship* tgt = ships.find_by_id(m.target_id);
            if (tgt && tgt->alive && tgt->id != m.owner_id) {
                const HMM_Vec3 tp = ship_world_pos(*tgt);
                const HMM_Vec3 d  = HMM_SubV3(tp, m.position);
                const float prox = m.prox_radius_m + ship::hit_radius_m(*tgt);
                if (HMM_DotV3(d, d) <= prox * prox) {
                    const HitFacing facing = ship::facing_of_hit(*tgt, m.position);
                    ship::take_damage(*tgt, m.damage_cm, facing);
                    if (!tgt->alive) tgt->killed_by_id = m.owner_id;
                    m.alive = false;
                    continue;
                }
            }
        }

        // --- direct swept-segment hit against any ship ------------------
        // Identical geometry to projectile::collide_and_damage. Missiles
        // can hit anything (not just their target) — a DF into a crowd, or
        // a homer clipping a ship between it and its mark.
        for (Ship& s : ships) {
            if (!s.alive)           continue;
            if (s.id == m.owner_id) continue;

            const HMM_Vec3 ship_pos = ship_world_pos(s);
            const float radius = ship::hit_radius_m(s);
            if (radius <= 0.0f) continue;
            const float r2 = radius * radius;

            const HMM_Vec3 seg     = HMM_SubV3(m.position, prev);
            const HMM_Vec3 to_ship = HMM_SubV3(ship_pos, prev);
            const float    seg_l2  = HMM_DotV3(seg, seg);
            float t = 0.0f;
            if (seg_l2 > 1e-6f)
                t = std::clamp(HMM_DotV3(to_ship, seg) / seg_l2, 0.0f, 1.0f);
            const HMM_Vec3 closest = HMM_AddV3(prev, HMM_MulV3F(seg, t));
            const HMM_Vec3 diff    = HMM_SubV3(ship_pos, closest);
            if (HMM_DotV3(diff, diff) < r2) {
                const HitFacing facing = ship::facing_of_hit(s, closest);
                ship::take_damage(s, m.damage_cm, facing);
                if (!s.alive) s.killed_by_id = m.owner_id;
                m.alive = false;
                break;
            }
        }
    }
}

} // namespace missile
