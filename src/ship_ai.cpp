#include "ship_ai.h"

#include "ai_brain.h"
#include "perception.h"
#include "ship.h"
#include "ship_sprite.h"   // full ShipSpriteObject def (zero residual omega on combat exit)
#include "ship_class.h"
#include "ship_registry.h"

#include <string_view>
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

// -----------------------------------------------------------------------------
// ship_ai.cpp -- top-level AI tick.
//
// As of the data-driven-brain refactor this file is thin: it owns the
// NON-combat gating (Idle / Patrol fallthrough, the same back-compat path that
// lets JSON-authored demo motion keep running) and the decision of WHETHER a
// ship is in combat. The combat brain itself -- the condition->maneuver table
// evaluator and every maneuver handler -- lives in ai_brain.cpp
// (docs/ai_maneuver_system.md). The AIState enum is retained purely as a
// coarse HUD/debug label; ai_brain maps the running maneuver onto it.
//
// Flow each tick (combat ship):
//   perception::tick has already filled s.perception. If there is a hostile,
//   we point the brain at it (run_combat), except for the one personality
//   override the condition metrics can't express -- "a hunted Coward flees" --
//   which forces FleeHome. No hostile -> Patrol/Idle, and we reset the brain's
//   per-ship runtime so the next engagement starts clean.
// -----------------------------------------------------------------------------

namespace {

// ---- civilian (non-combat) traffic --------------------------------------
// Nav waypoints for lane traffic / flee exits, set once per system load.
std::vector<ship_ai::NavWaypoint> g_waypoints;
std::mt19937 g_civ_rng{ 0xC1711A7Eu };

constexpr float k_travel_arrive_m = 6000.0f;  // "reached" a waypoint -> re-pick
constexpr float k_travel_min_hop  = 12000.0f; // don't pick a waypoint nearer than this

// Pick a random nav waypoint a decent hop away from `from`. Returns false
// only when no waypoints are loaded at all.
bool pick_travel_waypoint(HMM_Vec3 from, HMM_Vec3& out) {
    if (g_waypoints.empty()) return false;
    for (int tries = 0; tries < 6; ++tries) {
        const HMM_Vec3 p = g_waypoints[g_civ_rng() % g_waypoints.size()].pos;
        if (HMM_LenV3(HMM_SubV3(p, from)) > k_travel_min_hop) { out = p; return true; }
    }
    out = g_waypoints[g_civ_rng() % g_waypoints.size()].pos;   // give up: any
    return true;
}

// Drive a non-combat ship's controller for its civilian role. Runs on the
// Patrol fallthrough (no hostiles). Leaves guns/afterburner off throughout.
void civilian_behavior(Ship& s, const ShipRegistry& all, float t_now) {
    s.controller.fire_guns   = false;
    s.controller.afterburner = false;

    switch (s.ai.civ_role) {
    case CivRole::Traveler: {
        if (!s.ai.has_travel_dest ||
            HMM_LenV3(HMM_SubV3(s.ai.travel_dest, s.position)) < k_travel_arrive_m) {
            HMM_Vec3 dest;
            if (pick_travel_waypoint(s.position, dest)) {
                s.ai.travel_dest = dest;
                s.ai.has_travel_dest = true;
            }
        }
        if (s.ai.has_travel_dest) {
            s.behavior.kind       = ShipBehavior::PursueTarget;
            s.behavior.target_pos = s.ai.travel_dest;
            s.controller.speed_scale = 0.55f;   // unhurried merchant cruise
        } else {
            s.behavior.kind = ShipBehavior::None;
        }
        break;
    }
    case CivRole::Loiter: {
        // Lazy circle around the spawn anchor. Slow angular rate so the
        // capped throttle can actually keep up with the orbit point.
        const float r   = 2500.0f;
        const float ang = t_now * 0.05f + (float)(s.id % 16);
        const HMM_Vec3 c = s.ai.patrol_anchor;
        s.behavior.kind       = ShipBehavior::PursueTarget;
        s.behavior.target_pos = HMM_V3(c.X + std::cos(ang) * r, c.Y, c.Z + std::sin(ang) * r);
        s.controller.speed_scale = 0.5f;
        break;
    }
    case CivRole::Escort: {
        const ShipHandle h = all.find_handle_by_id(s.ai.formation_lead_id);
        const Ship* lead = all.get(h);
        if (!lead || !lead->alive) {
            // Lead's gone (killed / despawned) -> become a free traveler.
            s.ai.civ_role = CivRole::Traveler;
            s.ai.has_travel_dest = false;
            s.behavior.kind = ShipBehavior::None;
            break;
        }
        s.behavior.kind       = ShipBehavior::PursueTarget;
        s.behavior.target_pos = HMM_AddV3(lead->position, s.ai.formation_offset);
        s.controller.speed_scale = 0.7f;   // a touch faster so they can catch up
        break;
    }
    default:
        // No civilian role assigned: keep the legacy idle (None) so any
        // JSON-authored demo motion still runs untouched.
        s.behavior.kind = ShipBehavior::None;
        break;
    }
}

// Reset the data-driven brain's per-ship runtime when leaving combat so a
// fresh engagement re-selects from scratch (no stale priority / timer).
void reset_brain_runtime(ShipAIState& ai) {
    ai.cur_priority        = 0.0f;
    ai.maneuver_started_at = -1.0f;
    ai.fire_solution_at    = -1.0f;
    // Fresh engagement starts with full jink stamina.
    ai.jink_spent_s        = 0.0f;
    ai.jink_budget_s       = -1.0f;
    ai.jink_cooldown_until = -1.0f;
    ai.last_combat_t       = -1.0f;
}

} // namespace

