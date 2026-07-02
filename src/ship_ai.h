#pragma once
// -----------------------------------------------------------------------------
// ship_ai.h — high-level AI state machine.
//
// One layer up from the flight controller. Reads perception (L2),
// transitions between discrete behavioural states, and writes the
// concrete behaviour (L1) that the controller will close in on this
// tick. Not a full Privateer-grade brain yet — the v1 set is the
// minimal viable subset that produces visible reactivity:
//
//   Idle    — no perceived hostiles, no patrol assignment, sit still.
//             Behaviour stays None so any externally-set kinematics
//             (e.g. JSON-authored angular_velocity for the Tarsus's
//             demo circle) keep running. This is the back-compat
//             fallthrough that means "ship has no AI today".
//
//   Patrol  — same as Idle today (placeholder). When OrbitAnchor lands,
//             this becomes "circle around patrol_anchor at cruise/2".
//             Tracked separately so future commits can wire it without
//             reshuffling state IDs.
//
//   Engage  — perceived a hostile. Behaviour = PursueTarget pointed at
//             the target ship's CURRENT position (refreshed every tick
//             — this is the dynamic-target moment that distinguishes
//             AI chase from a static fire-and-forget waypoint).
//
//   Flee    — health below threshold AND a hostile is in range. Run
//             the OPPOSITE direction at cruise speed. Won't visibly
//             trigger until L4 (damage pipeline) lets ships actually
//             take hits — the wiring is here so L4 doesn't need an
//             AI revision.
//
// Each ship's state lives on Ship::ai (added in ship.h). ship_ai::tick
// is called once per frame, AFTER perception::tick (it consumes that
// data) and BEFORE ship::tick (which consumes the behaviour we write).
//
// The state machine is a pure function of (current state, perception,
// hp). No timers / cooldowns / hysteresis yet — those are the next
// nuance pass once we see ships flicker between states. Privateer's
// AI used a few seconds of "I just left this state" buffering to stop
// chatter; we'll add it when the demo demands it.
// -----------------------------------------------------------------------------

#include "ai_maneuver.h"

#include <HandmadeMath.h>
#include <cstdint>
#include <string_view>
#include <vector>

struct Ship;
struct StarSystem;
class ShipRegistry;

enum class AIState : uint8_t {
    Idle = 0,
    Patrol,
    Engage,
    BreakOff,        // Engage triggered "too close" → afterburn perpendicular,
                     // hold for ~2.5s, then re-enter Engage from a fresh angle.
                     // Breaks the scissors / orbital tail-chase pattern.
    Flee,
    Count
};

// Non-combat "what is this ship doing when nobody's shooting" role, driving
// the Patrol-state behaviour (ship_ai.cpp). Without this a Patrol ship just
// sits at 0 kps. Set at spawn by the encounter director.
//   Traveler — cruise the nav-point lanes (pick a nav, fly there, repeat).
//   Loiter   — laze in a slow orbit of the spawn anchor (stationed guard).
//   Escort   — hold a formation offset off a lead ship (convoys / wings).
enum class CivRole : uint8_t { None = 0, Traveler, Loiter, Escort };

struct ShipAIState {
    // True when this ship's behaviour should be driven by the AI state
    // machine (set by the JSON loader when an `ai` block is present).
    // When false, ship_ai::tick is a no-op for this ship — the existing
    // scripted-behaviour or legacy-motion path runs unchanged. This is
    // how Tarsus keeps its demo circle while the Talon goes AI-driven.
    bool      enabled = false;

    AIState   state = AIState::Idle;
    uint32_t  target_id = 0;          // Engage / Flee anchor; 0 = none
    float     state_entered_at = 0.0f;

    // Patrol parameters (placeholder for v1 — Patrol currently identical
    // to Idle). When OrbitAnchor behaviour lands, Patrol will read these.
    HMM_Vec3  patrol_anchor = { 0.0f, 0.0f, 0.0f };
    bool      has_patrol_anchor = false;

