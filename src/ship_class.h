#pragma once
// -----------------------------------------------------------------------------
// ship_class.h — per-ship-type design data ("the Talon weighs X").
//
// One ShipClass per kind of ship (talon, tarsus, centurion, ...). Loaded
// once at startup from assets/ships/<name>/ship.json next to the existing
// atlas_manifest.json — they're the visual side and stat side of the
// same logical ship. Pointers into g_ship_classes are stable for the
// program's lifetime; per-instance Ship structs (next commit) will hold
// `const ShipClass*` rather than copying.
//
// Mobility numbers come straight from the assets/data/privateer_ship_data.json
// canonical table: top speed in m/s (treating "kps" as flavour),
// acceleration + YPR as descriptive tiers (mobility.h supplies the
// multipliers). Armor in cm is per-facing (Fore/Aft/Side, sides
// symmetric L=R).
//
// Out of scope for v1:
//   * upgrade economy. ShipClass declares max engine/shield levels but
//     nothing reads them yet — the per-class default loadout is
//     authoritative for combat balance.
//   * turrets. Centurion etc. have rear/top/bottom mounts; we ignore
//     them and treat their fixed forward guns as the full loadout.
//   * cargo. The Galaxy carries 150 units, the Tarsus 100 — fields are
//     loaded so the trading layer can read them later, but combat
//     doesn't care today.
// -----------------------------------------------------------------------------

#include "faction.h"
#include "gun.h"
#include "mobility.h"

// AI personality — gates how the state machine reacts to perceived
// hostiles. Standard ships fight (Engage) and only flee at low HP;
// Cowards (merchants, civilians) flee on sight regardless of health.
// Lives on the class because it's a property of the ship's role, not
// the spawn — every Tarsus is a merchant by trade.
enum class AIPersonality : uint8_t {
    Standard = 0,   // Engage hostiles; Flee only when hurt (default)
    Coward,         // Flee on sight, never Engage
};

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ShieldType;
struct ArmorType;

struct ShipClass {
    // ---- identity ------------------------------------------------------
    std::string name;             // lowercase: "talon", "tarsus" — used as registry key
    std::string display_name;     // "Talon", "Tarsus"
    std::string class_label;      // "Light Fighter (used by militia, ...)"
    std::string atlas_manifest;   // path to existing sprite atlas, e.g.
                                  // "ships/talon/atlas_manifest.json"
    Faction default_faction = Faction::Civilian;

    // ---- hull category -------------------------------------------------
    // Capital ships (Drayman / Paradigm / Kamekh and any future cruisers)
    // are huge and slow. Marked true via "capital": true in ship.json so
    // the encounter spawner + travel steering can keep them well clear of
    // one another (a hull-to-hull capital ram is an instant double KO).
    // Data-driven flag, not a hardcoded name list — add the flag to a new
    // hull and the spacing rules apply automatically.
    bool capital = false;

    // ---- hull (cm of durasteel) ----------------------------------------
    // Base armor that's always present. Add ArmorType::xxx_cm for the
    // total per-facing protection of an instance.
    float armor_fore_cm = 10.0f;
    float armor_aft_cm  = 10.0f;
    float armor_side_cm =  8.0f;

    // ---- mobility ------------------------------------------------------
    float        cruise_speed       = 300.0f;   // m/s top speed without afterburner
    float        afterburner_speed  = 0.0f;     // 0 = no afterburner fitted
    MobilityTier acceleration       = MobilityTier::Average;
    MobilityTier max_ypr            = MobilityTier::Average;

    // Per-class agility override on top of the mobility tier. Multiplied
    // into the tier-derived rate, so 1.0 = no override (default), 1.75 =
    // "this ship turns 75% faster than its catalog tier suggests". Lets
    // an ace-pilot variant or a hot-rod retrofit feel sharper than its
    // class's nominal YPR without inventing new mobility tiers above
    // Excellent. Applies uniformly to yaw + pitch + roll.
    float        ypr_rate_multiplier = 1.0f;

    // ---- sensing / engagement -----------------------------------------
    // Privateer-canonical radar reach. weapons_range is a soft AI hint —
    // "engage within this distance". Each gun has its own true range_m.
    //
    // radar_range is the SENSOR / DETECTION / AWARENESS sphere — this is
    // what actually wakes the AI and starts an engage (gated by faction
    // stance in perception.cpp). Live-corrected to the Privateer-canonical
    // 15000 world units (docs/ai_model.md §11 / sensor cull 0x3a98). It is
    // NOT CNST f1 — f1 is the cosmetic comm/taunt range (see comms_f1).
    float radar_range   = 15000.0f;   // m  (sensor/detection, §11)
    float weapons_range =  3000.0f;   // m

