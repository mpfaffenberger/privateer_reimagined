#pragma once
// -----------------------------------------------------------------------------
// autopilot.h — nav-point cruise autopilot ("press A to fly there").
//
// The long-haul companion to docking.h. Where docking is the *terminal*
// approach controller — line up on a pad, ease onto it, flip to Landed —
// this is the *traversal* controller: point the ship at the currently-
// selected nav point (cockpit_hud's N-cycle / navmap selection), wind up
// the cruise engine, and barrel across the ~1M-unit system until you're
// within arrival range, then ease to a stop and hand the stick back.
//
// Why a separate module from docking?
//   * Different job. Docking gates on dockable + base_id and ends in a
//     mode transition; the nav autopilot targets ANY nav point and ends
//     in free flight at the destination. Conflating them would bloat
//     docking's verdict enum with cases that don't apply to half its
//     callers.
//   * Same discipline, reused. We borrow docking's patterns — autopilot
//     owns the camera while engaged (controls_locked mutes pilot input
//     in main.cpp), it steers with the same shortest-arc facing slerp,
//     and it's a free-function namespace over a plain state struct that
//     AppState owns. No hidden globals.
//
// Crucially the autopilot drives the *camera's own cruise engine*
// (camera.h: cruise_target / apply_thrust / integrate). It does NOT
// hand-roll a velocity ramp. That means the engine_level-adjusted speed
// caps from outfitting (np-9cu.3) apply for free — a better engine makes
// autopilot faster, same as manual cruise.
//
// Hostile gate: engagement (and every engaged frame) is vetted through
// threat::hostiles_near() (threat.h). Today that's a stub that returns
// false, so the gate never fires — but it's fully wired, so when the
// encounter director (np-ma2.3) supplies real hostiles the autopilot
// will refuse to engage / drop out under fire with zero changes here.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <cstdint>
#include <string>

struct Camera;
struct StarSystem;

// Where we are in the fly-to-nav lifecycle.
//
//   Idle      — free flight; autopilot is asleep.
//   Cruising  — autopilot owns the ship: input locked, nose swinging
//               toward the nav, cruise engine wound up, accelerating.
//   Arriving  — within k_arrival_radius_m: cruise cut, velocity easing
//               to zero. Flips back to Idle (control returned) once the
//               ship has effectively stopped.
enum class AutopilotPhase : uint8_t {
    Idle = 0,
    Cruising,
    Arriving,
};

// Outcome of a try_engage() — lets the caller log/sfx distinctly even
// though the human-facing message is already stashed on the struct.
enum class EngageResult : uint8_t {
    Engaged = 0,   // locked on, cruising
    NoNav,         // nothing selected — "NO NAV SELECTED"
    Hostiles,      // threat gate refused — "HOSTILES DETECTED"
};

struct Autopilot {
    AutopilotPhase phase     = AutopilotPhase::Idle;
    int            nav_index = -1;                 // index into system.nav_points
    std::string    nav_name;                       // cached for the HUD indicator
    HMM_Vec3       target    = { 0.0f, 0.0f, 0.0f };
    HMM_Vec3       start_pos = { 0.0f, 0.0f, 0.0f }; // position when autopilot engaged
    float          log_accum = 0.0f;               // approach-log throttle

    // Transient HUD banner (engage/disengage/refusal flashes). Drawn by
    // main.cpp while msg_timer_s > 0; decayed inside tick() so a single
    // per-frame call keeps it bookkept even when phase == Idle.
    char  msg[64]      = { 0 };
    float msg_timer_s  = 0.0f;

    // Camera speed-cap value before autopilot bumped it up to the high
    // autopilot cruising speed. Restored on disengage so player manual
    // throttle still respects the ship's engine_level cap.
    float saved_cruise1 = 0.0f;
};

namespace autopilot {

// ---- tuning knobs -----------------------------------------------------------
// Arrival radius bumped from 5 km to 15 km so autopilot ends well outside
// nav-point furniture (stations, planet billboards) rather than threading
// you in close. Pairs with the 15 km radar/target-lock ceiling — the
// autopilot drops out at the same range past which contacts disappear.
constexpr float k_arrival_radius_m = 15000.0f; // "we're here" — ease to a stop
constexpr float k_navpoint_break_m  = 15000.0f; // non-target nav proximity cancels autopilot
constexpr float k_navpoint_break_after_m = 30000.0f; // ignore nearby navs until we've moved this far
constexpr float k_threat_radius_m   = 15000.0f; // hostile bubble for the gate
constexpr float k_stop_speed        = 5.0f;     // u/s below which Arriving -> Idle
constexpr float k_turn_rate         = 2.0f;     // facing-slerp time constant (1/s)
constexpr float k_brake_rate        = 3.0f;     // arrival velocity-ease rate (1/s)
constexpr float k_msg_secs          = 2.0f;     // HUD banner dwell time
constexpr float k_cruise_speed      = 5000.0f;  // autopilot cruising speed (m/s)

// True while the autopilot owns the ship — main.cpp uses this to mute
// pilot thrust/aim/cruise so manual physics doesn't double-drive the
// camera that tick() is already moving.
bool controls_locked(const Autopilot& a);

// Convenience: engaged == not Idle. (Same predicate as controls_locked
// today, named for the HUD's intent.)
bool engaged(const Autopilot& a);

// The engage gate on its own, side-effect free: NoNav / Hostiles, or
// Engaged meaning "try_engage would succeed right now". The cockpit AUTO
// light polls this every frame so it can never disagree with the A key.
EngageResult engage_check(const Camera& cam, const StarSystem& system,
                          int selected_nav);

// Attempt to engage toward `selected_nav` (index into
// system.nav_points; -1 = none). Vets nav-selected + the hostile gate,
// stashes the right HUD banner for every outcome, and returns the
// verdict. On success: phase = Cruising, target/name cached.
EngageResult try_engage(Autopilot& a, Camera& cam,
                        const StarSystem& system, int selected_nav);

// Hand control back. Sets a HUD banner from `reason`, cuts the cruise
// throttle on `cam`, and returns to Idle. Used by the A-toggle cancel,
// the manual-input cancel, the hostile drop-out, and arrival.
void disengage(Autopilot& a, Camera& cam, const char* reason);

void tick(Autopilot& a, Camera& cam, const StarSystem& system,
          float dt, HMM_Vec3 sun_pos);

} // namespace autopilot
