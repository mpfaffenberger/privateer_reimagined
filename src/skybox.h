#pragma once
// -----------------------------------------------------------------------------
// skybox.h — owns a cubemap image, a view, a pipeline, and a unit cube.
//
// One instance = one skybox. Create at init, destroy at shutdown, draw
// exactly once per frame before any depth-writing geometry.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "camera.h"

#include <string>

struct Skybox {
    sg_image    cubemap{};
    sg_view     tex_view{};   // sokol's new bindings: images route through views
    sg_sampler  sampler{};
    sg_buffer   vbuf{};
    sg_buffer   ibuf{};
    sg_shader   shader{};
    sg_pipeline pipeline{};
    int         index_count = 0;

    // Procedural-generation state (B1): the cubemap is rendered on the fly
    // from `seed_` at `face_res_` on the first frame after init (generate()
    // issues offscreen passes, so it can't run during init/system-load).
    std::string seed_;
    int         face_res_  = 1024;
    bool        generated_ = false;

    // Set up draw-side resources (sampler, cube geometry, draw shader +
    // pipeline) and arm procedural generation for `seed`. No cubemap yet.
    bool init(const std::string& seed, int face_res = 1024);

    // Render the procedural cubemap for `seed_` (idempotent — first call
    // only). MUST be called inside frame_cb, BEFORE the scene pass begins.
    void generate();

    // True once the cubemap has been generated and is safe to draw.
    bool ready() const { return generated_; }

    // Render the sky. `aspect` = framebuffer width/height. No-op until ready.
    void draw(const Camera& cam, float aspect) const;

    void destroy();
};