    // ---- AI tuning: Privateer CNST skill vector (raw 1:1 world units) --
    // Decoded from PRCD.EXE + live-tested on vanilla; see docs/ai_model.md
    // §2.2 / §9.4 / §9.8 / §11. Stored raw (Privateer scale); consumers
    // multiply by ship_ai's single `kPvtScale` so the whole stack can be
    // rescaled in one place later. Per-faction defaults are applied in
    // ship_class.cpp when ship.json doesn't override them.
    //
    //   engage_f0          gun / break-off radius: the AI breaks off when
    //                      dist < (f0 + selfR + targetR). [C]+[L] (§11.3)
    //   comms_f1           COMMS / TAUNT chatter range — when the NPC
    //                      barks at a hostile. NOT detection. [L] (§11.1)
    //   maneuver_jitter_f3 evade/jink jitter gain: pirates/aces 75 (jerky),
    //                      merchants 30 (smooth), Kilrathi/drone 40. [C]
    //   skill_f2           experience/accuracy skill tier (FAQ §5.1 experience
    //                      axis, pairs with f3): elites 60, Kilrathi 40,
    //                      baseline 45. [I] no traceable consumer (§7.13.6).
    //   morale_f6          MORALE / CAUTION (FAQ §5.1 morale axis): LOW =
    //                      fanatical (Kilrathi 64, fight-to-death), HIGH =
    //                      timid (merchant 128, flees early). Mapped to a flee
    //                      HP threshold in ship_ai. [I tuning] — no resident
    //                      consumer found (§7.13.5); role from FAQ + faction
    //                      values, NOT a debugger poke (the old "aggression"
    //                      reading was noise, superseded by docs §0).
    float engage_f0          = 600.0f;
    float comms_f1           = 1500.0f;
    float maneuver_jitter_f3 = 75.0f;
    float skill_f2           = 45.0f;
    float morale_f6          = 76.0f;

    // ---- AI personality -----------------------------------------------
    // Gates the state machine's response to perceived hostiles.
    // Standard ships fight; Cowards (merchants, civilians) flee on
    // sight. See the enum at the bottom of this header.
    AIPersonality personality = AIPersonality::Standard;

    // Optional AI logic-table override (assets/ai/<name>.ai.json). When set,
    // ai_brain resolves this ship's brain to the named table FIRST (before the
    // per-faction / default tables). Lets heavy gunships (Orion/Galaxy) run a
    // "heavy" standoff profile while light fighters use the default dogfight.
    std::string ai_table;

    // ---- slot caps for the upgrade economy (informational v1) ---------
    uint8_t max_engine_level = 1;
    uint8_t max_shield_level = 1;

    // ---- default fitted loadout ---------------------------------------
    // Each instance starts with these unless the spawner overrides. The
    // string fields are looked up against the shield/armor tables at
    // ShipClass load time and resolved to pointers below; if the lookup
    // fails the pointer is null and the loader logs a warning.
    std::string default_shield_name;
    std::string default_armor_name;
    const ShieldType* default_shield = nullptr;
    const ArmorType*  default_armor  = nullptr;

    std::vector<GunMount> default_guns;

    // ---- energy --------------------------------------------------------
    float energy_max      = 200.0f;   // GJ
    float energy_recharge =  30.0f;   // GJ/s

    // ---- cargo (trading layer; combat ignores) ------------------------
    int cargo_units      = 0;
    int cargo_units_max  = 0;
};

namespace ship_class {

// Scan `assets/ships/*/ship.json`, build the registry. Logs one line per
// ship loaded plus a final summary. Safe to call before or after the
// gun/shield/armor table loaders, but they should all be loaded before
// any ship instance is spawned. Returns the count of successfully
// loaded ship classes (0 means nothing in assets/ships had a ship.json).
int load_all(const std::string& ships_dir);

// Look up by registry key (e.g. "talon"). Returns nullptr on miss.
// Pointers are stable for the program's lifetime.
const ShipClass*               find(std::string_view name);
const std::vector<ShipClass>&  all();

} // namespace ship_class