    // ---- non-combat civilian behaviour (ship_ai.cpp Patrol handler) ----
    CivRole   civ_role          = CivRole::None;
    HMM_Vec3  travel_dest       = { 0.0f, 0.0f, 0.0f };  // Traveler: current waypoint
    bool      has_travel_dest   = false;
    uint32_t  formation_lead_id = 0;                     // Escort: ship to hold off of
    HMM_Vec3  formation_offset  = { 0.0f, 0.0f, 0.0f };  // Escort: world offset from lead
    // Flee destination (gate/base) cached when a non-combatant runs for it,
    // so the panic vector stays stable instead of re-picking every tick.
    HMM_Vec3  flee_dest         = { 0.0f, 0.0f, 0.0f };
    bool      has_flee_dest     = false;
    // Last wall-clock time this ship hailed the player (comm rate-limit).
    float     hail_last_at      = -1000.0f;

    // BreakOff parameters. Cached at state entry so the break direction
    // stays stable for the duration of the maneuver — picking a fresh
    // direction every tick would just reproduce the orbital chase we're
    // trying to break out of. break_dir_world is unit world-space; the
    // BreakOff act phase uses (position + dir * far_distance) as its
    // ChaseTarget waypoint. break_duration_s is randomised per entry
    // (range below in ship_ai.cpp) so repeated breaks don't feel
    // metronomic — pursuer can't predict exactly when the prey turns.
    HMM_Vec3  break_dir_world  = { 0.0f, 0.0f, 0.0f };
    float     break_duration_s = 5.0f;

    // Break-off EXTENSION distance (world meters). Set per break-off entry
    // to a random 3-5 km. The break-off maneuver is held (won't re-select
    // an attack) until the ship has actually opened this much range, so
    // ships fly a proper extension out to 3-5 km before turning back for
    // another pass — instead of flipping back the instant they cross the
    // ~1 km pursuit boundary. Zero = not currently extending.
    float     break_extend_dist = 0.0f;

    // Maximum continuous-FIRING duration (seconds). Randomised on
    // Engage entry. The trigger is bound to fire_guns being held
    // true, NOT to time-in-Engage — a ship that's still closing the
    // distance and not yet shooting shouldn't burn its window. The
    // result: 3-6 seconds of actual blasting, then forced break.
    // Privateer-feel "don't be a sitting duck on someone's six":
    // even with tail position, pilots periodically extend so the prey
    // gets repositioning room and the fight doesn't degenerate into
    // a single sustained tail chase.
    float     firing_max_duration_s = 4.5f;

    // Wall-clock time at which the current uninterrupted firing burst
    // started, or -1 if not currently firing. Reset to -1 whenever
    // controller.fire_guns goes false (out of arc, out of range, AI
    // doesn't have a solution) or the ship leaves Engage.
    float     firing_started_at = -1.0f;

    // ---- decoded-vanilla maneuver state (docs/ai_model.md §9.4) --------
    // Privateer's Engage is two phases split at the 1000-unit
    // pursue->attack-run boundary (§9.4.2):
    //   * beyond 1000u  -> lead-pursuit (true intercept).
    //   * within 1000u  -> a TIMED attack run: guns hot from tick 76 to
    //                      153, then the run is "complete" and we break.
    // attack_run_started_at stamps when the ship first crossed inside
    // 1000u for the current run (-1 = not in an attack run). The tick
    // window is converted to seconds via ship_ai's kAiHz (the real AI
    // cadence / dt is still [?] from the disasm — see §9.3 / §9.8).
    float     attack_run_started_at = -1.0f;

    // EVADE/jink (§9.4.3): the break maneuver picks one of 4 cardinal
    // rotation axes + a roll in {-1,0,+1} at entry and holds it. Cached
    // here so the jink stays coherent for the whole break (re-rolling
    // every tick would just smear into noise). Set in the BreakOff
    // entry action; jink_roll scales the roll component.
    int       jink_axis = 0;       // 0:+pitch 1:-pitch 2:+yaw 3:-yaw
    float     jink_roll = 0.0f;    // -1 / 0 / +1

    // Comm/taunt bark cooldown (§11.1): wall-clock time of this ship's
    // last hostile bark, so f1-range chatter is rate-limited instead of
    // firing every frame the target sits inside comms_f1.
    float     last_bark_at = -1000.0f;

    bool      aggro_player = false;   // hailing/search override: treat the player as hostile regardless of reputation (transient, not serialized)

