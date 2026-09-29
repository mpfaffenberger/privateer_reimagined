// -----------------------------------------------------------------------------
// missile.cpp — guided-projectile guidance, lifetime, and detonation.
//
// See missile.h for the why (parallel-struct design, reused damage path).
// The hot loops mirror projectile.cpp deliberately so the two read the same.
// -----------------------------------------------------------------------------

#include "missile.h"

#include "faction.h"
#include "perception.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"
#include "world_scale.h"

#include <algorithm>
#include <cmath>
#include <iterator>

// ---- per-type stats ---------------------------------------------------------
// Tuned for "combat feel", not realism. Damage well above gun-per-shot
// (missiles are scarce); IR turns hardest (any-aspect tracker), HS softer
// (chasing the engine plume), DF doesn't turn at all. prox_radius gives the
// homing types a forgiving "close enough" so a near-miss still detonates.
const MissileStats g_missile_stats[kMissileTypeCount] = {
    //                short long          dmg   speed  turn   life  range   prox  lock  build auto
    // Damage = canonical Privateer cm-of-durasteel (Wing Commander CIC
    // reference): DF 13.0, HS 16.0, IR 17.5. The old 45/55/70 numbers hit
    // ~3-5x too hard (a single HS one-shot a Tarsus through armour+shield);
    // these line the missile up with the gun damage scale (1.8-10 cm/shot)
    // so a missile reads as a heavy single hit, not an instant kill.
    // Canonical Privateer (gamefaq): DF 13cm @1000kps, HS 16cm @800kps,
    // IR 17cm @850kps. Our muzzle speeds run hotter than canon's kps for
    // demo-scale readability, but the armour-penetration (damage) + lock
    // technique match the source exactly.
    /* DF */ { k_missile_codes[0], "Dumbfire",       13.0f, 1400.0f, 0.0f, 6.0f, 8000.0f, 60.0f, false, false },
    /* HS */ { k_missile_codes[1], "Heat-Seeker",    16.0f, 1200.0f, 1.2f, 8.0f, 9000.0f, 90.0f, true,  false },
    /* IR */ { k_missile_codes[2], "Image-Rec",      17.5f, 1100.0f, 2.4f, 9.0f,10000.0f,100.0f, true,  true  },
    // FF (#144): canon 17cm @900kps, 7200m. Speed/range scaled like HS
    // (same demo-scale bump); steering deliberately IS the HS seeker — FF
    // is "HS that picks its own target", not a better tracker.
    /* FF */ { k_missile_codes[3], "Friend-or-Foe",  17.0f, 1150.0f, 1.2f, 8.0f, 9000.0f, 90.0f, false, false, true },
    /* TORP*/ { k_missile_codes[4],"Torpedo",       60.0f,  600.0f, 0.0f,12.0f,14000.0f,100.0f, false, false },
};

namespace missile {

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
    // Only lock types take the launcher's target; DF/torpedo fly blind and
    // FF finds its own on the first tick (fire-and-forget, no lock).
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

// NPC rack pacing (#144). The first-launch delay means the opening gun
// pass comes before the first missile; the refire keeps a deep rack (a
// Kamekh's ten DFs) a steady threat rather than a single salvo.
constexpr float k_npc_first_launch_s = 4.0f;
constexpr float k_npc_refire_s       = 10.0f;

// npc_pick_round's preference: strongest seeker first.
constexpr MissileType k_npc_fire_order[] = {
    MissileType::IR, MissileType::HS, MissileType::FF, MissileType::DF,
};
static_assert(std::size(k_npc_fire_order) == kMissileRackTypeCount,
              "every rack type needs a place in the NPC fire order");

HMM_Vec3 ship_forward(const Ship& s) {
    const HMM_Mat4 r = HMM_QToM4(s.orientation);
    const HMM_Vec4 f = HMM_MulM4V4(r, HMM_V4(0.0f, 0.0f, 1.0f, 0.0f));
    return HMM_V3(f.X, f.Y, f.Z);
}

} // namespace

