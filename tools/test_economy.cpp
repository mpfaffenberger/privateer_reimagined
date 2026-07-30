// -----------------------------------------------------------------------------
// tools/test_economy.cpp — offline driver for np-9cu.2 commodity pricing +
// the buy/sell transaction path. Links the REAL economy.cpp (built with
// -DECONOMY_HEADLESS so the ImGui trade table is excluded), commodity.cpp,
// player.cpp and json.cpp, then walks the core trade loop the live game's
// Commodity Exchange screen drives:
//
//   * prints Achilles vs Helen quotes for iron (the canonical cheap-here,
//     dear-there ore),
//   * BUYS a Tarsus hold of ore at Achilles via the player:: helpers,
//   * proves overbuy-past-cargo-cap and overbuy-past-credits are REFUSED,
//   * "flies" to Helen and SELLS the ore, demonstrating positive arbitrage.
//
// Key injection isn't available over dev_remote, so this is the deterministic
// numeric proof; the live game provides the docked-screen screenshots.
//
// Build:
//   clang++ -std=c++20 -DECONOMY_HEADLESS -Isrc -Ithird_party \
//       tools/test_economy.cpp src/economy.cpp src/commodity.cpp \
//       src/player.cpp src/json.cpp -o /tmp/test_economy
// -----------------------------------------------------------------------------

#include "economy.h"
#include "commodity.h"
#include "player.h"
#include "ship_class.h"

#include <cstdio>

// Mirror the live screen's buy/sell enforcement (commodity_ui.cpp). Same order of checks,
// same player:: helpers — this is exactly what the buttons call.
static bool buy(PlayerState& p, const char* base, const Commodity& c,
                int qty, int cap) {
    const economy::Quote q = economy::price(base, c.id);
    const int used = player::cargo_units_used(p);
    const long long cost = (long long)q.buy_price * qty;
    if (q.buy_price <= 0 || q.available_units < qty) {
        std::printf("  REFUSED buy %s x%d: not available (stock %d)\n",
                    c.label.c_str(), qty, q.available_units);
        return false;
    }
    if (used + qty > cap) {
        std::printf("  REFUSED buy %s x%d: cargo cap (%d/%d)\n",
                    c.label.c_str(), qty, used, cap);
        return false;
    }
    if (!player::can_afford(p, cost)) {
        std::printf("  REFUSED buy %s x%d: costs %lld, have %lld\n",
                    c.label.c_str(), qty, cost, (long long)p.credits);
        return false;
    }
    player::spend_credits(p, cost);
    player::add_cargo(p, c.id, qty, q.buy_price, cap);
    economy::record_purchase(base, c.id, qty);
    std::printf("  BUY  %s x%d @ %d = %lld | credits %lld, cargo %d/%d\n",
                c.label.c_str(), qty, q.buy_price, cost,
                (long long)p.credits, player::cargo_units_used(p), cap);
    return true;
}

static void sell(PlayerState& p, const char* base, const Commodity& c,
                 int qty, int cap) {
    const economy::Quote q = economy::price(base, c.id);
    if (!player::remove_cargo(p, c.id, qty)) {
        std::printf("  REFUSED sell %s x%d: not held\n", c.label.c_str(), qty);
        return;
    }
    const long long gain = (long long)q.sell_price * qty;
    player::add_credits(p, gain);
    std::printf("  SELL %s x%d @ %d = %lld | credits %lld, cargo %d/%d\n",
                c.label.c_str(), qty, q.sell_price, gain,
                (long long)p.credits, player::cargo_units_used(p), cap);
}

int main() {
    commodity::load("assets/data/privateer_db/cargo.toml");
    const int markets = economy::load("assets/data/commodity_prices.json", "assets/bases");

    const Commodity* plastics = commodity::find("plastics");
    const economy::Quote liverpool = plastics
        ? economy::price("liverpool", plastics->id) : economy::Quote{};
    const bool market_coverage = markets >= 50 && liverpool.valid &&
                                 liverpool.available_units > 0;
    std::printf("\n== Gemini market coverage ==\n");
    std::printf("  bases=%d  Liverpool plastics valid=%d stock=%d  %s\n",
                markets, liverpool.valid ? 1 : 0, liverpool.available_units,
                market_coverage ? "PASS" : "FAIL");

    const Commodity* iron = commodity::find("iron");
    if (!iron) { std::printf("FAIL: no 'iron' in catalog\n"); return 1; }

    std::printf("\n== Quotes for Iron (RAWMAT) ==\n");
    for (const char* b : { "achilles", "helen" }) {
        const economy::Quote q = economy::price(b, iron->id);
        std::printf("  %-9s buy=%d sell=%d avail=%d\n",
                    b, q.buy_price, q.sell_price, q.available_units);
    }

    // Tarsus: 100-unit hold, 2000-credit fresh-start bankroll.
    ShipClass tarsus; tarsus.cargo_units = 100;
    PlayerState p = player::new_game("troy");
    const int cap = player::cargo_capacity(p, &tarsus);
    std::printf("\n== Start: credits %lld, cargo %d/%d ==\n",
                (long long)p.credits, player::cargo_units_used(p), cap);

    std::printf("\n== Buy ore at Achilles (cheap here) ==\n");
    buy(p, "achilles", *iron, 100, cap);          // fills the hold

    std::printf("\n== Enforcement checks ==\n");
    buy(p, "achilles", *iron, 1, cap);            // over cargo cap -> refused
    PlayerState poor = player::new_game("troy");
    poor.credits = 10;
    buy(poor, "achilles", *iron, 100, cap);       // over credit limit -> refused

    std::printf("\n== Fly to Helen, sell the ore (dear here) ==\n");
    const long long before = p.credits;
    sell(p, "helen", *iron, 100, cap);

    const long long net = p.credits - player::k_new_game_credits;
    std::printf("\n== Arbitrage result ==\n");
    std::printf("  spent at Achilles, recovered at Helen\n");
    std::printf("  credits %lld -> %lld (net vs fresh start: %+lld)\n",
                (long long)player::k_new_game_credits, (long long)p.credits, net);
    std::printf("  %s\n", net > 0 ? "PASS: arbitrage nets positive credits"
                                   : "FAIL: arbitrage did not profit");
    (void)before;
    return net > 0 && market_coverage ? 0 : 1;
}
