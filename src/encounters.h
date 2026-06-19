#pragma once
// -----------------------------------------------------------------------------
// encounters.h — the encounter director: procedural NPC traffic.
//
// This is the system that makes a star system feel inhabited instead of
// staged. Where the old world was a fixed set of JSON-placed ships that
// existed the moment you spawned and never changed, the director keeps a
// *living population* around the player: it spawns NPCs offscreen-ish as
// you travel, lets them fly in and react via the existing AI + faction
// stance machinery, and quietly retires the ones that drift far behind
// you so the registry never fills up with ghosts you'll never see again.
//
// Division of labour (this matters — it's why this file has no AppState):
//
//   * The director (here) owns WHEN, WHERE and WHO. It reads the per-
//     system spawn tables (system_def.h: EncounterRuleDef), tracks a
//     budget + a timer per rule, picks a faction and a class from the
//     weighted mixes, and computes a spawn point that's in-region and
//     8-15 km from the player (close enough to fly in, far enough not to
//     pop in on screen).
//
//   * The host (main.cpp) owns WHAT — the literal np-eag.1 spawn recipe:
//     claim a sprite slot, attach the class atlas, ship::spawn() for
//     class-derived health/mounts, assign faction + enable AI, and
//     register the Ship in the slot-map. The director never touches a
//     sprite or the registry's storage directly; it asks through the
//     SpawnFn / DespawnFn hooks below. That keeps the sprite-slot pool +
//     atlas map (both AppState-resident) out of this translation unit and
//     means the debug spawn button and the director run the exact same
//     recipe — DRY by construction, not by copy-paste.
//
// Spawn budget + the "don't pop in visibly" placement strategy:
//
//   * Each rule has its own max_concurrent; the director also enforces a
//     global cap (k_director_max) so the whole population stays modest
//     regardless of how many rules a system authors. A registry-wide hard
//     cap (k_registry_hard_cap) is the final backstop against runaway
//     spawning — perf + feel both want the world in the tens of ships.
//
//   * New ships spawn at a random point 8-15 km from the player. At that
//     range a fighter-sized billboard is a sub-pixel speck, so the player
//     never sees one blink into existence — they appear as a distant
//     contact and close the distance under their own power. Region rules
//     bias WHERE on that shell the point lands (inside the belt AABB,
//     along a station lane) and ALSO gate whether the rule fires at all:
//     belt pirates only spawn when you're actually near the belt.
//
//   * Despawn is distance-and-state gated: a director ship past
//     k_despawn_radius (40 km) that is NOT currently engaging/breaking is
//     retired (slot freed). The engagement check means a pirate chasing
//     you at 41 km is never yanked out from under the fight — only ships
//     that have genuinely lost interest and drifted away get cleaned up.
//
// No per-frame allocation in the hot path: rule state + the per-rule
// managed-id lists are built once at init and reused; tick() only does
// distance compares over existing vectors and the occasional spawn/
// despawn. The RNG is a member, distributions are stack-local scalars.
// -----------------------------------------------------------------------------

#include "faction.h"
#include "ship_ai.h"

#include <HandmadeMath.h>
#include <cstdint>
#include <functional>
#include <string>

struct StarSystem;
class ShipRegistry;

namespace encounters {

// ---- tuning knobs -----------------------------------------------------------
// World units (= metres). Spawn shell, keep-alive bubble, population caps.
constexpr float k_spawn_dist_min    =  8000.0f;  // nearest a fresh NPC appears
constexpr float k_spawn_dist_max    = 15000.0f;  // farthest — still a distant speck
constexpr float k_despawn_radius    = 40000.0f;  // beyond this, retire (if idle)
constexpr float k_region_activate_m = 25000.0f;  // player-to-region range to arm a rule
constexpr int   k_director_max      = 8;         // total director-owned ships (legacy rule director)
constexpr int   k_registry_hard_cap = 64;        // absolute backstop on registry size
constexpr int   k_entry_population_max = 24;     // cap for a one-shot system-entry roll

// What the director asks the host to bring into the world. The host
// resolves class_name -> atlas + display size and runs the spawn recipe.
struct SpawnRequest {
    std::string class_name;                 // ship_class::find key, e.g. "talon"
    HMM_Vec3    position    = { 0, 0, 0 };  // world spawn point (8-15 km from player)
    Faction     faction     = Faction::Civilian;
    AIState     initial_ai_state = AIState::Patrol;
    HMM_Vec3    patrol_anchor    = { 0, 0, 0 };  // loiter / flee-home tether (= spawn pos)
};

// Host hooks (the np-eag.1 recipe). spawn returns the new ship's
// monotonic id, or 0 on failure (class/atlas not loaded) — the director
// keys its bookkeeping off that id and skips a failed spawn. despawn
// frees the sprite slot + registry slot for the given id.
using SpawnFn   = std::function<uint32_t(const SpawnRequest&)>;
using DespawnFn = std::function<void(uint32_t id)>;

// Parse the system's spawn tables into runtime rule state. Idempotent —
// safe to call on every system load / jump; clears any prior state first.
void init(const StarSystem& system);

// Drop all director state (call on teardown or before re-init for a new
// system). Does NOT despawn live ships — the host owns the registry and
// tears it down its own way; this just forgets the bookkeeping.
void shutdown();

// Advance the population one frame. Prunes dead/gone ships from the
// bookkeeping, retires drifted-away ones via `despawn`, and spawns new
// ones via `spawn` when a rule is armed, under budget, and its interval
// has elapsed. Call once per Flight frame, after perception + AI (so a
// ship spawned this frame is simply processed next frame). No-op when no
// rules were authored.
void tick(const ShipRegistry& ships, HMM_Vec3 player_pos, float dt,
          const SpawnFn& spawn, const DespawnFn& despawn);

// wcnews encounter model (np): roll each nav point's per-nav probability
// table ONCE and spawn the chosen group near that nav. This is the ONLY
// thing that introduces NPC traffic now — there is no continuous refill.
// Call it when the dice should be re-rolled: on system entry (system load /
// jump-in) and on base launch. `player_pos` is used only to nudge a group
// off the player's own spawn point so nothing pops in point-blank. Spawns
// go through the same host SpawnFn recipe the director used.
void populate_on_entry(const StarSystem& system, HMM_Vec3 player_pos,
                       const SpawnFn& spawn);

// Live count of director-owned ships (for HUD / debug). Cheap.
int population();

} // namespace encounters
