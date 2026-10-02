// -----------------------------------------------------------------------------
// test_sky_props.cpp — backdrop seeding + override parsing: far-field
// galaxies / anomalies (#693), comets and the meteor schedule (#701), gas
// clouds and parallax (#704). Headless. Run from the repo root so assets/ resolves:
//
//   cmake --build build --target test_sky_props && ./build/test_sky_props
// -----------------------------------------------------------------------------

#include "json.h"
#include "sky_motion.h"
#include "sky_props.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

bool is_galaxy(const std::string& sprite) {
    int n = 0;
    const SkyPropCatalogEntry* cat = sky_prop_catalog(&n);
    for (int i = 0; i < k_sky_prop_galaxy_count; ++i)
        if (sprite == cat[i].sprite) return true;
    return false;
}

bool same(const std::vector<SkyPropDef>& a, const std::vector<SkyPropDef>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].sprite != b[i].sprite || !approx(a[i].direction.X, b[i].direction.X) ||
            !approx(a[i].angular_deg, b[i].angular_deg) || !approx(a[i].roll_rad, b[i].roll_rad))
            return false;
    }
    return true;
}

// Every invariant a seeded sky must hold. Returns a reason or nullptr.
const char* seeded_violation(const std::vector<SkyPropDef>& props) {
    if (props.empty() || props.size() > 3) return "count outside 1..3";
    if (!is_galaxy(props[0].sprite)) return "first prop is not a galaxy";
    std::set<std::string> seen;
    for (size_t i = 0; i < props.size(); ++i) {
        const SkyPropDef& p = props[i];
        if (!seen.insert(p.sprite).second) return "duplicate sprite";
        if (!find_sky_prop_catalog_entry(p.sprite)) return "sprite not in catalog";
        if (!approx(HMM_LenV3(p.direction), 1.0f, 1e-3f)) return "direction not unit";
        if (p.angular_deg < k_sky_prop_min_deg || p.angular_deg > k_sky_prop_max_deg)
            return "angular size out of range";
        if (p.intensity <= 0.0f || p.intensity > 1.0f) return "intensity out of range";
        for (size_t j = 0; j < i; ++j)
            if (HMM_DotV3(p.direction, props[j].direction) > 0.35f) return "props overlap";
    }
    return nullptr;
}

std::vector<std::string> shipped_seeds() {
    std::vector<std::string> seeds;
    for (const auto& e : std::filesystem::directory_iterator("assets/systems")) {
        if (e.path().extension() != ".json") continue;
        const json::Value root = json::parse_file(e.path().string());
        if (const json::Value* s = root.find("skybox_seed"); s && s->is_string())
            seeds.push_back(s->as_string());
    }
    return seeds;
}

