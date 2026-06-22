#pragma once
// -----------------------------------------------------------------------------
// docking.h — request-landing + autodock approach state machine.
//
// The bridge between Flight and Landed (game_state.h). The player lines
// up on a dockable nav point (system_def.h: NavPointDef::dockable +
// base_id), presses D, and — if they're close enough and slow enough —
// hands the controls to a small autopilot that flies the ship onto the
// pad, holds a brief cinematic beat, then requests GameMode::Landed.
//
// Why its own module instead of bolting onto camera or game_state?
//   * camera.h is pure 6-DOF physics + input; it shouldn't know what a
//     "base" is. Docking borrows the camera (it IS the ship this stage)
//     but the policy — ranges, speed gates, the approach curve — lives
//     here.
//   * game_state.h is the four-mode machine and stays deliberately tiny;
//     docking is the thing that *requests* a Landed transition, not the
//     transition machinery itself. We reuse its deferred-transition
//     discipline (np-eag.2): docking::tick calls game_state::request_mode
//     and lets apply_pending flip the mode at the top of the NEXT frame,
//     never mid-frame.
//
// Ownership: AppState owns one Docking instance (like it owns GameState).
// All functions are free functions that take it by reference — same
// no-hidden-globals shape as the game_state:: namespace.
//
// NOTE on scope: this stage delivers the approach + mode flip only. The
// actual base-screen rendering (concourse art, shops, hotspots) is
// np-9cu.4's job. Until then Landed mode shows np-eag.2's stub — but we
// stash the base_id in player.last_docked_base (and surface it in the
// stub text) so it's visible WHICH base we docked at.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <cstdint>
#include <string>

struct Camera;
struct GameState;
struct PlayerState;
struct NavPointDef;

// Where we are in the land-at-a-base lifecycle.
//
//   None        — free flight, nothing pending.
//   Requested   — request() accepted this frame; promoted to Approaching
//                 on the next tick (one-frame handshake so the SFX/log
//                 fire exactly once).
//   Approaching — autopilot owns the ship: input is locked, velocity
//                 eases toward the pad, nose swings to face it.
//   Docking     — parked on the pad, holding a short cinematic beat
//                 (k_docking_pause_s) before committing.
//   Docked      — committed; Landed transition requested. Reset to None
//                 by launch() when the player leaves the base.
enum class DockingState : uint8_t {
    None = 0,
    Requested,
    Approaching,
    Docking,
    Docked,
};

// Eligibility verdict from can_request(). Drives both the HUD prompt
// text and whether request() will take.
enum class DockResult : uint8_t {
    Cleared = 0,   // good to go — "DOCKING CLEARED"
    TooFar,        // outside k_dock_range_m — "TOO FAR"
    TooFast,       // speed over k_dock_speed_max — "TOO FAST"
    NotDockable,   // nav point isn't a base (no prompt)
    Busy,          // already docking, or in post-launch cooldown
};

struct Docking {
    DockingState state      = DockingState::None;
    std::string  base_id;                       // active base ("achilles")
    std::string  base_name;                     // human-readable, for HUD/logs
    HMM_Vec3     pad_pos    = { 0.0f, 0.0f, 0.0f }; // world dock point
    float        timer_s    = 0.0f;             // elapsed time in the current phase
    float        log_accum  = 0.0f;             // approach-log throttle accumulator
    float        cooldown_s = 0.0f;             // post-launch re-dock lockout
};

namespace docking {

// ---- tuning knobs -----------------------------------------------------------
// Gameplay feel, not facts — all in one place so they're easy to taste-test.
constexpr float k_dock_range_m        = 2000.0f; // how close to request a landing
constexpr float k_dock_speed_max      = 100.0f;  // m/s; faster = "TOO FAST"
constexpr float k_approach_speed_max  = 250.0f;  // autopilot cruise toward the pad
constexpr float k_arrive_dist_m       = 60.0f;   // "on the pad" threshold
constexpr float k_docking_pause_s     = 1.5f;    // cinematic beat before Landed
constexpr float k_launch_offset_m     = 800.0f;  // how far off the pad we respawn
constexpr float k_launch_speed        = 40.0f;   // gentle outward push on launch
constexpr float k_relaunch_cooldown_s = 2.0f;    // anti-instant-redock after launch

// True while the autopilot owns the ship — main.cpp uses this to mute
// player thrust / aim / cruise input so the player can't fight the dock.
bool controls_locked(const Docking& d);

// Eligibility test, side-effect free. Player nav points are static so
// "relative speed" is just |player_vel|. Call every frame to feed the
// HUD prompt; request() runs the same check before committing.
DockResult can_request(const Docking& d, HMM_Vec3 player_pos,
                       HMM_Vec3 player_vel, const NavPointDef& nav);

// HUD/log string for a verdict. NotDockable returns "" (draw nothing).
const char* result_str(DockResult r);

// Begin an auto-approach to `nav` if can_request clears. Returns the
// verdict so the caller can log/sfx the rejection too.
DockResult request(Docking& d, HMM_Vec3 player_pos, HMM_Vec3 player_vel,
                   const NavPointDef& nav);

// Force-begin the auto-approach (np-3dp.22): the proximity 'automatic
// landing zone' captures the ship regardless of speed/range — the approach
// autopilot eases the velocity onto the pad — so this SKIPS can_request's
// gates. No-op if already docking or in the post-launch cooldown.
void begin_auto(Docking& d, const NavPointDef& nav);

// Distance band knobs for the automatic landing zone (np-3dp.22):
// announce the zone (comms + sting) crossing inside k_zone_announce_m,
// auto-capture the ship inside k_auto_land_m.
constexpr float k_zone_announce_m = 900.0f;
constexpr float k_auto_land_m     = 600.0f;

// Advance the approach. Drives camera pose/velocity while Approaching,
// holds the beat while Docking, then requests Landed + flips the player
// flags on completion. Also bleeds the post-launch cooldown. Safe to
// call every Flight frame; a no-op when state == None and cooldown == 0.
void tick(Docking& d, Camera& cam, GameState& gs, PlayerState& player, float dt);

// Leave Landed: request Flight, place the ship k_launch_offset_m off the
// pad with a small outward velocity, clear the docked flag, and arm a
// brief cooldown so we don't instantly re-dock on the way out.
void launch(Docking& d, Camera& cam, GameState& gs, PlayerState& player);

} // namespace docking
