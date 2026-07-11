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
constexpr int   k_mission_force_max  = 8;        // hard cap on one mission's forced wing
constexpr float k_mission_arm_m      = 20000.0f; // arm a mission spawn within this of its objective
constexpr float k_bounty_base_standoff_m = 18000.0f; // bounty quarry won't arm while the player is this close to a base (no launch-pad ambush)

// Scout / Patrol RANDOM encounter knobs (the "enemies may or may not show
// up" model). Each nav rolls once on first approach; a hit spawns a small
// group, a miss marks the objective so it never re-rolls. Tunable.
constexpr float k_scout_encounter_chance = 0.6f;  // P(a random encounter fires at a nav)
constexpr int   k_scout_spawn_min       = 1;     // min ships in a random scout/patrol group
constexpr int   k_scout_spawn_max       = 3;     // max ships in a random scout/patrol group
// Cap on how many navs on a SINGLE patrol/scout route may actually yield
// enemies. Once this many objectives have spawned hostiles, every remaining
// nav auto-misses (marked one-time). Keeps a long patrol from turning into
// a gauntlet; 2 matches the owner's "max 2 rolls per mission that yield
// enemies" rule. Scout has one nav so the cap is moot for it, but applying
// it uniformly is harmless.
constexpr int   k_patrol_max_enemy_navs = 2;

// Anti-hammer cap for guarantee_full arming (Attack/DefendBase/Bounty): if a
// mission's objective can't be armed to its required count after this many
// top-up attempts (a faction with no spawnable class in this system), stop
// retrying and mark it one-time so we don't spawn-attempt every frame forever.
// ~0.5s at 60fps — generous enough to ride out a transient atlas-load delay.
constexpr int   k_max_arm_attempts = 30;

// What the director asks the host to bring into the world. The host
// resolves class_name -> atlas + display size and runs the spawn recipe.
struct SpawnRequest {
    std::string class_name;                 // ship_class::find key, e.g. "talon"
    std::string display_name;               // named pilot/NPC; empty = class label
    HMM_Vec3    position    = { 0, 0, 0 };  // world spawn point (8-15 km from player)
    Faction     faction     = Faction::Civilian;
    AIState     initial_ai_state = AIState::Patrol;
    HMM_Vec3    patrol_anchor    = { 0, 0, 0 };  // loiter / flee-home tether (= spawn pos)