void check_motion(const std::vector<std::string>& seeds) {
    std::printf("comets\n");
    int with_comet = 0;
    bool comets_clear = true, comets_valid = true;
    for (const std::string& seed : seeds) {
        const auto props = autogen_sky_props(seed);
        std::vector<HMM_Vec3> taken;
        for (const SkyPropDef& p : props) taken.push_back(p.direction);
        const SkyCometDef c = autogen_sky_comet(seed, taken);
        if (!c.enabled) continue;
        ++with_comet;
        comets_valid &= approx(HMM_LenV3(c.direction), 1.0f, 1e-3f) &&
                        c.tail_deg >= k_sky_comet_min_tail_deg &&
                        c.tail_deg <= k_sky_comet_max_tail_deg;
        for (const HMM_Vec3& d : taken) comets_clear &= HMM_DotV3(c.direction, d) < 0.87f;
    }
    std::printf("    %d of %zu systems have a comet\n", with_comet, seeds.size());
    expect("some systems have a comet, most don't",
           with_comet > (int)seeds.size() / 5 && with_comet < (int)seeds.size() * 3 / 5);
    expect("seeded comets are valid", comets_valid);
    expect("comets never sit on a sky prop (>30 deg apart)", comets_clear);

    expect("comet: false opts out", !parse_sky_comet(json::parse("false")).enabled);
    const SkyCometDef c = parse_sky_comet(json::parse(R"({"dir": [3, 0, 0], "tail_deg": 90})"));
    expect("authored comet enabled + normalised", c.enabled && approx(c.direction.X, 1.0f));
    expect("authored tail clamped", approx(c.tail_deg, k_sky_comet_max_tail_deg));

    std::printf("meteors\n");
    const SkyMeteorsDef m = autogen_sky_meteors("troy");
    expect("meteor schedule is deterministic",
           m.seed == autogen_sky_meteors("troy").seed && m.per_minute == autogen_sky_meteors("troy").per_minute);
    expect("seeded rate in range", m.per_minute >= 8.0f && m.per_minute <= 24.0f);

    // Sample 10 minutes at 60 Hz. Count streaks by their rising edge.
    int streaks = 0;
    bool was_alive = false, all_sane = true, repeatable = true;
    for (int f = 0; f < 600 * 60; ++f) {
        const float t = (float)f / 60.0f;
        SkyMeteor a{}, b{};
        const bool alive = sky_meteor_at(m, t, a);
        repeatable &= alive == sky_meteor_at(m, t, b) && (!alive || approx(a.head.X, b.head.X));
        if (alive) {
            const float arc_deg = std::acos(std::fmin(1.0f, HMM_DotV3(a.head, a.tail))) * 57.29578f;
            all_sane &= approx(HMM_LenV3(a.head), 1.0f, 1e-3f) &&
                        approx(HMM_LenV3(a.tail), 1.0f, 1e-3f) &&
                        a.brightness > 0.0f && a.brightness <= 1.0f && arc_deg <= 20.0f;
        }
        if (alive && !was_alive) ++streaks;
        was_alive = alive;
    }
    const float expected = m.per_minute * 10.0f * 0.75f;
    std::printf("    %.1f/min -> %d streaks in 10 min (expected ~%.0f)\n", m.per_minute, streaks, expected);
    expect("streak count tracks the rate", streaks > expected * 0.65f && streaks < expected * 1.35f);
    expect("streaks are unit-sphere, short, and dim enough", all_sane);
    expect("same time -> same streak", repeatable);

    SkyMeteorsDef off = m;
    off.per_minute = 0.0f;
    SkyMeteor unused{};
    bool any = false;
    for (int f = 0; f < 6000; ++f) any |= sky_meteor_at(off, (float)f * 0.01f, unused);
    expect("meteors_per_minute 0 = none", !any);
}

void check_clouds() {
    std::printf("clouds\n");
    const HMM_Vec3 a = HMM_V3(1.0f, 0.55f, 0.20f), b = HMM_V3(0.35f, 0.90f, 0.50f);
    const auto seeded = autogen_sky_clouds("troy", -1, a, b);
    expect("seeded cloud count is 3-5", seeded.size() >= 3 && seeded.size() <= 5);
    expect("clouds: 0 = none", autogen_sky_clouds("troy", 0, a, b).empty());
    expect("clouds: N forces N", autogen_sky_clouds("troy", 7, a, b).size() == 7);
    bool valid = true, art = true;
    for (const SkyPropDef& c : seeded) {
        const float peak = std::fmax(c.tint.X, std::fmax(c.tint.Y, c.tint.Z));
        valid &= approx(peak, 1.0f, 1e-3f) && c.parallax_m > 0.0f &&
                 c.intensity > 0.0f && c.intensity <= 0.8f &&
                 approx(HMM_LenV3(c.direction), 1.0f, 1e-3f);
        art &= std::filesystem::exists("assets/" + c.sprite + ".png");
    }
    expect("clouds are faint, tinted, and parallaxed", valid);
    expect("cloud art exists", art);

    SkyPropDef far;
    far.direction = HMM_V3(0, 0, 1);
    far.angular_deg = 30.0f;
    HMM_Vec3 dir;
    float deg;
    sky_prop_apparent(far, HMM_V3(1e5f, 0, 0), dir, deg);
    expect("parallax 0 ignores the camera", approx(dir.Z, 1.0f) && approx(deg, 30.0f));

    SkyPropDef near_cloud = far;
    near_cloud.parallax_m = 1e6f;
    sky_prop_apparent(near_cloud, HMM_V3(0, 0, 0), dir, deg);
    expect("parallax from the origin = authored", approx(dir.Z, 1.0f) && approx(deg, 30.0f));
    sky_prop_apparent(near_cloud, HMM_V3(1e5f, 0, 0), dir, deg);
    const float shift_deg = std::acos(dir.Z) * 57.29578f;
    std::printf("    100 km sideways at 1 Mm -> %.2f deg shift\n", shift_deg);
    expect("100 km sideways shifts a 1 Mm cloud ~5.7 deg the other way",
           approx(shift_deg, 5.71f, 0.05f) && dir.X < 0.0f);
    sky_prop_apparent(near_cloud, HMM_V3(0, 0, 5e5f), dir, deg);
    expect("flying toward a cloud grows it (capped)",
           deg > 30.0f && deg <= 2.0f * k_sky_prop_max_deg);
}
} // namespace

