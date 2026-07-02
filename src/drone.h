#pragma once
// -----------------------------------------------------------------------------
// drone.h — the Steltek drone, the campaign's cross-system pursuer
// (#133 M21 -> #135 M23).
//
// Taking the gun from the Delta Prime derelict sets `drone_active`; from
// then on an invulnerable green killer hunts the player in EVERY system
// until the finale kills it. Design:
//
//   * Code-side, not scenario data: the scenario director is per-trigger /
//     per-system, while the drone is a persistent world rule ("wherever
//     you fly, it finds you"). One module owns the whole lifecycle.
//   * Rate-limited dread, not spam: after launch / a jump the drone takes
//     k_first_spawn_delay_s to acquire the player, and if the player
//     outruns it (leaves it > k_leash_m behind) it despawns and re-stalks
//     after k_respawn_delay_s. Vanilla tuning: it harasses, it does not
//     guarantee death — you can always run for the pad or the gate.
//   * Ship::damage_immune stays TRUE for its whole life (the finale's
//     Confed fleet must bounce off it, #135). Once the M23 boost event
//     sets `steltek_gun_boosted`, the drone opens Ship::immune_bypass_gun
//     to GunType::SteltekGun — so ONLY the boosted gun's hits land.
//   * Death (only possible post-boost) stamps the kill-memory flag
//     `killed:steltek_drone` for the finale settle and stops respawns.
//
// The sandbox never sets drone_active, so tick() is a two-probe no-op.
// -----------------------------------------------------------------------------

#include "encounters.h"

struct PlayerState;
class ShipRegistry;

namespace drone {

// Seconds after reset() (launch / jump arrival) before the first stalk.
constexpr double k_first_spawn_delay_s = 25.0;
// Seconds after a despawn (outrun) before it finds the player again.
constexpr double k_respawn_delay_s     = 120.0;
// Player distance beyond which the drone loses the scent and despawns.
constexpr float  k_leash_m             = 60000.0f;
// How far out it materialises (outside gun range, inside dread range).
constexpr float  k_spawn_dist_m        = 14000.0f;

// Per-frame lifecycle (Flight mode only). Spawns/despawns the pursuer,
// keeps it aggroed on the player, re-derives invulnerability from plot
// state, and stamps killed:steltek_drone when it dies. `now_s` is any
// monotonic seconds clock; `despawn` frees a ship the player outran.
void tick(ShipRegistry& ships, PlayerState& p, HMM_Vec3 player_pos,
          double now_s, const encounters::SpawnFn& spawn,
          const encounters::DespawnFn& despawn);

// Forget the live drone + re-arm the first-spawn timer. Call on launch
// and on system switch (the registry frees NPCs there anyway; the drone
// "re-acquires" the player after k_first_spawn_delay_s).
void reset();

// Live drone ship id (0 = none). For the HUD/debug and the M23 settle.
uint32_t active_ship_id();

} // namespace drone
