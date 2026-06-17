#include "bolt_art.h"

#include <cstdio>
#include <cmath>

// stb_image for PNG loading — same one sprite.cpp uses.
#define STB_IMAGE_IMPLEMENTATION_SUPPRESS
#include "stb_image.h"

namespace {
constexpr float k_bolt_fps = 15.0f;   // bolt spin/flicker cadence
}

const char* BoltArtSet::prefix_for(GunType type) {
    switch (type) {
        case GunType::Laser:            return "laser";
        case GunType::MassDriver:       return "mass";
        case GunType::MesonBlaster:     return "meson";
        case GunType::NeutronGun:       return "neutron";
        case GunType::ParticleCannon:   return "particle";
        case GunType::TachyonCannon:    return "tachyon";
        case GunType::IonicPulseCannon: return "ionic";
        case GunType::PlasmaGun:        return "plasma";
        case GunType::SteltekGun:       return "plasma";   // alias
        default:                        return nullptr;
    }
}

// Load a single PNG into a sokol image. Mirrors sprite.cpp's load_png_slot
// but standalone (no art cache dependency).
static BoltTexture load_png(const std::string& path) {
    BoltTexture out{};
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) {
        std::fprintf(stderr, "[bolt_art] failed to load %s\n", path.c_str());
        return out;
    }
    sg_image_desc desc{};
    desc.width        = w;
    desc.height       = h;
    desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    desc.data.mip_levels[0] = { px, (size_t)w * h * 4 };
    out.img = sg_make_image(&desc);

    sg_view_desc vd{};
    vd.texture.image = out.img;
    out.view  = sg_make_view(&vd);

    stbi_image_free(px);
    out.valid = (sg_query_image_state(out.img) == SG_RESOURCESTATE_VALID);
    out.w     = w;
    out.h     = h;
    return out;
}

void BoltArtSet::load(const std::string& dir) {
    std::printf("[bolt_art] loading from %s\n", dir.c_str());
    int total = 0;
    for (int gi = 0; gi < kGunTypeCount; ++gi) {
        GunType type = (GunType)gi;
        const char* prefix = prefix_for(type);
        if (!prefix) continue;

        // SteltekGun aliases PlasmaGun — don't double-load, just point at it.
        if (type == GunType::SteltekGun) {
            m_guns[gi] = m_guns[(int)GunType::PlasmaGun];
            continue;
        }

        BoltFrames& bf = m_guns[gi];
        bf.elongated = (type == GunType::Laser);

        // Scan for <prefix>_NN.png until missing.
        for (int fi = 0; ; ++fi) {
            char fname[64];
            std::snprintf(fname, sizeof(fname), "%s_%02d.png", prefix, fi);
            std::string path = dir + "/" + fname;
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) break;
            std::fclose(f);
            BoltTexture tex = load_png(path);
            if (!tex.valid) break;
            bf.frames.push_back(tex);
            ++total;
        }
        if (!bf.frames.empty())
            std::printf("[bolt_art]   %-16s %zu frames\n", prefix, bf.frames.size());
    }
    std::printf("[bolt_art] loaded %d bolt textures\n", total);
}

void BoltArtSet::destroy() {
    for (int gi = 0; gi < kGunTypeCount; ++gi) {
        // SteltekGun aliases PlasmaGun — skip to avoid double-free.
        if (gi == (int)GunType::SteltekGun) continue;
        for (BoltTexture& t : m_guns[gi].frames) {
            if (t.valid) sg_destroy_image(t.img);
            t.valid = false;
        }
        m_guns[gi].frames.clear();
    }
}

const BoltTexture* BoltArtSet::frame(GunType type, float t) const {
    const BoltFrames& bf = m_guns[(int)type];
    if (bf.frames.empty()) return nullptr;
    const int idx = (bf.frames.size() == 1)
                  ? 0
                  : (int)(t * k_bolt_fps) % (int)bf.frames.size();
    return &bf.frames[idx];
}

bool BoltArtSet::has_bolts(GunType type) const {
    return !m_guns[(int)type].frames.empty();
}

void BoltArtSet::flatten(std::vector<sg_view>& out, int offsets[kGunTypeCount]) const {
    out.clear();
    for (int gi = 0; gi < kGunTypeCount; ++gi) {
        offsets[gi] = (int)out.size();
        for (const BoltTexture& t : m_guns[gi].frames)
            out.push_back(t.view);
    }
}

int BoltArtSet::frame_index(GunType type, float t) const {
    const BoltFrames& bf = m_guns[(int)type];
    if (bf.frames.size() <= 1) return 0;
    return (int)(t * k_bolt_fps) % (int)bf.frames.size();
}

float BoltArtSet::aspect(GunType type) const {
    const BoltFrames& bf = m_guns[(int)type];
    if (bf.frames.empty()) return 1.0f;
    const BoltTexture& t = bf.frames[0];
    return (t.h > 0) ? (float)t.w / (float)t.h : 1.0f;
}
