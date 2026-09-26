// -----------------------------------------------------------------------------
// test_sky_props.cpp — far-field galaxy / anomaly placement.
//
// Headless. Run from the repo root so system JSON + the prop catalog resolve:
//
//   cmake --build build --target test_sky_props && ./build/test_sky_props
// -----------------------------------------------------------------------------

#include "system_def.h"
#include "json.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

bool near(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

bool same_prop(const SkyPropDef& a, const SkyPropDef& b) {
    return a.sprite == b.sprite
        && near(a.direction.X, b.direction.X)
        && near(a.direction.Y, b.direction.Y)
        && near(a.direction.Z, b.direction.Z)
        && near(a.angular_deg, b.angular_deg)
        && near(a.roll_rad, b.roll_rad)
        && near(a.alpha, b.alpha);
}

bool in_catalog(const std::string& stem) {
    int n = 0;
    const SkyPropCatalogEntry* cat = sky_prop_catalog(&n);
    for (int i = 0; i < n; ++i) {
        if (stem == cat[i].sprite) return true;
    }
    return false;
}

bool is_galaxy(const std::string& stem) {
    return stem.find("pixel_galaxy_") != std::string::npos;
}

void check_layout(const char* label, const std::vector<SkyPropDef>& props) {
    check((std::string(label) + " count 1..3").c_str(),
          props.size() >= 1 && props.size() <= 3);
    if (props.empty()) return;
    check((std::string(label) + " leads with a galaxy").c_str(),
          is_galaxy(props[0].sprite));
    for (size_t i = 0; i < props.size(); ++i) {
        const SkyPropDef& p = props[i];
        const float len = std::sqrt(p.direction.X * p.direction.X +
                                    p.direction.Y * p.direction.Y +
                                    p.direction.Z * p.direction.Z);
        check((std::string(label) + " unit dir " + std::to_string(i)).c_str(),
              near(len, 1.0f, 1e-3f));
        check((std::string(label) + " catalog " + p.sprite).c_str(),
              in_catalog(p.sprite));
        check((std::string(label) + " angular in range").c_str(),
              p.angular_deg >= 2.0f && p.angular_deg <= 28.0f);
        const HMM_Vec3 pos = sky_prop_world_position(HMM_V3(0, 0, 0), p.direction);
        const float dist = std::sqrt(pos.X * pos.X + pos.Y * pos.Y + pos.Z * pos.Z);
        check((std::string(label) + " dome radius").c_str(),
              near(dist, k_sky_prop_dome_radius, 1.0f));
        const float size = sky_prop_world_size(p.angular_deg);
        // Rim of a facing billboard stays inside the 500 km far plane.
        const float half_rad = p.angular_deg * 0.5f * 0.01745329251f;
        const float rim = k_sky_prop_dome_radius / std::cos(half_rad);
        check((std::string(label) + " inside far plane").c_str(),
              rim < 500000.0f && size > 1000.0f);
        for (size_t j = 0; j < i; ++j) {
            const float dot = p.direction.X * props[j].direction.X
                            + p.direction.Y * props[j].direction.Y
                            + p.direction.Z * props[j].direction.Z;
            check((std::string(label) + " separated").c_str(), dot <= 0.60f);
        }
    }
}

} // namespace

int main() {
    const auto troy_a = autogen_sky_props("troy");
    const auto troy_b = autogen_sky_props("troy");
    check("troy stable", troy_a.size() == troy_b.size());
    for (size_t i = 0; i < troy_a.size() && i < troy_b.size(); ++i) {
        check("troy prop stable", same_prop(troy_a[i], troy_b[i]));
    }
    check_layout("troy", troy_a);
    std::printf("  troy sky:");
    for (const SkyPropDef& p : troy_a) {
        std::printf(" %s(%.1f°)", p.sprite.c_str(), p.angular_deg);
    }
    std::printf("\n");

    check("different seed differs",
          !same_prop(autogen_sky_props("troy")[0], autogen_sky_props("oxford")[0])
          || autogen_sky_props("troy").size() != autogen_sky_props("oxford").size());

    int catalog_n = 0;
    const SkyPropCatalogEntry* cat = sky_prop_catalog(&catalog_n);
    check("catalog has 8", catalog_n == 8);
    json::Value catalog = json::parse_file("assets/sky/props/catalog.json");
    check("catalog.json parses", catalog.is_array() && (int)catalog.as_array().size() == catalog_n);
    if (catalog.is_array()) {
        const auto& arr = catalog.as_array();
        for (int i = 0; i < catalog_n && i < (int)arr.size(); ++i) {
            const std::string stem = arr[i].find("sprite") ? arr[i]["sprite"].as_string() : "";
            const float ang = arr[i].find("angular_deg") ? arr[i]["angular_deg"].as_float() : -1.0f;
            check(("catalog match " + stem).c_str(),
                  stem == cat[i].sprite && near(ang, cat[i].angular_deg, 1e-3f));
            const std::string png = "assets/" + stem + ".png";
            check(("png exists " + png).c_str(), std::filesystem::exists(png));
        }
    }

    // Every shipped system that doesn't author sky_props gets a seeded sky,
    // and that sky matches hashing its skybox_seed.
    int systems = 0;
    for (const auto& ent : std::filesystem::directory_iterator("assets/systems")) {
        if (ent.path().extension() != ".json") continue;
        auto loaded = load_system(ent.path().string());
        if (!loaded) {
            check(("load " + ent.path().string()).c_str(), false);
            continue;
        }
        ++systems;
        if (loaded->sky_props_authored) continue;
        const auto expect = autogen_sky_props(loaded->skybox_seed);
        bool match = expect.size() == loaded->sky_props.size();
        for (size_t i = 0; match && i < expect.size(); ++i)
            match = same_prop(expect[i], loaded->sky_props[i]);
        check((loaded->name + " seeded from skybox_seed").c_str(), match);
        check_layout(loaded->skybox_seed.c_str(), loaded->sky_props);
    }
    check("scanned systems", systems > 50);

    const char* override_path = "/tmp/sky_prop_override.json";
    {
        std::ofstream out(override_path);
        out << R"({
            "name": "Sky Prop Override",
            "skybox_seed": "troy",
            "sky_props": [{
                "sprite": "sky/props/pixel_anomaly_pulsar_red",
                "dir": [0, 0, -2],
                "angular_deg": 100,
                "roll_deg": 180,
                "alpha": 0.5
            }]
        })";
    }
    auto over = load_system(override_path);
    check("override loads", over.has_value());
    if (over) {
        check("override authored", over->sky_props_authored);
        check("override count", over->sky_props.size() == 1);
        if (!over->sky_props.empty()) {
            const SkyPropDef& p = over->sky_props[0];
            check("override sprite", p.sprite == "sky/props/pixel_anomaly_pulsar_red");
            check("override dir normalised",
                  near(p.direction.X, 0.0f) && near(p.direction.Y, 0.0f) &&
                  near(p.direction.Z, -1.0f));
            check("override angular clamped", near(p.angular_deg, 28.0f));
            check("override roll", near(p.roll_rad, 3.14159265f, 1e-3f));
            check("override alpha", near(p.alpha, 0.5f));
        }
    }

    const char* empty_path = "/tmp/sky_prop_empty.json";
    {
        std::ofstream out(empty_path);
        out << R"({ "name": "No Sky", "skybox_seed": "troy", "sky_props": [] })";
    }
    auto none = load_system(empty_path);
    check("empty authored", none && none->sky_props_authored && none->sky_props.empty());

    std::printf("\n=== %s (%d) ===\n",
                failures == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED",
                failures);
    return failures == 0 ? 0 : 1;
}
