#include "ship_ai.h"

#include "ai_brain.h"
#include "hazards.h"
#include "perception.h"
#include "ship.h"
#include "ship_sprite.h"   // full ShipSpriteObject def (zero residual omega on combat exit)
#include "ship_class.h"
#include "ship_registry.h"
#include "system_def.h"

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

// Capital ships (data-driven "capital" flag in ship.json) keep a wide berth
// from one another in transit -- two cruisers sharing a lane look wrong and
// can slowly close into a hull-to-hull ram. We add a soft repulsion to the
// steering target: for every other capital within k_cap_travel_sep, push the
// target away (linear falloff, strongest when closest). It's a NUDGE on the
// existing destination, so the capital banks clear while still heading
// roughly on-course. Only meaningful for ships actively moving toward a
// target (Traveler / Loiter / Escort).
constexpr float k_cap_travel_sep = 1000.0f;   // min capital-vs-capital gap in transit

void apply_capital_separation(Ship& s, const ShipRegistry& all) {
    if (!s.klass || !s.klass->capital) return;
    if (s.behavior.kind != ShipBehavior::PursueTarget) return;
    HMM_Vec3 push = HMM_V3(0, 0, 0);
    int near_caps = 0;
    for (const Ship& o : all) {
        if (o.id == s.id || !o.alive) continue;
        if (!o.klass || !o.klass->capital) continue;
        const HMM_Vec3 away = HMM_SubV3(s.position, o.position);
        const float    d    = HMM_LenV3(away);
        if (d > 1e-3f && d < k_cap_travel_sep) {
            const float strength = (k_cap_travel_sep - d) / k_cap_travel_sep;  // 0..1
            push = HMM_AddV3(push, HMM_MulV3F(HMM_DivV3F(away, d), strength));
            ++near_caps;
        }
    }
    if (near_caps > 0) {
        s.behavior.target_pos =
            HMM_AddV3(s.behavior.target_pos, HMM_MulV3F(push, k_cap_travel_sep));
    }
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

    // Capital-vs-capital spacing: nudge the steering target clear of any
    // other nearby capital so cruisers don't share a lane / ram in transit.
    apply_capital_separation(s, all);
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

void ship_ai::tick(Ship& s, const ShipRegistry& all_ships, float t_now,
                    const StarSystem& system, HMM_Vec3 sun_pos) {
    if (!s.alive || s.is_player) return;
    if (!s.ai.enabled)            return;

    const ShipPerception& p = s.perception;
    bool has_hostile = (p.nearest_hostile_id != 0);

    // Campaign wingmen (#128): guard the player. Stance tables call the
    // blockade Demons "neutral" to a militia Talon, so perception never
    // hands a wingman a hostile — scan for whoever is gunning for the
    // player instead and lock the nearest one.
    if (s.ai.wingman && !has_hostile && s.sprite) {
        uint32_t tid  = 0;
        float    best = 1.0e30f;
        for (const Ship& o : all_ships) {
            if (!o.alive || o.is_player || o.id == s.id || !o.sprite) continue;
            if (!(o.ai.aggro_player || o.provoked_by_player)) continue;
            const float d = HMM_LenV3(HMM_SubV3(o.sprite->position,
                                                s.sprite->position));
            if (d < best) { best = d; tid = o.id; }
        }
        if (tid != 0) {
            s.ai.preferred_target_id = tid;
            has_hostile = true;   // fall into the combat path below
        }
    }

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
        s.ai.target_id = (p.nearest_hostile_id != 0) ? p.nearest_hostile_id
                                                     : s.ai.preferred_target_id;
        // Campaign escort waves (#140): a live preferred target overrides
        // the nearest-hostile pick (the wave focuses the escortee even
        // when the player is closer). Falls back automatically when the
        // preferred target is gone.
        if (s.ai.preferred_target_id != 0) {
            const Ship* pt = all_ships.find_by_id(s.ai.preferred_target_id);
            if (pt && pt->alive) s.ai.target_id = s.ai.preferred_target_id;
            else                 s.ai.preferred_target_id = 0;
        }
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

    // Hazard avoidance (np-3dp): bend the controller's heading away from
    // bases (and the sun, if it has one) so NPC ships never fly inside
    // the 7.5k base or 15k sun bubbles. Done as a post-pass so the AI's
    // tactical decisions (combat / patrol / idle) aren't overridden.
    const HMM_Vec3 base_avoid = hazards::base_repulsion(system.nav_points, s.position,
        hazards::k_base_warn_radius_m, hazards::k_base_no_fly_radius_m);
    HMM_Vec3 sun_avoid{0,0,0};
    if (hazards::inside_sun_avoid(sun_pos, s.position)) {
        float sun_t = 0.0f;
        sun_avoid = hazards::sun_repulsion(sun_pos, s.position, &sun_t);
        // Scale by sun_t so far away = no effect; close = strong push.
        sun_avoid = HMM_MulV3F(sun_avoid, sun_t * 1.5f);
    }
    const HMM_Vec3 nudge = HMM_AddV3(base_avoid, sun_avoid);
    if (HMM_LenV3(nudge) > 1e-3f) {
        // Blend the nudge into desired_forward. Base repulsion strength
        // is already 0..1 (linear ramp). Sun repulsion is amplified
        // 1.5x so the 15k bubble feels like a wall to the AI.
        HMM_Vec3 want = s.controller.desired_forward;
        const float want_l = HMM_LenV3(want);
        if (want_l < 1e-3f) want = HMM_V3(0,0,1); else want = HMM_DivV3F(want, want_l);
        HMM_Vec3 blended = HMM_AddV3(want, HMM_MulV3F(nudge, 0.45f));
        const float bl = HMM_LenV3(blended);
        if (bl > 1e-3f) {
            s.controller.desired_forward = HMM_DivV3F(blended, bl);
        }
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
