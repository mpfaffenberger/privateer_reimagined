// -----------------------------------------------------------------------------
// sky_card.cpp — see sky_card.h.
// -----------------------------------------------------------------------------

#include "sky_card.h"
#include "render_config.h"
#include "sky_rng.h"

#include "generated/sky_card.glsl.h"

#include <cmath>
#include <cstring>

namespace {

constexpr float k_deg_to_rad = 0.01745329251f;

constexpr float kQuadCorners[] = {
    -1.0f, -1.0f,   1.0f, -1.0f,   -1.0f, 1.0f,   1.0f, 1.0f,
};

// Half-extent on the tangent plane at unit distance for an angular span.
float half_extent(float deg) { return std::tan(deg * 0.5f * k_deg_to_rad); }

} // namespace

SkyCard sky_card_at(HMM_Vec3 dir, float deg_u, float deg_v, float roll_rad) {
    const HMM_Vec3 right = sky_any_tangent(dir);
    const HMM_Vec3 up    = HMM_Cross(right, dir);
    const float c = std::cos(roll_rad), s = std::sin(roll_rad);
    const HMM_Vec3 u = HMM_AddV3(HMM_MulV3F(right,  c), HMM_MulV3F(up, s));
    const HMM_Vec3 v = HMM_AddV3(HMM_MulV3F(right, -s), HMM_MulV3F(up, c));
    return { dir, HMM_MulV3F(u, half_extent(deg_u)), HMM_MulV3F(v, half_extent(deg_v)) };
}

SkyCard sky_card_along(HMM_Vec3 dir, HMM_Vec3 along, float deg_u, float deg_v,
                       float anchor_u) {
    // Project `along` onto the tangent plane; fall back if it's degenerate.
    HMM_Vec3 u = HMM_SubV3(along, HMM_MulV3F(dir, HMM_DotV3(along, dir)));
    u = HMM_LenV3(u) > 1e-4f ? HMM_NormV3(u) : sky_any_tangent(dir);
    const HMM_Vec3 v  = HMM_Cross(dir, u);
    const HMM_Vec3 au = HMM_MulV3F(u, half_extent(deg_u));
    return { HMM_SubV3(dir, HMM_MulV3F(au, anchor_u)), au,
             HMM_MulV3F(v, half_extent(deg_v)) };
}

SkyCard sky_card_streak(HMM_Vec3 tail, HMM_Vec3 head, float deg_width) {
    const HMM_Vec3 mid  = HMM_MulV3F(HMM_AddV3(head, tail), 0.5f);
    const HMM_Vec3 half = HMM_MulV3F(HMM_SubV3(head, tail), 0.5f);
    HMM_Vec3 side = HMM_Cross(mid, half);
    side = HMM_LenV3(side) > 1e-6f ? HMM_NormV3(side) : sky_any_tangent(mid);
    return { mid, half, HMM_MulV3F(side, half_extent(deg_width)) };
}

sg_buffer make_sky_card_quad() {
    sg_buffer_desc vbd{};
    vbd.data = SG_RANGE(kQuadCorners);
    return sg_make_buffer(&vbd);
}

sg_pipeline make_sky_card_pipeline(sg_shader shader) {
    sg_pipeline_desc pd{};
    pd.shader = shader;
    pd.layout.attrs[ATTR_sky_prop_a_corner].format = SG_VERTEXFORMAT_FLOAT2;
    pd.primitive_type          = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    pd.cull_mode               = SG_CULLMODE_NONE;
    pd.depth.compare           = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled     = false;
    pd.depth.pixel_format      = kSceneDepthFormat;
    pd.colors[0].pixel_format  = kSceneColorFormat;
    // Additive: the art is painted on black, so black contributes nothing.
    // Destination alpha is left untouched.
    pd.colors[0].blend.enabled          = true;
    pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.sample_count            = kSceneSampleCount;
    return sg_make_pipeline(&pd);
}

HMM_Mat4 sky_card_view_proj(const Camera& cam, float aspect) {
    return HMM_MulM4(cam.projection(aspect), cam.view_rotation_only());
}

void draw_sky_card(const SkyCard& card, const HMM_Mat4& view_proj) {
    vs_params_t vsp{};
    std::memcpy(vsp.view_proj, &view_proj, sizeof(float) * 16);
    vsp.center[0] = card.center.X; vsp.center[1] = card.center.Y; vsp.center[2] = card.center.Z;
    vsp.axis_u[0] = card.axis_u.X; vsp.axis_u[1] = card.axis_u.Y; vsp.axis_u[2] = card.axis_u.Z;
    vsp.axis_v[0] = card.axis_v.X; vsp.axis_v[1] = card.axis_v.Y; vsp.axis_v[2] = card.axis_v.Z;
    sg_apply_uniforms(UB_vs_params, SG_RANGE(vsp));
    sg_draw(0, 4, 1);
}
