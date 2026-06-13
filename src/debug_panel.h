#pragma once
// -----------------------------------------------------------------------------
// debug_panel.h — Dear ImGui-based live-tweak UI for the ship showcase.
//
// Keeps all the ImGui wiring (sokol_imgui init/frame/render) in one place
// so main.cpp doesn't have to care about the overlay's lifecycle, just
// when to kick off a frame and when to hand off back to the scene.
//
// Philosophy: the panel *shouldn't exist* in a shipped game. It's a dev
// affordance — a slider-fest to figure out what numbers look right. Once
// Mike's happy with a set of values he transcribes them into the system
// JSON and we can #ifdef the whole thing out for a release build later.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

struct PlacedMesh;
struct ShipSpriteObject;
struct StarSystem;
struct GameState;
struct PlayerState;
#include <deque>
#include <vector>

namespace debug_panel {

// Ship spawn/despawn smoke-test requests (np-eag.1). The panel's
// buttons only SET these flags; main.cpp consumes them at the top of
// the next frame — same deferred pattern as game_state::request_mode,
// for the same reason (no mid-frame mutation of the ship registry
// while half the frame's systems have already iterated it). Lives in
// AppState so the intent survives the frame boundary.
struct ShipDebugRequests {
    bool spawn_talon    = false;   // "spawn test talon near player"
    bool despawn_target = false;   // "despawn target" (player's T-lock)
    bool kill_player    = false;   // "kill player" (np-ma2.2 death test)
    // np-ma2.1 reputation test: >= 0 means "run the player-kill rep logic
    // against Faction(value)" for deterministic validation without having
    // to chase down and shoot an actual NPC. -1 = idle.
    int  sim_kill_faction = -1;
};

// Audio smoke-test requests (np-3gw.1), same deferred pattern. main.cpp
// consumes them where it has the camera + nav-point data the 3D test
// needs; the panel just raises intent.
struct AudioDebugRequests {
    bool play_blip_2d  = false;   // centered UI blip
    bool play_blip_nav = false;   // 3D blip at the selected nav point
};

// Lifecycle — call exactly once each.
void init();
void shutdown();

// Input forwarding. Call from sapp's event_cb; returns true if ImGui ate
// the event (so the scene's camera / input handler should ignore it).
bool handle_event(const sapp_event* e);

// Two-phase per-frame flow so the panel can live inside the existing
// composite swapchain pass instead of needing its own. Doing two
// swapchain passes per frame causes drawable-reacquisition flicker on
// Metal — unforgivable for a debug tool that's supposed to help us see.
//
//   build()   — call once per frame BEFORE any sg_begin_pass. Starts
//               the ImGui frame and populates widgets + reads back
//               slider mutations into the live ship list.
//
//   render()  — call once per frame INSIDE the existing swapchain pass.
//               Issues the actual draw calls.
void build(std::vector<PlacedMesh>& placed_meshes,
           std::deque<ShipSpriteObject>& ship_sprites,
           GameState& game,
           ShipDebugRequests& ship_debug,
           AudioDebugRequests& audio_debug,
           const PlayerState& player);
void render();

} // namespace debug_panel
