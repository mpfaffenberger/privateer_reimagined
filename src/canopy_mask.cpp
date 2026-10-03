// canopy_mask.cpp — PNG -> CanopyMask (see header, #732).
#include "canopy_mask.h"

#include "stb_image.h"

namespace cockpit_overlay {

bool load_canopy_mask(const std::string& path, CanopyMask& out) {
    int w = 0, h = 0, channels = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!px) return false;
    out = make_canopy_mask(px, w, h);
    stbi_image_free(px);
    return true;
}

} // namespace cockpit_overlay
