#include "rendertargets.h"
#include "render_config.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

sg_image make_color_image(int w, int h, sg_pixel_format fmt) {
    sg_image_desc d{};
    d.usage.color_attachment = true;
    d.width        = w;
    d.height       = h;
    d.pixel_format = fmt;
    d.sample_count = kSceneSampleCount;
    return sg_make_image(&d);
}

sg_image make_depth_image(int w, int h) {
    sg_image_desc d{};
    d.usage.depth_stencil_attachment = true;
    d.width        = w;
    d.height       = h;
    d.pixel_format = kSceneDepthFormat;
    d.sample_count = kSceneSampleCount;
    return sg_make_image(&d);
}

sg_view color_attachment_view(sg_image img) {
    sg_view_desc d{};
    d.color_attachment.image = img;
    return sg_make_view(&d);
}

sg_view depth_attachment_view(sg_image img) {
    sg_view_desc d{};
    d.depth_stencil_attachment.image = img;
    return sg_make_view(&d);
}

sg_view texture_view(sg_image img) {
    sg_view_desc d{};
    d.texture.image = img;
    return sg_make_view(&d);
}

} // namespace

bool RenderTargets::init(int width, int height) {
    w = width;  h = height;

    // --- scene -----------------------------------------------------------
    scene_color = make_color_image(w, h, kSceneColorFormat);
    scene_depth = make_depth_image(w, h);
    scene_color_att = color_attachment_view(scene_color);
    scene_depth_att = depth_attachment_view(scene_depth);
    scene_color_tex = texture_view(scene_color);

    // --- bloom mip chain (#724) ------------------------------------------
    // Half res, then halving per level (clamped to 1 px). Bloom is low
    // frequency by design, so even the half-res top level is plenty.
    std::vector<sg_view> views = { scene_color_att, scene_depth_att, scene_color_tex };
    int mw = w, mh = h;
    for (BloomMip& m : bloom) {
        mw = std::max(1, mw / 2);
        mh = std::max(1, mh / 2);
        m.w     = mw;
        m.h     = mh;
        m.color = make_color_image(mw, mh, kBloomColorFormat);
        m.att   = color_attachment_view(m.color);
        m.tex   = texture_view(m.color);
        views.push_back(m.att);
        views.push_back(m.tex);
    }

    // --- sampler ---------------------------------------------------------
    sg_sampler_desc ss{};
    ss.min_filter = SG_FILTER_LINEAR;
    ss.mag_filter = SG_FILTER_LINEAR;
    ss.wrap_u     = SG_WRAP_CLAMP_TO_EDGE;
    ss.wrap_v     = SG_WRAP_CLAMP_TO_EDGE;
    linear_clamp  = sg_make_sampler(&ss);

    for (sg_view v : views) {
        if (sg_query_view_state(v) != SG_RESOURCESTATE_VALID) {
            std::fprintf(stderr, "[rendertargets] view creation failed\n");
            return false;
        }
    }
    return true;
}

void RenderTargets::destroy() {
    sg_destroy_sampler(linear_clamp);
    for (BloomMip& m : bloom) {
        sg_destroy_view(m.tex);
        sg_destroy_view(m.att);
        sg_destroy_image(m.color);
    }
    sg_destroy_view(scene_color_tex);
    sg_destroy_view(scene_depth_att);
    sg_destroy_view(scene_color_att);
    sg_destroy_image(scene_depth);
    sg_destroy_image(scene_color);
}