    // Non-combat behaviour (ship_ai CivRole). The host copies these onto the
    // spawned ship's ai so it does something when no one's shooting:
    // Travelers cruise the lanes, Loiterers circle the anchor, Escorts hold
    // formation on formation_lead_id (returned from the lead's own spawn).
    CivRole     civ_role          = CivRole::None;
    uint32_t    formation_lead_id = 0;
    HMM_Vec3    formation_offset  = { 0, 0, 0 };
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
// `sun_pos` is the system centre (np-3dp.22): traffic that would roll at a
// dockable BASE is pushed ~7.5-10k toward it, so ships never clutter a
// base's auto-land approach and you fly in toward the pad through clear
// space.
void populate_on_entry(const StarSystem& system, HMM_Vec3 player_pos,
                       HMM_Vec3 sun_pos, const SpawnFn& spawn);

// ---- mission-driven forced spawns (#14) ------------------------------------
//
// SEPARATE from the ambient director + the per-nav entry roll: this is the
// ONLY path that GUARANTEES an active combat mission's required opposition
// shows up at its objective, so Attack / DefendBase / Bounty are actually
// completable. Because it keys off the LIVE mission set (not a system spawn
// table) and lives in its own bookkeeping, removing/abandoning the mission
// removes the bias entirely — the ambient feel is untouched.
//
// The per-mission spawned-id lists live in this TU alongside the director
// state. The spawned ships are plain registry NPCs: the normal kill path
// frees them when destroyed (so the objective CLEARS and #13 settles the
// payout), and leaving the system / launching from a base clears them with
// the rest of the wave — same lifecycle as ambient traffic. We never
// double-spawn: once a mission id is populated it stays recorded until
// sync_active_missions() drops it.

// What the host (main.cpp) distils from one active combat ActiveMission.
// encounters.cpp stays free of player.h / missions.h, so the host builds
// this POD and resolves the anchor + faction itself.
//
// `objective_key` is the per-objective one-time gate: the nav NAME for
// scout/patrol/attack, the base_id for defend, a sentinel ("__bounty__")
// for bounty. A mission's track records which objective_keys have already
// triggered, so a Patrol can spawn independently at each nav — one-time
// each — without re-rolling or re-spawning.
struct MissionForce {
    std::string mission_id;                 // ActiveMission::id (bookkeeping key)
    std::string objective_key;              // per-objective one-time gate (see above)
    HMM_Vec3    anchor   = { 0, 0, 0 };     // target nav/base world position
    Faction     faction  = Faction::Pirate; // resolved target_faction
    int         count    = 0;               // ships to guarantee at the objective
    bool        guarantee_full = false;     // GIVEN types: keep topping up until
                                            // `count` are live, then mark one-time.
                                            // RANDOM types (scout/patrol) leave this
                                            // false: a partial roll is acceptable.
};

// The distinct ship classes a `faction` would field in THIS system, drawn
// from the system's own spawn weighting (per-nav encounter groups + legacy
// rule class mixes) plus a per-faction fallback fighter. The host preloads
// these atlases before ensure_mission_force() so the spawn always resolves.
std::vector<std::string> faction_fighter_classes(const StarSystem& system,
                                                 Faction faction);

// Ensure the force for `mf` exists at its objective. Find-or-create the
// per-mission track; if `mf.objective_key` is already in the track's
// `triggered` set (this objective already rolled/spawned) → return 0.
// Otherwise spawn up to `count` ships of the faction's typical fighters
// (weighted by `system`'s own spawn tables) on the 8-15 km shell around the
// anchor, push their ids into the track, and insert `objective_key` into
// `triggered`. This lets a Patrol spawn independently at each nav —
// one-time each — while a Scout / Attack / DefendBase / Bounty key on a
// single objective. Returns the number of ships spawned this call.
int ensure_mission_force(const StarSystem& system, const MissionForce& mf,
                         HMM_Vec3 player_pos, const SpawnFn& spawn);

// Mark `objective_key` as triggered for `mission_id` WITHOUT spawning — used
// when a scout/patrol random roll MISSES so the objective is one-time and
// won't re-roll every frame. Find-or-create the track and insert the key.
void mark_mission_objective_triggered(const std::string& mission_id,
                                      const std::string& objective_key);

// Query whether `objective_key` has already triggered for `mission_id`
// (either spawned via ensure_mission_force or pre-rolled-miss via
// mark_mission_objective_triggered). The host calls this BEFORE rolling a
// scout/patrol chance so the roll happens exactly once per objective.
bool mission_objective_triggered(const std::string& mission_id,
                                 const std::string& objective_key);

// How many distinct objectives for `mission_id` have actually YIELDED
// enemies (spawned >0). The host uses this to cap a patrol/scout route at
// k_patrol_max_enemy_navs yielding navs.
int  mission_yielded_count(const std::string& mission_id);

// Reconcile the tracked mission set with the live one: drop bookkeeping for
// any mission id NOT in `active_ids` (abandoned / completed / jumped away) so
// a re-accepted/re-entered mission can repopulate. Does NOT despawn — the
// ships are plain NPCs the kill / leave paths already own. Call once per
// frame before the ensure_mission_force() calls.
void sync_active_missions(const std::vector<std::string>& active_ids);

// Prune stale ids from every mission track: drop ids whose ship is gone
// (!s || !s->alive) so the "already populated?" check reflects the LIVE wing,
// not ghosts that drifted away / were destroyed. Call once per Flight frame
// before ensure_mission_force() (update_mission_forces in main.cpp does).
void prune_mission_tracks(const ShipRegistry& ships);

// Live count of director-owned ships (for HUD / debug). Cheap.
int population();

} // namespace encounters
