#include "title_scene.h"

#include "ship_sprite.h"        // ShipSpriteObject, ShipSpriteAtlas, load helpers
#include "sprite.h"             // SpriteArt, SpriteObject
#include "HandmadeMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace title_scene {

namespace {

// Per-category table. Order MUST match the Category enum so the int
// casts in init/tick map cleanly to the same rows.
struct CatInfo {
    const char*  label;        // human label for HUD/title overlay
    const char*  star;         // star preset name
    const char*  ships;        // comma-separated atlas stems to spawn
};
constexpr CatInfo k_cats[] = {
    /*ConfedMilitary*/{"Confed Navy",  "yellow", "broadsword,stiletto,paradigm"},
    /*Militia      */{"Militia",       "yellow", "gladius,talon"},
    /*BountyHunter */{"Bounty Hunter", "yellow", "centurion,orion,demon"},
    /*Kilrathi     */{"Kilrathi",      "red",    "kamekh,dralthi,gothri"},
    /*Merchant     */{"Merchant",      "yellow", "drayman,galaxy,tarsus"},
};
constexpr int k_cat_count = sizeof(k_cats) / sizeof(k_cats[0]);

// Module state.
struct State {
    Category cat = Category::ConfedMilitary;
    std::vector<std::unique_ptr<ShipSpriteAtlas>> atlas_storage;
    std::vector<std::string> atlas_keys;

    // Patrol ships. ShipSpriteObject carries the sprite pose; tick()
    // mutates position / orientation to make them drift past the
    // camera.
    std::deque<ShipSpriteObject> patrol;

    std::unordered_map<std::string, SpriteArt>* sprite_art = nullptr;

    // Anchor for ship positioning. Ships orbit AROUND this point.
    HMM_Vec3 anchor_pos { 0, 0, 0 };
    HMM_Vec3 anchor_fwd { 0, 0, 1 };   // +Z convention (sprite nose)
};
State g;

// Helpers ---------------------------------------------------------------

// Split "broadsword,stiletto,paradigm" into {"broadsword", ...}.
std::vector<std::string> split_ships(const std::string& csv) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : csv) {
        if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Load one atlas, returning nullptr on failure. Caller owns.
ShipSpriteAtlas* load_one(const std::string& ship_name,
                           std::unordered_map<std::string, SpriteArt>& art) {
    const std::string stem = resolve_ship_atlas_stem(
        "ships/" + ship_name + "/atlas_manifest");
    auto a = std::make_unique<ShipSpriteAtlas>();
    if (!load_ship_sprite_atlas(stem, *a, art)) {
        std::fprintf(stderr, "[title_scene] failed to load '%s'\n", stem.c_str());
        return nullptr;
    }
    return a.release();
}

// Side table for the orbit animation: {base_angle, speed, radius, height}
// per ship, indexed by position in g.patrol.
struct Motion {
    float base_angle;
    float speed;
    float radius;
    float height;
};
std::vector<Motion>& motion_table() {
    static std::vector<Motion> s;   // module-local; lifetime matches g
    return s;
}

} // namespace

// Lifecycle -------------------------------------------------------------

