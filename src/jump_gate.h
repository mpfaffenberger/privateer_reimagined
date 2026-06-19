#pragma once
// -----------------------------------------------------------------------------
// jump_gate.h — pulsing translucent sphere around each "kind: jump" nav point.
//
// Jump nav points have no sprite/mesh in the system definition; until now
// they were invisible position markers that only existed in the HUD nav
// reticle. This adds a soft cyan-blue translucent shell at each gate's
// world position so the player can SEE jumpgates while flying — same cool
// blue family as the nav reticle (cockpit_hud kCyan), pulsing on a slow
// ~4 s breath plus a faster shimmer so it reads as "active" not "static".
//
// Geometry: a single unit-sphere UV mesh shared across all gates. The draw
// loop iterates a flat vector of world positions and stamps the same mesh
// at each with a per-instance world_pos+radius uniform. Cheap; the gate
// count per system is ~4-6.
//
// Rendering: additive-blend translucent shell (depth-test on, no write) so
// the shell composites over the skybox/dust/asteroids beneath, never
// obscures hulls or stations, and stacks cleanly with the warp streaks +
// tracers that draw later in the same pass.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "camera.h"
#include "HandmadeMath.h"

#include <vector>

struct JumpGate {
    // ---- mesh + GPU resources --------------------------------------------
    sg_buffer   vbuf{};
    sg_buffer   ibuf{};
    int         index_count = 0;
    sg_shader   shader{};
    sg_pipeline pipeline{};

    // ---- per-render knobs (sane defaults; main.cpp can override later) ---
    // radius_m is the world-space half-extent of each gate's shell. Default
    // 1500 m so a gate reads from a couple km out without blotting the
    // skybox at close range. Tint .rgb is the deep-blue core colour
    // (silhouette rim hue is shader-side, hotter white-blue); .a multiplies
    // shell intensity (0 = invisible, 1 = full-brightness pulse cycle).
    float    radius_m   = 7500.0f;
    HMM_Vec4 tint       = { 0.15f, 0.35f, 0.85f, 1.10f };
    float    pulse_slow_hz = 0.25f;   // ~4 s period breath
    float    pulse_fast_hz = 1.30f;   // shimmer overlay
    float    rim_exponent  = 2.5f;    // higher = thinner / hotter rim

    bool init(int lat_segments = 20, int lon_segments = 28);
    // Draws one shell at each position; no-op when positions is empty.
    void draw(const Camera& cam, float aspect, float time_sec,
              const std::vector<HMM_Vec3>& positions) const;
    void destroy();
};
