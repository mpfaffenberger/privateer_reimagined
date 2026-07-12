#pragma once
// -----------------------------------------------------------------------------
// dust.h — infinite parallax dust field.
//
// A fixed VBO of N random points in [-1, +1]^3 gets translated + wrapped in
// the vertex shader to follow the camera, creating an illusion of an
// unbounded field without re-uploading geometry. See shaders/dust.glsl.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "camera.h"

struct DustField {
    int   count         = 3500;    // sparse motion cue, not a snowstorm
    float wrap_extent   = 600.0f;
    float point_size_px = 2.0f;

    sg_buffer   vbuf{};
    sg_shader   shader{};
    sg_pipeline pipeline{};

    bool init();
    // `speed_kps` follows the game's canonical displayed-speed convention.
    // Debris fades from 1200 and is completely absent at/above 1500 kps.
    void draw(const Camera& cam, float aspect, float speed_kps) const;
    void destroy();
};
