#pragma once
// -----------------------------------------------------------------------------
// objectives.h — dynamic-objective ("lead") system (Phase 3 Wave 1,
//                issues #71/#73/#74/#75/#76/#77/#78).
//
// A "lead" is a rumor payoff: a transient nav point dropped into the
// CURRENT star system that marks a valuable loot cache. Fly to it and the
// payoff spawns at the marker (a good ace-tier drop you tractor into the
// hold). Leads are pure world-state — they clear on landing or on a system
// change, never persist to a save, never serialize into the system JSON.
//
// The design leans on the existing nav-point machinery instead of bolting
// on a parallel render/targeting path: every lead pushes a NavPointDef with
// `dynamic = true` into StarSystem::nav_points, so the nav-map, the HUD
// targeting cycle (press N), and the autopilot all pick it up for free.
// objectives owns a tiny side-table (id + pos + label) to track which leads
// are live; clear() and the arrival check find the matching dynamic nav by
// flag + position so they stay correct even if nav_points gets re-ordered.
//
// Adjacent-system leads ("the rumor points two jumps away") are future
// work; this wave is current-system only.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <string>
#include <vector>

struct StarSystem;

namespace objectives {

// One live lead. Read-only view for HUD / dev snapshots (GET /objectives);
// the canonical record lives in objectives.cpp's file-static side-table.
struct Lead {
    int         id = 0;
    HMM_Vec3    pos = { 0.0f, 0.0f, 0.0f };
    std::string label;
};

// Distance (world units / meters) at which the player is considered to have
// "reached" a lead. Public so dev tooling / tests can use the same value.
constexpr float k_arrive_m = 3000.0f;

// Register a new lead: record it in the side-table AND push a matching
// `dynamic = true` NavPointDef (kind="nav") into `sys.nav_points` so the
// nav-map / HUD / autopilot surface it automatically. `label` is the
// human-readable name shown on the HUD. Logs "[lead] new: ...".
void add_lead(StarSystem& sys, HMM_Vec3 pos, const std::string& label);

// Choose a placement for a fresh lead (#75). Roughly 50/50 between:
//   * the midpoint-ish of two existing nav points, plus a random offset, and
//   * a deep-space point > 500km from `sun_pos` in a random direction.
// Falls back to the deep-space branch when the system has < 2 nav points.
HMM_Vec3 pick_lead_pos(const StarSystem& sys, HMM_Vec3 sun_pos);

// Per-frame arrival check (#78). For each live lead, if the player is within
// k_arrive_m, grant the payoff (an ace-tier pirate loot drop at the marker),
// log "[lead] reached ...", then remove that lead AND its dynamic nav point
// from `sys.nav_points`. Call once per Flight frame, near hailing::tick.
void tick(StarSystem& sys, HMM_Vec3 player_pos);

// Drop every live lead (#77): erase every `dynamic` nav point from
// `sys.nav_points` and clear the side-table. Called on the Landed
// transition and on system (re)load, alongside loot::clear / hailing::reset.
void clear(StarSystem& sys);

// Number of live leads. Read-only; for HUD/dev snapshots.
int count();

// All live leads, in registration order. Read-only; the host iterates this
// each Flight frame to publish the GET /objectives snapshot (label +
// distance from the player). Do not mutate from outside.
const std::vector<Lead>& all();

} // namespace objectives
