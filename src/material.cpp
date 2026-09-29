// -----------------------------------------------------------------------------
// material.cpp — PNG → TextureSlot loader, Material teardown.
// -----------------------------------------------------------------------------

#include "material.h"

#include "stb_image.h"

#include <cstdio>

bool make_texture_rgba8(const uint8_t* rgba, int w, int h, TextureSlot& slot) {
    sg_image_desc id{};
    id.width  = w;
    id.height = h;
    id.pixel_format = SG_PIXELFORMAT_RGBA8;
    id.data.mip_levels[0] = { rgba, (size_t)w * (size_t)h * 4 };
    slot.image = sg_make_image(&id);

    sg_view_desc vd{};
    vd.texture.image = slot.image;
    slot.view = sg_make_view(&vd);

    slot.valid = (sg_query_image_state(slot.image) == SG_RESOURCESTATE_VALID);
    return slot.valid;
}

bool load_texture_png(const std::string& path, TextureSlot& slot) {
    int w = 0, h = 0, c = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!px) return false;
    const bool ok = make_texture_rgba8(px, w, h, slot);
    stbi_image_free(px);
    if (!ok) {
        std::fprintf(stderr, "[material] GPU image creation failed for '%s'\n",
                     path.c_str());
    }
    return ok;
}

void Material::destroy() {
    auto free_slot = [](TextureSlot& s) {
        if (s.valid) {
            sg_destroy_view(s.view);
            sg_destroy_image(s.image);
            s.valid = false;
        }
    };
    free_slot(diffuse);
    free_slot(spec);
    free_slot(glow);
    free_slot(normal);
}
