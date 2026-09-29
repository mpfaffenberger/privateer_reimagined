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

// ---- transactions (mutate ONLY through player:: credit helpers) --------------
// Fit scanner `id`, trading the currently fitted one back at full price, so
// the charge is (new price - old price). Refused (false, no mutation) for an
// unknown id, the scanner already fitted, or an unaffordable net cost.
bool buy(PlayerState& p, const std::string& id);

// Sell the fitted scanner back for its full price, leaving none fitted.
// Refused when nothing is fitted.
bool sell(PlayerState& p);

} // namespace scanner
