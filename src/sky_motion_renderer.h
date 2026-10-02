#pragma once
// -----------------------------------------------------------------------------
// sky_motion_renderer.h — draws a system's shooting stars and comet (#701).
// Data, seeding, and the meteor schedule live in sky_motion.h; this is just
// the GPU side. No per-frame state: everything is a function of time.
// -----------------------------------------------------------------------------

#include "camera.h"
#include "sky_motion.h"
#include "sokol_gfx.h"

struct SkyMotionRenderer {
    bool init();
    void destroy();

    void set(const SkyCometDef& comet, const SkyMeteorsDef& meteors);

    // Additive, on the far plane, right after the sky props. `sun_dir` is
    // the unit vector from the camera to the sun; comet tails point away
    // from it.
    void draw(const Camera& cam, float aspect, float time_sec, HMM_Vec3 sun_dir) const;

private:
    sg_buffer   vbuf_{};
    sg_shader   meteor_shader_{};
    sg_shader   comet_shader_{};
    sg_pipeline meteor_pipeline_{};
    sg_pipeline comet_pipeline_{};
    SkyCometDef   comet_{};
    SkyMeteorsDef meteors_{};
};
