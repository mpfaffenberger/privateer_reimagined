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
// Eligibility gate (mirrors the bead's acceptance criteria, #380):
//   1. a kind=="jump" nav sits within trigger range  (else: no prompt at all)
//      — the NEAREST such gate is the candidate; the nav-computer selection
//      is irrelevant. Flying up to a gate is enough, like the original.
//   2. the player has a Jump Drive fitted            (else: "JUMP: NO DRIVE")
//      and it isn't shot out (#141)             (else: "JUMP: DRIVE DAMAGED")
//   3. it resolves to a real galaxy edge             (else: "JUMP: NO ROUTE")
//   4. the campaign has opened that route            (else: "JUMP: UNSURVEYED")
//
// Hostiles are intentionally absent from this list. They block autopilot, not
// jumping: reaching a valid gate under fire is a legitimate escape.
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
// Verdict for the player against the nearest in-range jump gate.
enum class Status : uint8_t {
    Ready = 0,    // good to jump — "PRESS J TO JUMP - <dest>"
    NotJumpNav,   // no jump gate within trigger range — no prompt (caller
                  // falls back to the dock prompt for the same MFD slot)
    NoRoute,      // jump gate but dangling / unsurveyed — "JUMP: NO ROUTE"
    Locked,       // link exists but the campaign hasn't opened it (#130)
                  // — "JUMP: UNSURVEYED"
    NoDrive,      // player has no Jump Drive fitted — "JUMP: NO DRIVE"
    DriveDamaged, // drive fitted but shot out (#141) — "JUMP: DRIVE DAMAGED"
};

// State of the player's jump drive. One enum rather than two bools so the
// nonsense "not fitted but destroyed" combination can't be expressed.
enum class Drive : uint8_t {
    None,        // not fitted
    Destroyed,   // fitted, 0% integrity (component damage, #141)
    Online,      // fitted and working (partial damage still jumps)
};

struct Eligibility {
    Status      status = Status::NotJumpNav;
    int         nav_index = -1;      // gate in system.nav_points; -1 = none nearby
    std::string dest_id;        // destination galaxy system id ("pyrenees")
    std::string dest_name;      // destination display name for the prompt ("Pyrenees")
    std::string arrival_nav;    // arrival gate name on the far side ("Troy Jump")
    float       distance_m = 0.0f;   // player -> gate (for logs)
};

// Campaign route gate (#130, the locked frontier). When registered, a
// truthy return for (from_system, to_system) turns a Ready link into
// Status::Locked. main.cpp registers the campaign predicate (frontier
// systems refuse until monkhouse_done); unset = sandbox default, all
// surveyed links open.
void set_route_gate(
    std::function<bool(const std::string& from, const std::string& to)> gate);

// Evaluate the jump verdict against the nearest jump gate within
// k_trigger_range_m of the camera (ties keep the lower nav index). Pure:
// reads the camera pose, gate positions, and the galaxy topology — mutates
// nothing. No gate in range -> NotJumpNav with nav_index == -1.
// Pass the player's `drive` state so the verdict can include NoDrive /
// DriveDamaged.
Eligibility evaluate(const Camera& cam, const StarSystem& system,
                     const galaxy::Galaxy& galaxy,
                     const std::string& current_system_id,
                     Drive drive);

// HUD prompt for an eligibility, or nullptr when nothing should be drawn
// (NotJumpNav — no gate nearby, so no refusal line either). Sets *ready =
// true only for the green "PRESS J" line; every refusal line is amber. Returns a pointer into a static buffer — copy it if
// you need to keep it past the next call (the HUD consumes it immediately).
const char* prompt(const Eligibility& e, bool* ready);

// Lowercase-ish status label for logs ("ready", "no drive", ...).
const char* status_str(Status s);

} // namespace jump
