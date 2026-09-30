// Animated base-room data harness (#515). Run from the repo root:
//   cmake --build build --target test_room_anim && build/test_room_anim
#include "json.h"
#include "room_anim_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& label) {
    std::printf("%-66s %s\n", label.c_str(), condition ? "PASS" : "FAIL");
    if (!condition) ++failures;
}

constexpr const char* kSheet = R"({
  "atlas": "car.png", "canvas": [1536, 1024], "fps": 10, "period_frames": 6,
  "offset_frames": 0,
  "frames": [
    {"slot": 1, "src": [0, 0, 40, 20], "dst": [100, 700, 40, 20]},
    {"slot": 2, "src": [41, 0, 30, 15], "dst": [120, 690, 60, 30]}
  ]
})";

void timeline() {
    room_anim::SpriteSheet s;
    std::string err;
    check(room_anim::parse_sprite_sheet(json::parse(kSheet), s, err), "sprite sheet parses");
    check(s.frames.size() == 2 && s.slot_frame.size() == 6, "two frames on a six-slot timeline");
    check(room_anim::frame_at(s, 0.05) == nullptr, "slot 0 is a blank gap");
    check(room_anim::frame_at(s, 0.15) == &s.frames[0], "t=0.15 s shows slot 1");
    check(room_anim::frame_at(s, 0.25) == &s.frames[1], "t=0.25 s shows slot 2");
    check(room_anim::frame_at(s, 0.35) == nullptr, "slots after the pass are blank");
    check(room_anim::slot_at(s, 0.65) == 0 && room_anim::frame_at(s, 0.75) == &s.frames[0],
          "timeline loops every period");
    check(room_anim::slot_at(s, -0.05) == 5, "negative time wraps into the period");
    s.offset = 5;
    check(room_anim::slot_at(s, 0.0) == 5 && room_anim::frame_at(s, 0.25) == &s.frames[0],
          "offset_frames shifts the phase");
    check(s.frames[1].src[2] * 2 == s.frames[1].dst[2], "half-res frame keeps its full dst");
}

void rejects_bad_manifests() {
    room_anim::SpriteSheet s;
    std::string err;
    check(!room_anim::parse_sprite_sheet(json::parse(R"({"atlas":"a.png","canvas":[10,10],
        "fps":10,"period_frames":2,"frames":[{"slot":2,"src":[0,0,1,1],"dst":[0,0,1,1]}]})"),
                                         s, err), "slot beyond period_frames is rejected");
    check(!room_anim::parse_sprite_sheet(json::parse(R"({"atlas":"a.png","canvas":[10,10],
        "fps":0,"period_frames":2,"frames":[]})"), s, err), "zero fps is rejected");
    check(!room_anim::parse_sprite_sheet(json::parse(R"({"atlas":"a.png","canvas":[10,10],
        "fps":10,"period_frames":2,"frames":[{"slot":0,"src":[0,0,0,1],"dst":[0,0,1,1]}]})"),
                                         s, err), "degenerate rect is rejected");
    check(!room_anim::parse_sprite_sheet(json::parse("[]"), s, err), "non-object is rejected");
}

void room_keys() {
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(json::parse(R"({"background":"bg.png",
        "sky":{"mask":"m.png","fill":"f.png","stars":[{"tile":"s.png","velocity":[-2,0.5]},
                                                       {"tile":"bad.png"}]},
        "layers":["a.json","b.json"]})"), def);
    check(def.has_sky && def.sky.stars.size() == 1, "sky parses; star without velocity dropped");
    check(def.sky.stars[0].velocity[0] == -2.0f && def.layers.size() == 2,
          "star velocity and layer list read");
    room_anim::parse_room_anim(json::parse(R"({"background":"bg.png","overlays":[]})"), def);
    check(def.empty(), "legacy room without sky/layers stays static");
    room_anim::parse_room_anim(json::parse(R"({"sky":{"fill":"f.png"}})"), def);
    check(!def.has_sky, "sky without a mask is dropped");
}

