#pragma once
// -----------------------------------------------------------------------------
// repair.h — paid hull repair + rearm at a base (np-zte.2).
//
// A base service: for credits, restore the player's hull armor to full,
// refill the afterburner tank, and restock missiles. The hull state lives
// on the in-flight Ship (transient, per-session) while fuel + ammo live on
// PlayerState (persistent), so the repair functions take BOTH and route
// every credit movement through player::spend_credits (one enforcement
// path, same discipline as outfitting.cpp).
//
// Pricing scales with what's actually consumed:
//   * hull   — credits per cm of armor missing across all facings, so a
//              lightly-scratched hull is cheap and a near-wreck is dear.
//   * fuel   — credits per unit of afterburner fuel missing.
//   * ammo   — flat per-missile restock price, per type, up to a cap.
// All constants live in repair.cpp and are the feature's only tuning knobs.
//
// Headless-safe: links against ship.cpp + player.cpp with no UI/audio, so
// the offline harness can prove the credit math + heal without a window.
// -----------------------------------------------------------------------------

#include <cstdint>

struct Ship;
struct PlayerState;

namespace repair {

// What a full-service visit would cost RIGHT NOW, broken out so the UI can
// show line items and disable buttons the player can't afford. `total` is
// the sum; a field is 0 when nothing of that kind needs doing.
struct Quote {
    int64_t hull_cost    = 0;   // restore armor to full
    int64_t fuel_cost    = 0;   // top off afterburner
    int64_t missile_cost = 0;   // restock to the standard loadout
    int64_t total        = 0;
    bool    hull_damaged = false;
    bool    fuel_low     = false;
    bool    missiles_low = false;
};

// Price the work. `ship` may be null (no player ship yet) — hull_cost is
// then 0. Never mutates anything.
Quote quote(const Ship* ship, const PlayerState& p);

// Repair the hull armor to full (via ship::heal_to_full) for hull_cost.
// Returns false (no mutation) if the ship is null, undamaged, or the player
// can't afford it. Logs the before/after for the validation trail.
bool repair_hull(Ship& ship, PlayerState& p);

// Top off the afterburner tank for fuel_cost. False if already full or
// unaffordable.
bool refuel(PlayerState& p);

// Restock missiles to the standard loadout for missile_cost. False if
// already stocked or unaffordable.
bool rearm(PlayerState& p);

} // namespace repair
