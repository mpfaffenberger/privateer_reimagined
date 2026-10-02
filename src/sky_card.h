#pragma once
// -----------------------------------------------------------------------------
// sky_card.h — shared plumbing for the far-plane backdrop cards
// (shaders/sky_card.glsl): geometry on the celestial sphere, the unit quad,
// the additive pipeline, and the per-card vertex uniforms. Used by
// SkyPropRenderer (#693) and SkyMotionRenderer (#701).
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"
#include "camera.h"
#include "sokol_gfx.h"

// A card in rotation-only view space: centre plus half-axes.
struct SkyCard {
    HMM_Vec3 center;
    HMM_Vec3 axis_u;
    HMM_Vec3 axis_v;
};

// Card in the plane tangent to the unit sphere at `dir`, rotated by `roll`
// (radians) and spanning `deg_u` x `deg_v` degrees of sky.
SkyCard sky_card_at(HMM_Vec3 dir, float deg_u, float deg_v, float roll_rad);

// Card whose +U axis follows the unit-sphere tangent `along` at `dir`.
// `anchor_u` in [-1, 1] says where along U `dir` sits (-1 = the -U edge).
SkyCard sky_card_along(HMM_Vec3 dir, HMM_Vec3 along, float deg_u, float deg_v,
                       float anchor_u);

// Thin streak from `tail` to `head` (unit vectors), `deg_width` wide.
SkyCard sky_card_streak(HMM_Vec3 tail, HMM_Vec3 head, float deg_width);

// Shared GPU bits. Every sky card program uses the same quad + vertex
// layout; only the shader differs.
sg_buffer   make_sky_card_quad();
sg_pipeline make_sky_card_pipeline(sg_shader shader);

// projection * rotation-only view, the matrix every sky card uses.
HMM_Mat4 sky_card_view_proj(const Camera& cam, float aspect);

// Applies the shared vertex uniforms, then issues the quad draw. Bindings
// (quad + any texture) must already be applied.
void draw_sky_card(const SkyCard& card, const HMM_Mat4& view_proj);
