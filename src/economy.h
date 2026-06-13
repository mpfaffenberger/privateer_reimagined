#pragma once
// -----------------------------------------------------------------------------
// economy.h — per-base commodity pricing + the Commodity Exchange screen.
//
// The trading layer that sits between the commodity CATALOG (commodity.h —
// what exists) and the player's WALLET + HOLD (player.h — what they own).
// It answers one question per (base, commodity): "what does it cost to buy
// here, what will they pay if I sell, and how many can I buy?".
//
// Prices are DATA-DRIVEN, not hardcoded. There are no per-base price tables
// in C++ — only archetype x category math. The inputs are:
//   * assets/data/commodity_prices.json — canonical per-CATEGORY base price,
//     a global buy/sell spread, and a set of base archetypes (each a map of
//     category -> {price_mult, stock}). Hand-authored + tunable; documented
//     there because cargo.toml's price bytes aren't decoded yet (see the
//     privateer_db README).
//   * assets/bases/<id>/base.json's "market" block — picks the archetype for
//     that base ("mining", "agricultural", ...).
//
// A quote is computed as:
//   base      = category_base_price[commodity.category]
//   mult,stock= archetype.categories[category]  (else archetype.default)
//   buy_price = round(base * mult)
//   sell_price= round(buy_price * (1 - spread_pct/100))      (buy > sell)
//   available = stock     (0 => not stocked here; still sellable if carried)
//
// buy > sell means flipping on one base loses money — profit demands travel,
// which is the whole point of the Achilles-ore -> Helen-sell trade loop.
//
// Availability is also a live, per-session CAP: buying decrements the local
// stock (record_purchase), so you can't buy more than a base actually has.
// Selling is always allowed (you can offload anything you carry, anywhere).
// -----------------------------------------------------------------------------

#include <string>

namespace economy {

// One base's offer on one commodity. All prices are credits/unit.
struct Quote {
    bool valid          = false; // false => commodity unknown / no price data
    int  buy_price      = 0;     // what you pay to buy one unit here
    int  sell_price     = 0;     // what the base pays you per unit (buy>sell)
    int  available_units= 0;     // live remaining stock; 0 = not buyable here
};

// Load assets/data/commodity_prices.json (canonical category prices +
// archetypes) and scan bases_dir/<id>/base.json for each base's "market"
// archetype. Idempotent (reload replaces, and resets live stock). Returns
// the number of base archetypes resolved. Logs one summary line.
int load(const std::string& prices_path, const std::string& bases_dir);

// Quote for (base, commodity). available_units reflects the LIVE remaining
// stock for this session (seeded from the archetype, decremented by
// record_purchase). Returns {valid=false} for unknown commodities/bases.
Quote price(const std::string& base_id, const std::string& commodity_id);

// Decrement a base's live stock after a successful buy (see header). Clamped
// at zero. Call ONLY after the transaction actually went through.
void record_purchase(const std::string& base_id,
                     const std::string& commodity_id, int units);

// Register the Commodity Exchange screen body with the base-screen framework
// (np-9cu.4 seam). Call once at startup, after load(). The framework owns the
// background/title/Back; we draw the trade table into the body it hands us.
void register_exchange_screen();

} // namespace economy
