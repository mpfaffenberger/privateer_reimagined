#pragma once
// -----------------------------------------------------------------------------
// scanner.h — the player's radar / scanner product line (#143).
//
// Privateer sold nine scanners from three vendors (gamefaq 4.6.6): Iris
// (monochrome), Hunter AW (friend-or-foe colour) and B&S (full colour), each
// in three tiers that add Target Lock and then ITTS. This module is the
// data-driven catalog (equipment_prices.json `scanners`), the buy/sell
// transactions the Equipment screen calls, and the capability queries the
// HUD / perception / missile-lock paths key off.
//
// Who reads what:
//   * perception::radar_range_m — the fitted scanner's detection sphere
//     replaces the hull's class default. NPCs never fit one, so they keep
//     ShipClass::radar_range.
//   * HUD — contacts are stance-coloured only with a colour-IFF scanner;
//     the ITTS lead pip (and the matching aim gimbal) only with ITTS.
//   * missile lock — guided missiles only lock with a Target Lock scanner;
//     without one they launch unguided ("won't guide", gamefaq Q&A).
//   * identification (#516) — the targeted contact reads UNKNOWN until the
//     scanner wins a once-per-second identify_pct_per_s roll on it
//     (IdentifyTimer / tick_identify below; the flag lives on the contact,
//     Ship::identified_by_player).
//
// The owned scanner is PlayerState::scanner_id (by id, like armor_name);
// the live Ship caches the resolved ScannerType* as fitted_scanner at the
// loadout seam. A null ScannerType* means "no scanner fitted": hull-default
// range, monochrome, no lock, no ITTS. Scanners survive hull swaps (gamefaq:
// "your scanner ... transferred"), so outfitting::buy_hull leaves them be.
//
// Pure data (no ImGui) so the offline harness tools/test_scanner.cpp links it.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

struct PlayerState;

struct ScannerType {
    std::string id;                  // stable catalog + save key ("hunter_aw_6i")
    std::string name;                // display name ("Hunter AW 6i")
    int64_t     price       = 0;     // credits; also the full sell-back refund
    float       range_m     = 0.0f;  // detection sphere (0 = use hull default)
    bool        color_iff   = false; // stance-coloured contacts vs monochrome
    bool        target_lock = false; // guided missiles can acquire a lock
    bool        itts        = false; // lead-prediction reticle + aim gimbal
    float       identify_pct_per_s = 0.0f; // % chance per second to ID the target
};

namespace scanner {

// Load the `scanners` array from equipment_prices.json. Idempotent (reload
// replaces). Returns the number of catalog entries; logs one summary line.
int load(const std::string& equip_prices_path);

// Every scanner for sale, in authored order.
const std::vector<ScannerType>& catalog();

// Catalog lookup by id. nullptr for "" (no scanner) or an unknown id.
const ScannerType* find(const std::string& id);

// ---- capability queries (null == no scanner fitted) -------------------------
inline float range_m(const ScannerType* s, float hull_default_m) {
    return (s && s->range_m > 0.0f) ? s->range_m : hull_default_m;
}
inline bool color_iff(const ScannerType* s)   { return s && s->color_iff; }
inline bool target_lock(const ScannerType* s) { return s && s->target_lock; }
inline bool itts(const ScannerType* s)        { return s && s->itts; }

// ---- identification (#516) ---------------------------------------------------
// Per-target identify clock: time the CURRENT target has been held since
// its last roll, carried across frames. Retargeting restarts it.
struct IdentifyTimer {
    uint32_t target_id = 0;     // contact the carry belongs to (0 = none)
    float    carry_s   = 0.0f;  // time since the last roll on target_id
};

// Advance `t` by dt_s on `target_id`; returns how many 1 s identify rolls
// fell due. A new target_id (or 0 = nothing targeted) restarts the clock.
int identify_rolls_due(IdentifyTimer& t, uint32_t target_id, float dt_s);

// One roll with u01 in [0,1). No scanner or a 0% scanner never succeeds.
inline bool identify_roll(const ScannerType* s, float u01) {
    return s && u01 * 100.0f < s->identify_pct_per_s;
}

// Tick + roll: true when this call identified `target_id`. `roll01` is any
// callable returning a uniform float in [0,1) (injected so tests are exact).
template <class Roll01>
bool tick_identify(IdentifyTimer& t, uint32_t target_id, const ScannerType* s,
                   float dt_s, Roll01&& roll01) {
    for (int n = identify_rolls_due(t, target_id, dt_s); n > 0; --n)
        if (identify_roll(s, roll01())) return true;
    return false;
}

// ---- transactions (mutate ONLY through player:: credit helpers) --------------
// Fit scanner `id`, trading the currently fitted one back at full price, so
// the charge is (new price - old price). Refused (false, no mutation) for an
// unknown id, the scanner already fitted, or an unaffordable net cost.
bool buy(PlayerState& p, const std::string& id);

// Sell the fitted scanner back for its full price, leaving none fitted.
// Refused when nothing is fitted.
bool sell(PlayerState& p);

} // namespace scanner
