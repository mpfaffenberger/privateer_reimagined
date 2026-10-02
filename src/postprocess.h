#pragma once
// -----------------------------------------------------------------------------
// postprocess.h — bloom + lens flare + final composite pipeline.
//
// Owns the pipelines & shader modules. RenderTargets supplies the actual
// GPU memory. Use:
//
//     post.init();
//     ...
//     sg_begin_pass(scene attachments);
//     // draw everything
//     sg_end_pass();
//     post.apply_bloom(rt);
//     post.composite_to_swapchain(rt, ...);
//
// Bloom is a progressive mip chain (#724): kBloomLevels downsample passes
// (bright-pass on the first) then kBloomLevels-1 additive upsample passes,
// leaving the result in rt.bloom[0]. One more pass composites + flares.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "HandmadeMath.h"
#include "render_config.h"

#include <functional>

struct RenderTargets;

struct PostProcess {
    // Bloom chain pipelines (post_bloom.glsl): plain-write downsample and
    // additive-blend upsample.
    sg_shader   bloom_down_shader{};
    sg_pipeline bloom_down_pipeline{};
    sg_shader   bloom_up_shader{};
    sg_pipeline bloom_up_pipeline{};

    // Final composite pipeline — writes to the swapchain.
    sg_shader   composite_shader{};
    sg_pipeline composite_pipeline{};

    // Tunables ------------------------------------------------------------
    // Defaults calibrated for "I can still see the scene" rather than
    // "whole screen is a nova." Crank bloom_strength to 0.9+ if you want
    // the more aggressive Freelancer-ads vibe.
    float bloom_threshold = 0.8f;   // energy above this glows (soft knee)
    float bloom_radius    = 1.0f;   // upsample tent spread, in source texels
    float bloom_strength  = 0.3f;   // how strongly the summed chain adds back
    float flare_strength  = 0.7f;   // overall flare intensity
    // Peak brightness fed to bloom (0 = uncapped). HDR keeps the sun core
    // at several x; without a cap its glow balloons past the painted size.
    float bloom_clamp     = kHdrScene ? 2.5f : 0.0f;

    // HDR tonemap (#715), applied in the composite. Only meaningful with
    // kHdrScene; with an LDR scene nothing exceeds 1.0 for it to recover.
    bool  tonemap      = kHdrScene;
    float exposure     = 1.0f;     // linear scale before the shoulder
    float tonemap_knee = 0.75f;    // below this, colours pass through as authored

    bool init();
    void destroy();

    // Run the bloom chain. Result lives in rt.bloom[0].
    void apply_bloom(const RenderTargets& rt) const;

    // Composite scene + bloom + flare to the swapchain. `sun_world_pos` and
    // `view_proj` are used to project the sun into NDC for the flare.
    //
    // `extra_pass_draw` (if set) is invoked INSIDE the swapchain pass just
    // before sg_end_pass, so callers can piggyback extra draws (HUD text,
    // Dear ImGui, whatever) without opening a second swapchain pass —
    // Metal aggressively flickers when you acquire two drawables per frame.
    using ExtraPassDraw = std::function<void()>;
    void composite_to_swapchain(const RenderTargets& rt,
                                HMM_Vec3 sun_world_pos,
                                const HMM_Mat4& view_proj,
                                HMM_Vec3 flare_tint,
                                int fb_w, int fb_h,
                                const ExtraPassDraw& extra_pass_draw = {}) const;
};
