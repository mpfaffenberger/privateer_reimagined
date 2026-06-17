#pragma once
// -----------------------------------------------------------------------------
// warp_streaks.h — autopilot "cruise streaks" overlay (np-streaks).
//
// A camera-following particle field rendered as additive LINE segments. Each
// streak is two vertices (head + tail) sharing the same random [-1,+1]^3
// seed; the vertex shader wraps the head into the camera-local cube (same
// math as dust.glsl) and offsets the tail backwards along the camera's
// velocity by `streak_len_m`. Visually it's the "warp speed" look you get
// in space games when the engine kicks in.
//
// The field stays loaded all the time, but main.cpp only submits a draw when
// autopilot is engaged AND the player's speed is high enough to make the
// streaks actually elongate. `intensity` is a 0..1 fade the caller ticks up
// or down each frame so the effect spools in/out smoothly when autopilot
// engages or disengages — no popping in.
//
// Pure eye candy, deterministic seed, no game-state coupling. Owned by main
// alongside DustField.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "camera.h"

struct WarpStreaks {
    // ---- sizing -----------------------------------------------------------
    // Lower density than dust (15k specks) — line geometry costs 2 verts
    // each AND fills more pixels, so 3k streaks already feels like a
    // saturated warp field at cruise. Wrap extent matches dust's so the
    // two systems share a parallax bubble.
    int   count         = 3000;
    float wrap_extent   = 600.0f;   // half-extent of the cube around the camera

    // ---- runtime knobs ----------------------------------------------------
    // The caller updates these every frame (main.cpp). intensity is the
    // master fade (0 = off, 1 = full); streak_len_m is the world-space
    // length of the trailing offset. Pre-multiplied here so the shader
    // just reads field_params + vel_dir without doing per-vertex magic.
    float streak_len_m  = 0.0f;
    float intensity     = 0.0f;
    HMM_Vec3 vel_dir    = { 0.0f, 0.0f, 0.0f };  // unit camera velocity

    // ---- GPU resources ----------------------------------------------------
    sg_buffer   vbuf{};
    sg_shader   shader{};
    sg_pipeline pipeline{};

    bool init();
    // Submits one LINES draw call if intensity > 0; otherwise no-op so the
    // caller can call unconditionally each frame and let this gate itself.
    void draw(const Camera& cam, float aspect) const;
    void destroy();
};
