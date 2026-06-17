// -----------------------------------------------------------------------------
// warp_streaks.cpp — sister of dust.cpp; LINES primitive instead of POINTS.
//
// Buffer layout: per particle, two consecutive vertices (head + tail) sharing
// the same random a_pos. The `a_tip` vertex attribute is 0.0 for the head
// and 1.0 for the tail; the vertex shader uses it to gate the
// -vel_dir * streak_len offset. SG_PRIMITIVETYPE_LINES draws (verts[2k],
// verts[2k+1]) as one line each.
// -----------------------------------------------------------------------------

#include "warp_streaks.h"
#include "render_config.h"

#include "generated/warp_streaks.glsl.h"

#include <cstring>
#include <random>
#include <vector>

bool WarpStreaks::init() {
    // Same seed family as dust (deterministic field) but distinct so the
    // streak positions don't co-align with dust specks and pre-cancel.
    std::mt19937 rng(0xA9F1EE00u);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);

    // 2 verts per particle, each vertex = {pos.x, pos.y, pos.z, tip}.
    std::vector<float> verts;
    verts.reserve((size_t)count * 2 * 4);
    for (int i = 0; i < count; ++i) {
        const float x = U(rng), y = U(rng), z = U(rng);
        // head
        verts.push_back(x); verts.push_back(y); verts.push_back(z); verts.push_back(0.0f);
        // tail (same xyz, tip=1)
        verts.push_back(x); verts.push_back(y); verts.push_back(z); verts.push_back(1.0f);
    }

    sg_buffer_desc vbd{};
    vbd.data = { verts.data(), verts.size() * sizeof(float) };
    vbuf = sg_make_buffer(&vbd);

    shader = sg_make_shader(warp_streaks_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = shader;
    pd.layout.attrs[ATTR_warp_streaks_a_pos].format = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_warp_streaks_a_tip].format = SG_VERTEXFORMAT_FLOAT;
    pd.primitive_type = SG_PRIMITIVETYPE_LINES;

    // Additive alpha-blend, same recipe as dust + tracers so the streaks
    // brighten whatever's behind without darkening. Depth-test on (so a
    // station occludes the streak) but no depth write (so other transparent
    // layers like dust composite cleanly on top).
    pd.colors[0].blend.enabled          = true;
    pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = false;
    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;
    pipeline = sg_make_pipeline(&pd);
    return sg_query_pipeline_state(pipeline) == SG_RESOURCESTATE_VALID;
}

void WarpStreaks::draw(const Camera& cam, float aspect) const {
    // Self-gate: if the caller hasn't ramped intensity above zero (i.e.
    // autopilot off / not cruising), skip the draw entirely. Keeps the
    // call-site in main.cpp unconditional.
    if (intensity <= 0.0f || streak_len_m <= 0.5f) return;

    const HMM_Mat4 vp = HMM_MulM4(cam.projection(aspect), cam.view());

    vs_params_t vsp{};
    std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
    vsp.cam_pos[0] = cam.position.X; vsp.cam_pos[1] = cam.position.Y;
    vsp.cam_pos[2] = cam.position.Z; vsp.cam_pos[3] = 0.0f;
    vsp.field_params[0] = wrap_extent;
    vsp.field_params[1] = streak_len_m;
    vsp.field_params[2] = intensity;
    vsp.field_params[3] = 0.0f;
    vsp.vel_dir[0] = vel_dir.X; vsp.vel_dir[1] = vel_dir.Y;
    vsp.vel_dir[2] = vel_dir.Z; vsp.vel_dir[3] = 0.0f;

    sg_bindings b{};
    b.vertex_buffers[0] = vbuf;

    sg_apply_pipeline(pipeline);
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_vs_params, SG_RANGE(vsp));
    // 2 verts per particle, primitive LINES -> count lines, 2*count verts.
    sg_draw(0, count * 2, 1);
}

void WarpStreaks::destroy() {
    sg_destroy_pipeline(pipeline);
    sg_destroy_shader(shader);
    sg_destroy_buffer(vbuf);
}
