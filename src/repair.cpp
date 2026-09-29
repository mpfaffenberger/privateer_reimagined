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
// Per-missile price, indexed by MissileType (DF/HS/IR/FF). Canonical Privateer
// (gamefaq): Dumb-Fire 20, Heat-Seeker 35, Image-Rec 75. FF (#144) is tuned,
// not sourced: priced above IR because it needs no lock at all.
constexpr int64_t k_missile_price[k_missile_rack_types] = { 20, 35, 75, 100 };

// "DF/HS/IR/FF = a/b/c/d" for the dealer logs, so every log line agrees on
// the rack layout instead of hand-listing indices.
static_assert(k_missile_rack_types == 4, "rack_summary lists every rack type");
struct RackSummary { char text[64]; };
RackSummary rack_summary(const PlayerState& p) {
    RackSummary r;
    std::snprintf(r.text, sizeof r.text, "DF/HS/IR/FF = %d/%d/%d/%d",
                  p.missiles[0], p.missiles[1], p.missiles[2], p.missiles[3]);
    return r;
}

bool valid_rack_type(int type) {
    return type >= 0 && type < k_missile_rack_types;
}

// Per-torpedo price (single rate). Torpedoes don't split into DF/HS/IR
// variants in Privateer canon -- there's just one Proton Torpedo.
constexpr int64_t k_torpedo_unit_price = 35;

// Full per-facing armor for a ship's class (base + fitted armor tier).
// Mirrors ship::heal_to_full's armor math so the "missing" calc agrees with
// what a repair actually restores.
struct FullArmor { float fore, aft, port, starboard; };
FullArmor full_armor(const Ship& s) {
    FullArmor fa{ 0, 0, 0, 0 };
    if (!s.klass) return fa;
    const ShipClass& k = *s.klass;
    fa.fore      = k.armor_fore_cm;
    fa.aft       = k.armor_aft_cm;
    fa.port      = k.armor_port_cm;
    fa.starboard = k.armor_starboard_cm;
    // Fitted armor (purchasable upgrade) stacks on the base hull; null =
    // no package, base hull only.
    if (const ArmorType* fitted_armor = s.fitted_armor) {
        fa.fore      += fitted_armor->front_cm;
        fa.aft       += fitted_armor->back_cm;
        fa.port      += fitted_armor->port_cm;
        fa.starboard += fitted_armor->starboard_cm;
    }
    return fa;
}

