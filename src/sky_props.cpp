// -----------------------------------------------------------------------------
// sky_props.cpp — far-field prop catalog, seeding, and JSON parsing.
// See sky_props.h.
// -----------------------------------------------------------------------------

#include "sky_props.h"
#include "json.h"
#include "sky_family.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace {

constexpr float k_deg_to_rad = 0.01745329251f;
constexpr float k_two_pi     = 6.28318530718f;

// Pack art lives in assets/sky/props/ (see README.md there for the prompts).
//   sprite                                 deg   inten  spin  pulse  depth
const SkyPropCatalogEntry k_catalog[] = {
    { "sky/props/galaxy_spiral_blue",        13.0f, 0.80f,  0.0f, 0.00f, 0.00f },
    { "sky/props/galaxy_barred_gold",        12.0f, 0.80f,  0.0f, 0.00f, 0.00f },
    { "sky/props/galaxy_edgeon_dust",        17.0f, 0.85f,  0.0f, 0.00f, 0.00f },
    { "sky/props/galaxy_interacting_pair",   15.0f, 0.80f,  0.0f, 0.00f, 0.00f },
    { "sky/props/anomaly_pulsar_jets",        9.0f, 0.90f,  4.0f, 1.30f, 0.35f },
    { "sky/props/anomaly_ring_nebula",       10.0f, 0.75f,  0.0f, 0.12f, 0.20f },
    { "sky/props/anomaly_vortex_rift",       10.0f, 0.75f, -6.0f, 0.25f, 0.25f },
    { "sky/props/anomaly_supernova_remnant", 11.0f, 0.70f,  0.0f, 0.08f, 0.15f },
};
constexpr int k_catalog_count = (int)(sizeof(k_catalog) / sizeof(k_catalog[0]));
static_assert(k_sky_prop_galaxy_count < k_catalog_count, "need anomalies too");

// Minimum angle between two props, as a dot-product ceiling (~70 deg).
constexpr float k_min_separation_dot = 0.35f;

struct SkyRng {
    uint64_t s;
    explicit SkyRng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint32_t u32() {   // xorshift64*
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    float unit() { return (float)(u32() >> 8) * (1.0f / 16777216.0f); }
};

// Uniform direction on the unit sphere (Marsaglia 1972).
HMM_Vec3 random_direction(SkyRng& r) {
    for (;;) {
        const float x = r.unit() * 2.0f - 1.0f;
        const float y = r.unit() * 2.0f - 1.0f;
        const float q = x * x + y * y;
        if (q >= 1.0f || q == 0.0f) continue;
        const float k = 2.0f * std::sqrt(1.0f - q);
        return HMM_V3(x * k, y * k, 1.0f - 2.0f * q);
    }
}

bool separated(HMM_Vec3 dir, const std::vector<SkyPropDef>& prev) {
    return std::none_of(prev.begin(), prev.end(), [&](const SkyPropDef& p) {
        return HMM_DotV3(dir, p.direction) > k_min_separation_dot;
    });
}

// Best-of-N: the candidate whose nearest neighbour is farthest away. With
// at most 3 props on a sphere a fully separated pick almost always exists;
// this just guarantees termination without a hand-rolled fallback rotation.
HMM_Vec3 pick_direction(SkyRng& rng, const std::vector<SkyPropDef>& prev) {
    HMM_Vec3 best = random_direction(rng);
    float best_worst = 2.0f;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const HMM_Vec3 d = attempt == 0 ? best : random_direction(rng);
        if (separated(d, prev)) return d;
        float worst = -1.0f;
        for (const SkyPropDef& p : prev) worst = std::max(worst, HMM_DotV3(d, p.direction));
        if (worst < best_worst) { best_worst = worst; best = d; }
    }
    return best;
}

void apply_catalog(SkyPropDef& p, const SkyPropCatalogEntry& cat) {
    p.sprite      = cat.sprite;
    p.angular_deg = cat.angular_deg;
    p.intensity   = cat.intensity;
    p.spin_dps    = cat.spin_dps;
    p.pulse_hz    = cat.pulse_hz;
    p.pulse_depth = cat.pulse_depth;
}

