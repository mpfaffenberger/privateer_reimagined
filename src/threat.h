#pragma once
// -----------------------------------------------------------------------------
// threat.h — "is it safe to fly hands-off right now?" oracle.
//
// A deliberately tiny seam. The nav autopilot (autopilot.h) is the only
// caller today: before it engages — and every frame while it's engaged —
// it asks `threat::hostiles_near()` whether there are hostiles inside a
// danger bubble around the player. In classic Privateer you can't
// autopilot through a furball; the moment something shoots at you the
// computer hands the stick back.
//
// LIVE as of np-ma2.3. main.cpp wires the world in once at system load
// via set_world(); after that hostiles_near() runs a real spatial query
// over the ship registry, filtered by each NPC's faction stance toward
// the player (faction.h: stance_npc_vs_player folds baseline + rep).
// The autopilot's call sites never changed — the gate "just worked" the
// day the director shipped, exactly as the original seam promised.
//
// Before set_world() is called (or if it's reset to nullptrs on
// teardown / system change) the query falls back to the old always-false
// behaviour, so nothing crashes during the startup window.
//
// Kept in its own translation unit (not folded into autopilot.cpp) so
// that np-ma2.3 has an obvious, single place to grow without churning
// the autopilot state machine.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

class ShipRegistry;
struct PlayerReputation;

namespace threat {

// Wire the live world the oracle queries. Call once after the ship
// registry + player reputation exist (system load), and again with
// nullptrs on teardown / system change to revert to the safe
// always-false fallback. Stores non-owning pointers — the registry and
// reputation outlive the oracle (they live in AppState).
void set_world(const ShipRegistry* ships, const PlayerReputation* player_rep);

// True if any alive, hostile-to-the-player ship sits within `radius`
// (world units) of `player_pos`. "Hostile" = the NPC's faction stance
// toward the player (baseline + reputation) resolves to Stance::Hostile.
// Returns false when no world has been wired (see set_world).
bool hostiles_near(HMM_Vec3 player_pos, float radius);

// Distance (world units) from `player_pos` to the NEAREST alive,
// hostile-to-the-player ship, or FLT_MAX when there are none (or no
// world has been wired). Same "hostile" definition as hostiles_near —
// the two share one stance/alive predicate (threat.cpp) so they can't
// drift apart. Unlike hostiles_near (which short-circuits on the first
// in-range hit), this does a full nearest scan; the music director
// (np-ida) uses it to tier the in-flight combat score by how close the
// closest threat is (>5km vs <=5km).
float nearest_hostile_distance(HMM_Vec3 player_pos);

} // namespace threat
