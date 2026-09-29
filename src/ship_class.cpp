#include "ship_class.h"

#include "gun.h"
#include "json.h"
#include "mobility.h"
#include "shield.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

// Reserved up-front so pointers from ship_class::find() stay stable.
// 32 is well past Privateer's 14 canonical ship types.
std::vector<ShipClass> g_classes;

// Quick mode for "what are the canonical fields we read?" — stays in
// sync with the docs schema. Helper avoids three-line `find/is_*/cast`
// boilerplate at each call site.
float opt_num(const json::Value& v, const char* key, float fallback) {
    const json::Value* p = v.find(key);
    return (p && p->is_number()) ? p->as_float() : fallback;
}
std::string opt_str(const json::Value& v, const char* key,
                    const std::string& fallback = {}) {
    const json::Value* p = v.find(key);
    return (p && p->is_string()) ? p->as_string() : fallback;
}
bool opt_bool(const json::Value& v, const char* key, bool fallback) {
    const json::Value* p = v.find(key);
    return (p && p->is_bool()) ? p->as_bool() : fallback;
}

// Pull HMM_Vec3 out of a 3-element JSON array. Default to zero vec on
// missing/malformed — the caller is expected to know whether absence
// is an error.
HMM_Vec3 read_vec3(const json::Value* v, HMM_Vec3 fallback = {0,0,0}) {
    if (!v || !v->is_array() || v->as_array().size() != 3) return fallback;
    HMM_Vec3 out;
    out.X = (*v)[0].as_float();
    out.Y = (*v)[1].as_float();
    out.Z = (*v)[2].as_float();
    return out;
}

// File default_guns[mount] under its turret slot, creating the slot on first
// sight so turret_slots keeps ship.json's authored order. "rear" -> "Rear
// Turret"; the untagged default "turret" -> "Turret".
void add_turret_mount(ShipClass& c, const std::string& slot_id, int mount) {
    for (TurretSlot& t : c.turret_slots) {
        if (t.id == slot_id) { t.mounts.push_back(mount); return; }
    }
    TurretSlot t;
    t.id    = slot_id;
    t.label = slot_id == "turret" ? slot_id : slot_id + " Turret";
    if (!t.label.empty()) t.label[0] = (char)std::toupper((unsigned char)t.label[0]);
    t.mounts.push_back(mount);
    c.turret_slots.push_back(std::move(t));
}

