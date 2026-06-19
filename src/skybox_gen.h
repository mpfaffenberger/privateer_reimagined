#pragma once
// -----------------------------------------------------------------------------
// skybox_gen.h — procedural skybox cubemap generator (B1, on-the-fly).
//
// Renders a seeded starfield + nebula into a render-target cube image at
// runtime, replacing the shipped pre-rendered PNG cubemaps. Clean-room port
// of the skyboxgen passes (Tyro space-3d.js lineage); the painted-sun pass is
// omitted because the engine draws its own 3D sun in world space.
//
// IMPORTANT: generate() issues offscreen render passes, so it MUST be called
// from inside a frame (frame_cb), BEFORE the scene pass begins — you cannot
// begin a pass while the scene pass is open. Resource creation is fine
// anywhere; only the begin/end-pass pair is frame-scoped.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include <string>

namespace skybox_gen {

// Create a cube render-target image (face_res^2 per face, RGBA8) and render the
// procedural sky for `seed` into all six faces. Returns the cube image (also
// usable as a sampled texture); SG_INVALID image id on failure. Transient
// shaders/pipelines/buffers are created and destroyed internally.
sg_image generate(const std::string& seed, int face_res);

}  // namespace skybox_gen
