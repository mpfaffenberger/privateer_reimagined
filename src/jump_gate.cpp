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

// Same UV-sphere builder as sun.cpp's; duplicated here to keep this module's
// TU self-contained (sun's helper is in an anonymous namespace). Unit radius
// centered at origin so the vertex shader can scale + translate per-gate.
void build_uv_sphere(int lat, int lon,
                     std::vector<float>& verts,
                     std::vector<uint16_t>& idx) {
    verts.clear();
    idx.clear();
    verts.reserve((size_t)(lat + 1) * (lon + 1) * 3);
    idx.reserve((size_t)lat * lon * 6);

    for (int i = 0; i <= lat; ++i) {
        const float v     = (float)i / (float)lat;
        const float theta = v * (float)M_PI;
        const float sinT  = std::sin(theta);
        const float cosT  = std::cos(theta);
        for (int j = 0; j <= lon; ++j) {
            const float u    = (float)j / (float)lon;
            const float phi  = u * 2.0f * (float)M_PI;
            verts.push_back(sinT * std::cos(phi));
            verts.push_back(cosT);
            verts.push_back(sinT * std::sin(phi));
        }
    }
    const int stride = lon + 1;
    for (int i = 0; i < lat; ++i) {
        for (int j = 0; j < lon; ++j) {
            const uint16_t a = (uint16_t)(i * stride + j);
            const uint16_t b = (uint16_t)(a + 1);
            const uint16_t c = (uint16_t)((i + 1) * stride + j);
            const uint16_t d = (uint16_t)(c + 1);
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
    }
}

} // namespace

bool JumpGate::init(int lat_segments, int lon_segments) {
    std::vector<float>    verts;
    std::vector<uint16_t> idx;
    build_uv_sphere(lat_segments, lon_segments, verts, idx);
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
    pd.layout.attrs[ATTR_jump_gate_a_pos].format = SG_VERTEXFORMAT_FLOAT3;
    pd.index_type = SG_INDEXTYPE_UINT16;

    // CULL_NONE so we see both hemispheres additively — the back side
    // contributes a subtle inner glow, giving the shell its translucent
    // depth read. Depth-test on so a station/rock in front occludes the
    // shell; depth-write off so other transparent layers (dust, streaks,
    // tracers) composite cleanly on top.
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

    for (const HMM_Vec3& p : positions) {
        jg_vs_params_t vsp{};
        std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
        vsp.world_pos[0] = p.X; vsp.world_pos[1] = p.Y;
        vsp.world_pos[2] = p.Z; vsp.world_pos[3] = radius_m;
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
