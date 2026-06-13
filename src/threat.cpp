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

namespace threat {

namespace {
// Non-owning view of the world, set by main.cpp at system load. Null
// until then (and again after teardown) — the query degrades to the
// original always-false stub, so the startup window stays crash-free.
const ShipRegistry*     g_ships = nullptr;
const PlayerReputation* g_rep   = nullptr;
} // namespace

void set_world(const ShipRegistry* ships, const PlayerReputation* player_rep) {
    g_ships = ships;
    g_rep   = player_rep;
}

bool hostiles_near(HMM_Vec3 player_pos, float radius) {
    if (!g_ships || !g_rep) return false;   // world not wired yet — safe default

    const float r2 = radius * radius;
    for (const Ship& s : *g_ships) {
        if (s.is_player || !s.alive) continue;
        // Stance vs the player folds the faction baseline with accumulated
        // reputation (faction.h). Only genuine hostiles trip the gate —
        // a neutral merchant flying past doesn't ground the autopilot.
        if (faction::stance_npc_vs_player(s.faction, *g_rep) != Stance::Hostile)
            continue;
        const HMM_Vec3 d = HMM_SubV3(s.position, player_pos);
        if (HMM_DotV3(d, d) <= r2) return true;
    }
    return false;
}

} // namespace threat