// Parse a single ship.json file. On success appends to g_classes and
// returns true; on failure logs to stderr and returns false (the caller
// keeps walking the directory).
bool parse_one(const fs::path& path) {
    json::Value root = json::parse_file(path.string());
    if (!root.is_object()) {
        std::fprintf(stderr, "[ship_class] '%s' is not a JSON object\n",
                     path.string().c_str());
        return false;
    }

    ShipClass c;
    c.name           = opt_str(root, "name");
    c.display_name   = opt_str(root, "display_name", c.name);
    c.class_label    = opt_str(root, "class_label");
    c.ai_table       = opt_str(root, "ai_table");   // optional AI profile override
    c.atlas_manifest = opt_str(root, "atlas_manifest");

    if (c.name.empty()) {
        std::fprintf(stderr, "[ship_class] '%s' missing required 'name'\n",
                     path.string().c_str());
        return false;
    }

    // Faction — the JSON spells it lowercase ("merchant", "pirate", ...).
    if (auto fname = opt_str(root, "default_faction"); !fname.empty()) {
        Faction f = faction::from_name(fname);
        if (f == Faction::Count) {
            std::fprintf(stderr, "[ship_class] '%s': unknown faction '%s'\n",
                         c.name.c_str(), fname.c_str());
        } else {
            c.default_faction = f;
        }
    }

    // Hull category — capital ships get extra spawn/travel spacing.
    c.capital = opt_bool(root, "capital", c.capital);

    // Armor (base hull cm — independent of fitted ArmorType). The
    // old JSON 'armor_side_cm' is split across port + starboard here
    // (each face gets half the old cm) so ships keep their effective
    // total armor across the issue-30 cutover.
    c.armor_fore_cm = opt_num(root, "armor_fore_cm", c.armor_fore_cm);
    c.armor_aft_cm  = opt_num(root, "armor_aft_cm",  c.armor_aft_cm);
    c.armor_port_cm     = opt_num(root, "armor_port_cm",     c.armor_port_cm);
    c.armor_starboard_cm = opt_num(root, "armor_starboard_cm", c.armor_starboard_cm);

    // Mobility.
    c.cruise_speed      = opt_num(root, "cruise_speed",      c.cruise_speed);
    c.afterburner_speed = opt_num(root, "afterburner_speed", c.afterburner_speed);
    if (auto s = opt_str(root, "acceleration"); !s.empty()) {
        if (auto t = mobility::from_name(s); t != MobilityTier::Count) c.acceleration = t;
        else std::fprintf(stderr, "[ship_class] '%s': unknown acceleration tier '%s'\n",
                          c.name.c_str(), s.c_str());
    }
    if (auto s = opt_str(root, "max_ypr"); !s.empty()) {
        if (auto t = mobility::from_name(s); t != MobilityTier::Count) c.max_ypr = t;
        else std::fprintf(stderr, "[ship_class] '%s': unknown max_ypr tier '%s'\n",
                          c.name.c_str(), s.c_str());
    }

    // Per-class agility override (default 1.0).
    c.ypr_rate_multiplier = opt_num(root, "ypr_rate_multiplier", c.ypr_rate_multiplier);

    // Sensing.
    c.radar_range   = opt_num(root, "radar_range",   c.radar_range);
    c.weapons_range = opt_num(root, "weapons_range", c.weapons_range);

    // AI tuning (Privateer CNST f0/f1/f2/f3/f6, raw 1:1 world units). See
    // docs/ai_model.md §0 (source of truth) / §2.2 / §9.8. Per-pilot CNST
    // from the AIDS importer isn't wired yet, so we seed PER-FACTION
    // baselines here (the clean monotone tiers from the decoded table) and
    // let ship.json override. f0 is constant 600 across all vanilla pilots;
    // f1/f3 vary by role; f6 is the morale/caution axis (FAQ §5.1: LOW =
    // fanatical, HIGH = timid) and f2 the experience/accuracy tier.
    switch (c.default_faction) {
        case Faction::Merchant:                                        // tame + timid (flees early)
            c.maneuver_jitter_f3 = 30.0f;
            c.morale_f6          = 128.0f;
            break;
        case Faction::Kilrathi:                                        // skilled + fanatical (never flees)
            c.maneuver_jitter_f3 = 40.0f;
            c.skill_f2           = 40.0f;
            c.morale_f6          = 64.0f;
            break;
        case Faction::Confed:                                          // long-range escort, steady
            c.comms_f1           = 2100.0f;
            c.skill_f2           = 60.0f;
            c.morale_f6          = 102.0f;
            break;
        case Faction::Pirate:                                          // aggressive, stands and fights
            c.morale_f6          = 102.0f;
            break;
        default: break;                                                // hunter/militia keep 75 / 1500 / 45 / 76
    }
    // The scout drone is a special pilot (long sensor-flavoured bark, low
    // jitter, fight-to-death); keyed by registry name since it isn't its
    // own faction.
    if (c.name == "drone" || c.name == "sdrone") {
        c.comms_f1           = 1800.0f;
        c.maneuver_jitter_f3 = 40.0f;
        c.morale_f6          = 64.0f;   // fanatical: no flee tier (§9.6.1)
    }
    // ship.json explicit overrides win over the faction baseline.
    c.engage_f0          = opt_num(root, "cnst_f0", c.engage_f0);
    c.comms_f1           = opt_num(root, "cnst_f1", c.comms_f1);
    c.skill_f2           = opt_num(root, "cnst_f2", c.skill_f2);
    c.maneuver_jitter_f3 = opt_num(root, "cnst_f3", c.maneuver_jitter_f3);
    c.morale_f6          = opt_num(root, "cnst_f6", c.morale_f6);

    // AI personality. "standard" / "coward" — small enum today, room
    // for berserker/cautious/etc. as we add subtler behaviours.
    if (auto s = opt_str(root, "personality"); !s.empty()) {
        if      (s == "standard") c.personality = AIPersonality::Standard;
        else if (s == "coward")   c.personality = AIPersonality::Coward;
        else std::fprintf(stderr, "[ship_class] '%s': unknown personality '%s'\n",
                          c.name.c_str(), s.c_str());
    }

    // Slot caps.
    c.max_engine_level = (uint8_t)opt_num(root, "max_engine_level", c.max_engine_level);
    c.max_shield_level = (uint8_t)opt_num(root, "max_shield_level", c.max_shield_level);

    // Default fitted shield. Name points into the shield table loaded
    // earlier; resolve to a pointer now so spawn paths don't have to
    // re-look-up. Missing entry is warned but doesn't fail the load.
    // Armor is deliberately NOT a class default — it's a purchasable
    // upgrade fitted per-instance (see ship_class.h / Ship::fitted_armor).
    c.default_shield_name = opt_str(root, "default_shield");
    if (!c.default_shield_name.empty()) {
        c.default_shield = shield::find(c.default_shield_name);
        if (!c.default_shield) {
            std::fprintf(stderr, "[ship_class] '%s': default_shield '%s' not in table\n",
                         c.name.c_str(), c.default_shield_name.c_str());
        }
    }

    // Energy.
    c.energy_max      = opt_num(root, "energy_max",      c.energy_max);
    c.energy_recharge = opt_num(root, "energy_recharge", c.energy_recharge);

    // Cargo (trading; loaded for completeness).
    c.cargo_units     = (int)opt_num(root, "cargo_units",     0);
    c.cargo_units_max = (int)opt_num(root, "cargo_units_max", c.cargo_units);

    // Guns. Each entry: { offset_body: [x,y,z], type: "mass_driver",
    // forward_body?: [...], cone_half_angle_deg?: float, turret?: bool,
    // turret_slot?: "rear" }. turret=true flags an auto-firing turret mount
    // (firing.cpp, NPC and player alike); turret_slot names the purchasable
    // position it belongs to (#145, default "turret").
    if (auto* gs = root.find("default_guns"); gs && gs->is_array()) {
        for (const auto& gv : gs->as_array()) {
            if (!gv.is_object()) continue;
            GunMount m;
            m.offset_body  = read_vec3(gv.find("offset_body"));
            m.forward_body = read_vec3(gv.find("forward_body"), {0, 0, 1});
            const std::string tname = opt_str(gv, "type");
            const GunType t = gun::from_name(tname);
            if (t == GunType::Count) {
                std::fprintf(stderr, "[ship_class] '%s': unknown gun type '%s'\n",
                             c.name.c_str(), tname.c_str());
                continue;
            }
            m.type = t;
            m.cone_half_angle_deg = opt_num(gv, "cone_half_angle_deg", 1.0f);
            m.is_turret           = opt_bool(gv, "turret", false);
            if (m.is_turret)
                add_turret_mount(c, opt_str(gv, "turret_slot", "turret"),
                                 (int)c.default_guns.size());
            c.default_guns.push_back(m);
        }
    }

    // De-dup against an existing entry with the same name (shouldn't
    // happen since registry keys are directory names, but guard anyway).
    for (const auto& existing : g_classes) {
        if (existing.name == c.name) {
            std::fprintf(stderr, "[ship_class] duplicate name '%s' (ignored second copy)\n",
                         c.name.c_str());
            return false;
        }
    }
    g_classes.push_back(std::move(c));
    return true;
}

} // namespace