void per_plate() {
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(json::parse(R"({"anchors":"h/anchors.json",
        "sky":{"mask":"h/{plate}_mask.png","fill":"h/{plate}_fill.png",
                "stars":[{"tile":"s.png","velocity":[1,0]}]},
        "layers":["h/ship.json"]})"), def);
    const room_anim::RoomAnimDef t = room_anim::for_plate(def, "tarsus");
    check(t.sky.mask == "h/tarsus_mask.png" && t.sky.fill == "h/tarsus_fill.png",
          "{plate} is substituted in sky paths");
    check(t.plate == "tarsus" && t.anchors == "h/anchors.json" && t.layers[0] == "h/ship.json",
          "paths without {plate} are left alone");
    check(def.sky.mask == "h/{plate}_mask.png", "for_plate does not modify its input");

    const json::Value anchors = json::parse(R"({"tarsus":[662,312,173],"bad":[1,2,0]})");
    float a[3] = {0, 0, 0};
    check(room_anim::read_anchor(anchors, "tarsus", a) && a[0] == 662.0f && a[2] == 173.0f,
          "anchor read for a known plate");
    check(!room_anim::read_anchor(anchors, "galaxy", a), "unknown plate has no anchor");
    check(!room_anim::read_anchor(anchors, "bad", a), "zero-radius anchor is rejected");
    check(!room_anim::read_anchor(anchors, "", a), "unnamed plate has no anchor");
}

void anchored_layers() {
    room_anim::SpriteSheet s;
    std::string err;
    check(room_anim::parse_sprite_sheet(json::parse(R"({"atlas":"a.png","canvas":[1536,1024],
        "fps":24,"period_frames":1,"under":true,"anchor":[768,360,200],
        "frames":[{"slot":0,"src":[0,0,10,10],"dst":[768,360,20,10]}]})"), s, err),
          "anchored under-plate sheet parses");
    check(s.under && s.anchored && s.anchor[2] == 200.0f, "under + anchor read");
    const float to[3] = {668.0f, 300.0f, 100.0f};
    float r[4];
    room_anim::place(s, s.frames[0], to, r);
    check(r[0] == 668.0f && r[1] == 300.0f && r[2] == 10.0f && r[3] == 5.0f,
          "anchored frame maps onto the plate's anchor (move + scale)");
    room_anim::place(s, s.frames[0], nullptr, r);
    check(r[0] == 768.0f && r[2] == 20.0f, "no target anchor leaves dst unchanged");
    s.anchored = false;
    room_anim::place(s, s.frames[0], to, r);
    check(r[0] == 768.0f && r[2] == 20.0f, "unanchored sheet ignores the plate anchor");
    check(!room_anim::parse_sprite_sheet(json::parse(R"({"atlas":"a.png","canvas":[10,10],
        "fps":1,"period_frames":1,"anchor":[1,2],"frames":[]})"), s, err),
          "malformed anchor is rejected");
}

void star_scroll() {
    check(std::fabs(room_anim::scroll_uv(-2.0f, 1.0, 512.0f) - (-2.0f / 512.0f)) < 1e-6f,
          "stars move velocity/tile per second");
    const float late = room_anim::scroll_uv(-3.2f, 86400.0 * 3 + 0.5, 512.0f);
    check(std::fabs(late) < 1.0f, "scroll stays wrapped after three days of uptime");
    check(room_anim::scroll_uv(5.0f, 1.0, 0.0f) == 0.0f, "zero-size tile does not divide by 0");
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

void star_spin() {
    room_anim::StarLayerDef s;
    s.velocity[0] = -6.0f;
    s.velocity[1] = 1.5f;
    const float centre[2] = {700.0f, 300.0f};
    float uv[4][2];
    room_anim::star_uvs(s, 512.0f, 512.0f, 1536.0f, 1024.0f, centre, 2.5, uv);
    const float su = room_anim::scroll_uv(-6.0f, 2.5, 512.0f);
    const float sv = room_anim::scroll_uv(1.5f, 2.5, 512.0f);
    check(near(uv[0][0], -su) && near(uv[0][1], -sv) && near(uv[2][0], 3.0f - su) &&
              near(uv[2][1], 2.0f - sv) && near(uv[1][1], -sv) && near(uv[3][0], -su),
          "spin 0 is exactly the old axis-aligned drift");

    // 90 deg clockwise (y down): the screen's top-left samples the texel
    // that sat bottom-left of the centre.
    room_anim::StarLayerDef spin;
    spin.spin = 90.0f;
    const float c[2] = {100.0f, 100.0f};
    room_anim::star_uvs(spin, 1.0f, 1.0f, 200.0f, 200.0f, c, 1.0, uv);
    check(near(uv[0][0], 0.0f) && near(uv[0][1], 200.0f), "spin turns the field clockwise");
    const float origin[2] = {0.0f, 0.0f};
    spin.spin = 37.0f;
    room_anim::star_uvs(spin, 64.0f, 64.0f, 200.0f, 200.0f, origin, 3.3, uv);
    check(near(uv[0][0], 0.0f) && near(uv[0][1], 0.0f), "the spin centre stays put");
    spin.spin = 4.0f;
    room_anim::star_uvs(spin, 512.0f, 512.0f, 1536.0f, 1024.0f, centre, 86400.0 * 3 + 0.5, uv);
    const float d1 = std::hypot(uv[1][0] - uv[0][0], uv[1][1] - uv[0][1]) * 512.0f;
    check(near(d1 / 1536.0f, 1.0f), "spin stays rigid after three days of uptime");

    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(json::parse(R"({"sky":{"mask":"m","fill":"f",
        "stars":[{"tile":"s.png","velocity":[0,0],"spin":-2.5},{"tile":"t.png","velocity":[1,0]}]}})"),
                               def);
    check(def.sky.stars[0].spin == -2.5f && def.sky.stars[1].spin == 0.0f,
          "spin is read; absent spin means none");
}