void init(Category cat,
          std::unordered_map<std::string, SpriteArt>& art_cache) {
    g.cat = cat;
    g.sprite_art = &art_cache;
    g.patrol.clear();
    g.atlas_storage.clear();
    g.atlas_keys.clear();

    const CatInfo& ci = k_cats[(int)g.cat];
    const auto ship_names = split_ships(ci.ships);

    // Pre-load every distinct atlas; dedup so a category whose ships
    // share a name (e.g. all use 'tarsus' in Merchant) only loads once.
    for (const std::string& name : ship_names) {
        bool dup = false;
        for (const auto& k : g.atlas_keys) if (k == name) { dup = true; break; }
        if (dup) continue;
        if (ShipSpriteAtlas* a = load_one(name, *g.sprite_art)) {
            g.atlas_storage.emplace_back(a);
            g.atlas_keys.push_back(name);
        }
    }
    if (g.atlas_storage.empty()) {
        std::fprintf(stderr, "[title_scene] no atlases loaded for category %d\n",
                     (int)g.cat);
        return;
    }

    // Spawn N ships in a horizontal orbit. Speeds + radii are staggered
    // so they don't all bunch up at the same angle.
    constexpr int k_n = 5;
    auto& mt = motion_table();
    mt.clear();
    for (int i = 0; i < k_n; ++i) {
        ShipSpriteObject s{};
        s.atlas          = g.atlas_storage[i % (int)g.atlas_storage.size()].get();
        s.world_size     = 80.0f;   // readable at title-screen distances
        s.tint           = {1, 1, 1, 1};
        s.lights_enabled = true;
        s.position       = HMM_V3(0, 0, 0);
        s.orientation    = HMM_QFromAxisAngle_RH(
            HMM_V3(0, 1, 0), (float)i * (2.0f * 3.14159265f / k_n));
        g.patrol.push_back(s);

        const float base_angle = (float)i * (2.0f * 3.14159265f / k_n);
        const float speed = 0.06f + 0.02f * (float)(i % 3);    // ~3-6 deg/sec (slow drift)
        const float radius = 320.0f + 80.0f * (float)(i % 4);  // 320..560 m
        const float height = (float)((i % 5) - 2) * 12.0f;     // -24..+24 m
        mt.push_back({base_angle, speed, radius, height});
    }
}

void tick(float dt) {
    // Advance orbit positions. Each ship has its own base angle + speed;
    // we add `speed * dt` to its angle so motion is fps-independent.
    auto& mt = motion_table();
    if (mt.size() != g.patrol.size()) return;   // safety: not initialized
    static thread_local float s_t = 0.0f;
    s_t += dt;                                  // accumulator shared
    for (size_t i = 0; i < g.patrol.size(); ++i) {
        ShipSpriteObject& s = g.patrol[i];
        const Motion m = mt[i];
        // speed is in rad/sec; s_t is accumulated seconds. No frame-rate
        // multiplier (the old * 60 made it ~2 rotations/sec — way too fast).
        const float angle = m.base_angle + m.speed * s_t;
        // Orbit around the anchor: a small forward offset places the
        // ships in front of the camera so the patrol is in view no
        // matter where the player is in the system.
        const float ox = std::cos(angle) * m.radius;
        const float oz = std::sin(angle) * m.radius;
        s.position = HMM_V3(g.anchor_pos.X + ox,
                             g.anchor_pos.Y + m.height,
                             g.anchor_pos.Z + oz);
        // Face inward toward the anchor (camera). Yaw so the sprite's
        // +Z nose points back toward the anchor.
        const float dx = ox;   // vector from anchor to ship
        const float dz = oz;
        const float yaw = std::atan2(dx, -dz);
        s.orientation = HMM_QFromAxisAngle_RH(HMM_V3(0, 1, 0), yaw);
    }
}

void set_anchor(HMM_Vec3 pos, HMM_Vec3 fwd) {
    g.anchor_pos = pos;
    g.anchor_fwd = fwd;
}

void shutdown() {
    g.patrol.clear();
    g.atlas_storage.clear();
    g.atlas_keys.clear();
    g.sprite_art = nullptr;
    motion_table().clear();
}

bool inited() { return g.sprite_art != nullptr; }

Category category() { return g.cat; }

const char* category_label(Category c) {
    return k_cats[(int)c % k_cat_count].label;
}

const char* star_preset(Category c) {
    return k_cats[(int)c % k_cat_count].star;
}

const char* skybox_seed(Category c) {
    return k_cats[(int)c % k_cat_count].star;
}

void append_to_frame_sprites(const Camera& cam,
                              std::vector<SpriteObject>& out_sprites) {
    if (g.patrol.empty()) return;
    append_ship_sprites_for_camera(g.patrol, cam, out_sprites);
}

} // namespace title_scene
