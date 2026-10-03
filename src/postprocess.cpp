// -----------------------------------------------------------------------------
// postprocess.cpp — owns the bloom chain (post_bloom.glsl, #724) and runs it
// into RenderTargets::bloom[0]. Composite lives in a separate TU
// (postprocess_composite.cpp) so its shader header doesn't collide with
// post_bloom.glsl.h via sokol-shdc's static source blobs.
// -----------------------------------------------------------------------------

#include "postprocess.h"
#include "rendertargets.h"
#include "render_config.h"

#include "generated/post_bloom.glsl.h"

#include <cstdio>

// Initialises ONLY the bloom pipelines. Composite pipeline is owned by
// postprocess_composite.cpp via init_composite() / destroy_composite().
bool init_composite_pipeline(PostProcess& p);
void destroy_composite_pipeline(PostProcess& p);

namespace {

sg_pipeline make_bloom_pipeline(sg_shader shd, bool additive) {
    sg_pipeline_desc pd{};
    pd.shader                 = shd;
    pd.primitive_type         = SG_PRIMITIVETYPE_TRIANGLES;
    pd.colors[0].pixel_format = kBloomColorFormat;
    pd.sample_count           = kSceneSampleCount;
    pd.depth.pixel_format     = SG_PIXELFORMAT_NONE;
    if (additive) {
        pd.colors[0].blend.enabled          = true;
        pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_ONE;
        pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE;
        pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
        pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    }
    return sg_make_pipeline(&pd);
}

// One fullscreen pass sampling `src` into `dst`. LOAD keeps dst's contents
// for the additive upsample; downsamples overwrite every pixel anyway.
template <typename Uniforms>
void bloom_pass(sg_pipeline pip, sg_view dst, sg_view src, sg_sampler smp,
                int ub_slot, const Uniforms& u, bool keep_dst) {
    sg_pass p{};
    p.attachments.colors[0] = dst;
    p.action.colors[0].load_action = keep_dst ? SG_LOADACTION_LOAD : SG_LOADACTION_DONTCARE;
    sg_begin_pass(&p);
    sg_apply_pipeline(pip);
    sg_bindings b{};
    b.views[0]    = src;
    b.samplers[0] = smp;
    sg_apply_bindings(&b);
    sg_apply_uniforms(ub_slot, SG_RANGE(u));
    sg_draw(0, 3, 1);   // fullscreen triangle from gl_VertexIndex
    sg_end_pass();
}

} // namespace

bool PostProcess::init() {
    bloom_down_shader   = sg_make_shader(bloom_down_shader_desc(sg_query_backend()));
    bloom_up_shader     = sg_make_shader(bloom_up_shader_desc(sg_query_backend()));
    bloom_down_pipeline = make_bloom_pipeline(bloom_down_shader, false);
    bloom_up_pipeline   = make_bloom_pipeline(bloom_up_shader, true);

    if (sg_query_pipeline_state(bloom_down_pipeline) != SG_RESOURCESTATE_VALID ||
        sg_query_pipeline_state(bloom_up_pipeline)   != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[post] bloom pipeline creation failed\n");
        return false;
    }
    return init_composite_pipeline(*this);
}

void PostProcess::destroy() {
    destroy_composite_pipeline(*this);
    sg_destroy_pipeline(bloom_up_pipeline);
    sg_destroy_pipeline(bloom_down_pipeline);
    sg_destroy_shader(bloom_up_shader);
    sg_destroy_shader(bloom_down_shader);
}

void PostProcess::apply_bloom(const RenderTargets& rt) const {
    // -- Down: scene -> bloom[0] (bright-pass) -> bloom[1] -> ... ---------
    sg_view src   = rt.scene_color_tex;
    int     src_w = rt.w, src_h = rt.h;
    for (int i = 0; i < kBloomLevels; ++i) {
        bloom_down_params_t u{};
        u.texel_and_cfg[0] = 1.0f / (float)src_w;
        u.texel_and_cfg[1] = 1.0f / (float)src_h;
        u.texel_and_cfg[2] = (i == 0) ? bloom_threshold : 0.0f;
        u.texel_and_cfg[3] = (i == 0) ? bloom_clamp     : 0.0f;
        u.dst_texel[0]     = 1.0f / (float)rt.bloom[i].w;
        u.dst_texel[1]     = 1.0f / (float)rt.bloom[i].h;
        bloom_pass(bloom_down_pipeline, rt.bloom[i].att, src, rt.linear_clamp,
                   UB_bloom_down_params, u, false);
        src   = rt.bloom[i].tex;
        src_w = rt.bloom[i].w;
        src_h = rt.bloom[i].h;
    }

    // -- Up: bloom[n-1] -> + bloom[n-2] -> ... -> + bloom[0] ---------------
    for (int i = kBloomLevels - 1; i > 0; --i) {
        bloom_up_params_t u{};
        u.texel_and_radius[0] = 1.0f / (float)rt.bloom[i].w;
        u.texel_and_radius[1] = 1.0f / (float)rt.bloom[i].h;
        u.texel_and_radius[2] = bloom_radius;
        u.texel_and_radius[3] = 1.0f;
        u.dst_texel[0]        = 1.0f / (float)rt.bloom[i - 1].w;
        u.dst_texel[1]        = 1.0f / (float)rt.bloom[i - 1].h;
        bloom_pass(bloom_up_pipeline, rt.bloom[i - 1].att, rt.bloom[i].tex,
                   rt.linear_clamp, UB_bloom_up_params, u, true);
    }
}
