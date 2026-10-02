// -----------------------------------------------------------------------------
// test_sky_props.cpp — far-field galaxy / anomaly seeding + override parsing
// (#693). Headless. Run from the repo root so assets/ resolves:
//
//   cmake --build build --target test_sky_props && ./build/test_sky_props
// -----------------------------------------------------------------------------

#include "json.h"
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

    std::printf("\n=== %s ===\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
