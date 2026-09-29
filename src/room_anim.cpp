// -----------------------------------------------------------------------------
// room_anim.cpp — see room_anim.h.
//
// Samplers: full-resolution sprites use sokol-imgui's default NEAREST sampler,
// the same one the plate is drawn with, so each sprite texel lands exactly on
// the plate texel it was encoded against (bake_layer.py) at any window size.
// Half-resolution and anchored (rescaled) sprites and the stretched sky fill
// use LINEAR, and the star tiles use LINEAR + REPEAT so they drift smoothly
// by sub-pixel amounts.
// -----------------------------------------------------------------------------

#include "room_anim.h"

#include "imgui.h"
#include "sokol_app.h"     // must precede sokol_imgui.h
#include "sokol_imgui.h"
#include "stb_image.h"

#include <cstdio>

namespace room_anim {

namespace {

sg_sampler g_linear_clamp{};
sg_sampler g_linear_repeat{};

sg_sampler linear_sampler(sg_sampler& cached, sg_wrap wrap, const char* label) {
    if (!cached.id) {
        sg_sampler_desc desc{};
        desc.min_filter = SG_FILTER_LINEAR;
        desc.mag_filter = SG_FILTER_LINEAR;
        desc.wrap_u = wrap;
        desc.wrap_v = wrap;
        desc.label = label;
        cached = sg_make_sampler(&desc);
    }
    return cached;
}

ImTextureID linear_clamp(const TextureSlot& t) {
    return simgui_imtextureid_with_sampler(
        t.view, linear_sampler(g_linear_clamp, SG_WRAP_CLAMP_TO_EDGE, "room-anim-linear"));
}

ImTextureID linear_repeat(const TextureSlot& t) {
    return simgui_imtextureid_with_sampler(
        t.view, linear_sampler(g_linear_repeat, SG_WRAP_REPEAT, "room-anim-repeat"));
}

void free_slot(TextureSlot& t) {
    if (t.valid) {
        sg_destroy_view(t.view);
        sg_destroy_image(t.image);
    }
    t = TextureSlot{};
}

void image_size(const TextureSlot& t, float& w, float& h) {
    w = (float)sg_query_image_width(t.image);
    h = (float)sg_query_image_height(t.image);
}

std::string parent_dir(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// The plate with the sky mask as alpha (mask 255 = sky = transparent).
bool load_masked_plate(const std::string& plate, const std::string& mask, TextureSlot& out) {
    int pw = 0, ph = 0, pc = 0, mw = 0, mh = 0, mc = 0;
    uint8_t* px = stbi_load(plate.c_str(), &pw, &ph, &pc, 4);
    uint8_t* mk = stbi_load(mask.c_str(), &mw, &mh, &mc, 1);
    bool ok = false;
    if (px && mk && mw == pw && mh == ph) {
        const size_t n = (size_t)pw * (size_t)ph;
        for (size_t i = 0; i < n; ++i) px[i * 4 + 3] = (uint8_t)(255 - mk[i]);
        ok = make_texture_rgba8(px, pw, ph, out);
    } else if (px) {
        std::fprintf(stderr, "[room_anim] sky mask '%s' missing or not %dx%d\n",
                     mask.c_str(), pw, ph);
    }
    stbi_image_free(mk);
    stbi_image_free(px);
    return ok;
}

bool load_sky(const std::string& dir, const std::string& plate, const SkyDef& def,
              TextureSlot& background, RoomAnim& out) {
    TextureSlot fill;
    if (!load_texture_png(dir + def.fill, fill)) {
        std::fprintf(stderr, "[room_anim] sky fill '%s' missing\n", def.fill.c_str());
        return false;
    }
    if (!load_masked_plate(plate, dir + def.mask, background)) {
        free_slot(fill);
        return false;
    }
    out.sky_fill = fill;
    for (const StarLayerDef& s : def.stars) {
        StarLayer layer;
        if (!load_texture_png(dir + s.tile, layer.tile)) {
            std::fprintf(stderr, "[room_anim] star tile '%s' missing\n", s.tile.c_str());
            continue;
        }
        image_size(layer.tile, layer.tile_w, layer.tile_h);
        layer.velocity[0] = s.velocity[0];
        layer.velocity[1] = s.velocity[1];
        out.stars.push_back(layer);
    }
    return true;
}

void load_layer(const std::string& manifest_path, RoomAnim& out) {
    SpriteLayer layer;
    std::string err;
    if (!parse_sprite_sheet(json::parse_file(manifest_path), layer.sheet, err)) {
        std::fprintf(stderr, "[room_anim] layer '%s': %s\n", manifest_path.c_str(), err.c_str());
        return;
    }
    if (layer.sheet.anchored && !out.has_anchor) {
        std::fprintf(stderr, "[room_anim] layer '%s' is anchored but the plate has no anchor\n",
                     manifest_path.c_str());
        return;
    }
    const std::string atlas = parent_dir(manifest_path) + layer.sheet.atlas;
    if (!load_texture_png(atlas, layer.atlas)) {
        std::fprintf(stderr, "[room_anim] layer atlas '%s' missing\n", atlas.c_str());
        return;
    }
    image_size(layer.atlas, layer.atlas_w, layer.atlas_h);
    out.layers.push_back(std::move(layer));
}

void load_anchor(const std::string& path, const std::string& plate, RoomAnim& out) {
    out.has_anchor = read_anchor(json::parse_file(path), plate, out.anchor);
    if (!out.has_anchor)
        std::fprintf(stderr, "[room_anim] no anchor for '%s' in '%s'\n", plate.c_str(),
                     path.c_str());
}

// Sprite layers on one side of the plate (`under` or not), in JSON order.
void draw_layers(ImDrawList* dl, float w, float h, const RoomAnim& anim, double seconds,
                 bool under) {
    for (const SpriteLayer& layer : anim.layers) {
        if (layer.sheet.under != under) continue;
        const SpriteFrame* f = frame_at(layer.sheet, seconds);
        if (!f) continue;
        float dst[4];
        place(layer.sheet, *f, anim.has_anchor ? anim.anchor : nullptr, dst);
        const float sx = w / layer.sheet.canvas_w, sy = h / layer.sheet.canvas_h;
        const ImVec2 p0(dst[0] * sx, dst[1] * sy);
        const ImVec2 p1((dst[0] + dst[2]) * sx, (dst[1] + dst[3]) * sy);
        const ImVec2 uv0(f->src[0] / layer.atlas_w, f->src[1] / layer.atlas_h);
        const ImVec2 uv1((f->src[0] + f->src[2]) / layer.atlas_w,
                         (f->src[1] + f->src[3]) / layer.atlas_h);
        const bool texel_exact = !layer.sheet.anchored && f->src[2] == dst[2] &&
                                 f->src[3] == dst[3];
        dl->AddImage(texel_exact ? simgui_imtextureid(layer.atlas.view)
                                 : linear_clamp(layer.atlas),
                     p0, p1, uv0, uv1);
    }
}

}  // namespace

bool load(const std::string& dir, const json::Value& room, TextureSlot& background,
          RoomAnim& out) {
    RoomAnimDef def;
    parse_room_anim(room, def);
    const json::Value* bg = room.find("background");
    return load(dir, def, dir + (bg ? bg->string_or("") : std::string()), background, out);
}

bool load(const std::string& dir, const RoomAnimDef& def, const std::string& plate_path,
          TextureSlot& background, RoomAnim& out) {
    out = RoomAnim{};
    const bool sky = def.has_sky && load_sky(dir, plate_path, def.sky, background, out);
    if (!sky && !load_texture_png(plate_path, background)) return false;
    image_size(background, out.canvas_w, out.canvas_h);
    if (!def.anchors.empty()) load_anchor(dir + def.anchors, def.plate, out);
    for (const std::string& manifest : def.layers) load_layer(dir + manifest, out);
    return true;
}

void draw_under(ImDrawList* dl, float w, float h, const RoomAnim& anim, double seconds) {
    if (anim.sky_fill.valid) {
        dl->AddImage(linear_clamp(anim.sky_fill), ImVec2(0, 0), ImVec2(w, h));
        for (const StarLayer& s : anim.stars) {
            // Tiles repeat across the whole plate; the masked plate on top
            // only lets them show through the windows.
            const ImVec2 uv0(-scroll_uv(s.velocity[0], seconds, s.tile_w),
                             -scroll_uv(s.velocity[1], seconds, s.tile_h));
            const ImVec2 uv1(uv0.x + anim.canvas_w / s.tile_w,
                             uv0.y + anim.canvas_h / s.tile_h);
            dl->AddImage(linear_repeat(s.tile), ImVec2(0, 0), ImVec2(w, h), uv0, uv1);
        }
    }
    draw_layers(dl, w, h, anim, seconds, /*under=*/true);
}

void draw_over(ImDrawList* dl, float w, float h, const RoomAnim& anim, double seconds) {
    draw_layers(dl, w, h, anim, seconds, /*under=*/false);
}

void release(RoomAnim& anim) {
    free_slot(anim.sky_fill);
    for (StarLayer& s : anim.stars) free_slot(s.tile);
    for (SpriteLayer& l : anim.layers) free_slot(l.atlas);
    anim = RoomAnim{};
}

}  // namespace room_anim
