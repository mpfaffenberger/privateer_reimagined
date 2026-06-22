// cockpit_hud.h — modern flight-sim HUD overlay (ship-agnostic).
//
// Per-frame ImGui draws for the in-flight HUD:
//
//   * gun crosshair          — small static '+' at screen centre,
//                              indicates where weapons will fire
//   * nav target reticle     — amber circle that floats over the
//                              currently-selected waypoint, with
//                              edge-clamp + chevron when off-screen
//   * target MFD             — bottom-right ImGui panel showing
//                              TARGET / DIST / AZ-EL data block
//   * radar MFD              — bottom-left top-down radar disc with
//                              colour-coded nav dots and sweep line
//
// All draws happen via ImGui (drawlists for primitives, regular
// windows for MFD frames) so they composite naturally with the
// debug_panel + sprite_light_editor widgets that share the same
// frame. Call cockpit_hud::build() once per frame, between
// simgui_new_frame() and simgui_render().
//
// The HUD is intentionally ship-agnostic. Per-ship cosmetic 'cockpit
// rails' (decorative side/bottom hull sprites that vary by hull) are
// a separate future system that will composite UNDER these widgets.
#pragma once

#include <cstdint>
#include "HandmadeMath.h"

struct Camera;
struct StarSystem;
struct Ship;
class ShipRegistry;

namespace cockpit_hud {

// One call, one frame.
//
//   selected_nav   index into system.nav_points; -1 = no target
//                  (reticle hidden, target MFD shows placeholder).
//   mouse_x/y      logical-pixel mouse position; drives the aim cursor.
//   fly_by_wire    true = aim cursor drawn (player is flying with
//                  mouse). false = cursor mode (OS cursor visible,
//                  ship doesn't turn). We HIDE the in-game aim
//                  reticle in cursor mode so we don't double-draw.
//   ships          live ship registry (player at the well-known slot-0
//                  handle; see ship_registry.h). Used by the radar to
//                  project the player's perception contacts onto the
//                  same disc that already shows nav points.
//   target_ship_id currently-locked ship target (T-cycle); 0 = none.
//                  The radar adds a highlight ring to that blip.
//   dock_prompt    docking feedback line for the NAV MFD (np-9cu.1):
//                  nullptr/"" draws nothing; otherwise the string is
//                  shown under the nav data. `dock_ready` tints it
//                  green ("PRESS D TO DOCK") vs amber ("DOCK: TOO FAST").
void build(const Camera& cam, const StarSystem& system, int selected_nav,
           float mouse_x, float mouse_y, bool fly_by_wire,
           const ShipRegistry& ships, uint32_t target_ship_id,
           const char* dock_prompt = nullptr, bool dock_ready = false);

// Sun-proximity warning overlay (np-3dp). Centre-screen banner that
// fires whenever the camera is inside the 20k avoid bubble around the
// sun. Between 15k and 20k: yellow "WARNING - APPROACHING SUN" with the
// current distance. Inside 15k: BIG red "DESTRUCTION IMMINENT" with a
// pulsing border + flash text. No-op when outside the bubble.
void draw_sun_warning(const Camera& cam, HMM_Vec3 sun_pos);

// Weapons + ordnance status block (np-zte.2). A small left-edge HUD
// readout: selected missile type + remaining count and the target-lock
// state (---- / SEEK / LOCK with an IR build-up bar). Afterburner fuel
// is gone here — the energy bar in the STATUS panel doubles as the
// burner gauge now that gun and afterburner share one pool. Pure draw
// — the caller (main.cpp) owns the lock state machine and passes a
// snapshot. Drawn each Flight frame after build().
struct WeaponsHudState {
    const char* missile_name   = "DF";   // selected type short name
    int         missile_count  = 0;      // remaining of the selected type
    bool        needs_lock     = false;  // selected type homes (HS/IR)
    int         lock_state     = 0;      // 0 none, 1 seeking, 2 locked
    float       lock_progress  = 0.0f;   // 0..1 IR build-up (seeking only)
};
void build_weapons_status(const WeaponsHudState& w);

// Top-centre FLIGHT panel — speed / cruise mode / sun distance / position,
// plus optional autopilot status lines. Same amber-border boxed styling as
// the STATUS / TARGET panels in the screen corners. Replaces the old
// sdtx text block that used to sit free-floating at the top.
struct FlightStatusHudState {
    float       speed = 0.0f;            // m/s (length of camera velocity)
    const char* mode  = "NORMAL";        // NORMAL / SPOOL / CRUISE
    float       d_sun = 0.0f;            // world units to the system sun
    float       pos_x = 0.0f, pos_y = 0.0f, pos_z = 0.0f;
    const char* autopilot_nav = nullptr; // null = none engaged
    const char* autopilot_msg = nullptr; // null = no transient banner
};
void draw_flight_status_mfd(const FlightStatusHudState& s);

// Big top-down navigation map. Caller (main.cpp, N key) toggles the
// bool — first N opens, subsequent N presses cycle the selected nav
// in place (same effect as outside-the-map N); Esc or the close
// button flip `shown_in_out` to false. When shown_in_out is true
// this draws a centered overlay with nav points and ship contacts
// on a system-scale projection. Clicking a nav point selects it
// (mutates `selected_nav_in_out`). Called AFTER build() so it draws
// on top of the regular HUD.
void build_navmap(const Camera& cam, const StarSystem& system,
                  int& selected_nav_in_out,
                  const ShipRegistry& ships,
                  bool& shown_in_out);

} // namespace cockpit_hud
