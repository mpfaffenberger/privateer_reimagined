// -----------------------------------------------------------------------------
// jump.cpp — jump-gate eligibility verdict + HUD prompt (np-6al.3).
//
// See jump.h for the design rationale and the trigger-range reconciliation.
// This file is the whole brain of "can I jump?"; the brawn (Loading
// cinematic, system rebuild, reciprocal arrival placement) lives in main.cpp.
// -----------------------------------------------------------------------------

#include "jump.h"

#include "camera.h"
#include "galaxy.h"
#include "player.h"
#include "system_def.h"

#include <cstdio>
#include <functional>
#include <utility>

namespace {

// Campaign route gate (#130): a truthy return for (from, to) locks an
// otherwise-valid jump link ("the frontier is unsurveyed"). Unset =
// everything open (sandbox default).
std::function<bool(const std::string&, const std::string&)> g_route_gate;

// Index of the nearest kind=="jump" nav within the trigger range of `pos`,
// or -1 when none is close enough. The range is inclusive; strict < keeps
// the lower index on distance ties.
int nearest_gate_in_range(const StarSystem& system, HMM_Vec3 pos,
                          float* out_distance) {
    int   best      = -1;
    float best_dist = 0.0f;
    for (int i = 0; i < (int)system.nav_points.size(); ++i) {
        const NavPointDef& nav = system.nav_points[i];
        if (nav.kind != "jump") continue;
        const float d = HMM_LenV3(HMM_SubV3(nav.position, pos));
        if (d > jump::k_trigger_range_m) continue;
        if (best < 0 || d < best_dist) {
            best      = i;
            best_dist = d;
        }
    }
    if (best >= 0 && out_distance) *out_distance = best_dist;
    return best;
}

} // namespace

namespace jump {

void set_route_gate(
    std::function<bool(const std::string&, const std::string&)> gate) {
    g_route_gate = std::move(gate);
}

Eligibility evaluate(const Camera& cam, const StarSystem& system,
                     const galaxy::Galaxy& galaxy,
                     const std::string& current_system_id,
                     bool has_jump_drive) {
    Eligibility e;

    // 1. A jump gate must be physically nearby (#380) — whatever is
    // selected in the nav computer doesn't matter. Nothing in range leaves
    // the verdict at NotJumpNav (the HUD then falls back to the dock prompt
    // for the same MFD slot, and J stays silent).
    e.nav_index = nearest_gate_in_range(system, cam.position, &e.distance_m);
    if (e.nav_index < 0) {
        return e;
    }
    const NavPointDef& nav = system.nav_points[e.nav_index];

    // 2. (np-3dp.27): the player needs a fitted Jump Drive to USE the gate.
    // The prompt string still says "PRESS J"; we refuse with a dedicated
    // NoDrive status so the UI can render a distinct amber "JUMP: NO DRIVE".
    if (!has_jump_drive) {
        e.status = Status::NoDrive;
        return e;
    }

    // 3. Resolve the destination through the authoritative galaxy graph.
    // A dangling / unsurveyed gate (Troy's "War Jump") returns ok=false.
    const galaxy::JumpTarget jt =
        galaxy.jump_target(current_system_id, nav.name);
    if (!jt.ok) {
        e.status = Status::NoRoute;
        return e;
    }
    // 4. Campaign route gate (#130): the link exists in the galaxy graph
    // but the plot hasn't opened it yet (Exploratory Services hasn't
    // surveyed the frontier). Distinct verdict so the HUD can explain.
    if (g_route_gate && g_route_gate(current_system_id, jt.system)) {
        e.status = Status::Locked;
        return e;
    }
    e.dest_id     = jt.system;
    e.arrival_nav = jt.nav;
    if (const galaxy::SystemEntry* se = galaxy.find(jt.system)) {
        e.dest_name = se->display_name;
    } else {
        e.dest_name = jt.system;   // fall back to the raw id if uncatalogued
    }

    // Hostiles deliberately do not participate in jump eligibility. They
    // block long-distance autopilot travel, but a pilot who physically reaches
    // a valid gate may jump away under fire.
    e.status = Status::Ready;
    return e;
}

const char* prompt(const Eligibility& e, bool* ready) {
    static char buf[64];
    if (ready) *ready = false;
    switch (e.status) {
        case Status::Ready:
            if (ready) *ready = true;
            std::snprintf(buf, sizeof(buf), "PRESS J TO JUMP - %s",
                          e.dest_name.c_str());
            return buf;
        case Status::NoRoute:  return "JUMP: NO ROUTE";
        case Status::Locked:   return "JUMP: UNSURVEYED";
        case Status::NoDrive:  return "JUMP: NO DRIVE";
        case Status::NotJumpNav:
        default:               return nullptr;   // not a gate — no prompt
    }
}

const char* status_str(Status s) {
    switch (s) {
        case Status::Ready:      return "ready";
        case Status::NotJumpNav: return "not a jump nav";
        case Status::NoRoute:    return "no route";
        case Status::Locked:     return "locked (unsurveyed)";
        case Status::NoDrive:    return "no drive";
        default:                 return "?";
    }
}

} // namespace jump
