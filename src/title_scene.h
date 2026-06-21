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

// Lifecycle (np-3dp). All four are safe to call multiple times.
void init(Category cat,
          std::unordered_map<std::string, SpriteArt>& art_cache);
void tick(float dt);            // advance ship positions
void shutdown();                // release cached atlases
bool inited();                  // true once init has been called
Category category();            // current category after init
const char* category_label(Category c);  // "Confed Navy", "Kilrathi", etc.

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
