// -----------------------------------------------------------------------------
// player.cpp — PlayerState helpers.
//
// Pure data manipulation: no rendering, no registry access, no catalog
// lookups (callers resolve ShipClass* / commodity ids and pass them
// in). That keeps every function here trivially unit-testable and the
// future save/load layer free of hidden dependencies.
// -----------------------------------------------------------------------------

#include "player.h"

#include "commodity.h"   // is_contraband (Phase 1.1) — only needed by .cpp body
#include "missile.h"
#include "ship_class.h"

#include <algorithm>
#include <limits>

namespace player {

PlayerState new_game(const std::string& start_system) {
    PlayerState p;
    p.credits         = k_new_game_credits;
    // Canonical Privateer start (np-3dp.25): a stock 'barfy' Tarsus with a
    // SINGLE laser cannon (not the class-default twin mass drivers) and a
    // single missile launcher loaded with 4 heat-seekers. The spawn path in
    // main.cpp fits these onto the live ship via apply_player_loadout, so
    // the data here is the single source of truth for the starting hull.
    p.ship_class_name = "tarsus";
    p.gun_mounts      = { MountSlot{"laser"} }; // one laser, one muzzle
    p.current_system  = start_system;
    // Start docked at Achilles Mining Base (Troy) — you begin in the
    // concourse, the way the 1995 game drops you on a base.
    p.docked           = true;
    p.last_docked_base = "achilles";
    // Start fitted with Shield Generator 1 (10cm / facing). Privateer
    // never drops you WITHOUT a shield gen — np-3dp.28.
    p.shield_level = 1;
    // Starter missile loadout (np-zte.2): 4 heat-seekers, nothing else.
    // Afterburner shares the ship's energy bank (no separate fuel tank).
    for (int i = 0; i < 3; ++i) p.missiles[i] = k_new_game_missiles[i];
    // No starter torpedoes -- the dealer is the only place to load them,
    // so a new pilot starts empty on the torpedo rack.
    p.torpedoes = 0;
    // Hardware: Tarsus starts with one missile launcher (LEFT hardpoint)
    // and no torpedo launchers at all.
    p.missile_launcher_left  = true;
    p.missile_launcher_right = false;
    p.torpedo_launcher_left  = false;
    p.torpedo_launcher_right = false;
    // Universal tractor (#83): every new ship has the tractor beam from
    // the start. #82 stops the dealer selling it, so this is now the only
    // way to acquire one — every pilot can pull loot on day one. The
    // flag stays in the save schema in case a future design wants to
    // gate pulling (e.g. a heavier cargo tractor that ships don't all
    // mount).
    p.has_tractor_beam = true;
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
    // Phase 4 Wave 1 (#81): unified-hold accounting. Each item's qty
    // contributes to capacity. A Weapon/Upgrade item has qty=1 (one
    // cargo space each); a Salvage/Commodity-kind stack counts as
    // `qty` cargo spaces.
    for (const inventory::InventoryItem& it : p.items) used += it.qty;
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
    if (type_index < 0 || type_index >= kMissileTypeCount) return 0;
    if (type_index == (int)MissileType::TORPEDO) return p.torpedoes;
    return p.missiles[type_index];
}

bool consume_missile(PlayerState& p, int type_index) {
    if (type_index < 0 || type_index >= kMissileTypeCount) return false;
    if (type_index == (int)MissileType::TORPEDO) {
        if (p.torpedoes <= 0) return false;
        --p.torpedoes;
        return true;
    }
    if (p.missiles[type_index] <= 0) return false;   // empty rack
    --p.missiles[type_index];
    return true;
}

void add_missiles(PlayerState& p, int type_index, int count) {
    if (type_index < 0 || type_index >= 3 || count <= 0) return;
    p.missiles[type_index] += count;
}

int  torpedo_count(const PlayerState& p) {
    return p.torpedoes;
}

bool consume_torpedo(PlayerState& p) {
    if (p.torpedoes <= 0) return false;
    --p.torpedoes;
    return true;
}

void add_torpedoes(PlayerState& p, int count) {
    if (count <= 0) return;
    p.torpedoes += count;
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

// add_item for the unified hold (Phase 4 Wave 1, #80 + #81). Refuses
// (false, no mutation) when qty <= 0 or the unified hold (cargo +
// items) would overflow `capacity`. Capacity is passed in by the caller
// (same convention as add_cargo) — pass cargo_capacity(p, klass).
// On success, a Weapon/Upgrade item always appends a fresh entry;
// a Salvage- or Commodity-kind item merges into an existing stack of
// the SAME id + kind + rarity by adding qty, or appends if no match.
bool add_item(PlayerState& p, const inventory::InventoryItem& it, int capacity) {
    if (it.qty <= 0) return false;
    // Refuse overflow up-front. Use cargo_units_used (which now includes
    // items) so capacity is the unified-hold cap from #81, not a
    // separate "items" cap.
    if (cargo_units_used(p) + it.qty > capacity) return false;

    // Only Salvage- and Commodity-kind items are eligible for stack-
    // merging; Weapons and Upgrades always get a fresh entry.
    const bool stackable =
        it.kind == inventory::ItemKind::Salvage ||
        it.kind == inventory::ItemKind::Commodity;
    if (stackable) {
        for (inventory::InventoryItem& ex : p.items) {
            if (ex.kind  != it.kind)  continue;
            if (ex.id    != it.id)    continue;
            if (ex.rarity != it.rarity) continue;
            ex.qty += it.qty;
            return true;
        }
    }
    p.items.push_back(it);
    return true;
}

bool carrying_contraband(const PlayerState& p) {
    // Empty stacks contribute nothing — skip cheaply.
    for (const CargoEntry& e : p.cargo) {
        if (e.units <= 0) continue;
        if (commodity::is_contraband(e.commodity_id)) return true;
    }
    return false;
}

} // namespace player
// 1781715641481656000