// PNG width/height from the IHDR chunk (big-endian u32s at bytes 16 and 20).
bool png_size(const std::string& path, unsigned& w, unsigned& h) {
    std::ifstream f(path, std::ios::binary);
    unsigned char b[24] = {};
    if (!f.read(reinterpret_cast<char*>(b), sizeof b)) return false;
    auto be32 = [&](int i) { return (unsigned)b[i] << 24 | b[i + 1] << 16 | b[i + 2] << 8 | b[i + 3]; };
    w = be32(16);
    h = be32(20);
    return b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
}

// Atlases must load on the GPU (16384 max on D3D11/Metal); bake_layer.py
// widens them to stay under 8192.
constexpr unsigned kMaxAtlasSide = 8192;

// Every shipped plate-aware layer parses, has a GPU-loadable atlas, matches
// the plate, and never draws outside it.
void check_layers(const std::string& dir, const std::vector<std::string>& layers) {
    for (const std::string& layer : layers) {
        room_anim::SpriteSheet s;
        std::string err;
        const bool ok = room_anim::parse_sprite_sheet(json::parse_file(dir + layer), s, err);
        check(ok, "layer parses: " + layer + (ok ? "" : " (" + err + ")"));
        if (!ok) continue;
        const std::string atlas = dir + layer.substr(0, layer.find_last_of('/') + 1) + s.atlas;
        unsigned aw = 0, ah = 0;
        check(png_size(atlas, aw, ah), "  atlas exists");
        check(aw <= kMaxAtlasSide && ah <= kMaxAtlasSide,
              "  atlas " + std::to_string(aw) + "x" + std::to_string(ah) + " fits the GPU");
        check(s.canvas_w == 1536.0f && s.canvas_h == 1024.0f, "  canvas matches the plate");
        bool inside = !s.frames.empty();
        for (const room_anim::SpriteFrame& f : s.frames)
            inside = inside && f.dst[0] >= 0 && f.dst[1] >= 0 &&
                     f.dst[0] + f.dst[2] <= s.canvas_w && f.dst[1] + f.dst[3] <= s.canvas_h;
        check(inside, "  every frame lands inside the plate");
    }
}

// The `target` link in a links list has exactly this rect.
bool link_rect_is(const json::Value& links, const char* target, float x, float y, float w,
                  float h) {
    for (const json::Value& l : links.as_array())
        if (l["target"].string_or("") == target) {
            const json::Value& r = l["rect"];
            return r[size_t{0}].as_float() == x && r[size_t{1}].as_float() == y &&
                   r[size_t{2}].as_float() == w && r[size_t{3}].as_float() == h;
        }
    return false;
}