uint32_t acquire_iff_target(const ShipRegistry& ships, uint32_t owner_id,
                            const HMM_Vec3& from) {
    const Ship* owner = ships.find_by_id(owner_id);
    if (!owner || !owner->alive) return 0;   // no launcher, no IFF reference

    uint32_t best    = 0;
    float    best_d2 = 0.0f;
    for (const PerceivedContact& c : owner->perception.visible) {
        if (c.stance != Stance::Hostile) continue;
        const Ship* s = ships.find_by_id(c.ship_id);
        if (!s || !s->alive || s->id == owner_id) continue;
        const HMM_Vec3 d  = HMM_SubV3(ship_world_pos(*s), from);
        const float    d2 = HMM_DotV3(d, d);
        if (best == 0 || d2 < best_d2) { best = s->id; best_d2 = d2; }
    }
    return best;
}

void arm_npc_rack(Ship& s) {
    s.npc_missiles       = s.klass ? s.klass->default_missiles : MissileRack{};
    s.missile_cooldown_s = k_npc_first_launch_s;
    // Pirates' vanilla FF preference (#144) as a refit of the hull's load.
    if (s.faction == Faction::Pirate) {
        const int total = rack_total(s.npc_missiles);
        s.npc_missiles  = MissileRack{};
        s.npc_missiles[(int)MissileType::FF] = total;
    }
}

MissileType npc_pick_round(const MissileRack& rack, bool has_lock) {
    for (MissileType t : k_npc_fire_order) {
        if (rack[(int)t] <= 0) continue;
        if (g_missile_stats[(int)t].needs_lock && !has_lock) continue;
        return t;
    }
    return MissileType::Count;
}

int npc_launch(std::vector<Missile>& missiles, ShipRegistry& ships, float dt) {
    int launched = 0;
    for (Ship& s : ships) {
        if (s.is_player || !s.alive || rack_total(s.npc_missiles) <= 0) continue;
        // Shot-out launchers (#141) ground the rack, same as the player's.
        if (!ship_systems::operational(s.systems, ShipSystem::Launchers)) continue;
        // Only mid gun-run (target in arc + range): the refire clock
        // pauses otherwise, so a pirate never lobs one at a contact it
        // isn't actually fighting.
        if (s.ai.state != AIState::Engage || !s.controller.fire_guns) continue;
        s.missile_cooldown_s -= dt;
        if (s.missile_cooldown_s > 0.0f) continue;

        // Lock types lock the AI's own target — the ship it's gun-running
        // — not an IFF pick. No live target = only FF/DF are eligible.
        const Ship* tgt = s.ai.target_id ? ships.find_by_id(s.ai.target_id) : nullptr;
        const bool has_lock = tgt && tgt->alive && tgt->id != s.id;
        const MissileType type = npc_pick_round(s.npc_missiles, has_lock);
        if (type == MissileType::Count) continue;   // only lock rounds, no lock

        const HMM_Vec3 fwd    = ship_forward(s);
        const HMM_Vec3 muzzle = HMM_AddV3(ship_world_pos(s),
                                          HMM_MulV3F(fwd, ship::hit_radius_m(s)));
        // spawn() keeps the target only for lock types: FF self-acquires
        // on its first tick, DF flies straight along the nose.
        missiles.push_back(spawn(type, muzzle, fwd, s.world_velocity,
                                 s.id, has_lock ? tgt->id : 0));
        --s.npc_missiles[(int)type];
        s.missile_cooldown_s = k_npc_refire_s;
        ++launched;
    }
    return launched;
}

void tick(std::vector<Missile>& missiles, ShipRegistry& ships, float dt) {
    const float scaled_dt = dt * world_scale::k_world_velocity_scale;

    for (Missile& m : missiles) {
        if (!m.alive) continue;

        // ---- guidance: steer heading toward the target ----------------
        // Only homing missiles with a still-alive target steer. A DF (or a
        // homer whose target died / fell out of the registry) coasts on its
        // last heading — exactly what you want when a lock breaks. An FF
        // with no live mark asks the shooter's IFF for a new one first.
        const bool seeks = g_missile_stats[(int)m.type].auto_acquire;
        if (m.turn_rate_radps > 0.0f && (m.target_id != 0 || seeks)) {
            const Ship* tgt = m.target_id ? ships.find_by_id(m.target_id) : nullptr;
            if ((!tgt || !tgt->alive) && seeks) {
                m.target_id = acquire_iff_target(ships, m.owner_id, m.position);
                tgt = m.target_id ? ships.find_by_id(m.target_id) : nullptr;
            }
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
