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
#include "threat.h"

#include <cstdio>

namespace jump {

Eligibility evaluate(const Camera& cam, const StarSystem& system,
                     const galaxy::Galaxy& galaxy,
                     const std::string& current_system_id,
                     int selected_nav,
                     bool has_jump_drive) {
    Eligibility e;

    // 1. Must have a selected nav, and it must be a jump gate. Anything
    // else leaves the verdict at NotJumpNav (the HUD then falls back to the
    // dock prompt for the same MFD slot).
    if (selected_nav < 0 || selected_nav >= (int)system.nav_points.size()) {
        return e;
    }
    const NavPointDef& nav = system.nav_points[selected_nav];
    if (nav.kind != "jump") {
        return e;
    }

    // (np-3dp.27): the player needs a fitted Jump Drive to USE the gate.
    // The prompt string still says "PRESS J"; we refuse with a dedicated
    // NoDrive status so the UI can render a distinct amber "JUMP: NO DRIVE".
    if (!has_jump_drive) {
        e.status = Status::NoDrive;
        return e;
    }

    // 2. Resolve the destination through the authoritative galaxy graph.
    // A dangling / unsurveyed gate (Troy's "War Jump") returns ok=false.
    const galaxy::JumpTarget jt =
        galaxy.jump_target(current_system_id, nav.name);
    if (!jt.ok) {
        e.status = Status::NoRoute;
        return e;
    }
    e.dest_id     = jt.system;
    e.arrival_nav = jt.nav;
    if (const galaxy::SystemEntry* se = galaxy.find(jt.system)) {
        e.dest_name = se->display_name;
    } else {
        e.dest_name = jt.system;   // fall back to the raw id if uncatalogued
    }

    // 3. Range gate. distance carried out for logs regardless of verdict.
    e.distance_m = HMM_LenV3(HMM_SubV3(nav.position, cam.position));
    if (e.distance_m > k_trigger_range_m) {
        e.status = Status::TooFar;
        return e;
    }

    // 4. Hostile gate — same oracle the autopilot uses (threat.h). Stubbed
    // false until the encounter director wires a world in; live thereafter.
    if (threat::hostiles_near(cam.position, k_threat_radius_m)) {
        e.status = Status::Hostiles;
        return e;
    }

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
        case Status::TooFar:   return "JUMP: TOO FAR";
        case Status::Hostiles: return "JUMP: HOSTILES NEAR";
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
        case Status::TooFar:     return "too far";
        case Status::Hostiles:   return "hostiles near";
        case Status::NoDrive:    return "no drive";
        default:                 return "?";
    }
}

} // namespace jump