void shipped_newcon() {
    const std::string dir = "assets/concourse/newcon/";
    const json::Value root = json::parse_file(dir + "concourse.json");
    const json::Value& room = root["rooms"]["concourse"];
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(room, def);
    check(def.has_sky && def.sky.stars.size() >= 2, "New Con concourse has a sky with parallax");
    for (const std::string* p : {&def.sky.mask, &def.sky.fill})
        check(std::filesystem::exists(dir + *p), "sky asset exists: " + *p);
    for (const room_anim::StarLayerDef& s : def.sky.stars)
        check(std::filesystem::exists(dir + s.tile), "star tile exists: " + s.tile);
    check(def.layers.size() >= 4, "New Con concourse has vehicle + pedestrian layers");
    check_layers(dir, def.layers);
    // Acceptance: the clickable hotspots are untouched by the animation work.
    const json::Value* links = room.find("links");
    check(links && links->is_array() && links->as_array().size() == 7,
          "concourse keeps its seven link hotspots");
    check(link_rect_is(*links, "Bar", 0.575f, 0.49f, 0.10312f, 0.1f),
          "Bar hotspot rect unchanged");
}

// Every per-hull landing composite (landing_ships/<hull>.png) has its sky
// mask, fill, star tiles and an on-plate anchor.
void check_composite_skies(const std::string& dir, const room_anim::RoomAnimDef& def,
                           const std::string& what) {
    const json::Value anchors = json::parse_file(dir + def.anchors);
    int plates = 0, complete = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir + "landing_ships")) {
        if (entry.path().extension() != ".png") continue;
        ++plates;
        const std::string ship = entry.path().stem().string();
        const room_anim::RoomAnimDef p = room_anim::for_plate(def, ship);
        float a[3];
        bool tiles = true;
        for (const room_anim::StarLayerDef& s : p.sky.stars)
            tiles = tiles && std::filesystem::exists(dir + s.tile);
        if (tiles && std::filesystem::exists(dir + p.sky.mask) &&
            std::filesystem::exists(dir + p.sky.fill) && room_anim::read_anchor(anchors, ship, a) &&
            a[0] > 0 && a[0] < 1536 && a[1] > 0 && a[1] < 1024)
            ++complete;
        else
            check(false, "  " + what + " sky/stars/anchor for " + ship);
    }
    check(plates >= 18 && complete == plates,
          "every " + what + " composite has a mask, fill, star tiles + anchor");
}

// #561: the mining landing pad's starry sky and the Galaxy freighter overhead.
void shipped_mining_landing() {
    const std::string dir = "assets/concourse/mining/";
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(
        json::parse_file(dir + "concourse.json")["rooms"]["landing"]["composite"], def);
    check(def.has_sky && def.sky.stars.size() >= 2 && !def.anchors.empty(),
          "mining landing has a starry sky and rim anchors");
    check_composite_skies(dir, def, "mining landing");
    check(def.layers.size() == 1, "mining landing has its freighter layer");
    check_layers(dir, def.layers);
    for (const std::string& layer : def.layers) {
        room_anim::SpriteSheet s;
        std::string err;
        check(room_anim::parse_sprite_sheet(json::parse_file(dir + layer), s, err) && s.under &&
                  s.anchored,
              "  the freighter flies under the plate (the rim occludes it), anchored");
    }
    const json::Value links = json::parse_file(dir + "links.json")["landing"];
    check(links.as_array().size() == 2 &&
              link_rect_is(links, "Launch", 0.08844f, 0.53458f, 0.69063f, 0.39458f),
          "mining landing keeps its Launch + Concourse hotspots");
}

// #583: the Agricultural landing pad: aircraft in the dusk sky behind the
// tower and pylons, anchored to the big moon on every composite.
void shipped_agricultural_landing() {
    const std::string dir = "assets/concourse/agricultural/";
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(
        json::parse_file(dir + "concourse.json")["rooms"]["landing"]["composite"], def);
    check(def.has_sky && !def.anchors.empty(), "agricultural landing has a sky and moon anchors");
    check_composite_skies(dir, def, "agricultural landing");
    check(def.layers.size() == 2, "agricultural landing has its two aircraft layers");
    check_layers(dir, def.layers);
    for (const std::string& layer : def.layers) {
        room_anim::SpriteSheet s;
        std::string err;
        check(room_anim::parse_sprite_sheet(json::parse_file(dir + layer), s, err) && s.under &&
                  s.anchored,
              "  " + layer + " flies under the plate (tower and pylons occlude it), anchored");
    }
    const json::Value links = json::parse_file(dir + "links.json")["landing"];
    check(links.as_array().size() == 2 &&
              link_rect_is(links, "Launch", -0.105f, 0.50167f, 0.8975f, 0.54f),
          "agricultural landing keeps its Launch + Concourse hotspots");
}

