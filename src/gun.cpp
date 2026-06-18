#include "gun.h"

#include "json.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_map>

GunStats g_gun_stats[kGunTypeCount] = {};

namespace {

// Map the human-readable names in assets/data/privateer_ship_data.json to our
// enum values. The canonical name comes straight from the JSON so the
// docs file remains source-of-truth; the lowercase_underscore alias is
// what ship.json files use to reference the type.
struct NameMap {
    GunType     type;
    const char* json_name;        // exact match for the source data
    const char* short_name;       // lowercase_underscore for ship.json
    HMM_Vec3    tracer_color;     // canonical-feeling colour per gun
};

constexpr NameMap k_name_map[] = {
    { GunType::Laser,            "Laser",              "laser",              {1.00f, 0.20f, 0.20f} },
    { GunType::MassDriver,       "Mass Driver",        "mass_driver",        {1.00f, 0.55f, 0.10f} },
    { GunType::MesonBlaster,     "Meson Blaster",      "meson_blaster",      {0.30f, 1.00f, 0.40f} },
    { GunType::NeutronGun,       "Neutron Gun",        "neutron_gun",        {0.40f, 0.95f, 1.00f} },
    { GunType::ParticleCannon,   "Particle Cannon",    "particle_cannon",    {1.00f, 0.95f, 0.30f} },
    { GunType::TachyonCannon,    "Tachyon Cannon",     "tachyon_cannon",     {1.00f, 0.30f, 1.00f} },
    { GunType::IonicPulseCannon, "Ionic Pulse Cannon", "ionic_pulse_cannon", {0.30f, 0.40f, 1.00f} },
    { GunType::PlasmaGun,        "Plasma Gun",         "plasma_gun",         {1.00f, 0.85f, 0.55f} },
    { GunType::SteltekGun,       "Steltek Gun",        "steltek_gun",        {0.30f, 1.00f, 0.50f} },
};
static_assert(sizeof(k_name_map)/sizeof(k_name_map[0]) == kGunTypeCount,
              "k_name_map must list every GunType");

// JSON helper: read an optional numeric field. Returns 0 (and reports
// "missing" via *was_null) when the source JSON has `null` (which is
// how the docs flag partial-data rows like Plasma Gun RF / Steltek RF).
float read_num_or_null(const json::Value* v, bool& was_null) {
    if (!v || v->is_null()) { was_null = true; return 0.0f; }
    return v->as_float();
}

} // namespace

