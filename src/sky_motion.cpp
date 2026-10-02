// -----------------------------------------------------------------------------
// sky_motion.cpp — comet + meteor seeding, parsing, and the stateless meteor
// schedule. See sky_motion.h.
// -----------------------------------------------------------------------------

#include "sky_motion.h"
#include "json.h"
#include "sky_rng.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float k_deg_to_rad = 0.01745329251f;

constexpr float k_comet_chance    = 0.40f;   // share of systems with a comet
constexpr float k_meteor_fire_p   = 0.75f;   // share of windows that fire
constexpr float k_meteor_max_dur  = 1.1f;    // seconds; < the shortest window
static_assert(60.0f / k_sky_meteors_max_per_minute > k_meteor_max_dur,
              "a streak must fit inside one schedule window");

// Comets hug the ecliptic-ish band so they read as part of the system
// rather than pinned to the zenith.
constexpr float k_comet_max_abs_y = 0.6f;

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

HMM_Vec3 on_great_circle(HMM_Vec3 start, HMM_Vec3 tangent, float angle_rad) {
    return HMM_AddV3(HMM_MulV3F(start, std::cos(angle_rad)),
                     HMM_MulV3F(tangent, std::sin(angle_rad)));
}

// Window `k`'s streak, if it fires and is alive at `t`.
bool meteor_in_window(const SkyMeteorsDef& def, float window_s, int64_t k, float t,
                      SkyMeteor& out) {
    SkyRng rng(def.seed ^ ((uint64_t)k * 0x9E3779B97F4A7C15ull));
    rng.u32();   // decorrelate neighbouring windows
    if (rng.unit() >= k_meteor_fire_p) return false;

    const float dur   = rng.range(0.4f, k_meteor_max_dur);
    const float start = (float)k * window_s + rng.unit() * std::max(0.0f, window_s - dur);
    const float p     = (t - start) / dur;
    if (p < 0.0f || p > 1.0f) return false;

    const HMM_Vec3 s0    = sky_random_direction(rng);
    // Rotate the arbitrary tangent by a random heading so streaks fall
    // every which way, not only "down".
    const HMM_Vec3 ta    = sky_any_tangent(s0);
    const HMM_Vec3 tb    = HMM_Cross(s0, ta);
    const float heading  = rng.range(0.0f, 6.28318530718f);
    const HMM_Vec3 dir   = HMM_AddV3(HMM_MulV3F(ta, std::cos(heading)),
                                     HMM_MulV3F(tb, std::sin(heading)));
    const float arc      = rng.range(6.0f, 20.0f) * k_deg_to_rad;
    const float peak     = rng.range(0.45f, 0.85f);

    const float head_a = arc * p;
    const float tail_a = std::max(0.0f, head_a - arc * 0.35f);
    out.head       = on_great_circle(s0, dir, head_a);
    out.tail       = on_great_circle(s0, dir, tail_a);
    out.brightness = peak * smoothstep(0.0f, 0.15f, p) * (1.0f - smoothstep(0.6f, 1.0f, p));
    return out.brightness > 0.0f && head_a > tail_a;
}

} // namespace

SkyCometDef autogen_sky_comet(const std::string& skybox_seed,
                              const std::vector<HMM_Vec3>& avoid) {
    SkyRng rng(sky_seed_hash(skybox_seed, "#sky_comet"));
    SkyCometDef c;
    c.enabled = rng.unit() < k_comet_chance;
    if (!c.enabled) return c;
    HMM_Vec3 d = sky_pick_direction(rng, avoid, k_sky_min_separation_dot);
    d.Y = std::clamp(d.Y, -k_comet_max_abs_y, k_comet_max_abs_y);
    c.direction = HMM_NormV3(d);
    c.tail_deg  = rng.range(10.0f, 18.0f);
    c.intensity = rng.range(0.70f, 0.90f);
    return c;
}

SkyMeteorsDef autogen_sky_meteors(const std::string& skybox_seed) {
    SkyRng rng(sky_seed_hash(skybox_seed, "#sky_meteors"));
    SkyMeteorsDef m;
    m.per_minute = rng.range(8.0f, 24.0f);
    m.seed       = rng.u32() | ((uint64_t)rng.u32() << 32);
    return m;
}

SkyCometDef parse_sky_comet(const json::Value& v) {
    SkyCometDef c;
    if (!v.is_object()) return c;   // `false` (or junk) = no comet
    c.enabled = true;
    if (const json::Value* d = v.find("dir"); d && d->is_array() && d->as_array().size() == 3) {
        const auto& a = d->as_array();
        if (a[0].is_number() && a[1].is_number() && a[2].is_number())
            c.direction = HMM_V3(a[0].as_float(), a[1].as_float(), a[2].as_float());
    }
    const float len = HMM_LenV3(c.direction);
    c.direction = len > 1e-4f ? HMM_DivV3F(c.direction, len) : HMM_V3(0.0f, 1.0f, 0.0f);
    if (const json::Value* x = v.find("tail_deg"); x && x->is_number())
        c.tail_deg = x->as_float();
    if (const json::Value* x = v.find("intensity"); x && x->is_number())
        c.intensity = x->as_float();
    c.tail_deg  = std::clamp(c.tail_deg, k_sky_comet_min_tail_deg, k_sky_comet_max_tail_deg);
    c.intensity = std::clamp(c.intensity, 0.0f, 1.0f);
    return c;
}

bool sky_meteor_at(const SkyMeteorsDef& def, float time_sec, SkyMeteor& out) {
    const float rate = std::min(def.per_minute, k_sky_meteors_max_per_minute);
    if (rate <= 0.0f || time_sec < 0.0f) return false;
    const float window_s = 60.0f / rate;
    return meteor_in_window(def, window_s, (int64_t)std::floor(time_sec / window_s),
                            time_sec, out);
}
