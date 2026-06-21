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

    // Anchor for ship positioning + camera basis. Ships fly along the
    // right axis across the view, offset by depth (fwd) + height (up).
    HMM_Vec3 anchor_pos   { 0, 0, 0 };
    HMM_Vec3 anchor_fwd   { 0, 0, 1 };
    HMM_Vec3 anchor_right { 1, 0, 0 };
    HMM_Vec3 anchor_up    { 0, 1, 0 };
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

// Side table for the linear flight animation. Each ship flies along the
// camera's right axis (left<->right across the view). `phase` runs 0..1
// and wraps; we map it to a position from -span to +span. `depth` is
// the forward distance from the anchor (so ships sit at different ranges)
// and `height` is the up offset. `dir` is +1 (left->right) or -1.
struct Motion {
    float phase;     // 0..1 position along the track
    float speed;     // tracks/sec (1.0 = one full crossing per second)
    float depth;     // forward offset from anchor (m)
    float height;    // up offset from anchor (m)
    float dir;       // +1 or -1 (travel direction)
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

    // Spawn N ships flying in straight lines across the view. Each gets a
    // different lane (depth + height), a staggered start phase so they
    // don't bunch, and alternating direction so some cross L->R and
    // others R->L.
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
        s.orientation    = HMM_Q(0, 0, 0, 1);
        g.patrol.push_back(s);

        // Phase staggered across the 5 ships so they're spread along the
        // track. speed ~0.04-0.07 tracks/sec -> a full crossing takes
        // ~14-25 seconds (slow, cinematic). depth varies the range;
        // height stacks them vertically; dir alternates.
        const float phase  = (float)i / (float)k_n;
        const float speed  = 0.04f + 0.012f * (float)(i % 3);
        const float depth  = -120.0f + 90.0f * (float)(i % 4);   // -120..+150 m
        const float height = (float)((i % 5) - 2) * 55.0f;       // -110..+110 m
        const float dir    = (i % 2 == 0) ? 1.0f : -1.0f;
        mt.push_back({phase, speed, depth, height, dir});
    }
}

// Build a quaternion that points the sprite's +Z nose along `dir`
// (world space), with `up` as the reference up. Used so each ship faces
// the direction it's flying.
HMM_Quat look_rotation(HMM_Vec3 dir, HMM_Vec3 up) {
    const HMM_Vec3 f = HMM_NormV3(dir);
    HMM_Vec3 r = HMM_Cross(up, f);
    const float rlen = HMM_LenV3(r);
    if (rlen < 1e-4f) r = HMM_V3(1, 0, 0);   // dir parallel to up: fallback
    else              r = HMM_DivV3F(r, rlen);
    const HMM_Vec3 u = HMM_Cross(f, r);
    // Column-major basis [r u f] -> rotation matrix -> quaternion.
    HMM_Mat4 m = HMM_M4D(1.0f);
    m.Columns[0] = HMM_V4(r.X, r.Y, r.Z, 0.0f);
    m.Columns[1] = HMM_V4(u.X, u.Y, u.Z, 0.0f);
    m.Columns[2] = HMM_V4(f.X, f.Y, f.Z, 0.0f);
    return HMM_M4ToQ_RH(m);
}

void tick(float dt) {
    // Advance linear flight along the camera's right axis. Each ship
    // moves at `speed` tracks/sec; phase wraps at 1.0 so ships loop
    // back to the start of the lane.
    auto& mt = motion_table();
    if (mt.size() != g.patrol.size()) return;   // safety: not initialized
    // span = half-width of the track in meters. The ships travel from
    // -span to +span along the camera right axis. 1100m comfortably
    // covers + overshoots a 600m-anchored view so ships enter / exit
    // off the screen edges.
    constexpr float k_span = 1100.0f;
    for (size_t i = 0; i < g.patrol.size(); ++i) {
        ShipSpriteObject& s = g.patrol[i];
        Motion& m = mt[i];
        m.phase += m.speed * dt;
        if (m.phase > 1.0f) m.phase -= 1.0f;
        if (m.phase < 0.0f) m.phase += 1.0f;
        // Map phase [0,1] -> track position [-span, +span], flipped by dir.
        const float along = (m.phase * 2.0f - 1.0f) * k_span * m.dir;
        const HMM_Vec3 pos =
            HMM_AddV3(g.anchor_pos,
                      HMM_AddV3(HMM_MulV3F(g.anchor_right, along),
                                HMM_AddV3(HMM_MulV3F(g.anchor_fwd, m.depth),
                                          HMM_MulV3F(g.anchor_up,  m.height))));
        s.position = pos;
        // Face the direction of travel: +right * dir.
        const HMM_Vec3 travel = HMM_MulV3F(g.anchor_right, m.dir);
        s.orientation = look_rotation(travel, g.anchor_up);
    }
}

void set_anchor(HMM_Vec3 pos, HMM_Vec3 fwd, HMM_Vec3 right, HMM_Vec3 up) {
    g.anchor_pos   = pos;
    g.anchor_fwd   = fwd;
    g.anchor_right = right;
    g.anchor_up    = up;
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
