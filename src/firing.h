#pragma once
// -----------------------------------------------------------------------------
// firing.h — turn `controller.fire_guns` into projectiles.
//
// Sits between ship::tick (which sets fire_guns based on AI / player input)
// and projectile::tick (which advances live projectiles). Each frame:
//
//   1. Decrement every gun's cooldown by dt; clamp at 0.
//   2. Recharge every ship's energy_gj toward klass->energy_recharge.
//   3. Fire each ready mount. The aim model is now PER-MOUNT, with two
//      kinds of mount (GunMount::is_turret):
//
//      FIXED forward gun (is_turret == false): fires only when the
//      ship's controller.fire_guns is set, the mount is off-cooldown,
//      has energy, AND is currently "armed" (gun_armed[i] == true). A
//      successful fire spawns one Projectile along the SHIP's nose
//      direction (body +Z; player uses the gimballed aim vector),
//      drains energy_cost_gj from the shared pool, and sets cooldown to
//      refire_delay_s. Off-center mounts still fire parallel from their
//      offset — the "wing guns converge somewhere ahead" look. Misses
//      (energy too low, on cooldown, disarmed) silently skip.
//
//      TURRET mount (is_turret == true): fires FREE and INDEPENDENT of
//      controller.fire_guns — a fleeing merchant's tail turret still
//      bites. Each frame it picks a target from the ship's perception
//      (nearest hostile, else the nearest in-cone Hostile contact),
//      lead-predicts the intercept point (aim.h) for that gun's
//      projectile speed, and fires along that per-mount direction IF the
//      lead bearing lies within the mount's forward_body cone
//      (cone_half_angle_deg) AND the target is within range_m. No energy
//      cost, no energy gate — only cooldown + arc + range gate it.
//      Works for BOTH player and NPC ships (issue #109 removed the old
//      NPC-only guard); gun_armed[] still gates which mounts fire so the
//      player keeps control via the G-key arm modes.
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
