#pragma once
// -----------------------------------------------------------------------------
// firing.h — turn `controller.fire_guns` into projectiles.
//
// Sits between ship::tick (which sets fire_guns based on AI / player input)
// and projectile::tick (which advances live projectiles). Each frame:
//
//   1. Decrement every gun's cooldown by dt; clamp at 0.
//   2. Recharge every ship's energy_gj toward klass->energy_recharge.
//   3. For ships whose controller.fire_guns is set: try to fire each
//      mount that's off-cooldown, has energy, AND is currently "armed"
//      (gun_armed[i] == true). A successful fire spawns one Projectile,
//      drains energy_cost_gj from the shared pool, and sets cooldown to
//      refire_delay_s. Misses (energy too low, gun on cooldown, mount
//      disarmed) silently skip — Privateer didn't have an "out of ammo"
//      feedback loop and we don't need one yet.
//
// Aim model for v1: every mount fires straight along the SHIP's body
// +Z (forward). No gimbal, no per-mount aim, no lead prediction. The
// AI's Engage state already turns the ship to point at the target; if
// the target is in front of the nose, fire_guns is set. Mounts at off-
// center body offsets still fire forward, just from their offset
// position — produces the "wing guns converge somewhere ahead" look.
//
// Inheritance velocity: projectile starts with the SHIP's forward
// velocity added to the gun's muzzle speed. Realistic-feel — a fast
// ship's bullets fly faster, a fleeing ship's bullets fall short.
//
// Gun arm-mode (np-3dp): the G key cycles the player through a list of
// modes built dynamically from the ship's CURRENT mount list.
//   * 1 unique gun type  -> cycle {UNARMED, ALL}              (2 modes)
//   * N unique types     -> cycle {UNARMED, T1, T2, ..., TN, ALL}
//                            (N+2 modes, one slot per type)
// gun_mode_count_for_mounts returns the N+2 count; apply_gun_mode writes
// the gun_armed[] flags for a mode; gun_mode_label produces the HUD/
// console label; gun_mode_armed_count tallies "X of M armed" for the HUD.
// -----------------------------------------------------------------------------

#include <vector>

struct Ship;
struct Projectile;
struct GunMount;
class ShipRegistry;

namespace firing {

// Per-frame tick. Updates cooldowns + energy, spawns projectiles for
// any ship with controller.fire_guns set whose mounts are ready and
// armed. Player ships fire too — fire_guns is populated by main's input
// code. Iterates the registry (slot-map) — occupied slots only, alive
// checks unchanged from the old vector walk.
void tick(ShipRegistry& ships,
          std::vector<Projectile>& projectiles,
          float dt);

// Number of gun arm-modes for a ship's current mount list (np-3dp).
// Modes = unique GunTypes + 2 (unarmed slot + all slot). Each press of
// G cycles one step through this list. For a Tarsus with mass drivers
// only the result is 3: {UNARMED, MASS_DRIVER, ALL}.
int  gun_mode_count_for_mounts(const std::vector<GunMount>& mounts);

// Same vector the mode helpers use, cached so the HUD doesn't have to
// recompute every frame. Keyed on the mounts* address; cleared when
// outfitting replaces the mounts list. Order is first-occurrence.
const std::vector<int>& gun_unique_types_cache(
    const std::vector<GunMount>& mounts);

// Write per-mount gun_armed[] flags for the given mode index. mode_idx
// is taken mod the current mode count, so callers can pass the raw
// ++gun_mode_idx result without % cleanup. Also stores the normalised
// index back on the ship so the HUD/console can refer to it.
void apply_gun_mode(Ship& s, uint8_t mode_idx);

// Map a mode index to a short, UPPERCASE label for HUD/console:
// "UNARMED", "<GUN TYPE>" (e.g. "MESON BLASTER"), or "ALL". Caller
// passes the unique-types vector so the result reflects the current
// loadout. The returned string is in a small static buffer; copy it
// before calling again.
const char* gun_mode_label(const std::vector<int>& unique_types,
                           uint8_t mode_idx);

// Count of currently-armed mounts for a ship (used by the HUD's
// "(N of M armed)" indicator so the player sees what the active mode
// enables without having to fire first).
int  gun_mode_armed_count(const Ship& s);

} // namespace firing