void ship_ai::tick(Ship& s, const ShipRegistry& all_ships, float t_now) {
    if (!s.alive || s.is_player) return;
    if (!s.ai.enabled)            return;

    const ShipPerception& p = s.perception;
    const bool  has_hostile = (p.nearest_hostile_id != 0);

    // Personality gate. Cowards (merchants, civilians) prefer to flee -- but
    // ONLY when something is actively hunting them. When no hostile is
    // locked-on, even a Coward fights back rather than running pre-emptively.
    const bool is_coward = s.klass && s.klass->personality == AIPersonality::Coward;

    // "Is anyone Engage-locked on me right now?" A pursuer that is mid-extend
    // (BreakOff/evade -> state BreakOff) deliberately does NOT count, which
    // gives the prey a window to counter-attack while the hunter disengages.
    bool being_targeted = false;
    if (has_hostile) {
        for (const Ship& other : all_ships) {
            if (&other == &s || !other.alive) continue;
            if (other.ai.target_id == s.id && other.ai.state == AIState::Engage) {
                being_targeted = true;
                break;
            }
        }
    }

    if (has_hostile) {
        s.ai.target_id = p.nearest_hostile_id;
        if (is_coward && being_targeted) {
            // Personality override the table can't see: a hunted Coward runs.
            // Healthy/un-hunted cowards fall through to the brain and fight.
            ai_brain::run_forced(s, all_ships, t_now, AIManeuver::FleeHome);
        } else {
            // The data-driven combat brain owns maneuver selection + execution.
            ai_brain::run_combat(s, all_ships, t_now);
        }
        return;
    }

    // ---- non-combat fallthrough (unchanged behaviour) ----------------------
    reset_brain_runtime(s.ai);
    s.ai.target_id = 0;

    // If we just dropped out of a combat state (no hostiles left), kill the
    // residual body-frame turn rate the flight controller left in the
    // sprite. ShipBehavior::None leaves sprite kinematics untouched, so
    // without this the ship keeps integrating its last dogfight omega and
    // flies in endless loops ("loopies") after the fight ends. Only zero it
    // on the combat->idle transition so ships that were never fighting keep
    // any JSON-authored demo motion.
    const bool was_combat = (s.ai.state == AIState::Engage ||
                             s.ai.state == AIState::BreakOff ||
                             s.ai.state == AIState::Flee);
    if (was_combat && s.sprite) {
        s.sprite->angular_velocity = HMM_V3(0.0f, 0.0f, 0.0f);
    }

    const bool has_role = (s.ai.civ_role != CivRole::None);
    AIState next = (s.ai.has_patrol_anchor || has_role) ? AIState::Patrol : AIState::Idle;
    if (next != s.ai.state) {
        s.ai.state = next;
        s.ai.state_entered_at = t_now;
        // Re-pick a travel goal fresh on (re-)entering Patrol, e.g. after a
        // fight ends, so the ship gets back on the lanes instead of idling.
        s.ai.has_travel_dest = false;
    }

    // Patrol now drives real civilian traffic (Traveler/Loiter/Escort);
    // Idle (and an unassigned role) stays None so JSON-authored demo motion
    // keeps running untouched.
    s.controller.speed_scale = 1.0f;
    if (s.ai.state == AIState::Patrol && has_role) {
        civilian_behavior(s, all_ships, t_now);
    } else {
        s.behavior.kind          = ShipBehavior::None;
        s.controller.fire_guns   = false;
        s.controller.afterburner = false;
    }
}

void ship_ai::set_nav_waypoints(const std::vector<NavWaypoint>& wps) {
    g_waypoints = wps;
}

const char* ship_ai::to_name(AIState st) {
    switch (st) {
        case AIState::Idle:     return "idle";
        case AIState::Patrol:   return "patrol";
        case AIState::Engage:   return "engage";
        case AIState::BreakOff: return "breakoff";
        case AIState::Flee:     return "flee";
        default:                return "?";
    }
}

AIState ship_ai::from_name(std::string_view s) {
    if (s == "idle")     return AIState::Idle;
    if (s == "patrol")   return AIState::Patrol;
    if (s == "engage")   return AIState::Engage;
    if (s == "breakoff") return AIState::BreakOff;
    if (s == "flee")     return AIState::Flee;
    return AIState::Count;
}