// #558: the mining concourse's ore train.
void shipped_mining() {
    const std::string dir = "assets/concourse/mining/";
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(json::parse_file(dir + "concourse.json")["rooms"]["concourse"],
                               def);
    check(!def.has_sky && def.layers.size() == 1, "mining concourse has its ore-train layer");
    check_layers(dir, def.layers);
    // Acceptance: the hotspots (links.json overrides) are untouched.
    const json::Value links = json::parse_file(dir + "links.json")["concourse"];
    check(links.as_array().size() == 8 &&
              link_rect_is(links, "ShipDealer", 0.42054f, 0.4304f, 0.10286f, 0.15483f),
          "mining keeps its eight hotspots (ShipDealer rect unchanged)");
}

// #582: the Agricultural concourse: dusk clouds drifting through the windows,
// and a freighter and an aircar outside, drawn over the plate (clipped to the
// glass in the bake) because they fly in front of the painted farmland.
void shipped_agricultural() {
    const std::string dir = "assets/concourse/agricultural/";
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(json::parse_file(dir + "concourse.json")["rooms"]["concourse"],
                               def);
    check(def.has_sky && def.sky.stars.size() == 2, "agricultural concourse has two cloud tiles");
    for (const std::string* p : {&def.sky.mask, &def.sky.fill})
        check(std::filesystem::exists(dir + *p), "sky asset exists: " + *p);
    for (const room_anim::StarLayerDef& s : def.sky.stars)
        check(std::filesystem::exists(dir + s.tile) && s.velocity[1] == 0.0f && s.spin == 0.0f,
              "cloud tile exists and drifts sideways only: " + s.tile);
    check(def.layers.size() == 2, "agricultural concourse has its two aircraft layers");
    check_layers(dir, def.layers);
    for (const std::string& layer : def.layers) {
        room_anim::SpriteSheet s;
        std::string err;
        check(room_anim::parse_sprite_sheet(json::parse_file(dir + layer), s, err) && !s.under &&
                  !s.anchored,
              "  " + layer + " flies over the plate, unanchored");
    }
    // Acceptance: the hotspots (links.json overrides) are untouched.
    const json::Value links = json::parse_file(dir + "links.json")["concourse"];
    check(links.as_array().size() == 8 &&
              link_rect_is(links, "ShipDealer", 0.16169f, 0.60082f, 0.23133f, 0.26946f),
          "agricultural keeps its eight hotspots (ShipDealer rect unchanged)");
}

// #564, #566, #568, #570, #572, #571: the mining bar's 3D patrons (all six). Each has a one-frame
// clean-plate patch that paints the painted one out, and an idle loop. The
// patrons overlap (the left table), so every patch draws first, then every
// patron back to front: patch i belongs to patron i. bar_bg.png is untouched.
void shipped_mining_bar() {
    const std::string dir = "assets/concourse/mining/";
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(json::parse_file(dir + "concourse.json")["rooms"]["bar"], def);
    const size_t pairs = def.layers.size() / 2;
    check(!def.has_sky && pairs == 6 && def.layers.size() % 2 == 0,
          "mining bar has six patrons (patches first, then patrons)");
    check_layers(dir, def.layers);
    std::vector<int> phases;
    for (size_t i = 0; i < pairs; ++i) {
        room_anim::SpriteSheet patch, patron;
        std::string err;
        const std::string& name = def.layers[pairs + i];
        const bool ok =
            def.layers[i] == name.substr(0, name.size() - 5) + "_patch.json" &&
            room_anim::parse_sprite_sheet(json::parse_file(dir + def.layers[i]), patch, err) &&
            room_anim::parse_sprite_sheet(json::parse_file(dir + name), patron, err);
        check(ok && patch.frames.size() == 1 && patron.frames.size() > 1,
              "  " + name + ": its one-frame patch draws before every patron");
        // Slots without a frame draw nothing: on any longer loop the painted
        // patron would flicker back between patch frames.
        check(ok && patch.period == 1 && room_anim::slot_at(patch, 12.34f) == 0,
              "  " + name + ": the patch is always on screen (a one-slot loop)");
        check(ok && !patch.under && !patron.under, "  " + name + ": both over the plate");
        phases.push_back(patron.offset);
    }
    std::sort(phases.begin(), phases.end());
    check(phases.size() == pairs &&
              std::adjacent_find(phases.begin(), phases.end()) == phases.end(),
          "  patrons idle out of phase (no two share a loop phase)");
    const json::Value links = json::parse_file(dir + "links.json")["bar"];
    check(links.is_array() && links.as_array().empty(), "mining bar hotspots unchanged");
}

