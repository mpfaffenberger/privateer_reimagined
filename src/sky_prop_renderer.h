#pragma once
// -----------------------------------------------------------------------------
// sky_prop_renderer.h — draws a system's far-field galaxies / anomalies
// (#693). Data and seeding live in sky_props.h; this is just the GPU side.
//
// Lifecycle: init() once, set_props() on every system load (textures are
// cached by sprite stem, so revisiting a system costs nothing), draw() once
// per frame right after the skybox, destroy() at shutdown.
// -----------------------------------------------------------------------------

#include "camera.h"
#include "sky_props.h"
#include "sokol_gfx.h"

#include <string>
#include <unordered_map>
#include <vector>

struct SkyPropRenderer {
    bool init();
    void destroy();

    // Copy the props to draw and make sure their textures are resident.
    // Props whose PNG fails to load are dropped (and logged).
    void set_props(const std::vector<SkyPropDef>& props);

    // Additive, no depth write, on the far plane. `time_sec` drives spin
    // and pulse. No-op when there are no props.
    void draw(const Camera& cam, float aspect, float time_sec) const;

private:
    struct Texture { sg_image image{}; sg_view view{}; };
    bool load_texture(const std::string& sprite);

    sg_shader   shader_{};
    sg_pipeline pipeline_{};
    sg_sampler  sampler_{};
    sg_buffer   vbuf_{};
    std::unordered_map<std::string, Texture> textures_;
    std::vector<SkyPropDef> props_;
};
