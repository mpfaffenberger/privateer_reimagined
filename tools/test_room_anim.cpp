// Animated base-room data harness (#515). Run from the repo root:
//   cmake --build build --target test_room_anim && build/test_room_anim
#include "json.h"
#include "room_anim_data.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

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
    for (const std::string& layer : def.layers) {
        room_anim::SpriteSheet s;
        std::string err;
        const bool ok = room_anim::parse_sprite_sheet(json::parse_file(dir + layer), s, err);
        check(ok, "layer parses: " + layer + (ok ? "" : " (" + err + ")"));
        if (!ok) continue;
        const std::string atlas = dir + layer.substr(0, layer.find_last_of('/') + 1) + s.atlas;
        check(std::filesystem::exists(atlas), "  atlas exists");
        check(s.canvas_w == 1536.0f && s.canvas_h == 1024.0f, "  canvas matches the plate");
        bool inside = !s.frames.empty();
        for (const room_anim::SpriteFrame& f : s.frames)
            inside = inside && f.dst[0] >= 0 && f.dst[1] >= 0 &&
                     f.dst[0] + f.dst[2] <= s.canvas_w && f.dst[1] + f.dst[3] <= s.canvas_h;
        check(inside, "  every frame lands inside the plate");
    }
    // Acceptance: the clickable hotspots are untouched by the animation work.
    const json::Value* links = room.find("links");
    check(links && links->is_array() && links->as_array().size() == 7,
          "concourse keeps its seven link hotspots");
    bool bar = false;
    for (const json::Value& l : links->as_array())
        if (l["target"].string_or("") == "Bar") {
            const json::Value& r = l["rect"];
            bar = r[size_t{0}].as_float() == 0.575f && r[size_t{1}].as_float() == 0.49f &&
                  r[size_t{2}].as_float() == 0.10312f && r[size_t{3}].as_float() == 0.1f;
        }
    check(bar, "Bar hotspot rect unchanged");
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
    const json::Value anchors = json::parse_file(dir + def.anchors);
    int plates = 0, complete = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir + "landing_ships")) {
        if (entry.path().extension() != ".png") continue;
        ++plates;
        const std::string ship = entry.path().stem().string();
        const room_anim::RoomAnimDef p = room_anim::for_plate(def, ship);
        float a[3];
        if (std::filesystem::exists(dir + p.sky.mask) && std::filesystem::exists(dir + p.sky.fill) &&
            room_anim::read_anchor(anchors, ship, a) && a[0] > 0 && a[0] < 1536 && a[1] > 0 &&
            a[1] < 1024)
            ++complete;
        else
            check(false, "  hangar sky/anchor for " + ship);
    }
    check(plates >= 18 && complete == plates, "every landing composite has a mask, fill + anchor");
    for (const room_anim::StarLayerDef& s : def.sky.stars)
        check(std::filesystem::exists(dir + s.tile), "hangar star tile exists: " + s.tile);

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
    bool launch = false;
    for (const json::Value& l : links.as_array())
        if (l["target"].string_or("") == "Launch") {
            const json::Value& r = l["rect"];
            launch = r[size_t{0}].as_float() == -0.105f && r[size_t{1}].as_float() == 0.50167f &&
                     r[size_t{2}].as_float() == 0.8975f && r[size_t{3}].as_float() == 0.54f;
        }
    check(links.as_array().size() == 2 && launch, "landing keeps its Launch + Concourse hotspots");
}

void other_archetypes_static() {
    int animated = 0, rooms = 0;
    for (const auto& entry : std::filesystem::directory_iterator("assets/concourse")) {
        if (!entry.is_directory() || entry.path().filename() == "newcon") continue;
        const json::Value root = json::parse_file((entry.path() / "concourse.json").string());
        const json::Value* rs = root.find("rooms");
        if (!rs || !rs->is_object()) continue;
        for (const auto& [name, room] : rs->as_object()) {
            room_anim::RoomAnimDef def;
            room_anim::parse_room_anim(room, def);
            ++rooms;
            if (!def.empty()) ++animated;
        }
    }
    check(rooms > 0 && animated == 0, "no other archetype's room gains animation");
}

}  // namespace

int main() {
    timeline();
    rejects_bad_manifests();
    room_keys();
    per_plate();
    anchored_layers();
    star_scroll();
    shipped_newcon();
    shipped_newcon_hangar();
    other_archetypes_static();
    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