// #553: every hull's landing composite gets a sky + a mouth anchor, and the
// ship traffic layers are anchored and split under/over on shared timelines.
void shipped_newcon_hangar() {
    const std::string dir = "assets/concourse/newcon/";
    const json::Value root = json::parse_file(dir + "concourse.json");
    room_anim::RoomAnimDef def;
    room_anim::parse_room_anim(root["rooms"]["landing"]["composite"], def);
    check(def.has_sky && def.sky.stars.size() >= 2 && !def.anchors.empty(),
          "hangar composite has a starry sky and mouth anchors");
    check_composite_skies(dir, def, "hangar");
    bool spins = !def.sky.stars.empty();
    for (const room_anim::StarLayerDef& s : def.sky.stars) spins = spins && s.spin != 0.0f;
    check(spins, "hangar stars spin about the mouth");

    int under = 0, over = 0;
    room_anim::SpriteSheet first;
    for (const std::string& layer : def.layers) {
        room_anim::SpriteSheet s;
        std::string err;
        const bool ok = room_anim::parse_sprite_sheet(json::parse_file(dir + layer), s, err);
        check(ok && s.anchored && !s.frames.empty(), "hangar layer parses, anchored: " + layer);
        if (!ok) continue;
        check(std::filesystem::exists(dir + layer.substr(0, layer.find_last_of('/') + 1) + s.atlas),
              "  atlas exists");
        (s.under ? under : over)++;
        if (first.frames.empty()) first = s;
        check(s.period == first.period, "  shares the hangar loop length");
    }
    check(under >= 1 && over >= 1, "ships fly both beyond the mouth and through the tunnel");

    // Acceptance: the landing pad's hotspots (links.json overrides) are untouched.
    const json::Value links = json::parse_file(dir + "links.json")["landing"];
    check(links.as_array().size() == 2 &&
              link_rect_is(links, "Launch", -0.105f, 0.50167f, 0.8975f, 0.54f),
          "landing keeps its Launch + Concourse hotspots");
}

// #577: rooms shared by every base (the guilds) name their layers relative to
// the base dir, "../../shared_rooms/<room>/<layer>.json". The engine joins
// dir + path as-is and finds the atlas next to the manifest, as check_layers
// does, so a layer reached through ".." from another base (here the mining
// bar's, from New Con) must resolve like a local one, at the same depth too.
void cross_dir_layer_paths() {
    const std::string dir = "assets/concourse/newcon/";
    check_layers(dir, {"../mining/anim/bar/patron_orange.json"});
    check_layers(dir, {"../../concourse/mining/anim/bar/patron_orange_patch.json"});
}

// Rooms whose painting every base shares, animated once in
// assets/shared_rooms/<room>/ (#577).
const std::vector<std::string> kSharedRooms = {"mercguild", "merchguild"};

// A shared room (#577): every base shows the same painting and names exactly
// `layers` (each <name>.json under assets/shared_rooms/<room>/), in order;
// they resolve, and the first, the clean-plate patch, is always on screen.
void shipped_shared_room(const std::string& room, const std::vector<std::string>& names) {
    std::vector<std::string> want;
    for (const std::string& n : names)
        want.push_back("../../shared_rooms/" + room + "/" + n + ".json");
    int bases = 0;
    for (const auto& entry : std::filesystem::directory_iterator("assets/concourse")) {
        if (!entry.is_directory()) continue;
        const std::string dir = entry.path().generic_string() + "/";
        room_anim::RoomAnimDef def;
        room_anim::parse_room_anim(json::parse_file(dir + "concourse.json")["rooms"][room], def);
        check(def.layers == want,
              entry.path().filename().string() + " " + room + ": the shared layers, in order");
        if (bases++ == 0) check_layers(dir, def.layers);   // one resolves them; all match
    }
    check(bases == 9, "  all nine bases checked");
    room_anim::SpriteSheet patch;
    std::string err;
    check(room_anim::parse_sprite_sheet(
              json::parse_file("assets/shared_rooms/" + room + "/" + names[0] + ".json"),
              patch, err) &&
              patch.frames.size() == 1 && patch.period == 1,
          "  " + names[0] + " is one frame on a one-slot loop (always on screen)");
}

