// -----------------------------------------------------------------------------
// sky_prop_renderer.cpp — see sky_prop_renderer.h.
// -----------------------------------------------------------------------------

#include "sky_prop_renderer.h"
#include "sky_card.h"
#include "stb_image.h"

#include "generated/sky_card.glsl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace {

constexpr float k_deg_to_rad = 0.01745329251f;
constexpr float k_two_pi     = 6.28318530718f;

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
    vbuf_ = make_sky_card_quad();

    sg_sampler_desc sd{};
    sd.min_filter    = SG_FILTER_LINEAR;
    sd.mag_filter    = SG_FILTER_LINEAR;
    sd.mipmap_filter = SG_FILTER_LINEAR;
    sd.wrap_u        = SG_WRAP_CLAMP_TO_EDGE;
    sd.wrap_v        = SG_WRAP_CLAMP_TO_EDGE;
    sampler_ = sg_make_sampler(&sd);

    shader_   = sg_make_shader(sky_prop_shader_desc(sg_query_backend()));
    pipeline_ = make_sky_card_pipeline(shader_);

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
    const HMM_Mat4 vp = sky_card_view_proj(cam, aspect);

    sg_apply_pipeline(pipeline_);
    sg_bindings b{};
    b.vertex_buffers[0] = vbuf_;
    b.samplers[SMP_u_smp] = sampler_;

    for (const SkyPropDef& p : props_) {
        const float roll = p.roll_rad + p.spin_dps * k_deg_to_rad * time_sec;
        fs_params_t fsp{};
        const float k = p.intensity * pulse(p, time_sec);
        fsp.tint[0] = fsp.tint[1] = fsp.tint[2] = k;

        b.views[VIEW_u_tex] = textures_.at(p.sprite).view;
        sg_apply_bindings(&b);
        sg_apply_uniforms(UB_fs_params, SG_RANGE(fsp));
        draw_sky_card(sky_card_at(p.direction, p.angular_deg, p.angular_deg, roll), vp);
    }
}
