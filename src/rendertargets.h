#pragma once
// -----------------------------------------------------------------------------
// rendertargets.h — offscreen attachments used by the post-process pipeline.
//
// sokol's newer API treats `sg_view` as the universal indirection layer:
// you create views onto images, and those views are what you bind as
// color/depth attachments in a pass, OR as texture samples in a shader.
// For each image we therefore keep two views: one for writing (as an
// attachment) and one for reading (as a texture).
//
// Resizing is out of scope for now — window is fixed at startup.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "render_config.h"

struct RenderTargets {
    int w = 0, h = 0;        // scene resolution

    // ---- scene target ---------------------------------------------------
    sg_image scene_color{};
    sg_image scene_depth{};
    sg_view  scene_color_att{};   // write (attachment)
    sg_view  scene_depth_att{};
    sg_view  scene_color_tex{};   // read (texture for post)

    // ---- bloom mip chain (#724, no depth) -------------------------------
    // Level 0 is half res; each next level halves again. After
    // PostProcess::apply_bloom, level 0 holds the finished bloom.
    struct BloomMip {
        int      w = 0, h = 0;
        sg_image color{};
        sg_view  att{}, tex{};
    };
    BloomMip bloom[kBloomLevels];

    // ---- shared sampler --------------------------------------------------
    sg_sampler linear_clamp{};

    bool init(int width, int height);
    void destroy();
};
