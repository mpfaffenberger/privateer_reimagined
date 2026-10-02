#pragma once
// -----------------------------------------------------------------------------
// sky_rng.h — the tiny deterministic RNG behind every seeded backdrop item
// (sky props, comets, meteors). Header-only; shared so each seeding site
// doesn't grow its own xorshift copy.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"
#include "sky_family.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

struct SkyRng {
    uint64_t s;
    explicit SkyRng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t u32() {   // xorshift64*
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    float unit() { return (float)(u32() >> 8) * (1.0f / 16777216.0f); }   // [0, 1)
    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }
};

// Same FNV-1a as the sky-family picker, salted so each backdrop feature
// draws independently of the nebula family (and of each other).
inline uint64_t sky_seed_hash(const std::string& skybox_seed, const char* salt) {
    return sky_family_hash(skybox_seed + salt);
}

// Uniform direction on the unit sphere (Marsaglia 1972).
inline HMM_Vec3 sky_random_direction(SkyRng& r) {
    for (;;) {
        const float x = r.unit() * 2.0f - 1.0f;
        const float y = r.unit() * 2.0f - 1.0f;
        const float q = x * x + y * y;
        if (q >= 1.0f || q == 0.0f) continue;
        const float k = 2.0f * std::sqrt(1.0f - q);
        return HMM_V3(x * k, y * k, 1.0f - 2.0f * q);
    }
}

// Minimum angle between two backdrop items, as a dot-product ceiling
// (~70 deg), so props and comets never pile up on each other.
constexpr float k_sky_min_separation_dot = 0.35f;

// A direction at least acos(max_dot) away from everything in `taken`.
// Best-of-N: if no fully separated candidate turns up, return the one
// whose nearest neighbour is farthest away (always terminates).
inline HMM_Vec3 sky_pick_direction(SkyRng& rng, const std::vector<HMM_Vec3>& taken,
                                   float max_dot) {
    HMM_Vec3 best{};
    float best_worst = 2.0f;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const HMM_Vec3 d = sky_random_direction(rng);
        float worst = -1.0f;
        for (const HMM_Vec3& t : taken) worst = std::fmax(worst, HMM_DotV3(d, t));
        if (worst <= max_dot) return d;
        if (worst < best_worst) { best_worst = worst; best = d; }
    }
    return best;
}

// Unit vector perpendicular to unit `d` (any one; deterministic).
inline HMM_Vec3 sky_any_tangent(HMM_Vec3 d) {
    const HMM_Vec3 ref = std::fabs(d.Y) > 0.99f ? HMM_V3(1, 0, 0) : HMM_V3(0, 1, 0);
    return HMM_NormV3(HMM_Cross(d, ref));
}
