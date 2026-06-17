#include "ship_ai.h"

#include "ai_brain.h"
#include "perception.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"

#include <string_view>

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

// Reset the data-driven brain's per-ship runtime when leaving combat so a
// fresh engagement re-selects from scratch (no stale priority / timer).
void reset_brain_runtime(ShipAIState& ai) {
    ai.cur_priority        = 0.0f;
    ai.maneuver_started_at = -1.0f;
    ai.fire_solution_at    = -1.0f;
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

    AIState next = s.ai.has_patrol_anchor ? AIState::Patrol : AIState::Idle;
    if (next != s.ai.state) { s.ai.state = next; s.ai.state_entered_at = t_now; }

    // Both Idle and Patrol are "controller idle" today (Patrol's OrbitAnchor
    // behaviour is still a placeholder). None keeps any externally-set
    // kinematics (e.g. JSON-authored demo motion) running untouched.
    s.behavior.kind          = ShipBehavior::None;
    s.controller.fire_guns   = false;
    s.controller.afterburner = false;
    s.controller.speed_scale = 1.0f;
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
