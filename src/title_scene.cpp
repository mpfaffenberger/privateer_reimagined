#include "title_scene.h"

#include "ship_sprite.h"        // ShipSpriteObject, ShipSpriteAtlas, load helpers
#include "sprite.h"             // SpriteArt, SpriteObject
#include "HandmadeMath.h"

#include <algorithm>
#include <chrono>
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

// Full roster for the 'random ships' title mode. Every flyable hull,
// regardless of faction — the title just wants varied silhouettes
// Every flyable hull in assets/ships/ is eligible for the title screen,
// modulo Mike's curated list (kamekh / orion / gladius removed because
// they read oddly at title scale; derelict / drone / scout omitted
// because they're not really 'patrol' ships and read oddly at title
// scale).
constexpr const char* k_all_ships[] = {
    "broadsword", "stiletto", "paradigm",         "talon",     "demon",
    "dralthi",    "gothri",   "drayman",          "galaxy",    "tarsus",
    "strakha",    "centurion",
};
constexpr int k_all_ships_count = sizeof(k_all_ships) / sizeof(k_all_ships[0]);

// Module state.
struct State {
    Category cat = Category::ConfedMilitary;
    Variant  variant = Variant::PatrolFlyby;
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

    // ChaseCam state. The hero ship cruises in a straight line along
    // world +Z; the camera orbits it slowly. chase_t advances 0..1 over
    // one pass, then we swap hull. chase_idx = which atlas is fitted.
    float    chase_t        = 0.0f;
    int      chase_idx      = 0;
    HMM_Vec3 chase_ship_pos { 0, 0, 0 };   // advances forward each frame
    float    chase_orbit    = 0.0f;        // camera orbit angle (radians)

    // Jump-hole approach (np-3dp.13). Each pass begins exactly
    // k_chase_cruise * k_chase_cut_s klicks short of a jump hole and ends
    // AT it (the ship 'jumps through' the moment the hull swaps). The
    // white flash ramps in over the last k_jump_flash_s of the approach,
    // peaks at the hole, then fades after. jump_event latches one frame
    // on arrival so main can reskin the sky + reposition the sun.
    HMM_Vec3 chase_hole_pos { 0, 0, 0 };   // world pos of the target hole
    float    chase_flash    = 0.0f;        // 0..1 hyperspace flash alpha
    bool     post_jump      = false;       // true while fading out post-jump
    float    post_jump_t    = 0.0f;        // seconds since the jump
    bool     jump_event     = false;       // latched 1 frame on arrival
};
State g;

