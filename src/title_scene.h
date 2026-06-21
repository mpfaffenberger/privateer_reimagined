#pragma once
// -----------------------------------------------------------------------------
// title_scene.h — background scene for the WCPrivateer-style title screen.
//
// Picks a faction category at startup and renders a small patrol of
// canonical ships for that category flying past the camera. The caller
// is expected to feed the ship's star preset + skybox_seed into the
// existing star / skybox paths so the background reads as canonical.
// -----------------------------------------------------------------------------

#include "ship_sprite.h"
#include "sprite.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace title_scene {

enum class Category {
    ConfedMilitary,    // broadsword, stiletto, paradigm (heavy military)
    Militia,           // gladius, talon
    BountyHunter,      // centurion, orion, demon (combat hunters)
    Kilrathi,          // kamekh, dralthi, gothri
    Merchant,          // drayman, galaxy, tarsus
};

// Title background variant (np-3dp). Picked randomly at init.
enum class Variant {
    PatrolFlyby,   // several ships cross the view in straight lines
    ChaseCam,      // one ship centered, cruising w/ warp streaks; cycles
};

// Per-frame render hints the chase-cam variant needs main.cpp to apply:
// warp streaks (the autopilot cruise trails) + the sun position. The
// patrol variant returns warp_on=false / sun_override=false so the
// caller can query unconditionally.
struct ChaseConfig {
    bool     warp_on        = false;
    float    warp_intensity = 0.0f;
    float    warp_len       = 0.0f;
    HMM_Vec3 warp_dir       { 0, 0, 1 };
    bool     sun_override   = false;
    HMM_Vec3 sun_pos        { 0, 0, 0 };
    // Full camera pose for the chase shot. When cam_override is true the
    // caller renders the scene from (cam_pos, cam_orient) instead of the
    // player camera — the chase cam orbits a ship cruising in a straight
    // line, and the camera motion is what makes the warp streaks flow.
    bool     cam_override   = false;
    HMM_Vec3 cam_pos        { 0, 0, 0 };
    HMM_Quat cam_orient     { 0, 0, 0, 1 };
};

// Lifecycle (np-3dp). All four are safe to call multiple times.
void init(Category cat,
          std::unordered_map<std::string, SpriteArt>& art_cache);
void tick(float dt);            // advance ship positions
void shutdown();                // release cached atlases
bool inited();                  // true once init has been called
Category category();            // current category after init
Variant  variant();             // PatrolFlyby or ChaseCam (random per init)
bool     wants_camera_roll();   // true if the variant needs the 180° view roll

// Engine-hum inputs for the title screen (np-3dp.7). Both fields are 0 /
// false when the variant isn't the chase cam — the patrol ships just drift
// and the hum bed fades out, same as landed mode.
//   hum_speed_frac: 0..1 throttle fraction for the engine-hum bed gain
//                   (idle 0.04 + 0.25 * sf, capped at 0.4).
//   hum_cruise     : the afterburner spool flag (true while > 0.10).
float    hum_speed_frac();
bool     hum_cruise();
const char* category_label(Category c);  // "Confed Navy", "Kilrathi", etc.

// Chase-cam render hints (warp streaks + sun position). Computed from the
// camera basis; main.cpp applies them. Returns an all-off config for the
// patrol variant.
ChaseConfig chase_config(HMM_Vec3 cam_pos, HMM_Vec3 cam_fwd,
                         HMM_Vec3 cam_right, HMM_Vec3 cam_up);

// Anchor the patrol ships (np-3dp). Call each frame the title is up.
// Ships fly in straight lines across the view (left<->right along the
// camera's right axis) at varying depth + height. Pass the camera's
// position + full basis so ships are framed in front of the player and
// face their direction of travel.
void set_anchor(HMM_Vec3 pos, HMM_Vec3 fwd, HMM_Vec3 right, HMM_Vec3 up);

// Per-category data lookups. main.cpp uses these to drive the sun +
// skybox so the title background reads canonical.
const char* star_preset(Category c);     // "yellow", "red", etc.
const char* skybox_seed(Category c);     // skybox seed for SkyFamily

// Feed the spawned patrol ships into the existing sprite render pass.
// Must be called each frame when the title is up, AFTER frame_sprites
// has been cleared by sprite_render setup but BEFORE sprite_render.draw().
// Ships pose is set by tick() called earlier in frame_cb().
void append_to_frame_sprites(const Camera& cam,
                              std::vector<SpriteObject>& out_sprites);

} // namespace title_scene
