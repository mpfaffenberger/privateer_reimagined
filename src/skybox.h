#pragma once
// -----------------------------------------------------------------------------
// skybox.h — owns a cubemap image, a view, a pipeline, and a unit cube.
//
// One instance = one skybox. Create at init, destroy at shutdown, draw
// exactly once per frame before any depth-writing geometry.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "camera.h"

#include "HandmadeMath.h"
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
    int         face_res_  = 4096;
    bool        generated_ = false;
    float       sun_warmth_ = 0.0f;    // nebula palette bias [-1, +1]
    HMM_Vec3    target_a_  = {1.0f, 0.55f, 0.20f};   // warm target anchor
    HMM_Vec3    target_b_  = {0.35f, 0.90f, 0.50f};  // cool/alt target anchor

    // Set up draw-side resources (sampler, cube geometry, draw shader +
    // pipeline) and arm procedural generation for `seed`. No cubemap yet.
    // face_res 4096 = crisp; cube is 6 x face_res^2 x RGBA8 (~402 MB at
    // 4096), generated once per system load on the GPU. `sun_warmth` in
    // [-1, 1] biases nebula colours: +ve = warm (orange/red/rose),
    // -ve = cool (blue/teal/green). 0 = pure random. `target_a`/`target_b`
    // are the palette anchors; pass the family-appropriate pair.
    bool init(const std::string& seed, int face_res = 4096,
              float sun_warmth = 0.0f,
              HMM_Vec3 target_a = HMM_V3(1.0f, 0.55f, 0.20f),
              HMM_Vec3 target_b = HMM_V3(0.35f, 0.90f, 0.50f));

    // Render the procedural cubemap for `seed_` (idempotent — first call
    // only). MUST be called inside frame_cb, BEFORE the scene pass begins.
    void generate();

    // True once the cubemap has been generated and is safe to draw.
    bool ready() const { return generated_; }

    // Render the sky. `aspect` = framebuffer width/height. No-op until ready.
    void draw(const Camera& cam, float aspect) const;

    void destroy();
};