int main() {
    std::printf("catalog\n");
    int n = 0;
    const SkyPropCatalogEntry* cat = sky_prop_catalog(&n);
    bool all_art = true;
    for (int i = 0; i < n; ++i) {
        const std::string png = std::string("assets/") + cat[i].sprite + ".png";
        if (!std::filesystem::exists(png)) {
            std::printf("    missing %s\n", png.c_str());
            all_art = false;
        }
    }
    expect("every catalog sprite has a PNG", all_art);
    expect("catalog has galaxies and anomalies", n > k_sky_prop_galaxy_count);

    std::printf("seeding\n");
    expect("same seed -> same sky", same(autogen_sky_props("troy"), autogen_sky_props("troy")));
    expect("different seeds -> different skies",
           !same(autogen_sky_props("troy"), autogen_sky_props("oxford")));

    const std::vector<std::string> seeds = shipped_seeds();
    expect("found shipped system seeds", seeds.size() > 20);
    int counts[4] = {};
    std::set<std::string> first_props;
    bool all_valid = true;
    for (const std::string& seed : seeds) {
        const auto props = autogen_sky_props(seed);
        if (const char* why = seeded_violation(props)) {
            std::printf("    seed '%s': %s\n", seed.c_str(), why);
            all_valid = false;
            continue;
        }
        ++counts[props.size()];
        first_props.insert(props[0].sprite);
    }
    expect("every shipped system gets a valid seeded sky", all_valid);
    expect("systems vary in prop count", counts[1] > 0 && counts[2] > 0 && counts[3] > 0);
    expect("every galaxy shows up somewhere", (int)first_props.size() == k_sky_prop_galaxy_count);
    std::printf("    %zu seeds: %d with 1 prop, %d with 2, %d with 3\n",
                seeds.size(), counts[1], counts[2], counts[3]);

    std::printf("override parsing\n");
    const json::Value v = json::parse(R"({
        "sprite": "sky/props/anomaly_pulsar_jets",
        "dir": [0, 0, -2], "angular_deg": 99, "roll_deg": 90, "intensity": 3
    })");
    const SkyPropDef p = parse_sky_prop(v);
    const SkyPropCatalogEntry* pulsar = find_sky_prop_catalog_entry("sky/props/anomaly_pulsar_jets");
    expect("sprite kept", p.sprite == "sky/props/anomaly_pulsar_jets");
    expect("direction normalised", approx(p.direction.Z, -1.0f) && approx(p.direction.X, 0.0f));
    expect("angular size clamped", approx(p.angular_deg, k_sky_prop_max_deg));
    expect("roll in radians", approx(p.roll_rad, 1.5707963f, 1e-3f));
    expect("intensity clamped", approx(p.intensity, 1.0f));
    expect("catalog motion inherited", pulsar && approx(p.spin_dps, pulsar->spin_dps) &&
                                       approx(p.pulse_hz, pulsar->pulse_hz));

    const SkyPropDef custom = parse_sky_prop(json::parse(R"({"sprite": "sky/props/mine"})"));
    expect("unknown sprite allowed, motion off",
           custom.sprite == "sky/props/mine" && custom.spin_dps == 0.0f && custom.pulse_hz == 0.0f);
    expect("missing sprite rejected", parse_sky_prop(json::parse(R"({"dir": [1,0,0]})")).sprite.empty());
    expect("non-object rejected", parse_sky_prop(json::parse("[1, 2]")).sprite.empty());

    check_motion(seeds);
    check_clouds();

    std::printf("\n=== %s ===\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
