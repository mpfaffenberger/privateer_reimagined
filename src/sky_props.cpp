// -----------------------------------------------------------------------------
// sky_props.cpp — far-field prop catalog, seeding, and JSON parsing.
// See sky_props.h.
// -----------------------------------------------------------------------------

#include "sky_props.h"
#include "json.h"
#include "sky_rng.h"

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

// Gas clouds (#704): monochrome art, tinted at draw time.
const char* const k_cloud_sprites[] = {
    "sky/clouds/cloud_wisp_drift",
    "sky/clouds/cloud_billow",
    "sky/clouds/cloud_filament_arc",
    "sky/clouds/cloud_tattered",
};
constexpr int   k_cloud_sprite_count = (int)(sizeof(k_cloud_sprites) / sizeof(k_cloud_sprites[0]));
// ~1.2 Mm: beyond every nav in the catalog, close enough that an autopilot
// leg (~100 km) shifts a cloud by a few degrees against the skybox.
constexpr float k_cloud_parallax_m = 1.2e6f;
static_assert(k_sky_prop_galaxy_count < k_catalog_count, "need anomalies too");

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
    SkyRng rng(sky_seed_hash(skybox_seed, "#sky_props"));
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
    std::vector<HMM_Vec3>   taken;
    out.reserve((size_t)count);
    for (int n = 0; n < count; ++n) {
        SkyPropDef p;
        apply_catalog(p, k_catalog[order[n]]);
        p.direction    = sky_pick_direction(rng, taken, k_sky_min_separation_dot);
        taken.push_back(p.direction);
        p.angular_deg *= 0.85f + 0.30f * rng.unit();
        p.roll_rad     = rng.unit() * k_two_pi;
        p.intensity   *= 0.90f + 0.10f * rng.unit();
        p.phase        = rng.unit();
        out.push_back(p);
    }
    return out;
}

std::vector<SkyPropDef> autogen_sky_clouds(const std::string& skybox_seed, int count,
                                           HMM_Vec3 tint_a, HMM_Vec3 tint_b) {
    SkyRng rng(sky_seed_hash(skybox_seed, "#sky_clouds"));
    const int seeded = 3 + (int)(rng.u32() % 3u);
    const int n = count < 0 ? seeded : count;

    std::vector<SkyPropDef> out;
    out.reserve((size_t)std::max(n, 0));
    for (int i = 0; i < n; ++i) {
        SkyPropDef p;
        p.sprite      = k_cloud_sprites[rng.u32() % (uint32_t)k_cloud_sprite_count];
        p.direction   = sky_random_direction(rng);
        p.angular_deg = rng.range(25.0f, 45.0f);
        p.roll_rad    = rng.range(0.0f, k_two_pi);
        p.intensity   = rng.range(0.50f, 0.75f);
        p.spin_dps    = rng.range(-0.15f, 0.15f);
        p.parallax_m  = k_cloud_parallax_m * rng.range(0.7f, 1.3f);
        // Somewhere between the two nebula anchors, renormalised so the
        // brightest channel is 1 and intensity alone sets the brightness.
        const HMM_Vec3 mix = HMM_LerpV3(tint_a, rng.unit(), tint_b);
        const float peak = std::max({ mix.X, mix.Y, mix.Z, 1e-3f });
        p.tint = HMM_MulV3F(mix, 1.0f / peak);
        out.push_back(p);
    }
    return out;
}

void sky_prop_apparent(const SkyPropDef& p, HMM_Vec3 camera_pos,
                       HMM_Vec3& out_dir, float& out_deg) {
    out_dir = p.direction;
    out_deg = p.angular_deg;
    if (p.parallax_m <= 0.0f) return;
    const HMM_Vec3 v    = HMM_SubV3(HMM_MulV3F(p.direction, p.parallax_m), camera_pos);
    const float    dist = HMM_LenV3(v);
    if (dist < 1.0f) return;   // camera at the virtual spot; keep the defaults
    out_dir = HMM_DivV3F(v, dist);
    out_deg = std::min(p.angular_deg * p.parallax_m / dist, 2.0f * k_sky_prop_max_deg);
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