    // Campaign escort waves (#140): while this ship id is alive, Engage
    // locks IT instead of the nearest hostile ("the Retros focus the
    // Drayman"). Cleared automatically when the preferred target dies.
    // Transient, not serialized — set by the scenario director's
    // aggro:"escortee" spawn groups.
    uint32_t  preferred_target_id = 0;

    // ---- data-driven brain runtime (src/ai_maneuver.h, ai_brain.cpp) ---
    // The combat brain is now a condition->maneuver table evaluator. The
    // STATIC table (3 morale tiers of logic+interrupt rules) is shared and
    // loaded from assets/ai/*.ai.json; only the per-ship RUNTIME state lives
    // here. `brain` is resolved lazily on first combat tick (by faction /
    // class, fallback "default"). `morale_tier` is seeded from CNST f6
    // (low=fanatical, high=timid). The current maneuver runs until
    // (t_now - maneuver_started_at) >= cur_duration, then the logic channel
    // reselects; the interrupt channel can preempt earlier if a
    // higher-priority rule's window passes. `personality_seed` is a stable
    // per-pilot RNG stamp (like PGG's personalityseed) layered on top of the
    // per-faction CNST so same-faction pilots vary.
    const AILogicTable* brain               = nullptr;
    int                 morale_tier         = 1;     // 0 timid /1 steady /2 fanatical
    AIManeuver          cur_maneuver        = AIManeuver::LeadPursuit;
    float               cur_priority        = 0.0f;
    float               cur_duration        = 0.0f;
    float               maneuver_started_at = -1.0f;
    uint32_t            personality_seed    = 0;      // 0 = not yet stamped

    // Reaction-time fire gate (firing upgrade): wall-clock time at which the
    // firing solution first became valid (cone aligned + in range). Guns are
    // held until t_now >= solution_at + per-pilot reaction time, so pilots
    // don't snap-fire the instant the cone lines up. -1 = no current solution.
    float               fire_solution_at    = -1.0f;

    // ---- jink stamina (dogfight evasion budget) ------------------------
    // A pilot can only juke/break for so long before tiring and having to
    // fly predictably for a recovery window — which is what lets the
    // attacker actually land shots instead of chasing an endlessly-jinking
    // target. Skill (CNST f2) sets the budget: novices gas out fast, aces
    // sustain. While an evasive maneuver runs, `jink_spent_s` accrues; once
    // it passes `jink_budget_s` the pilot enters a ~5 s (+/- 3 s) cooldown
    // (`jink_cooldown_until`) during which evasive maneuvers are suppressed
    // (it lead-pursues instead), then stamina refreshes with a fresh roll.
    // All times are the same wall-clock t_now the maneuver timers use.
    float               jink_spent_s        = 0.0f;
    float               jink_budget_s       = -1.0f;   // <0 = roll a fresh budget
    float               jink_cooldown_until = -1.0f;   // >=0 && t_now< it = suppressed
    float               last_combat_t       = -1.0f;   // for dt inside run_combat
};

namespace ship_ai {

// Update one ship's state and write a fresh behaviour for the controller
// to consume this frame. Called once per ship per frame between
// perception::tick (L2) and ship::tick (L1).
//
// `all_ships` is the ship registry (slot-map, see ship_registry.h) —
// needed for target lookup by ID (Engage / Flee resolve
// nearest_hostile_id from perception into the target's actual position).
// Linear scan via ShipRegistry::find_by_id; cheap at the demo's N —
// promote to a hash on the way to hundreds of ships.
//
// `t_now` is the engine's monotonic seconds counter; used to stamp
// state_entered_at for future hysteresis logic.
void tick(Ship& s, const ShipRegistry& all_ships, float t_now,
          const StarSystem& system, HMM_Vec3 sun_pos);

// Convert AIState to / from JSON-friendly lowercase strings.
const char* to_name(AIState st);
AIState     from_name(std::string_view s);

// Nav waypoints the civilian (non-combat) AI uses for lane traffic and as
// flee destinations. `dock_or_gate` marks bases + jump points (where a
// fleeing/arriving ship can actually exit the system). Set once per system
// load (build_system_scene); kept module-global so the per-ship tick stays
// free of any system_def coupling, exactly like threat::set_world.
struct NavWaypoint { HMM_Vec3 pos; bool dock_or_gate; };
void set_nav_waypoints(const std::vector<NavWaypoint>& wps);

} // namespace ship_ai
