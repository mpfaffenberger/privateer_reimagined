#pragma once
// -----------------------------------------------------------------------------
// sky_props.h — far-field galaxies and anomalies painted on the celestial
// sphere (#693, revives PR #425).
//
// Pure data + seeding, no GPU: system_def parses/seeds a list of SkyPropDef,
// SkyPropRenderer (sky_prop_renderer.h) draws it. Props are purely visual —
// never in placed_sprites, so no collision, targeting, radar, or nav map.
//
// Each system gets 1-3 props from a hash of its skybox_seed (at least one
// galaxy), or an authored override in the system JSON:
//
//   "sky": {
//     "props": [
//       { "sprite": "sky/props/galaxy_spiral_blue",
//         "dir": [0.2, 0.8, -0.5], "angular_deg": 12,
//         "roll_deg": 20, "intensity": 0.8 }
//     ]
//   }
//
// An empty "props" array is an explicit opt-out.
//
// Gas clouds (#704) are props too: big, faint, tinted to the system's nebula
// palette, with a finite `parallax_m` so they shift as you fly. They are
// seeded in main (they need the sky family's palette); `"clouds": N` in the
// "sky" object forces a count (0 = none).
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <string>
#include <vector>

namespace json { struct Value; }

struct SkyPropDef {
    std::string sprite;                          // assets-relative stem (no .png)
    HMM_Vec3    direction   = { 0.0f, 1.0f, 0.0f };  // world-space unit vector
    float       angular_deg = 10.0f;             // apparent diameter
    float       roll_rad    = 0.0f;              // initial in-plane rotation
    float       intensity   = 0.8f;              // additive brightness scale
    // Motion. Spin is in-plane rotation; pulse modulates brightness by
    // up to `pulse_depth` (0 = steady) at `pulse_hz`.
    float       spin_dps    = 0.0f;
    float       pulse_hz    = 0.0f;
    float       pulse_depth = 0.0f;
    float       phase       = 0.0f;              // pulse phase offset [0, 1)
    HMM_Vec3    tint        = { 1.0f, 1.0f, 1.0f };  // multiplies the art
    // Virtual distance in metres. 0 = infinitely far (no parallax). Otherwise
    // the prop sits at direction * parallax_m and is seen from the camera,
    // but is still drawn on the far plane, so it can never be reached.
    float       parallax_m  = 0.0f;
};

// Built-in art pack. Index order is part of the seed hash: entries
// [0, k_sky_prop_galaxy_count) are galaxies, the rest anomalies.
struct SkyPropCatalogEntry {
    const char* sprite;
    float       angular_deg;   // typical apparent diameter
    float       intensity;
    float       spin_dps;
    float       pulse_hz;
    float       pulse_depth;
};
constexpr int k_sky_prop_galaxy_count = 4;
const SkyPropCatalogEntry* sky_prop_catalog(int* count);

// Authored props get catalog motion/brightness defaults when their sprite
// is in the pack, so an override only needs "sprite" + "dir".
const SkyPropCatalogEntry* find_sky_prop_catalog_entry(const std::string& sprite);

// Deterministic 1-3 props for `skybox_seed`. Same seed -> same sky.
std::vector<SkyPropDef> autogen_sky_props(const std::string& skybox_seed);

// Parse one authored prop. Returns a def with an empty sprite if invalid.
SkyPropDef parse_sky_prop(const json::Value& v);

// Deterministic gas clouds for `skybox_seed`, tinted between the sky
// family's two palette anchors. `count` < 0 = seeded (3-5).
std::vector<SkyPropDef> autogen_sky_clouds(const std::string& skybox_seed, int count,
                                           HMM_Vec3 tint_a, HMM_Vec3 tint_b);

// Where a prop appears from `camera_pos`: unit direction and apparent
// diameter (degrees). Props with parallax_m == 0 ignore the camera.
void sky_prop_apparent(const SkyPropDef& p, HMM_Vec3 camera_pos,
                       HMM_Vec3& out_dir, float& out_deg);

// Clamp range for angular_deg. Keeps a typo from filling the screen.
constexpr float k_sky_prop_min_deg = 2.0f;
constexpr float k_sky_prop_max_deg = 28.0f;
