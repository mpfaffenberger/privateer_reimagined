#pragma once
// -----------------------------------------------------------------------------
// escort.h — the campaign escort mechanic (#140; missions M10/M12/M13).
//
// An ESCORTEE is a friendly scripted ship with a travel goal: it spawns at
// the mission's meet point, cruises to a destination nav (a base), and
// "lands" there — despawn + a thank-you comm + a plot flag the campaign's
// landing-order gate reads. If it dies en route, the mission fails on the
// spot (active flags cleared -> the fixer re-offers, vanilla retry policy).
//
// Ownership split (mirrors the fixers/campaign face-vs-hands pattern):
//   * scripted_encounters.json AUTHORS the escort: a scenario may carry an
//     `escort{...}` block; the director calls escort::begin when the
//     scenario triggers, so meet-point, hail dialogue, and attack waves
//     stay in ONE data entry. Waves aggro the escortee via the spawn-group
//     field `aggro: "escortee"` (ShipAIState::preferred_target_id).
//   * THIS module owns the escortee's lifecycle: travel-goal pinning
//     (Traveler AI re-picks random lanes; we re-pin the dest every frame),
//     arrival detection (despawn + landed_flag), death detection (fail).
//   * campaign.cpp owns the settle: docking at the mission base pays out
//     iff landed_flag is set — landing BEFORE the escortee is the
//     vanilla-accurate landing-order failure.
//
// One escort at a time (the campaign never runs two), same single-slot
// policy as the scenario director. reset() — called at the scripted::reset
// sites (launch + system switch) — FAILS any in-flight escort: leaving the
// system mid-escort abandons the Drayman, and that costs you the mission.
// -----------------------------------------------------------------------------

#include "encounters.h"   // encounters::SpawnFn / DespawnFn

#include <string>
#include <vector>

struct PlayerState;
struct StarSystem;
class ShipRegistry;

namespace escort {

// One authored escort job (parsed by the director from a scenario's
// `escort` block; see scripted_encounters.h for the JSON shape).
struct Def {
    std::string faction    = "merchant";  // stance source (merchant = lawful)
    std::string ship_class = "drayman";
    std::string name;                     // display: "Toth's Drayman"
    std::string dest_nav;                 // NavPointDef::name to land at
    float       hull_scale = 1.0f;        // M13's tissue-paper Drayman < 1
    std::string landed_flag;              // set on safe arrival
    // Cleared when the escortee dies or the escort is abandoned — the
    // mission's active/underway flags, so the fixer re-offers.
    std::vector<std::string> fail_clear_flags;
};

// Spawn the escortee near `spawn_pos` and start tracking it. Refuses
// (false, logged) when an escort is already active or the spawn fails.
bool begin(const Def& def, HMM_Vec3 spawn_pos, ShipRegistry& ships,
           const StarSystem& system, PlayerState& player,
           const encounters::SpawnFn& spawn);

// Per-frame lifecycle: re-pin the travel goal, detect arrival (despawn +
// landed_flag + thanks) and death (fail_clear_flags + comm). Call once per
// Flight update after ship_ai::tick and before ship::tick.
//
// `player_warp` + `player_pos`: while the player's nav autopilot is
// engaged, the escortee KEEPS PACE (teleported alongside each frame) —
// the vanilla behavior where escorted ships auto-travel with you. Without
// it a Drayman covers 150 km at merchant cruise: a 30-minute crawl.
void tick(ShipRegistry& ships, const StarSystem& system, PlayerState& player,
          const encounters::DespawnFn& despawn,
          bool player_warp, HMM_Vec3 player_pos);

// The live escortee's ship id (0 = none). The director points
// `aggro: "escortee"` waves here.
uint32_t active_ship_id();

// Drop tracking. If an escort is still in flight it FAILS (flags cleared,
// comm line) — the player left it behind. `player` may be null during
// teardown frames (fails silently without flag writes in that case).
void reset(PlayerState* player);

} // namespace escort
