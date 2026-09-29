#pragma once
// -----------------------------------------------------------------------------
// missile.h — guided projectiles with finite ammo (np-zte.2).
//
// Missiles are the "smart" cousin of Projectile (projectile.h). Where a gun
// bolt flies dead-straight and is spawned every frame the trigger's held,
// a missile:
//   * steers toward a target each frame, clamped to a per-type turn rate,
//   * comes from a FINITE inventory in PlayerState (one fired = one gone),
//   * detonates on PROXIMITY (within prox_radius_m of the target) as well
//     as on a direct swept-segment hit, and
//   * has a hard lifetime — it self-destructs (no damage) if it runs out
//     of fuel before reaching anything.
//
// DESIGN CHOICE — parallel struct, NOT an extended Projectile.
// The spec offered two routes (extend Projectile with optional guidance, or
// a parallel Missile struct). We took the parallel struct because:
//   * Projectile is documented as orientation-less ("render as a radial
//     glow"); a homing missile fundamentally needs a heading to steer.
//   * Missile firing is gated on ammo + target lock, not gun mounts +
//     energy — bolting that onto firing::tick would muddy a clean loop.
//   * Keeping Projectile untouched means zero risk to the existing gun
//     pipeline (the thing combat balance currently rests on).
// The ONE thing we deliberately reuse is the DAMAGE path: missile::collide
// calls ship::take_damage / ship::facing_of_hit exactly like
// projectile::collide_and_damage, so there's a single damage definition
// (no forked damage logic, per the task constraint).
//
// Guidance kinds (the DF / HS / IR / FF of the bead title):
//   DF  Dumbfire     — no lock, no steering. Flies straight; turn_rate 0.
//   HS  Heat-seeking — needs a locked target; homes toward it. Modelled as
//                      a slower turn rate (it chases the engine glow, so
//                      it's a bit sluggish reacquiring a jinking target).
//   IR  Image-recog  — needs a locked target HELD in the reticle for a
//                      build-up window (the lock delay lives in main.cpp);
//                      once away, homes hard (high turn rate, any aspect).
//   FF  Friend-or-Foe — fire-and-forget, NO lock (#144). HS-grade homing,
//                      but it picks its own mark: the nearest ship whose
//                      IFF reads hostile TO THE SHOOTER (read straight off
//                      the shooter's perception, so it can't disagree with
//                      the radar colours). Re-acquires if its mark dies —
//                      and, vanilla-accurately, gets spoofed onto a
//                      friendly whose IFF reads hostile (e.g. one the
//                      shooter provoked).
//
// The lock STATE MACHINE (seeking -> locked, tones, the IR build-up timer)
// lives at the call site (main.cpp) because it's bound up with the camera
// reticle + sfx; this module just exposes the per-type facts (needs_lock,
// lock_buildup) it keys off.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>
#include <cstdint>
#include <vector>

enum class MissileType : uint8_t {
    DF = 0,   // dumbfire
    HS,       // heat-seeking
    IR,       // image-recognition
    FF,       // friend-or-foe (#144) — auto-acquires the nearest IFF hostile
    TORPEDO,  // torpedo (np-3dp.26 + np-zte.2) — its own launcher + ammo rack
    Count
};
constexpr int kMissileTypeCount = (int)MissileType::Count;
// Every type before TORPEDO shares the missile launcher's rack
// (PlayerState::missiles); torpedoes have their own tube + counter.
constexpr int kMissileRackTypeCount = (int)MissileType::TORPEDO;

struct MissileStats {
    const char* short_name;        // "DF", "HS", "IR" — HUD readout
    const char* long_name;         // "Dumbfire", "Heat-Seeker", "Image-Rec"
    float damage_cm        = 0.0f; // cm-of-durasteel, same unit as guns
    float speed_mps        = 0.0f; // muzzle speed (added to shooter velocity)
    float turn_rate_radps  = 0.0f; // max steering rate; 0 = no homing (DF)
    float lifetime_s       = 0.0f; // self-destruct fuse
    float range_m          = 0.0f; // also self-destructs past this travel
    float prox_radius_m    = 0.0f; // detonation distance to target center
    bool  needs_lock       = false;// HS/IR require a target id
    bool  lock_buildup     = false;// IR needs the held-reticle delay
    bool  auto_acquire     = false;// FF picks (and re-picks) its own IFF target
};

