#pragma once
// -----------------------------------------------------------------------------
// jump.h — "press J to jump through the gate" eligibility oracle (np-6al.3).
//
// The galaxy layer (galaxy.h) answers WHERE a jump point leads; this answers
// WHETHER the player may take it right now, and supplies the HUD prompt for
// the NAV MFD. It is the jump twin of docking::can_request — a pure, side-
// effect-free verdict the cockpit HUD reads every frame and the J keypress
// re-checks before committing.
//
// The actual jump EXECUTION (Loading-mode cinematic, load_and_build_system on
// the destination, dropping the player at the reciprocal gate) lives in
// main.cpp, the only place that owns the scene/GPU/mode machinery — same
// division of labour docking uses (docking.cpp decides, main.cpp tears down +
// rebuilds). Keeping the verdict here keeps it trivially testable and lets the
// HUD and the keypress share ONE source of truth for "can I jump?".
//
// Eligibility gate (mirrors the bead's acceptance criteria):
//   1. the selected nav is kind=="jump"             (else: no prompt at all)
//   2. it resolves to a real galaxy edge             (else: "JUMP: NO ROUTE")
//   3. we're inside the trigger range of the gate    (else: "JUMP: TOO FAR")
//   4. no hostiles inside the danger bubble          (else: "JUMP: HOSTILES NEAR")
//
// Trigger range note (np-6al.3): the bead floated ~500u, but the nav autopilot
// (autopilot.h) eases to a stop within k_arrival_radius_m == 5000u of its
// target. A 500u trigger would mean "autopilot to a jump gate" leaves you
// 10x outside jump range — directly contradicting the bead's own integration
// flow ("autopilot A -> arrive near gate -> J prompt appears"). So we set the
// trigger a hair past the autopilot's arrival radius: dropping out of
// autopilot at a jump gate lands you comfortably inside jump range, and the
// prompt is waiting. Documented here so the 500u-vs-6000u discrepancy is a
// deliberate reconciliation, not a typo.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <cstdint>
#include <functional>
#include <string>

struct Camera;
struct StarSystem;
namespace galaxy { struct Galaxy; }

namespace jump {

// ---- tuning knobs -----------------------------------------------------------
// Tight: you must be basically on top of the gate to jump (Privateer feel).
constexpr float k_trigger_range_m = 3000.0f;
// Hostile bubble for the gate — matches autopilot::k_threat_radius_m so a
// furball that blocks the autopilot also blocks the jump (Privateer rule).
constexpr float k_threat_radius_m = 8000.0f;

// Verdict for a single (player, selected nav) pair.
enum class Status : uint8_t {
    Ready = 0,    // good to jump — "PRESS J TO JUMP - <dest>"
    NotJumpNav,   // selected nav isn't a jump gate — no prompt (caller falls
                  // back to the dock prompt for the same MFD slot)
    NoRoute,      // jump gate but dangling / unsurveyed — "JUMP: NO ROUTE"
    Locked,       // link exists but the campaign hasn't opened it (#130)
                  // — "JUMP: UNSURVEYED"
    TooFar,       // outside trigger range — "JUMP: TOO FAR"
    Hostiles,     // hostiles in the bubble — "JUMP: HOSTILES NEAR"
    NoDrive,      // player has no Jump Drive fitted — "JUMP: NO DRIVE"
};

struct Eligibility {
    Status      status = Status::NotJumpNav;
    std::string dest_id;        // destination galaxy system id ("pyrenees")
    std::string dest_name;      // destination display name for the prompt ("Pyrenees")
    std::string arrival_nav;    // arrival gate name on the far side ("Troy Jump")
    float       distance_m = 0.0f;   // player -> gate (for logs)
};

// Evaluate the jump verdict for `selected_nav` (index into
// system.nav_points; -1 = none). Pure: reads the camera pose, the gate's
// position, the galaxy topology, and the threat oracle — mutates nothing.
// Pass `has_jump_drive` so the verdict can include NoDrive.
// Campaign route gate (#130, the locked frontier). When registered, a
// truthy return for (from_system, to_system) turns a Ready link into
// Status::Locked. main.cpp registers the campaign predicate (frontier
// systems refuse until monkhouse_done); unset = sandbox default, all
// surveyed links open.
void set_route_gate(
    std::function<bool(const std::string& from, const std::string& to)> gate);

Eligibility evaluate(const Camera& cam, const StarSystem& system,
                     const galaxy::Galaxy& galaxy,
                     const std::string& current_system_id,
                     int selected_nav,
                     bool has_jump_drive);

// HUD prompt for an eligibility, or nullptr when nothing should be drawn
// (NotJumpNav). Sets *ready = true only for the green "PRESS J" line; every
// refusal line is amber. Returns a pointer into a static buffer — copy it if
// you need to keep it past the next call (the HUD consumes it immediately).
const char* prompt(const Eligibility& e, bool* ready);

// Lowercase-ish status label for logs ("ready", "too far", ...).
const char* status_str(Status s);

} // namespace jump