int ship_class::load_all(const std::string& ships_dir) {
    g_classes.clear();
    g_classes.reserve(32);

    if (!fs::is_directory(ships_dir)) {
        std::fprintf(stderr, "[ship_class] ships_dir '%s' is not a directory\n",
                     ships_dir.c_str());
        return 0;
    }

    int total_guns = 0;
    for (const auto& entry : fs::directory_iterator(ships_dir)) {
        if (!entry.is_directory()) continue;
        const fs::path ship_json = entry.path() / "ship.json";
        if (!fs::exists(ship_json)) continue;     // ships without ship.json
                                                  // are silently skipped —
                                                  // they're either WIP or
                                                  // pure-art assets
        if (!parse_one(ship_json)) continue;
        total_guns += (int)g_classes.back().default_guns.size();

        const auto& c = g_classes.back();
        std::printf("[ship_class] %-12s (%s, %s)  hull=%.0f/%.0f/%.0f/%.0f cm  "
                    "speed=%.0f/%.0f m/s  guns=%zu  turret_slots=%zu\n",
                    c.name.c_str(),
                    c.class_label.empty() ? "?" : c.class_label.c_str(),
                    faction::to_name(c.default_faction),
                    c.armor_fore_cm, c.armor_aft_cm,
                    c.armor_port_cm, c.armor_starboard_cm,
                    c.cruise_speed, c.afterburner_speed,
                    c.default_guns.size(), c.turret_slots.size());
    }

    std::printf("[ship_class] loaded %zu ship classes (%d total gun mounts)\n",
                g_classes.size(), total_guns);
    return (int)g_classes.size();
}

const ShipClass* ship_class::find(std::string_view name) {
    for (const auto& c : g_classes) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

const std::vector<ShipClass>& ship_class::all() { return g_classes; }
