// -----------------------------------------------------------------------------
// repair.cpp — pricing + application for the base repair/rearm service.
//
// See repair.h for the design. Every credit movement goes through
// player::spend_credits (refuses when short), and the hull restore reuses
// ship::heal_to_full so there's no second "what does full look like?"
// definition to drift from spawn().
// -----------------------------------------------------------------------------

#include "repair.h"

#include "armor.h"
#include "player.h"
#include "ship.h"
#include "ship_class.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace repair {

namespace {

// ---- tuning knobs (the only ones the feature exposes) ----------------------
constexpr double k_credits_per_armor_cm = 20.0;   // hull repair
// k_credits_per_fuel removed: afterburner now shares the ship's energy bank
// (recharges for free, no top-off service to sell).
// Per-missile restock price, indexed by MissileType (DF/HS/IR). Mirrors the
// rough firepower ordering (IR dearest).
constexpr int64_t k_missile_price[3] = { 250, 600, 1200 };

// Full per-facing armor for a ship's class (base + fitted armor tier).
// Mirrors ship::heal_to_full's armor math so the "missing" calc agrees with
// what a repair actually restores.
struct FullArmor { float fore, aft, side; };
FullArmor full_armor(const Ship& s) {
    FullArmor fa{ 0, 0, 0 };
    if (!s.klass) return fa;
    const ShipClass& k = *s.klass;
    fa.fore = k.armor_fore_cm;
    fa.aft  = k.armor_aft_cm;
    fa.side = k.armor_side_cm;
    if (k.default_armor) {
        fa.fore += k.default_armor->front_cm;
        fa.aft  += k.default_armor->back_cm;
        fa.side += k.default_armor->side_cm;
    }
    return fa;
}

float armor_missing(const Ship& s) {
    const FullArmor fa = full_armor(s);
    float miss = 0.0f;
    miss += std::max(0.0f, fa.fore - s.armor_fore_cm);
    miss += std::max(0.0f, fa.aft  - s.armor_aft_cm);
    miss += std::max(0.0f, fa.side - s.armor_side_cm);
    return miss;
}

} // namespace

Quote quote(const Ship* ship, const PlayerState& p) {
    Quote q;

    if (ship && ship->klass) {
        const float miss = armor_missing(*ship);
        if (miss > 0.5f) {
            q.hull_damaged = true;
            q.hull_cost = (int64_t)std::ceil(miss * k_credits_per_armor_cm);
        }
    }

    const float fuel_missing = 0.0f;   // afterburner shares energy_gj now;
                                       // no top-off charge any more.
    if (fuel_missing > 0.5f) {
        q.fuel_low  = true;
        q.fuel_cost = 0;
    }

    for (int i = 0; i < 3; ++i) {
        const int need = player::k_new_game_missiles[i] - p.missiles[i];
        if (need > 0) {
            q.missiles_low = true;
            q.missile_cost += (int64_t)need * k_missile_price[i];
        }
    }

    q.total = q.hull_cost + q.fuel_cost + q.missile_cost;
    return q;
}

bool repair_hull(Ship& ship, PlayerState& p) {
    if (!ship.klass) {
        std::printf("[repair] hull refused: no ship class (nothing to repair)\n");
        return false;
    }
    const Quote q = quote(&ship, p);
    if (!q.hull_damaged) {
        std::printf("[repair] hull refused: already at full armor\n");
        return false;
    }
    if (!player::spend_credits(p, q.hull_cost)) {
        std::printf("[repair] hull refused: costs %lld, have %lld\n",
                    (long long)q.hull_cost, (long long)p.credits);
        return false;
    }
    const float before = ship.armor_fore_cm + ship.armor_aft_cm + ship.armor_side_cm;
    ship::heal_to_full(ship);
    const float after = ship.armor_fore_cm + ship.armor_aft_cm + ship.armor_side_cm;
    std::printf("[repair] hull restored: armor %.0f -> %.0f cm | paid %lld | credits %lld\n",
                before, after, (long long)q.hull_cost, (long long)p.credits);
    return true;
}

bool rearm(PlayerState& p) {
    const Quote q = quote(nullptr, p);
    if (!q.missiles_low) {
        std::printf("[repair] rearm refused: missile racks already full\n");
        return false;
    }
    if (!player::spend_credits(p, q.missile_cost)) {
        std::printf("[repair] rearm refused: costs %lld, have %lld\n",
                    (long long)q.missile_cost, (long long)p.credits);
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        const int need = player::k_new_game_missiles[i] - p.missiles[i];
        if (need > 0) player::add_missiles(p, i, need);
    }
    std::printf("[repair] missiles restocked DF/HS/IR = %d/%d/%d | paid %lld | credits %lld\n",
                p.missiles[0], p.missiles[1], p.missiles[2],
                (long long)q.missile_cost, (long long)p.credits);
    return true;
}

} // namespace repair
// 1781715641492252000
