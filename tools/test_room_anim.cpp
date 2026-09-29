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
    star_scroll();
    shipped_newcon();
    other_archetypes_static();
    std::printf("\n%s\n", failures == 0 ? "ALL PASS" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