float armor_missing(const Ship& s) {
    const FullArmor fa = full_armor(s);
    float miss = 0.0f;
    miss += std::max(0.0f, fa.fore      - s.armor_fore_cm);
    miss += std::max(0.0f, fa.aft       - s.armor_aft_cm);
    miss += std::max(0.0f, fa.port      - s.armor_port_cm);
    miss += std::max(0.0f, fa.starboard - s.armor_starboard_cm);
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

    for (int i = 0; i < k_missile_rack_types; ++i) {
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
    const float before = ship.armor_fore_cm + ship.armor_aft_cm
                        + ship.armor_port_cm + ship.armor_starboard_cm;
    ship::heal_to_full(ship);
    // Landed repair happens outside the Flight-frame Ship -> PlayerState
    // mirror, so persist the repaired condition immediately. Otherwise a
    // save made while still landed could keep the old damage snapshot.
    p.hp_valid = true;
    p.hp_armor_fore      = ship.armor_fore_cm;
    p.hp_armor_aft       = ship.armor_aft_cm;
    p.hp_armor_port      = ship.armor_port_cm;
    p.hp_armor_starboard = ship.armor_starboard_cm;
    p.hp_shield_fore     = ship.shield_fore_cm;
    p.hp_shield_aft      = ship.shield_aft_cm;
    p.hp_shield_port     = ship.shield_port_cm;
    p.hp_shield_starboard = ship.shield_starboard_cm;
    p.hp_energy          = ship.energy_gj;
    const float after = ship.armor_fore_cm + ship.armor_aft_cm
                       + ship.armor_port_cm + ship.armor_starboard_cm;
    std::printf("[repair] hull restored: armor %.0f -> %.0f cm | paid %lld | credits %lld\n",
                before, after, (long long)q.hull_cost, (long long)p.credits);
    return true;
}

int64_t missile_price(int type) {
    return valid_rack_type(type) ? k_missile_price[type] : 0;
}

int missile_rack_capacity(const PlayerState& p) {
    const int n = (p.missile_launcher_left ? 1 : 0) + (p.missile_launcher_right ? 1 : 0);
    return n * k_missile_rack_per_launcher;
}

int torpedo_rack_capacity(const PlayerState& p) {
    const int n = (p.torpedo_launcher_left ? 1 : 0) + (p.torpedo_launcher_right ? 1 : 0);
    return n * k_torpedo_rack_per_tube;
}

int missile_launchers_owned(const PlayerState& p) {
    return (p.missile_launcher_left ? 1 : 0) + (p.missile_launcher_right ? 1 : 0);
}

int torpedo_launchers_owned(const PlayerState& p) {
    return (p.torpedo_launcher_left ? 1 : 0) + (p.torpedo_launcher_right ? 1 : 0);
}

bool left_hardpoint_free(const PlayerState& p) {
    return !p.missile_launcher_left && !p.torpedo_launcher_left;
}

bool right_hardpoint_free(const PlayerState& p) {
    return !p.missile_launcher_right && !p.torpedo_launcher_right;
}

int missiles_total(const PlayerState& p) {
    int total = 0;
    for (int m : p.missiles) total += m;
    return total;
}

int64_t torpedo_price() {
    return k_torpedo_unit_price;
}

int torpedoes_total(const PlayerState& p) {
    return p.torpedoes;
}

bool buy_missiles(PlayerState& p, int type, int count) {
    if (!valid_rack_type(type) || count <= 0) {
        std::printf("[repair] buy missiles refused: bad type/count %d x%d\n", type, count);
        return false;
    }
    const int cap  = missile_rack_capacity(p);
    const int room = cap - missiles_total(p);
    if (room <= 0) {
        std::printf("[repair] buy missiles refused: rack full (%d/%d)\n",
                    missiles_total(p), cap);
        return false;
    }
    const int n = std::min(count, room);
    const int64_t cost = (int64_t)n * k_missile_price[type];
    if (!player::spend_credits(p, cost)) {
        std::printf("[repair] buy missiles refused: %d x type%d costs %lld, have %lld\n",
                    n, type, (long long)cost, (long long)p.credits);
        return false;
    }
    player::add_missiles(p, type, n);
    std::printf("[repair] bought %d type%d missile(s) | %s | paid %lld | credits %lld\n",
                n, type, rack_summary(p).text, (long long)cost, (long long)p.credits);
    return true;
}

bool buy_torpedo(PlayerState& p, int count) {
    if (count <= 0) {
        std::printf("[repair] buy torpedo refused: bad count %d\n", count);
        return false;
    }
    const int cap  = torpedo_rack_capacity(p);
    const int room = cap - torpedoes_total(p);
    if (room <= 0) {
        std::printf("[repair] buy torpedo refused: rack full (%d/%d)\n",
                    torpedoes_total(p), cap);
        return false;
    }
    const int n = std::min(count, room);
    const int64_t cost = (int64_t)n * k_torpedo_unit_price;
    if (!player::spend_credits(p, cost)) {
        std::printf("[repair] buy torpedo refused: %d costs %lld, have %lld\n",
                    n, (long long)cost, (long long)p.credits);
        return false;
    }
    player::add_torpedoes(p, n);
    std::printf("[repair] bought %d torpedo(es) | total %d/%d | paid %lld | credits %lld\n",
                n, p.torpedoes, cap, (long long)cost, (long long)p.credits);
    return true;
}

bool buy_missile_launcher_left(PlayerState& p) {
    if (!left_hardpoint_free(p)) {
        std::printf("[repair] buy missile launcher LEFT refused: LEFT hardpoint occupied\n");
        return false;
    }
    if (!player::spend_credits(p, k_missile_launcher_price)) {
        std::printf("[repair] buy missile launcher LEFT refused: costs %lld, have %lld\n",
                    (long long)k_missile_launcher_price, (long long)p.credits);
        return false;
    }
    p.missile_launcher_left = true;
    std::printf("[repair] bought missile launcher LEFT | paid %lld | credits %lld\n",
                (long long)k_missile_launcher_price, (long long)p.credits);
    return true;
}

bool buy_missile_launcher_right(PlayerState& p) {
    if (!right_hardpoint_free(p)) {
        std::printf("[repair] buy missile launcher RIGHT refused: RIGHT hardpoint occupied\n");
        return false;
    }
    if (!player::spend_credits(p, k_missile_launcher_price)) {
        std::printf("[repair] buy missile launcher RIGHT refused: costs %lld, have %lld\n",
                    (long long)k_missile_launcher_price, (long long)p.credits);
        return false;
    }
    p.missile_launcher_right = true;
    std::printf("[repair] bought missile launcher RIGHT | paid %lld | credits %lld\n",
                (long long)k_missile_launcher_price, (long long)p.credits);
    return true;
}

bool buy_torpedo_launcher_left(PlayerState& p) {
    if (!left_hardpoint_free(p)) {
        std::printf("[repair] buy torpedo launcher LEFT refused: LEFT hardpoint occupied\n");
        return false;
    }
    if (!player::spend_credits(p, k_torpedo_launcher_price)) {
        std::printf("[repair] buy torpedo launcher LEFT refused: costs %lld, have %lld\n",
                    (long long)k_torpedo_launcher_price, (long long)p.credits);
        return false;
    }
    p.torpedo_launcher_left = true;
    std::printf("[repair] bought torpedo launcher LEFT | paid %lld | credits %lld\n",
                (long long)k_torpedo_launcher_price, (long long)p.credits);
    return true;
}

bool buy_torpedo_launcher_right(PlayerState& p) {
    if (!right_hardpoint_free(p)) {
        std::printf("[repair] buy torpedo launcher RIGHT refused: RIGHT hardpoint occupied\n");
        return false;
    }
    if (!player::spend_credits(p, k_torpedo_launcher_price)) {
        std::printf("[repair] buy torpedo launcher RIGHT refused: costs %lld, have %lld\n",
                    (long long)k_torpedo_launcher_price, (long long)p.credits);
        return false;
    }
    p.torpedo_launcher_right = true;
    std::printf("[repair] bought torpedo launcher RIGHT | paid %lld | credits %lld\n",
                (long long)k_torpedo_launcher_price, (long long)p.credits);
    return true;
}

bool sell_missile_launcher_left(PlayerState& p) {
    if (!p.missile_launcher_left) {
        std::printf("[repair] sell missile launcher LEFT refused: not fitted\n");
        return false;
    }
    p.missile_launcher_left = false;
    player::add_credits(p, k_missile_launcher_sell_price);
    std::printf("[repair] sold missile launcher LEFT | refund %lld | credits %lld\n",
                (long long)k_missile_launcher_sell_price, (long long)p.credits);
    return true;
}

bool sell_missile_launcher_right(PlayerState& p) {
    if (!p.missile_launcher_right) {
        std::printf("[repair] sell missile launcher RIGHT refused: not fitted\n");
        return false;
    }
    p.missile_launcher_right = false;
    player::add_credits(p, k_missile_launcher_sell_price);
    std::printf("[repair] sold missile launcher RIGHT | refund %lld | credits %lld\n",
                (long long)k_missile_launcher_sell_price, (long long)p.credits);
    return true;
}

bool sell_torpedo_launcher_left(PlayerState& p) {
    if (!p.torpedo_launcher_left) {
        std::printf("[repair] sell torpedo launcher LEFT refused: not fitted\n");
        return false;
    }
    p.torpedo_launcher_left = false;
    player::add_credits(p, k_torpedo_launcher_sell_price);
    std::printf("[repair] sold torpedo launcher LEFT | refund %lld | credits %lld\n",
                (long long)k_torpedo_launcher_sell_price, (long long)p.credits);
    return true;
}

bool sell_torpedo_launcher_right(PlayerState& p) {
    if (!p.torpedo_launcher_right) {
        std::printf("[repair] sell torpedo launcher RIGHT refused: not fitted\n");
        return false;
    }
    p.torpedo_launcher_right = false;
    player::add_credits(p, k_torpedo_launcher_sell_price);
    std::printf("[repair] sold torpedo launcher RIGHT | refund %lld | credits %lld\n",
                (long long)k_torpedo_launcher_sell_price, (long long)p.credits);
    return true;
}

bool sell_missile(PlayerState& p, int type) {
    if (!valid_rack_type(type)) {
        std::printf("[repair] sell missile refused: bad type %d\n", type);
        return false;
    }
    if (p.missiles[type] <= 0) {
        std::printf("[repair] sell missile refused: type %d count %d\n",
                    type, p.missiles[type]);
        return false;
    }
    --p.missiles[type];
    player::add_credits(p, k_missile_price[type]);
    std::printf("[repair] sold 1 type%d missile | %s | +%lld | credits %lld\n",
                type, rack_summary(p).text,
                (long long)k_missile_price[type], (long long)p.credits);
    return true;
}

bool sell_torpedo(PlayerState& p) {
    if (p.torpedoes <= 0) {
        std::printf("[repair] sell torpedo refused: count %d\n", p.torpedoes);
        return false;
    }
    --p.torpedoes;
    player::add_credits(p, k_torpedo_unit_price);
    std::printf("[repair] sold 1 torpedo | now %d | +%lld | credits %lld\n",
                p.torpedoes, (long long)k_torpedo_unit_price,
                (long long)p.credits);
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
    for (int i = 0; i < k_missile_rack_types; ++i) {
        const int need = player::k_new_game_missiles[i] - p.missiles[i];
        if (need > 0) player::add_missiles(p, i, need);
    }
    std::printf("[repair] missiles restocked %s | paid %lld | credits %lld\n",
                rack_summary(p).text, (long long)q.missile_cost, (long long)p.credits);
    return true;
}

} // namespace repair
// 1781715641492252000
