// -----------------------------------------------------------------------------
// sky_motion_renderer.cpp — see sky_motion_renderer.h.
// -----------------------------------------------------------------------------

#include "sky_motion_renderer.h"
#include "sky_card.h"

#include "generated/sky_card.glsl.h"

#include <cstdio>

namespace {

constexpr float k_meteor_width_deg = 0.25f;
const HMM_Vec3  k_meteor_color     = { 0.85f, 0.92f, 1.0f };   // icy blue-white

// fs_comet puts the coma at u = 0.12 (corner -0.76); the tail fills the rest.
constexpr float k_comet_coma_anchor = -0.76f;
constexpr float k_comet_tail_share  = 0.88f;
constexpr float k_comet_aspect      = 0.9f;    // card width / length

void apply_tint(float r, float g, float b, float a) {
    fs_params_t fsp{};
    fsp.tint[0] = r; fsp.tint[1] = g; fsp.tint[2] = b; fsp.tint[3] = a;
    sg_apply_uniforms(UB_fs_params, SG_RANGE(fsp));
}

} // namespace

bool SkyMotionRenderer::init() {
    vbuf_            = make_sky_card_quad();
    meteor_shader_   = sg_make_shader(sky_meteor_shader_desc(sg_query_backend()));
    comet_shader_    = sg_make_shader(sky_comet_shader_desc(sg_query_backend()));
    meteor_pipeline_ = make_sky_card_pipeline(meteor_shader_);
    comet_pipeline_  = make_sky_card_pipeline(comet_shader_);
    const bool ok = sg_query_pipeline_state(meteor_pipeline_) == SG_RESOURCESTATE_VALID
                 && sg_query_pipeline_state(comet_pipeline_)  == SG_RESOURCESTATE_VALID;
    if (!ok) std::fprintf(stderr, "[sky] meteor/comet pipeline creation failed\n");
    return ok;
}

void SkyMotionRenderer::destroy() {
    sg_destroy_pipeline(comet_pipeline_);
    sg_destroy_pipeline(meteor_pipeline_);
    sg_destroy_shader(comet_shader_);
    sg_destroy_shader(meteor_shader_);
    sg_destroy_buffer(vbuf_);
}

void SkyMotionRenderer::set(const SkyCometDef& comet, const SkyMeteorsDef& meteors) {
    comet_   = comet;
    meteors_ = meteors;
}

void SkyMotionRenderer::draw(const Camera& cam, float aspect, float time_sec,
                             HMM_Vec3 sun_dir) const {
    SkyMeteor meteor;
    const bool has_meteor = sky_meteor_at(meteors_, time_sec, meteor);
    if (!comet_.enabled && !has_meteor) return;

    const HMM_Mat4 vp = sky_card_view_proj(cam, aspect);
    sg_bindings b{};
    b.vertex_buffers[0] = vbuf_;

    if (comet_.enabled) {
        const float len = comet_.tail_deg / k_comet_tail_share;
        const HMM_Vec3 away_from_sun = HMM_MulV3F(sun_dir, -1.0f);
        sg_apply_pipeline(comet_pipeline_);
        sg_apply_bindings(&b);
        apply_tint(comet_.intensity, comet_.intensity, comet_.intensity, time_sec);
        draw_sky_card(sky_card_along(comet_.direction, away_from_sun, len,
                                     len * k_comet_aspect, k_comet_coma_anchor), vp);
    }
    if (has_meteor) {
        sg_apply_pipeline(meteor_pipeline_);
        sg_apply_bindings(&b);
        apply_tint(k_meteor_color.X * meteor.brightness, k_meteor_color.Y * meteor.brightness,
                   k_meteor_color.Z * meteor.brightness, 0.0f);
        draw_sky_card(sky_card_streak(meteor.tail, meteor.head, k_meteor_width_deg), vp);
    }
}