// Chase-cam tuning. Cut duration = how long we follow one ship before
// swapping hull (np-3dp.5: 90s). Cruise speed (world units/s) is well past
// the warp-streak floor so the trails read; the camera moves WITH the ship,
// which is what makes the streaks flow. Each pass starts exactly
// cruise*cut klicks short of a jump hole and arrives AT it on the swap
// (np-3dp.13): 2000 * 90 = 180,000. Orbit rate ~5.7 deg/s (full lap ~63s).
constexpr float k_chase_cut_s      = 90.0f;
constexpr float k_chase_cruise     = 2000.0f;
constexpr float k_chase_orbit_rate = 0.10f;
constexpr float k_chase_hole_dist  = k_chase_cruise * k_chase_cut_s;  // 180,000
constexpr float k_jump_flash_s     = 0.6f;   // flash in/out duration (s)

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

    // Seed the RNG off the wall clock so the random ship pick differs
    // every launch. One-time per process is enough; repeated init calls
    // just keep advancing the same sequence.
    {
        static bool s_seeded = false;
        if (!s_seeded) {
            const auto t = std::chrono::steady_clock::now().time_since_epoch().count();
            std::srand((unsigned)t);
            s_seeded = true;
        }
    }

    // Random-ship title mode (np-3dp): pick a handful of DISTINCT hulls
    // from the full roster, regardless of faction. Shuffle the roster
    // and take the first few that load. The category is still tracked
    // for the star preset, but ship selection is no longer tied to it.
    constexpr int k_want = 5;
    std::vector<int> idx(k_all_ships_count);
    for (int i = 0; i < k_all_ships_count; ++i) idx[i] = i;
    // Fisher-Yates shuffle seeded off rand() (seeded by the caller's
    // wall-clock category pick, so each launch differs).
    for (int i = k_all_ships_count - 1; i > 0; --i) {
        const int j = std::rand() % (i + 1);
        std::swap(idx[i], idx[j]);
    }
    for (int k = 0; k < k_all_ships_count && (int)g.atlas_storage.size() < k_want; ++k) {
        const std::string name = k_all_ships[idx[k]];
        if (ShipSpriteAtlas* a = load_one(name, *g.sprite_art)) {
            g.atlas_storage.emplace_back(a);
            g.atlas_keys.push_back(name);
        }
    }
    if (g.atlas_storage.empty()) {
        std::fprintf(stderr, "[title_scene] no atlases loaded (random mode)\n");
        return;
    }

    // Pick the background variant at random (50/50). The chase cam is the
    // np-3dp 'cruise behind a ship' shot; the patrol is the original
    // ships-crossing flyby.
    g.variant = (std::rand() & 1) ? Variant::ChaseCam : Variant::PatrolFlyby;
    g.chase_t = 0.0f;
    g.chase_idx = std::rand() % (int)g.atlas_storage.size();
    g.chase_ship_pos = HMM_V3(0, 0, 0);
    g.chase_orbit = 0.0f;
    // First jump hole sits k_chase_hole_dist straight ahead (+Z); the ship
    // arrives exactly on the 90s hull swap.
    g.chase_hole_pos = HMM_V3(0, 0, k_chase_hole_dist);
    g.chase_flash = 0.0f;
    g.post_jump = false;
    g.post_jump_t = 0.0f;
    g.jump_event = false;

    if (g.variant == Variant::ChaseCam) {
        // One hero ship cruising in a straight line; camera orbits it.
        // Pose is fully driven by tick() + chase_config().
        ShipSpriteObject s{};
        s.atlas          = g.atlas_storage[g.chase_idx].get();
        s.world_size     = 110.0f;   // a touch bigger — it's the hero ship
        s.tint           = {1, 1, 1, 1};
        s.lights_enabled = true;
        s.position       = HMM_V3(0, 0, 0);
        s.orientation    = HMM_Q(0, 0, 0, 1);
        g.patrol.push_back(s);
        std::printf("[title_scene] variant=ChaseCam ship='%s'\n",
                    g.atlas_keys[g.chase_idx % g.atlas_keys.size()].c_str());
        return;
    }

    std::printf("[title_scene] variant=PatrolFlyby\n");

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
    // --- ChaseCam variant: hero ship cruising straight; camera orbits. -
    if (g.variant == Variant::ChaseCam) {
        if (g.patrol.empty()) return;
        g.jump_event = false;                      // cleared unless we arrive
        g.chase_t     += dt / k_chase_cut_s;       // 0..1 over one pass
        g.chase_orbit += k_chase_orbit_rate * dt;  // camera orbit angle
        // Advance the ship along world +Z (flying straight toward the hole).
        g.chase_ship_pos = HMM_AddV3(g.chase_ship_pos,
                                     HMM_V3(0, 0, k_chase_cruise * dt));
        if (g.chase_t >= 1.0f) {
            // Arrived at the jump hole on the 90s mark: swap hull, fire the
            // jump event (main reskins the sky + repositions the sun), and
            // place the NEXT hole one full pass ahead. The ship keeps
            // flying +Z — no teleport — so it threads through a chain of
            // holes, one per system, the flash hiding each transition.
            g.chase_t = 0.0f;
            if (g.atlas_storage.size() > 1) {
                int next = g.chase_idx;
                while (next == g.chase_idx)
                    next = std::rand() % (int)g.atlas_storage.size();
                g.chase_idx = next;
            }
            g.patrol[0].atlas = g.atlas_storage[g.chase_idx].get();
            g.chase_hole_pos = HMM_AddV3(g.chase_ship_pos,
                                         HMM_V3(0, 0, k_chase_hole_dist));
            g.jump_event  = true;
            g.post_jump   = true;
            g.post_jump_t = 0.0f;
            g.chase_flash = 1.0f;
        }
        // Drive the hyperspace flash: ramp IN over the last k_jump_flash_s
        // of the approach, hold peak across the swap, then fade OUT.
        if (g.post_jump) {
            g.post_jump_t += dt;
            g.chase_flash = 1.0f - std::min(g.post_jump_t / k_jump_flash_s, 1.0f);
            if (g.post_jump_t >= k_jump_flash_s) {
                g.post_jump   = false;
                g.chase_flash = 0.0f;
            }
        } else {
            const float remaining = (1.0f - g.chase_t) * k_chase_cut_s;  // s to hole
            g.chase_flash = (remaining < k_jump_flash_s)
                ? (1.0f - remaining / k_jump_flash_s) : 0.0f;
        }
        // Place the ship at its cruising position, nose along +Z (its
        // travel direction).
        ShipSpriteObject& s = g.patrol[0];
        s.position    = g.chase_ship_pos;
        s.orientation = look_rotation(HMM_V3(0, 0, 1), HMM_V3(0, 1, 0));
        return;
    }

    // --- PatrolFlyby variant: linear lanes across the view. ------------
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
Variant  variant()  { return g.variant; }

