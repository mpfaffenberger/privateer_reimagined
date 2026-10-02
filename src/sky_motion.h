#pragma once
// -----------------------------------------------------------------------------
// sky_motion.h — backdrop things that move (#701): shooting stars and a
// per-system comet. Pure data + seeding + the meteor schedule, no GPU;
// SkyMotionRenderer draws them. Purely visual, on the far plane.
//
// Seeded from skybox_seed unless the system JSON says otherwise:
//
//   "sky": {
//     "meteors_per_minute": 12,                      // whole sky; 0 = none
//     "comet": { "dir": [x, y, z], "tail_deg": 14 }  // or false = none
//   }
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <cstdint>
#include <string>
#include <vector>

namespace json { struct Value; }

struct SkyCometDef {
    bool     enabled   = false;
    HMM_Vec3 direction = { 0.0f, 1.0f, 0.0f };  // world-space unit vector to the coma
    float    tail_deg  = 14.0f;                 // apparent tail length
    float    intensity = 0.8f;
};

struct SkyMeteorsDef {
    float    per_minute = 0.0f;
    uint64_t seed       = 0;      // schedule hash; same seed -> same shower
};

// One streak as seen this frame, on the unit sphere.
struct SkyMeteor {
    HMM_Vec3 head;
    HMM_Vec3 tail;
    float    brightness;   // [0, 1], already includes the fade in/out
};

constexpr float k_sky_meteors_max_per_minute = 30.0f;   // whole sky
constexpr float k_sky_comet_min_tail_deg     = 4.0f;
constexpr float k_sky_comet_max_tail_deg     = 30.0f;

// `avoid` = directions already used (e.g. sky props) so the comet doesn't
// sit on a galaxy.
SkyCometDef   autogen_sky_comet(const std::string& skybox_seed,
                                const std::vector<HMM_Vec3>& avoid);
SkyMeteorsDef autogen_sky_meteors(const std::string& skybox_seed);

// `v` is the "comet" value: an object, or `false` for an explicit opt-out.
SkyCometDef   parse_sky_comet(const json::Value& v);

// Streaks alive at `time_sec`. Stateless: time is cut into windows of
// 60/per_minute seconds, and each window hashes to "fires or not" plus a
// start time, direction, arc, and duration. A streak always ends inside
// its own window, so at most one is alive at a time. `per_minute` counts
// the whole sky; only ~15% of it is on screen at once.
// Returns false when no streak is alive.
bool sky_meteor_at(const SkyMeteorsDef& def, float time_sec, SkyMeteor& out);