bool gun::load_table(const std::string& json_path) {
    json::Value root = json::parse_file(json_path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[gun] could not parse '%s'\n", json_path.c_str());
        return false;
    }
    const json::Value* arr = root.find("guns");
    if (!arr || !arr->is_array()) {
        std::fprintf(stderr, "[gun] '%s': missing 'guns' array\n", json_path.c_str());
        return false;
    }

    // Index entries in the JSON by canonical name so we can look up each
    // GunType regardless of source-array ordering.
    std::unordered_map<std::string, const json::Value*> by_name;
    for (const auto& v : arr->as_array()) {
        if (!v.is_object()) continue;
        if (auto* n = v.find("Name"); n && n->is_string()) {
            by_name[n->as_string()] = &v;
        }
    }

    int n_loaded = 0, n_complete = 0;
    for (const auto& nm : k_name_map) {
        auto it = by_name.find(nm.json_name);
        if (it == by_name.end()) {
            std::fprintf(stderr, "[gun] '%s' missing from '%s'\n",
                         nm.json_name, json_path.c_str());
            continue;
        }
        const json::Value& v = *it->second;
        GunStats& g = g_gun_stats[(int)nm.type];
        g.name           = nm.json_name;
        g.tracer_color   = nm.tracer_color;
        // Load canonical fields verbatim from the source data.
        // Source lists projectile speed in kps; we treat as m/s so the
        // same numbers feel right at engine scale ("k" is flavour, not
        // literal — see the speed-table design discussion).
        g.damage_cm      = v.find("Damage (cm)")    ? v["Damage (cm)"].as_float() : 0.0f;
        g.range_m        = v.find("Range (m)")      ? v["Range (m)"].as_float()   : 0.0f;
        g.speed_mps      = v.find("Speed (kps)")    ? v["Speed (kps)"].as_float() : 0.0f;
        bool refire_null = false, energy_null = false;
        g.refire_delay_s = read_num_or_null(v.find("Refire Delay (s)"), refire_null);
        g.energy_cost_gj = read_num_or_null(v.find("Energy Use (GJ)"),  energy_null);
        g.complete       = !refire_null && !energy_null;

        // Tuning knobs. Applied AFTER load so they multiply the loaded
        // values rather than the zero-initialised pre-load values.
        //
        // FIRING CADENCE — now CANONICAL (restored May 2025).
        //   refire × 1.0   — guns fire at the real Privateer cadence
        //                     (e.g. Mass Driver every 0.35s) instead of
        //                     the old 0.167 "6x arcade" tracer stream.
        //   damage × 1.0   — canonical per-shot armour penetration.
        //   energy × 1.0   — canonical per-shot energy cost.
        //
        //   The old build coupled all three at 0.167 (6x faster fire,
        //   6x smaller per-shot damage + energy). Because DPS =
        //   damage/refire and energy-drain/sec = energy/refire, scaling
        //   damage, energy AND refire by the SAME factor leaves both
        //   per-second rates UNCHANGED — the 0.167 build was already
        //   canonical in net DPS / energy economy; it only differed in
        //   *texture* (fast weak tracers vs slow punchy shots).
        //   Restoring all three to 1.0 therefore preserves net balance
        //   (time-to-kill, energy starvation behaviour) while making
        //   shots fire at the genuine Privateer rate with full per-shot
        //   magnitude. The JSON refire values were also corrected to
        //   Damage/DamageRate so the canonical DPS column is now exact.
        //
        // ENGINE-SCALE FEEL TWEAKS — still demo (NOT firing-rate related).
        //   range × 2.0    — bullets relevant at long engagement
        //                     distances after afterburner extensions.
        //   speed × 2.0    — projectiles snap to target instead of
        //                     drifting; proper space-shooter feel.
        //   Set these two to 1.0 for full canon range/velocity.
        constexpr float k_range_multiplier  = 2.0f;  // demo feel tweak
        constexpr float k_speed_multiplier  = 2.0f;  // demo feel tweak
        constexpr float k_refire_multiplier = 1.0f;  // canonical cadence
        constexpr float k_energy_multiplier = 1.0f;  // canonical per-shot
        constexpr float k_damage_multiplier = 1.0f;  // canonical per-shot
        g.range_m        *= k_range_multiplier;
        g.speed_mps      *= k_speed_multiplier;
        g.refire_delay_s *= k_refire_multiplier;
        g.energy_cost_gj *= k_energy_multiplier;
        g.damage_cm      *= k_damage_multiplier;
        ++n_loaded;
        if (g.complete) ++n_complete;

        // One-time load dump: prove effective post-multiplier cadence and
        // DPS match the canonical Damage Rate column in the JSON. DPS is
        // computed from the effective (post-multiplier) numbers; with the
        // firing multipliers at 1.0 it should equal Damage Rate exactly.
        if (g.complete && g.refire_delay_s > 0.0f) {
            std::printf("[gun] %-18s refire=%.3fs dps=%.2f\n",
                        g.name, g.refire_delay_s,
                        g.damage_cm / g.refire_delay_s);
        }
    }

    std::printf("[gun] loaded %d gun types (%d complete) from %s\n",
                n_loaded, n_complete, json_path.c_str());
    return true;
}

GunType gun::from_name(std::string_view s) {
    // Case- and underscore-tolerant — accept "Mass Driver", "mass_driver",
    // "MASS_DRIVER" etc. uniformly. Cheap because the table is tiny.
    std::string norm;
    norm.reserve(s.size());
    for (char c : s) {
        if (c == ' ') norm.push_back('_');
        else norm.push_back((char)std::tolower((unsigned char)c));
    }
    for (const auto& nm : k_name_map) {
        if (norm == nm.short_name) return nm.type;
    }
    return GunType::Count;
}

const char* gun::to_name(GunType t) {
    if ((int)t < 0 || (int)t >= kGunTypeCount) return "?";
    return k_name_map[(int)t].short_name;
}
