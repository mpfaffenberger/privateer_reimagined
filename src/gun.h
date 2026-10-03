#pragma once
// -----------------------------------------------------------------------------
// gun.h — energy-weapon types and per-type stats.
//
// Privateer ships in the manual carry roughly nine kinds of forward gun.
// We enumerate them as compile-time constants so call-sites can write
// `GunType::MassDriver` and so the JSON loader has a fixed set to match.
// The actual numbers (damage in cm, projectile speed, refire delay,
// energy cost, range, tracer color) live in g_gun_stats[], populated at
// startup from assets/data/privateer_ship_data.json — single source of truth for
// the canonical balance numbers.
//
// Damage units are CM OF DURASTEEL — same unit as armor and shields. A
// hit subtracts directly: no scaling, no "shield damage" vs "armor
// damage" distinction (that's how the original game worked, and it makes
// the damage pipeline trivially simple).
//
// Missiles and torpedoes live in missile.*. Turreted guns are mounts flagged
// via GunMount::is_turret; firing.cpp owns their per-mount lead/arc model for
// both NPC and player ships.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>
#include <cstdint>
#include <string>
#include <string_view>

enum class GunType : uint8_t {
    Laser = 0,
    MassDriver,
    MesonBlaster,
    NeutronGun,
    ParticleCannon,
    TachyonCannon,
    IonicPulseCannon,
    PlasmaGun,
    SteltekGun,        // endgame; stats partially specified in source data
    Count
};
constexpr int kGunTypeCount = (int)GunType::Count;

struct GunStats {
    const char* name = "?";       // "Laser", "Mass Driver", ...
    float damage_cm        = 0;   // armor/shield penetration per shot
    float speed_mps        = 0;   // projectile velocity
    float range_m          = 0;   // despawn after this distance
    float refire_delay_s   = 0;   // min interval between shots
    float energy_cost_gj   = 0;   // drains the ship's shared pool
    HMM_Vec3 tracer_color  = {1, 1, 1};
    bool   complete        = false;  // true if all numeric fields populated
};

// Populated by gun::load_table() at startup. Indexed by GunType.
extern GunStats g_gun_stats[kGunTypeCount];

// Per-mount data on a ship — geometry + which gun is fitted.
struct GunMount {
    HMM_Vec3 offset_body  = {0, 0, 0};       // mount position, body frame
    HMM_Vec3 forward_body = {0, 0, 1};       // fire direction, body frame
    GunType  type         = GunType::Laser;
    float    cone_half_angle_deg = 1.0f;     // 0 = strictly fixed
    // Turret mounts auto-aim with lead prediction at the ship's perceived
    // hostile, fire FREE (no energy gate) along their own forward_body cone
    // — independent of controller.fire_guns. Fixed
    // forward guns leave this false and keep the legacy nose-forward path.
    bool     is_turret    = false;
};

namespace gun {

// Parse the "guns" array from assets/data/privateer_ship_data.json (or any path).
// Logs `[gun] loaded N gun types (M complete)`. Entries with null
// refire_delay or energy_cost (the Steltek/RF/Mega rows in the source
// data) get loaded with `complete=false` so we know not to spawn them
// from AI loadouts until the numbers are filled in.
//
// Returns true on success; false (and logs to stderr) on parse error.
bool load_table(const std::string& json_path);

// String <-> enum. Names are the lowercase_underscore form of the
// canonical Privateer name: "laser", "mass_driver", "meson_blaster",
// "neutron_gun", "particle_cannon", "tachyon_cannon",
// "ionic_pulse_cannon", "plasma_gun", "steltek_gun". Returns
// GunType::Count on unknown.
GunType     from_name(std::string_view s);
const char* to_name(GunType t);

// Player-facing name, the canonical Privateer spelling ("Meson Blaster").
// UI shows this, never the short id (#747). The string_view overload takes
// a short id and hands unknown ids back unchanged.
const char* display_name(GunType t);
std::string display_name(std::string_view short_name);

} // namespace gun