// Per-type stats. Indexed by MissileType. Hand-tuned constants (this is
// combat-feel polish, not a data-driven weapons system — see the bead);
// promote to JSON if/when missiles get a shop catalog.
extern const MissileStats g_missile_stats[kMissileTypeCount];

// One in-flight missile. Mirrors Projectile's fields plus a heading (so we
// can steer it) and a target_id (0 = unguided / lost target).
struct Missile {
    HMM_Vec3    position { 0, 0, 0 };
    HMM_Vec3    velocity { 0, 0, 0 };   // m/s, world frame
    HMM_Vec3    heading  { 0, 0, 1 };   // unit, current travel direction
    float       damage_cm       = 0.0f;
    float       speed_mps       = 0.0f;
    float       turn_rate_radps = 0.0f;
    float       life_remaining  = 0.0f; // s; counts down each tick
    float       range_remaining = 0.0f; // m; counts down by traveled distance
    float       prox_radius_m   = 0.0f;
    uint32_t    owner_id        = 0;    // ship that fired (skip self-hits)
    uint32_t    target_id       = 0;    // homing target; 0 = dumbfire
    MissileType type            = MissileType::DF;
    bool        alive           = true;
};

struct Ship;
class ShipRegistry;

namespace missile {

// String <-> enum for HUD + logs. Returns Count on unknown.
MissileType from_name(const char* s);
const char* to_name(MissileType t);

// Build a freshly-armed Missile from its type, launch pose, and target.
// `forward` is the launch direction (unit); `shooter_vel` is inherited so
// the missile leaves a moving ship cleanly (same rationale as guns). For an
// unguided type, pass target_id 0 — guidance is skipped regardless.
Missile spawn(MissileType type, const HMM_Vec3& muzzle_pos,
              const HMM_Vec3& forward, const HMM_Vec3& shooter_vel,
              uint32_t owner_id, uint32_t target_id);

// Per-frame guidance + integration. For homing missiles with a live target,
// rotates `heading` toward the target by at most turn_rate*dt, rebuilds
// velocity along the new heading, then advances position. Counts down the
// fuse + range; expired missiles flip alive=false (no damage). Uses the
// same world-velocity scale as projectile::tick so missiles pace with the
// rest of the sim. Prunes dead entries (swap-compact) like projectile::tick.
void tick(std::vector<Missile>& missiles, ShipRegistry& ships, float dt);

// Detonation + damage pass. For each alive missile: a swept-segment test
// against every ship (direct hit) PLUS a proximity test against its target
// (homing splash). On detonation applies damage through ship::take_damage
// (the shared damage path — NOT a fork) and marks the missile dead. dt must
// match the value passed to tick (for the swept previous-position reconstruct).
void collide_and_damage(std::vector<Missile>& missiles, ShipRegistry& ships,
                        float dt);

// ---- IFF seeker (#144) ------------------------------------------------------
// Id of the nearest-to-`from` alive ship the shooter's IFF reads as hostile
// (Stance::Hostile in the owner's perception list), or 0 if none / the owner
// is gone. Reusing perception means FF sees exactly what the shooter's radar
// sees — including the spoof case of a provoked friendly.
uint32_t acquire_iff_target(const ShipRegistry& ships, uint32_t owner_id,
                            const HMM_Vec3& from);

// ---- NPC racks (#144) -------------------------------------------------------
// NPC missiles are FF-only today (pirates' vanilla favourite): a flat round
// count on Ship::ff_missiles, no launcher hardware model.
// Fits the faction's default FF rack onto a freshly spawned NPC (call once
// its faction is final). Pirates carry FF; everyone else flies without.
void arm_npc_rack(Ship& s);

// Per-frame NPC launch pass: an armed NPC mid gun-run (Engage + guns hot)
// fires one FF along its nose each time its refire cooldown elapses.
// Returns the number launched this frame (for logging).
int npc_launch(std::vector<Missile>& missiles, ShipRegistry& ships, float dt);

} // namespace missile
