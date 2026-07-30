// Data-driven equipment hardpoint layouts.
#include "equipment_hardpoints.h"

#include "json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace equipment_hardpoints {
namespace {

constexpr const char* k_names[] = {
    "gun", "turret", "launcher", "armor", "shield",
    "engine", "cargo", "systems", "service"
};

void add_zone(Layout& out, const char* id, const char* label, Kind kind,
              int slot, float x, float y, float w, float h) {
    Zone zone;
    zone.id = id;
    zone.label = label;
    zone.kind = kind;
    zone.slot = slot;
    zone.rect[0] = x; zone.rect[1] = y; zone.rect[2] = w; zone.rect[3] = h;
    out.zones.push_back(zone);
}

} // namespace

const char* kind_name(Kind kind) {
    const int i = static_cast<int>(kind);
    return i >= 0 && i < static_cast<int>(std::size(k_names)) ? k_names[i] : "gun";
}

bool kind_from_name(const std::string& name, Kind& out) {
    for (int i = 0; i < static_cast<int>(std::size(k_names)); ++i) {
        if (name == k_names[i]) { out = static_cast<Kind>(i); return true; }
    }
    return false;
}

std::string layout_path(const std::string& ship) {
    return "assets/ships/" + ship + "/equipment_hardpoints.json";
}

std::string find_top_down_sprite(const std::string& ship) {
    namespace fs = std::filesystem;
    fs::path manifest = fs::path("assets/ships") / ship / "atlas_manifest_3d.json";
    if (!fs::exists(manifest))
        manifest = fs::path("assets/ships") / ship / "atlas_manifest.json";
    const json::Value root = json::parse_file(manifest.string());
    const json::Value* samples = root.find("samples");
    if (!samples || !samples->is_array()) return {};

    const json::Value* best = nullptr;
    float best_score = 1e9f;
    for (const json::Value& sample : samples->as_array()) {
        if (!sample.is_object() || !sample.contains("sprite")) continue;
        const float el = sample.contains("el") ? sample["el"].as_float() : 0.0f;
        const float az = sample.contains("az") ? sample["az"].as_float() : 0.0f;
        // Prefer the exact +90 top view, then azimuth nearest zero.
        const float score = std::fabs(el - 90.0f) * 1000.0f + std::fabs(az);
        if (score < best_score) { best = &sample; best_score = score; }
    }
    if (!best) return {};
    return "assets/" + (*best)["sprite"].as_string();
}

Layout fallback(const std::string& ship, int gun_mounts) {
    Layout out;
    out.ship = ship;
    out.sprite = find_top_down_sprite(ship);

    // Fan gun zones across the lower half of the hull. This is deliberately
    // usable rather than pretty; the in-game editor turns it into authored data.
    const int guns = std::max(0, gun_mounts);
    for (int i = 0; i < guns; ++i) {
        const int col = i % 4;
        const int row = i / 4;
        char id[24], label[32];
        std::snprintf(id, sizeof id, "gun_%d", i);
        std::snprintf(label, sizeof label, "Gun %d", i + 1);
        add_zone(out, id, label, i >= 4 ? Kind::Turret : Kind::Gun, i,
                 0.20f + col * 0.17f, 0.62f - row * 0.48f, 0.12f, 0.10f);
    }
    add_zone(out, "launcher_left", "Left Launcher", Kind::Launcher, 0,
             0.12f, 0.43f, 0.15f, 0.13f);
    add_zone(out, "launcher_right", "Right Launcher", Kind::Launcher, 1,
             0.73f, 0.43f, 0.15f, 0.13f);
    add_zone(out, "armor", "Armor", Kind::Armor, 0, 0.37f, 0.42f, 0.26f, 0.18f);
    add_zone(out, "shield", "Shields", Kind::Shield, 0, 0.39f, 0.32f, 0.22f, 0.10f);
    add_zone(out, "engine", "Engine", Kind::Engine, 0, 0.40f, 0.12f, 0.20f, 0.13f);
    add_zone(out, "cargo", "Cargo", Kind::Cargo, 0, 0.42f, 0.52f, 0.16f, 0.10f);
    add_zone(out, "systems", "Ship Systems", Kind::Systems, 0, 0.42f, 0.70f, 0.16f, 0.10f);
    add_zone(out, "service", "Repair & Rearm", Kind::Service, 0, 0.02f, 0.82f, 0.24f, 0.12f);
    return out;
}

bool load(const std::string& ship, int gun_mounts, Layout& out) {
    out = fallback(ship, gun_mounts);
    if (!std::filesystem::exists(layout_path(ship))) return false;
    const json::Value root = json::parse_file(layout_path(ship));
    if (!root.is_object()) return false;
    if (root.contains("sprite")) out.sprite = root["sprite"].string_or(out.sprite);
    const json::Value* zones = root.find("zones");
    if (!zones || !zones->is_array()) return true;

    std::vector<Zone> parsed;
    for (const json::Value& item : zones->as_array()) {
        if (!item.is_object() || !item.contains("id") || !item.contains("kind") ||
            !item.contains("rect") || !item["rect"].is_array() ||
            item["rect"].as_array().size() != 4) continue;
        Zone zone;
        zone.id = item["id"].as_string();
        zone.label = item.contains("label") ? item["label"].as_string() : zone.id;
        if (!kind_from_name(item["kind"].as_string(), zone.kind)) continue;
        zone.slot = item.contains("slot") ? item["slot"].as_int() : 0;
        for (int i = 0; i < 4; ++i)
            zone.rect[i] = std::clamp(item["rect"].as_array()[i].as_float(), 0.0f, 1.0f);
        if (zone.rect[2] <= 0.0f || zone.rect[3] <= 0.0f) continue;
        parsed.push_back(zone);
    }
    if (!parsed.empty()) out.zones = std::move(parsed);
    return true;
}

bool save(const Layout& layout) {
    if (layout.ship.empty()) return false;
    const std::string path = layout_path(layout.ship);
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    std::fprintf(file, "{\n  \"ship\": \"%s\",\n  \"sprite\": \"%s\",\n  \"zones\": [",
                 layout.ship.c_str(), layout.sprite.c_str());
    for (size_t i = 0; i < layout.zones.size(); ++i) {
        const Zone& zone = layout.zones[i];
        std::fprintf(file,
            "%s\n    { \"id\": \"%s\", \"label\": \"%s\", \"kind\": \"%s\", "
            "\"slot\": %d, \"rect\": [%.5f, %.5f, %.5f, %.5f] }",
            i ? "," : "", zone.id.c_str(), zone.label.c_str(), kind_name(zone.kind),
            zone.slot, zone.rect[0], zone.rect[1], zone.rect[2], zone.rect[3]);
    }
    std::fprintf(file, "%s  ]\n}\n", layout.zones.empty() ? "\n" : "\n");
    const bool ok = std::fclose(file) == 0;
    if (ok) std::printf("[equipment] saved hardpoints: %s\n", path.c_str());
    return ok;
}

} // namespace equipment_hardpoints
