#pragma once
// Data-driven clickable zones for the visual equipment bay.

#include <string>
#include <vector>

namespace equipment_hardpoints {

enum class Kind {
    Gun = 0,
    Turret,
    Launcher,
    Armor,
    Shield,
    Engine,
    Cargo,
    Systems,
    Service,
};

struct Zone {
    std::string id;
    std::string label;
    Kind kind = Kind::Gun;
    int slot = 0;               // gun index, or launcher side (0 left / 1 right)
    float rect[4] = {0.42f, 0.42f, 0.16f, 0.12f}; // normalized to ship image
};

struct Layout {
    std::string ship;
    std::string sprite;         // asset-relative top-down sprite path
    std::vector<Zone> zones;
};

const char* kind_name(Kind kind);
bool is_physical_hardpoint(Kind kind);
bool kind_from_name(const std::string& name, Kind& out);
std::string layout_path(const std::string& ship);
std::string find_top_down_sprite(const std::string& ship);

// Missing files are not errors: a generated fallback keeps every hull usable.
Layout fallback(const std::string& ship, int gun_mounts);
bool load(const std::string& ship, int gun_mounts, Layout& out);
bool save(const Layout& layout);

} // namespace equipment_hardpoints
