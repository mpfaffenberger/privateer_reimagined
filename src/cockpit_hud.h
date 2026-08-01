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
#include <string>
#include "HandmadeMath.h"

struct Camera;
struct StarSystem;
struct Ship;
struct PlayerState;
struct PlayerReputation;
struct ShipSpriteAtlas;
class ShipRegistry;

namespace galaxy { struct Galaxy; }

namespace cockpit_hud {

// Which screen the top-left STATUS panel is currently showing. The canonical
// Privateer cockpit cycles a single MFD frame between sub-displays — here the
// STATUS window flips between the hull diagram (Ship), Comms, live Weapons,
// and the pending component-Damage panel. main.cpp's key handler drives the cycle
// (C/R/W toggles); draw_player_status dispatches on the current value.
enum class StatusScreen { Ship, Comms, Damage, Weapons };

// Set / query the active STATUS sub-screen. File-static inside cockpit_hud;
// the dev_remote /panel endpoint and the flight key handler both poke it.
void        set_status_screen(StatusScreen s);
StatusScreen status_screen();

// World->screen projection (shared between cockpit HUD overlays and the
// loot marker renderer, #84 + DRY). Camera-relative: returns false when
// the point is behind the camera or clip.W <= 0 so the caller can simply
// skip drawing rather than smear a marker across the wrong half of the
// screen. Same engine quirk as the rest of the cockpit: no NDC Y flip
// (cockpit hud rotates via the camera matrix, not the projection).
bool project_world_point(const Camera& cam, HMM_Vec3 world,
                         float& sx, float& sy);

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
//   player_rep     player reputation, forwarded to the STATUS panel's Comms
//                  sub-screen so it can resolve friendly-vs-hostile hail
//                  lines. nullptr = no rep available (Comms shows friendly).
void build(const Camera& cam, const StarSystem& system, int selected_nav,
           float mouse_x, float mouse_y, bool fly_by_wire,
           const ShipRegistry& ships, uint32_t target_ship_id,
           const ShipSpriteAtlas* player_preview_atlas = nullptr,
           const char* dock_prompt = nullptr, bool dock_ready = false,
           bool draw_world = true,    // false hides nav-reticle + mission glyphs
           const PlayerReputation* player_rep = nullptr);

// Mission objective markers + per-type progress readout (#18). Read-only
// over PlayerState::missions + the current system's nav set: floats a cyan
// objective diamond on the targeted nav/base for active jobs in this system
// (`current_system` is the galaxy system id, e.g. PlayerState::current_system),
// and lists a compact progress string for every active mission. Pure draw
// — never mutates the mission model. Call once per Flight frame after
// build(). `draw_world` gates the in-world diamond+label draw; the on-screen
// readout list is always drawn so autopilot/navmap don't leave the player
// blind to active jobs.
void build_mission_objectives(const Camera& cam, const StarSystem& system,
                              const std::string& current_system,
                              const PlayerState& player,
                              bool draw_world = true);

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
    const char* no_lock_label  = "DUMBFIRE";  // override label when needs_lock=false (torpedo)
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
    float       energy     = 0.0f;       // current energy bank (GJ)
    float       energy_max = 0.0f;       // max bank (GJ); 0 hides the bar
    const char* autopilot_nav = nullptr; // null = none engaged
    const char* autopilot_msg = nullptr; // null = no transient banner
};
void draw_flight_status_mfd(const FlightStatusHudState& s);

// Big top-down navigation map. Caller (main.cpp, N key) toggles the
// bool — first N opens, subsequent N presses cycle the selected nav
// in place (same effect as outside-the-map N); Esc or the close
// button flip `shown_in_out` to false. When shown_in_out is true
// this draws a near-fullscreen overlay split into a LEFT map pane
// (top-down system map) and a RIGHT mission-status pane (one row per
// active mission, click a row to select its nav target). Clicking a
// nav point OR a mission row sets `selected_nav_in_out`. The mission
// panel is rendered via missions::mission_status so the text can't
// drift from the in-flight readout (#18). Pure read on `player` +
// the live `current_system_id`; the galaxy graph is consulted only
// to pick a cross-system jump nav toward cargo/bounty targets. Called
// AFTER build() so it draws on top of the regular HUD.
void build_navmap(const Camera& cam, const StarSystem& system,
                  int& selected_nav_in_out,
                  const ShipRegistry& ships,
                  const PlayerState& player,
                  const std::string& current_system_id,
                  const galaxy::Galaxy& galaxy,
                  bool& shown_in_out,
                  bool sector_pane_open = false);

// Sector navmap: the whole galaxy as a graph — every known system as a
// node at its galaxy_position, jump links as edges (drawn once per
// undirected pair). Highlights the current system. Toggled by M.
// No interaction with local nav selection; purely a reference map.
void build_sector_navmap(const galaxy::Galaxy& galaxy,
                         const std::string& current_system_id,
                         bool& shown_in_out,
                         bool beside_navmap = false);

} // namespace cockpit_hud
