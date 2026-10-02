// -----------------------------------------------------------------------------
// sky_prop_renderer.cpp — see sky_prop_renderer.h.
// -----------------------------------------------------------------------------

#include "sky_prop_renderer.h"
#include "render_config.h"
#include "stb_image.h"

#include "generated/sky_prop.glsl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr float k_deg_to_rad = 0.01745329251f;
constexpr float k_two_pi     = 6.28318530718f;

constexpr float kQuadCorners[] = {
    -1.0f, -1.0f,   1.0f, -1.0f,   -1.0f, 1.0f,   1.0f, 1.0f,
};

// Box-filter mip chain. A 512px galaxy lands at ~150-250px on screen, so
// without mips it shimmers as the camera turns.
std::vector<std::vector<uint8_t>> build_mips(const uint8_t* px, int w, int h) {
    std::vector<std::vector<uint8_t>> levels;
    levels.emplace_back(px, px + (size_t)w * h * 4);
    while ((w > 1 || h > 1) && (int)levels.size() < SG_MAX_MIPMAPS) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const std::vector<uint8_t>& src = levels.back();
        std::vector<uint8_t> dst((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                for (int c = 0; c < 4; ++c) {
                    const int sum = src[((size_t)y0 * w + x0) * 4 + c]
                                  + src[((size_t)y0 * w + x1) * 4 + c]
                                  + src[((size_t)y1 * w + x0) * 4 + c]
                                  + src[((size_t)y1 * w + x1) * 4 + c];
                    dst[((size_t)y * nw + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
                }
            }
        }
        levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return levels;
}

// Brightness multiplier in [1 - depth, 1] for a slow cosine pulse.
float pulse(const SkyPropDef& p, float time_sec) {
    if (p.pulse_hz <= 0.0f || p.pulse_depth <= 0.0f) return 1.0f;
    const float wave = 0.5f - 0.5f * std::cos(k_two_pi * (p.pulse_hz * time_sec + p.phase));
    return 1.0f - p.pulse_depth * wave;
}

} // namespace

bool SkyPropRenderer::init() {
    sg_buffer_desc vbd{};
    vbd.data = SG_RANGE(kQuadCorners);
    vbuf_ = sg_make_buffer(&vbd);

    sg_sampler_desc sd{};
    sd.min_filter    = SG_FILTER_LINEAR;
    sd.mag_filter    = SG_FILTER_LINEAR;
    sd.mipmap_filter = SG_FILTER_LINEAR;
    sd.wrap_u        = SG_WRAP_CLAMP_TO_EDGE;
    sd.wrap_v        = SG_WRAP_CLAMP_TO_EDGE;
    sampler_ = sg_make_sampler(&sd);

    shader_ = sg_make_shader(sky_prop_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = shader_;
    pd.layout.attrs[ATTR_sky_prop_a_corner].format = SG_VERTEXFORMAT_FLOAT2;
    pd.primitive_type          = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    pd.cull_mode               = SG_CULLMODE_NONE;
    pd.depth.compare           = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled     = false;
    pd.depth.pixel_format      = kSceneDepthFormat;
    pd.colors[0].pixel_format  = kSceneColorFormat;
    // Additive: the art is painted on black, so black contributes nothing.
    // Alpha is left untouched.
    pd.colors[0].blend.enabled          = true;
    pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.sample_count            = kSceneSampleCount;
    pipeline_ = sg_make_pipeline(&pd);

    const bool ok = sg_query_pipeline_state(pipeline_) == SG_RESOURCESTATE_VALID;
    if (!ok) std::fprintf(stderr, "[sky] sky prop pipeline creation failed\n");
    return ok;
}

void SkyPropRenderer::destroy() {
    for (auto& [_, t] : textures_) {
        sg_destroy_view(t.view);
        sg_destroy_image(t.image);
    }
    textures_.clear();
    props_.clear();
    sg_destroy_pipeline(pipeline_);
    sg_destroy_shader(shader_);
    sg_destroy_sampler(sampler_);
    sg_destroy_buffer(vbuf_);
}

bool SkyPropRenderer::load_texture(const std::string& sprite) {
    if (textures_.count(sprite)) return true;
    const std::string path = "assets/" + sprite + ".png";
    int w = 0, h = 0, channels = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!px) {
        std::fprintf(stderr, "[sky] skipping far-field prop '%s': %s\n",
                     path.c_str(), stbi_failure_reason());
        return false;
    }
    const auto mips = build_mips(px, w, h);
    stbi_image_free(px);

    sg_image_desc id{};
    id.width        = w;
    id.height       = h;
    id.num_mipmaps  = (int)mips.size();
    id.pixel_format = SG_PIXELFORMAT_RGBA8;
    for (size_t i = 0; i < mips.size(); ++i)
        id.data.mip_levels[i] = { mips[i].data(), mips[i].size() };

    Texture t;
    t.image = sg_make_image(&id);
    sg_view_desc vd{};
    vd.texture.image = t.image;
    t.view = sg_make_view(&vd);
    if (sg_query_image_state(t.image) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[sky] texture creation failed for '%s'\n", path.c_str());
        sg_destroy_view(t.view);
        sg_destroy_image(t.image);
        return false;
    }
    textures_.emplace(sprite, t);
    return true;
}

void SkyPropRenderer::set_props(const std::vector<SkyPropDef>& props) {
    props_.clear();
    for (const SkyPropDef& p : props) {
        if (load_texture(p.sprite)) props_.push_back(p);
    }
}

void SkyPropRenderer::draw(const Camera& cam, float aspect, float time_sec) const {
    if (props_.empty()) return;
    const HMM_Mat4 vp = HMM_MulM4(cam.projection(aspect), cam.view_rotation_only());

    sg_apply_pipeline(pipeline_);
    sg_bindings b{};
    b.vertex_buffers[0] = vbuf_;
    b.samplers[SMP_u_smp] = sampler_;

    vs_params_t vsp{};
    std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);

    for (const SkyPropDef& p : props_) {
        const HMM_Vec3 d = p.direction;
        // Tangent basis at d; fall back to +X as the reference near the poles.
        const HMM_Vec3 ref   = std::fabs(d.Y) > 0.99f ? HMM_V3(1, 0, 0) : HMM_V3(0, 1, 0);
        const HMM_Vec3 right = HMM_NormV3(HMM_Cross(d, ref));
        const HMM_Vec3 up    = HMM_Cross(right, d);

        const float roll = p.roll_rad + p.spin_dps * k_deg_to_rad * time_sec;
        const float half = std::tan(p.angular_deg * 0.5f * k_deg_to_rad);
        const float c = std::cos(roll) * half, s = std::sin(roll) * half;
        const HMM_Vec3 u = HMM_AddV3(HMM_MulV3F(right,  c), HMM_MulV3F(up, s));
        const HMM_Vec3 v = HMM_AddV3(HMM_MulV3F(right, -s), HMM_MulV3F(up, c));

        vsp.center[0] = d.X; vsp.center[1] = d.Y; vsp.center[2] = d.Z;
        vsp.axis_u[0] = u.X; vsp.axis_u[1] = u.Y; vsp.axis_u[2] = u.Z;
        vsp.axis_v[0] = v.X; vsp.axis_v[1] = v.Y; vsp.axis_v[2] = v.Z;

        fs_params_t fsp{};
        const float k = p.intensity * pulse(p, time_sec);
        fsp.tint[0] = fsp.tint[1] = fsp.tint[2] = k;

        b.views[VIEW_u_tex] = textures_.at(p.sprite).view;
        sg_apply_bindings(&b);
        sg_apply_uniforms(UB_vs_params, SG_RANGE(vsp));
        sg_apply_uniforms(UB_fs_params, SG_RANGE(fsp));
        sg_draw(0, 4, 1);
    }
}
