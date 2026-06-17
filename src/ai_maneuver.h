#pragma once
// -----------------------------------------------------------------------------
// ai_maneuver.h -- data model for the data-driven condition->maneuver AI.
//
// This is the clean-room reimplementation of the architecture shared by BOTH
// vanilla Privateer (the decoded 3-tier MNVR maneuver-object tree -- per-tick
// `+0x0c` condition / `+0x14` update, docs/ai_model.md s9.3-9.4) AND Privateer
// Gemini Gold / Vega Strike (the XML condition->script logic table,
// docs/ai_vegastrike_comparison.md s1.2). Both originals are the same idea:
// "evaluate a table of {metric-in-a-window -> named maneuver for N seconds}
// each tick; the first/active match runs a bounded-time maneuver that carries
// its own timer." We reimplement that *pattern* in our own types, driven by
// our VANILLA-DECODED numbers (docs/ai_model.md s0). No GPL code was copied --
// see docs/ai_maneuver_system.md for the license / clean-room note.
//
// The data model is three layers:
//   * AICondClause -- one metric tested against a [min,max) window (with an
//     optional negate = "outside the window"). min/max are AIScalars so a
//     window edge can be a literal OR a per-pilot CNST token ("f0_radii",
//     "pursue_switch", ...) resolved against the ship at eval time.
//   * AILogicItem  -- a rule: AND of clauses -> a Maneuver, with a priority
//     and a duration_s (how long the maneuver runs before reselection).
//   * AILogicTable -- 3 morale tiers (timid / steady / fanatical), each with a
//     normal "logic" channel and a priority-preempt "interrupt" channel.
//
// The static table is shared (loaded once from assets/ai/*.ai.json, registry
// in ai_brain.cpp); the per-ship runtime state (current maneuver + timer +
// personality seed) lives on ShipAIState (ship_ai.h).
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// ---- Condition metrics --------------------------------------------------
// Evaluated per tick against a [min,max) window. Kept deliberately small and
// ours -- a subset that covers vanilla's used set plus the PGG metrics our
// engine can actually compute (no shield-heal-rate / flightgroup machinery).
enum class AICondition : uint8_t {
    DistanceToTarget = 0,  // world units, observer->target (raw, kPvtScale 1:1)
    HullFraction,          // self hull 0..1
    ShieldFraction,        // self shield 0..1 (averaged facings)
    TargetInFront,         // cos(angle nose->target), -1..1 (1 = dead ahead)
    TargetFacingMe,        // cos(angle target-nose->me), -1..1 (1 = it aims at me)
    TargetInRange,         // 1 if within gun range_m, else 0
    Random01,              // fresh uniform 0..1 each eval
    ClosingRate,           // line-of-sight closing speed, m/s (+ = closing)
    TimeInState,           // seconds the current maneuver has run
    Count
};

// ---- Maneuver catalog ---------------------------------------------------
// Clean reimplementations writing our behavior/controller outputs. The first
// four are ports of the existing working math (docs/ai_model.md s9.4); the
// middle block are the new named maneuvers borrowed (concept-only) from the
// PGG/VS catalog (docs/ai_vegastrike_comparison.md s2).
enum class AIManeuver : uint8_t {
    LeadPursuit = 0,    // intercept-aim close (vanilla APPROACH far branch)
    AttackRun,          // close + timed guns-hot run (vanilla 0x397c8 near)
    BreakOff,           // afterburn away + cached jink (vanilla EVADE 0x39981)
    EvadeJink,          // 4-way cardinal jink + roll, f3-scaled
    BarrelRoll,         // corkscrew approach
    LoopAround,         // overshoot-and-curl-back
    EvadeLeftRight,     // sign-flipping lateral weave
    EvadeUpDown,        // sign-flipping vertical weave
    FacePerpendicular,  // fly perpendicular to target facing (deflection setup)
    TurnAway,           // point/run directly away (no afterburn)
    MatchSpeed,         // match target speed, hold aim (Kickstop-ish)
    FleeHome,           // run home with sinusoidal jink (existing Flee)
    Count
};

// ---- Per-pilot scalar token ---------------------------------------------
// A window edge is either a literal number or a token resolved per-ship from
// CNST / the decoded range stack (docs/ai_model.md s0). Lets a JSON rule say
// `max: "f0_radii"` so the same table works for every pilot's own f0.
enum class AIScalarToken : uint8_t {
    Literal = 0,
    F0,            // CNST f0 (break-off base, world units)
    F0Radii,       // f0 + selfR + targetR (the real break-off radius)
    F1,            // CNST f1 (comms/taunt range)
    F2,            // CNST f2 (skill/accuracy tier)
    F3,            // CNST f3 (evade jitter gain)
    F6,            // CNST f6 (morale/caution)
    PursueSwitch,  // 1000 -- pursue->attack-run boundary
    GunRange,      // class weapons_range
    FleeThreshold, // f6-derived flee HP fraction
    Sensor,        // class radar_range (15000 sensor/awareness)
};

struct AIScalar {
    AIScalarToken tok = AIScalarToken::Literal;
    float         lit = 0.0f;   // used when tok == Literal
};

struct AICondClause {
    AICondition cond   = AICondition::DistanceToTarget;
    AIScalar    min;
    AIScalar    max;
    bool        negate = false;   // true: pass when value is OUTSIDE [min,max)
};

struct AILogicItem {
    std::vector<AICondClause> clauses;     // AND of all clauses (empty = always)
    AIManeuver maneuver   = AIManeuver::LeadPursuit;
    float      priority   = 0.0f;
    float      duration_s = 1.0f;          // run time before reselection
};

struct AIMoraleTier {
    std::vector<AILogicItem> logic;        // normal reselection channel
    std::vector<AILogicItem> interrupt;    // priority-preempt channel
};

struct AILogicTable {
    std::string  name;                     // registry key (faction / "default")
    AIMoraleTier tiers[3];                 // 0 = timid, 1 = steady, 2 = fanatical
};

namespace ai_maneuver {

// enum <-> JSON string. *_from_name returns Count on miss.
const char* condition_name(AICondition c);
AICondition condition_from_name(std::string_view s);
const char* maneuver_name(AIManeuver m);
AIManeuver  maneuver_from_name(std::string_view s);

// Scalar token parse: a JSON number -> {Literal, n}; a JSON string ->
// {token, 0}. Unknown string tokens log and fall back to Literal 0.
AIScalarToken token_from_name(std::string_view s);
const char*   token_name(AIScalarToken t);

// HUD/debug: collapse a maneuver to a coarse AIState-style label so the
// existing debug panel keeps reading sensibly.
const char* maneuver_state_label(AIManeuver m);

} // namespace ai_maneuver
