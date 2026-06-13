// -----------------------------------------------------------------------------
// galaxy.cpp — JSON → Galaxy graph materialisation + lookup.
//
// Mirror of system_def.cpp's loader, one level up. parse_file → walk the
// `systems` + `jumps` arrays → fill the graph. The lookup methods are linear
// scans over those vectors: a galaxy is a handful of systems with a handful of
// edges each, so a map would cost more in ceremony than it saves in cycles.
// If the universe ever grows to hundreds of systems this is the obvious place
// to drop in an id→index table — but not before.
// -----------------------------------------------------------------------------

#include "galaxy.h"
#include "json.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace galaxy {

namespace {

// Guarded string accessor (mirrors savegame.cpp's is_string() checks):
// a valid-but-mistyped value (e.g. "id": 123) must not throw out of
// as_string() and crash the load — absent/non-string yields the default.
std::string str_or(const json::Value* v, const std::string& def = {}) {
    return (v && v->is_string()) ? v->as_string() : def;
}

// Read an optional [x, y] pair into a HMM_Vec2, keeping the default if the
// key is absent or malformed. (system_def.cpp has the Vec3 twin; the galaxy
// map is 2D, so this lives here rather than dragging in a shared util.)
HMM_Vec2 vec2_or(const json::Value* v, HMM_Vec2 def) {
    if (!v || !v->is_array() || v->as_array().size() != 2) return def;
    const auto& arr = v->as_array();
    if (!arr[0].is_number() || !arr[1].is_number()) return def;
    return { arr[0].as_float(), arr[1].as_float() };
}

SystemEntry parse_system(const json::Value& v) {
    SystemEntry e;
    e.id           = str_or(v.find("id"),           e.id);
    e.display_name = str_or(v.find("display_name"), e.display_name);
    e.sector       = str_or(v.find("sector"),       e.sector);
    e.json_path    = str_or(v.find("json_path"),    e.json_path);
    e.galaxy_position = vec2_or(v.find("galaxy_position"), e.galaxy_position);
    // A display name is nice-to-have; default it to the id so the jump map
    // never shows a blank label for a terse entry.
    if (e.display_name.empty()) e.display_name = e.id;
    return e;
}

JumpLink parse_jump(const json::Value& v) {
    JumpLink j;
    j.from     = str_or(v.find("from"),     j.from);
    j.from_nav = str_or(v.find("from_nav"), j.from_nav);
    j.to       = str_or(v.find("to"),       j.to);
    j.to_nav   = str_or(v.find("to_nav"),   j.to_nav);
    return j;
}

} // namespace

const SystemEntry* Galaxy::find(const std::string& id) const {
    for (const auto& s : systems) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

std::string Galaxy::json_path_for(const std::string& id) const {
    const SystemEntry* e = find(id);
    return e ? e->json_path : std::string{};
}

std::vector<std::string> Galaxy::neighbors(const std::string& id) const {
    std::vector<std::string> out;
    for (const auto& j : jumps) {
        if (j.from != id) continue;
        // De-dup: two gates in one system could lead to the same neighbour.
        bool seen = false;
        for (const auto& n : out) {
            if (n == j.to) { seen = true; break; }
        }
        if (!seen) out.push_back(j.to);
    }
    return out;
}

JumpTarget Galaxy::jump_target(const std::string& system,
                               const std::string& jump_nav) const {
    for (const auto& j : jumps) {
        if (j.from == system && j.from_nav == jump_nav) {
            return { true, j.to, j.to_nav };
        }
    }
    return {};   // ok=false: dangling / unsurveyed gate
}

bool load(const std::string& path, Galaxy& out) {
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "[galaxy] file not found: %s\n", path.c_str());
        return false;
    }

    // Whole-body guard: parse_file and the as_*() accessors below can
    // throw on malformed/mistyped JSON. A bad galaxy file must fail
    // cleanly (keep the prior galaxy) rather than abort the process.
    try {
    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[galaxy] '%s': root is not a JSON object\n", path.c_str());
        return false;
    }

    Galaxy g;
    if (auto* arr = root.find("systems"); arr && arr->is_array()) {
        for (const auto& s : arr->as_array()) {
            SystemEntry e = parse_system(s);
            if (e.id.empty() || e.json_path.empty()) {
                std::fprintf(stderr, "[galaxy] skipping system with missing id/json_path\n");
                continue;
            }
            g.systems.push_back(std::move(e));
        }
    }
    if (auto* arr = root.find("jumps"); arr && arr->is_array()) {
        for (const auto& j : arr->as_array()) {
            JumpLink link = parse_jump(j);
            if (link.from.empty() || link.to.empty()) {
                std::fprintf(stderr, "[galaxy] skipping jump with missing from/to\n");
                continue;
            }
            g.jumps.push_back(std::move(link));
        }
    }

    if (g.systems.empty()) {
        std::fprintf(stderr, "[galaxy] '%s': no usable systems — keeping prior galaxy\n",
                     path.c_str());
        return false;
    }

    std::printf("[galaxy] loaded '%s' — %zu systems, %zu jump links\n",
                path.c_str(), g.systems.size(), g.jumps.size());
    for (const auto& s : g.systems) {
        std::printf("[galaxy]   %-14s '%s' (%s) -> %s\n",
                    s.id.c_str(), s.display_name.c_str(),
                    s.sector.c_str(), s.json_path.c_str());
    }

    out = std::move(g);
    return true;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[galaxy] '%s': parse error (%s) — keeping prior galaxy\n",
                     path.c_str(), ex.what());
        return false;
    }
}

} // namespace galaxy