// Jump-hole accessors (np-3dp.13). Valid only for the ChaseCam variant.
HMM_Vec3 jump_hole_pos() { return g.chase_hole_pos; }
float    jump_flash()    { return g.chase_flash; }
bool consume_jump_event() {
    const bool e = g.jump_event;
    g.jump_event = false;   // one-shot: cleared on read
    return e;
}

// Both variants render the sprites with the convention that needs the
// 180° view roll to read right-side-up, so both want it.
bool wants_camera_roll() { return true; }

// Build a CAMERA orientation (forward = -Z convention) that looks from
// `eye` toward `target`, with `up` as the reference up.
HMM_Quat camera_look_at(HMM_Vec3 eye, HMM_Vec3 target, HMM_Vec3 up) {
    const HMM_Vec3 back = HMM_NormV3(HMM_SubV3(eye, target));   // camera +Z
    HMM_Vec3 right = HMM_Cross(up, back);
    const float rl = HMM_LenV3(right);
    right = (rl < 1e-4f) ? HMM_V3(1, 0, 0) : HMM_DivV3F(right, rl);
    const HMM_Vec3 u = HMM_Cross(back, right);
    HMM_Mat4 m = HMM_M4D(1.0f);
    m.Columns[0] = HMM_V4(right.X, right.Y, right.Z, 0.0f);
    m.Columns[1] = HMM_V4(u.X,     u.Y,     u.Z,     0.0f);
    m.Columns[2] = HMM_V4(back.X,  back.Y,  back.Z,  0.0f);
    return HMM_M4ToQ_RH(m);
}

ChaseConfig chase_config(HMM_Vec3 /*cam_pos*/, HMM_Vec3 /*cam_fwd*/,
                         HMM_Vec3 /*cam_right*/, HMM_Vec3 /*cam_up*/) {
    ChaseConfig c;
    if (g.variant != Variant::ChaseCam) return c;   // all-off for patrol

    // Camera orbits the cruising ship. Base offset sits behind + above
    // the ship; we spin it around world-up by chase_orbit so the camera
    // slowly circles. The ship flies along +Z, so 'behind' is -Z.
    const HMM_Vec3 ship = g.chase_ship_pos;
    constexpr float k_dist   = 240.0f;   // camera distance from the ship
    constexpr float k_height = 55.0f;    // camera height above the ship
    const float ca = std::cos(g.chase_orbit);
    const float sa = std::sin(g.chase_orbit);
    // Base offset (behind + up), rotated around world Y by chase_orbit.
    const HMM_Vec3 base_off = HMM_V3(0.0f, k_height, -k_dist);
    const HMM_Vec3 off = HMM_V3(
        base_off.X * ca + base_off.Z * sa,
        base_off.Y,
       -base_off.X * sa + base_off.Z * ca);
    const HMM_Vec3 eye = HMM_AddV3(ship, off);
    // Aim straight AT the ship so it sits dead-centre in frame (np-3dp.14).
    // The earlier 'look 120 ahead' chase framing pushed bigger hulls low
    // enough to clip the bottom of the view.
    const HMM_Vec3 target = ship;
    c.cam_override = true;
    c.cam_pos      = eye;
    c.cam_orient   = camera_look_at(eye, target, HMM_V3(0, 1, 0));

    // Warp streaks flow along the ship's travel direction (+Z). The
    // camera moves with the ship (eye tracks ship_pos), so the field
    // actually flows. Full intensity + max length for a strong cruise look.
    c.warp_on        = true;
    c.warp_intensity = 1.0f;
    c.warp_len       = 350.0f;
    c.warp_dir       = HMM_V3(0, 0, 1);
    return c;
}

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
