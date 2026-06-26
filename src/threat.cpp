// -----------------------------------------------------------------------------
// threat.cpp — hostile-proximity oracle (live as of np-ma2.3).
//
// See threat.h for the why. The function signature was final from day one
// (the np-opa.3 autopilot gate calls it unchanged); np-ma2.3 only filled
// in the body and added set_world() to hand it the live ship population.
//
// The query is a plain linear scan over the registry's occupied slots.
// At the encounter director's ship count (tens, hard-capped) that's a few
// dozen distance compares — cheaper than the spatial-index bookkeeping it
// would replace. Promote to perception's bucketing if the world ever
// holds hundreds of combatants at once.
// -----------------------------------------------------------------------------

#include "threat.h"

#include "faction.h"
#include "ship.h"
#include "ship_registry.h"

#include <cfloat>
#include <cmath>

namespace threat {

namespace {
// Non-owning view of the world, set by main.cpp at system load. Null
// until then (and again after teardown) — the query degrades to the
// original always-false stub, so the startup window stays crash-free.
const ShipRegistry*     g_ships = nullptr;
const PlayerReputation* g_rep   = nullptr;

// The ONE "is this ship a live threat to the player?" predicate, shared
// by both queries below so the stance/alive rules can never drift apart
// (np-ida refactor). Stance vs the player folds the faction baseline
// with accumulated reputation (faction.h): only genuine hostiles count —
// a neutral merchant flying past is not a threat. Caller guarantees
// g_rep is non-null.
bool is_live_threat(const Ship& s) {
    if (s.is_player || !s.alive) return false;
    return s.ai.aggro_player || faction::stance_npc_vs_player(s.faction, *g_rep) == Stance::Hostile;
}
} // namespace

void set_world(const ShipRegistry* ships, const PlayerReputation* player_rep) {
    g_ships = ships;
    g_rep   = player_rep;
}

bool hostiles_near(HMM_Vec3 player_pos, float radius) {
    if (!g_ships || !g_rep) return false;   // world not wired yet — safe default

    const float r2 = radius * radius;
    for (const Ship& s : *g_ships) {
        if (!is_live_threat(s)) continue;   // shared predicate (see above)
        const HMM_Vec3 d = HMM_SubV3(s.position, player_pos);
        if (HMM_DotV3(d, d) <= r2) return true;   // first in-range hit wins
    }
    return false;
}

float nearest_hostile_distance(HMM_Vec3 player_pos) {
    if (!g_ships || !g_rep) return FLT_MAX;  // world not wired yet — "no threat"

    // Full nearest scan (no early out): the music director needs the
    // ACTUAL closest distance to tier far(>5k) vs near(<=5k) combat.
    float best2 = FLT_MAX;
    for (const Ship& s : *g_ships) {
        if (!is_live_threat(s)) continue;   // same predicate as hostiles_near
        const HMM_Vec3 d  = HMM_SubV3(s.position, player_pos);
        const float    d2 = HMM_DotV3(d, d);
        if (d2 < best2) best2 = d2;
    }
    return (best2 == FLT_MAX) ? FLT_MAX : std::sqrt(best2);
}

} // namespace threat
