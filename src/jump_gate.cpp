// -----------------------------------------------------------------------------
// jump_gate.cpp — see jump_gate.h. Sphere mesh + additive pipeline + per-gate
// draw loop.
// -----------------------------------------------------------------------------

#include "jump_gate.h"
#include "render_config.h"

#include "generated/jump_gate.glsl.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Billboard quad in [-1,1]^2 (z handled in the VS by camera right/up). The
// black-hole shader maps this to a circular cutout. Two triangles.
void build_billboard(std::vector<float>& verts, std::vector<uint16_t>& idx) {
    verts = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
    };
    idx = { 0, 1, 2, 0, 2, 3 };
}

} // namespace

bool JumpGate::init(int /*lat_segments*/, int /*lon_segments*/) {
    std::vector<float>    verts;
    std::vector<uint16_t> idx;
    build_billboard(verts, idx);
    index_count = (int)idx.size();

    sg_buffer_desc vbd{};
    vbd.data = { verts.data(), verts.size() * sizeof(float) };
    vbuf = sg_make_buffer(&vbd);

    sg_buffer_desc ibd{};
    ibd.usage.index_buffer = true;
    ibd.data = { idx.data(), idx.size() * sizeof(uint16_t) };
    ibuf = sg_make_buffer(&ibd);

    shader = sg_make_shader(jump_gate_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = shader;
    pd.layout.attrs[ATTR_jump_gate_a_quad].format = SG_VERTEXFORMAT_FLOAT2;
    pd.index_type = SG_INDEXTYPE_UINT16;

    // CULL_NONE (billboard winding can flip as the camera orbits). Depth-
    // test on so a station/rock in front occludes the disk; depth-write off
    // so other transparent layers composite cleanly on top.
    pd.cull_mode = SG_CULLMODE_NONE;
    pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = false;

    // Additive premultiplied blend: SRC + DST*(1-SRC.a). Matches the dust /
    // tracer / warp_streaks recipe.
    pd.colors[0].blend.enabled          = true;
    pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;
    pipeline = sg_make_pipeline(&pd);
    if (sg_query_pipeline_state(pipeline) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[jump_gate] pipeline creation failed\n");
        return false;
    }
    return true;
}

void JumpGate::draw(const Camera& cam, float aspect, float time_sec,
                    const std::vector<HMM_Vec3>& positions) const {
    if (positions.empty()) return;

    const HMM_Mat4 vp = HMM_MulM4(cam.projection(aspect), cam.view());

    // Pipeline + bindings are shared across all gates — bind once, then
    // re-upload the per-gate VS uniform (world_pos) per instance.
    sg_bindings b{};
    b.vertex_buffers[0] = vbuf;
    b.index_buffer      = ibuf;
    sg_apply_pipeline(pipeline);
    sg_apply_bindings(&b);

    jg_fs_params_t fsp{};
    fsp.camera_pos[0] = cam.position.X; fsp.camera_pos[1] = cam.position.Y;
    fsp.camera_pos[2] = cam.position.Z; fsp.camera_pos[3] = 0.0f;
    fsp.tint[0] = tint.X; fsp.tint[1] = tint.Y;
    fsp.tint[2] = tint.Z; fsp.tint[3] = tint.W;
    fsp.anim[0] = time_sec;
    fsp.anim[1] = pulse_slow_hz * 6.2831853f;   // convert Hz -> rad/s for sin
    fsp.anim[2] = pulse_fast_hz * 6.2831853f;
    fsp.anim[3] = rim_exponent;
    sg_apply_uniforms(UB_jg_fs_params, SG_RANGE(fsp));

    // Camera basis for the billboard (shared across all gates this frame).
    const HMM_Vec3 cr = cam.right();
    const HMM_Vec3 cu = cam.up();

    for (const HMM_Vec3& p : positions) {
        jg_vs_params_t vsp{};
        std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
        vsp.world_pos[0] = p.X; vsp.world_pos[1] = p.Y;
        vsp.world_pos[2] = p.Z; vsp.world_pos[3] = radius_m;
        vsp.cam_right[0] = cr.X; vsp.cam_right[1] = cr.Y; vsp.cam_right[2] = cr.Z; vsp.cam_right[3] = 0.0f;
        vsp.cam_up[0]    = cu.X; vsp.cam_up[1]    = cu.Y; vsp.cam_up[2]    = cu.Z; vsp.cam_up[3]    = 0.0f;
        sg_apply_uniforms(UB_jg_vs_params, SG_RANGE(vsp));
        sg_draw(0, index_count, 1);
    }
}

void JumpGate::destroy() {
    sg_destroy_pipeline(pipeline);
    sg_destroy_shader(shader);
    sg_destroy_buffer(ibuf);
    sg_destroy_buffer(vbuf);
}
