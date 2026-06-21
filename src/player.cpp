// -----------------------------------------------------------------------------
// player.cpp — PlayerState helpers.
//
// Pure data manipulation: no rendering, no registry access, no catalog
// lookups (callers resolve ShipClass* / commodity ids and pass them
// in). That keeps every function here trivially unit-testable and the
// future save/load layer free of hidden dependencies.
// -----------------------------------------------------------------------------

#include "player.h"

#include "ship_class.h"

#include <algorithm>
#include <limits>

namespace player {

PlayerState new_game(const std::string& start_system) {
    PlayerState p;
    p.credits         = k_new_game_credits;
    p.ship_class_name = "centurion";
    // Centurion loadout: 4x Tachyon Cannon, matching the player-spawn
    // block in main.cpp (which sources the same types from the
    // Centurion ShipClass). This pure-data layer keeps no catalog
    // dependency (see file header), so the literal quadruple is
    // intentional — it just has to agree with the class default_guns,
    // which it does (np-e3x).
    p.gun_mounts      = { "tachyon_cannon", "tachyon_cannon",
                           "tachyon_cannon", "tachyon_cannon" };
    p.current_system  = start_system;
    // Starter missile loadout (np-zte.2). Afterburner now shares the
    // ship's energy bank (no separate fuel tank to initialize).
    for (int i = 0; i < 3; ++i) p.missiles[i] = k_new_game_missiles[i];
    // rep zero-initialized = unknown stranger; faction baselines decide
    // first impressions (see faction.h).
    return p;
}

// ---- credits ----------------------------------------------------------------

bool can_afford(const PlayerState& p, int64_t cost) {
    return cost >= 0 && p.credits >= cost;
}

bool spend_credits(PlayerState& p, int64_t cost) {
    if (!can_afford(p, cost)) return false;
    p.credits -= cost;
    return true;
}

void add_credits(PlayerState& p, int64_t amount) {
    if (amount <= 0) return;   // negative "additions" go through spend_credits
    // Clamp instead of wrap — see header.
    constexpr int64_t k_max = std::numeric_limits<int64_t>::max();
    p.credits = (p.credits > k_max - amount) ? k_max : p.credits + amount;
}

// ---- cargo ------------------------------------------------------------------

int cargo_units_used(const PlayerState& p) {
    int used = 0;
    for (const CargoEntry& e : p.cargo) used += e.units;
    return used;
}

int cargo_capacity(const PlayerState& p, const ShipClass* klass) {
    if (!klass) return 0;
    const int base = klass->cargo_units;
    // +25% Cargo Expansion, floor-rounded — Tarsus 100 -> 125, matching
    // the original game's upgrade math.
    return p.cargo_expansion ? base + base / 4 : base;
}

bool add_cargo(PlayerState& p, const std::string& commodity_id,
               int units, int price_per_unit, int capacity) {
    if (units <= 0) return false;
    if (cargo_units_used(p) + units > capacity) return false;

    for (CargoEntry& e : p.cargo) {
        if (e.commodity_id != commodity_id) continue;
        // Blend bought_at_price by unit-weighted average so the profit
        // display stays honest across multiple buys at different
        // prices. int64 intermediate dodges overflow on huge stacks.
        const int64_t total_paid = (int64_t)e.bought_at_price * e.units
                                 + (int64_t)price_per_unit    * units;
        e.units          += units;
        e.bought_at_price = (int)(total_paid / e.units);
        return true;
    }
    p.cargo.push_back({ commodity_id, units, price_per_unit });
    return true;
}

// ---- ordnance ---------------------------------------------------------------

int missile_count(const PlayerState& p, int type_index) {
    if (type_index < 0 || type_index >= 3) return 0;
    return p.missiles[type_index];
}

bool consume_missile(PlayerState& p, int type_index) {
    if (type_index < 0 || type_index >= 3) return false;
    if (p.missiles[type_index] <= 0)       return false;   // empty rack
    --p.missiles[type_index];
    return true;
}

void add_missiles(PlayerState& p, int type_index, int count) {
    if (type_index < 0 || type_index >= 3 || count <= 0) return;
    p.missiles[type_index] += count;
}

// drain_afterburner / regen_afterburner / refuel_full removed (np-zte.2):
// afterburner now drains from the player Ship's energy_gj. Callers touch
// energy_gj directly; firing.cpp owns the regen tick.

bool remove_cargo(PlayerState& p, const std::string& commodity_id, int units) {
    if (units <= 0) return false;
    for (size_t i = 0; i < p.cargo.size(); ++i) {
        CargoEntry& e = p.cargo[i];
        if (e.commodity_id != commodity_id) continue;
        if (e.units < units) return false;   // can't sell what you don't have
        e.units -= units;
        if (e.units == 0) p.cargo.erase(p.cargo.begin() + i);
        return true;
    }
    return false;   // commodity not in hold at all
}

} // namespace player
// 1781715641481656000