HMM_Vec3 normalized_or_up(HMM_Vec3 v) {
    const float len = HMM_LenV3(v);
    return len > 1e-4f ? HMM_DivV3F(v, len) : HMM_V3(0.0f, 1.0f, 0.0f);
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

const SkyPropCatalogEntry* sky_prop_catalog(int* count) {
    if (count) *count = k_catalog_count;
    return k_catalog;
}

const SkyPropCatalogEntry* find_sky_prop_catalog_entry(const std::string& sprite) {
    for (const SkyPropCatalogEntry& c : k_catalog) {
        if (sprite == c.sprite) return &c;
    }
    return nullptr;
}

std::vector<SkyPropDef> autogen_sky_props(const std::string& skybox_seed) {
    // Same FNV-1a as the sky-family picker, but salted so the prop draw is
    // independent of which nebula family the seed lands in.
    SkyRng rng(sky_family_hash(skybox_seed + "#sky_props"));
    const int count = 1 + (int)(rng.u32() % 3u);

    // Slot 0 is always a galaxy; the rest are a shuffle of the remaining
    // pack, so no system shows the same prop twice.
    int order[k_catalog_count];
    for (int i = 0; i < k_catalog_count; ++i) order[i] = i;
    std::swap(order[0], order[rng.u32() % (uint32_t)k_sky_prop_galaxy_count]);
    for (int i = k_catalog_count - 1; i > 1; --i) {
        const int j = 1 + (int)(rng.u32() % (uint32_t)i);   // j in [1, i]
        std::swap(order[i], order[j]);
    }

    std::vector<SkyPropDef> out;
    out.reserve((size_t)count);
    for (int n = 0; n < count; ++n) {
        SkyPropDef p;
        apply_catalog(p, k_catalog[order[n]]);
        p.direction    = normalized_or_up(pick_direction(rng, out));
        p.angular_deg *= 0.85f + 0.30f * rng.unit();
        p.roll_rad     = rng.unit() * k_two_pi;
        p.intensity   *= 0.90f + 0.10f * rng.unit();
        p.phase        = rng.unit();
        out.push_back(p);
    }
    return out;
}

SkyPropDef parse_sky_prop(const json::Value& v) {
    SkyPropDef p;
    if (!v.is_object()) return p;
    const json::Value* sprite = v.find("sprite");
    if (!sprite || !sprite->is_string()) return p;
    if (const SkyPropCatalogEntry* cat = find_sky_prop_catalog_entry(sprite->as_string()))
        apply_catalog(p, *cat);
    p.sprite = sprite->as_string();

    auto num = [&](const char* key, float& dst, float scale = 1.0f) {
        if (const json::Value* x = v.find(key); x && x->is_number())
            dst = x->as_float() * scale;
    };
    if (const json::Value* d = v.find("dir"); d && d->is_array() && d->as_array().size() == 3) {
        const auto& a = d->as_array();
        if (a[0].is_number() && a[1].is_number() && a[2].is_number())
            p.direction = HMM_V3(a[0].as_float(), a[1].as_float(), a[2].as_float());
    }
    p.direction = normalized_or_up(p.direction);
    num("angular_deg", p.angular_deg);
    num("roll_deg",    p.roll_rad, k_deg_to_rad);
    num("intensity",   p.intensity);
    num("spin_dps",    p.spin_dps);
    num("pulse_hz",    p.pulse_hz);
    num("pulse_depth", p.pulse_depth);
    num("phase",       p.phase);

    p.angular_deg = std::clamp(p.angular_deg, k_sky_prop_min_deg, k_sky_prop_max_deg);
    p.intensity   = clamp01(p.intensity);
    p.pulse_depth = clamp01(p.pulse_depth);
    p.pulse_hz    = std::max(p.pulse_hz, 0.0f);
    return p;
}