// #578: the Mercenaries' Guild woman: her clean-plate patch, then her idle.
void shipped_mercguild() {
    shipped_shared_room("mercguild", {"merc_woman_patch", "merc_woman"});
    std::string err;
    // Mike's review: she holds one pose; only small things move (her
    // fingers at her nails, her eyes, a little smile). So no frame's sprite
    // lands more than a few px from the rest frame's, and the distinct
    // images stay few (write_sheet packs each once).
    room_anim::SpriteSheet idle;
    const bool ok = room_anim::parse_sprite_sheet(
        json::parse_file("assets/shared_rooms/mercguild/merc_woman.json"), idle, err);
    std::vector<std::vector<float>> srcs;
    float drift = 0.0f;
    for (const room_anim::SpriteFrame& f : idle.frames) {
        const std::vector<float> src(f.src, f.src + 4);
        if (std::find(srcs.begin(), srcs.end(), src) == srcs.end()) srcs.push_back(src);
        for (int i = 0; i < 4; ++i)
            drift = std::max(drift, std::fabs(f.dst[i] - idle.frames[0].dst[i]));
    }
    check(ok && idle.frames.size() == static_cast<size_t>(idle.period) && drift <= 4.0f,
          "  she holds one pose: every slot drawn, sprites within 4 px (" +
              std::to_string(static_cast<int>(drift)) + " px)");
    check(ok && srcs.size() <= 64,
          "  her small moves are few images (" + std::to_string(srcs.size()) + ")");
}

// #579: the Merchants' Guild man, smoking: his patch, his loop (a drag on
// his cigar), then the smoke he exhales, drawn over him. He's on screen in
// every slot; the smoke comes and goes (slots without a frame draw nothing).
void shipped_merchguild() {
    shipped_shared_room("merchguild", {"merch_man_patch", "merch_man", "merch_man_smoke"});
    room_anim::SpriteSheet man, smoke;
    std::string err;
    const std::string dir = "assets/shared_rooms/merchguild/";
    const bool ok = room_anim::parse_sprite_sheet(json::parse_file(dir + "merch_man.json"), man,
                                                  err) &&
                    room_anim::parse_sprite_sheet(json::parse_file(dir + "merch_man_smoke.json"),
                                                  smoke, err);
    check(ok && man.frames.size() == static_cast<size_t>(man.period),
          "  he's drawn in every slot of his loop");
    check(ok && !smoke.frames.empty() && smoke.frames.size() < static_cast<size_t>(smoke.period) &&
              smoke.period == man.period,
          "  his smoke comes and goes on his loop (" + std::to_string(smoke.frames.size()) + "/" +
              std::to_string(smoke.period) + " slots)");
}

void other_archetypes_static() {
    int animated = 0, rooms = 0;
    for (const auto& entry : std::filesystem::directory_iterator("assets/concourse")) {
        const std::string base = entry.path().filename().string();
        if (!entry.is_directory() || base == "newcon" || base == "mining" ||
            base == "agricultural")
            continue;
        const json::Value root = json::parse_file((entry.path() / "concourse.json").string());
        const json::Value* rs = root.find("rooms");
        if (!rs || !rs->is_object()) continue;
        for (const auto& [name, room] : rs->as_object()) {
            if (std::find(kSharedRooms.begin(), kSharedRooms.end(), name) != kSharedRooms.end())
                continue;                   // every base's, checked above
            room_anim::RoomAnimDef def;
            room_anim::parse_room_anim(room, def);
            ++rooms;
            if (!def.empty()) ++animated;
        }
    }
    check(rooms > 0 && animated == 0, "no other archetype's own room gains animation");
}

}  // namespace

int main() {
    timeline();
    rejects_bad_manifests();
    room_keys();
    per_plate();
    anchored_layers();
    star_scroll();
    star_spin();
    shipped_newcon();
    shipped_mining();
    shipped_agricultural();
    shipped_mining_landing();
    shipped_agricultural_landing();
    shipped_mining_bar();
    shipped_newcon_hangar();
    cross_dir_layer_paths();
    shipped_mercguild();
    shipped_merchguild();
    other_archetypes_static();
    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
