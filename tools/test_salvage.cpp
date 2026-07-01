// Offline check for issue #87: scrap_metal & friends load as commodities and
// price/sell through the existing exchange. Build:
//   clang++ -std=c++20 -DECONOMY_HEADLESS -Isrc -Ithird_party \
//     tools/test_salvage.cpp src/economy.cpp src/commodity.cpp \
//     src/player.cpp src/json.cpp -o /tmp/test_salvage
#include "economy.h"
#include "commodity.h"
#include <cstdio>

int main() {
    commodity::load("assets/data/privateer_db/cargo.toml");
    commodity::load_extra("assets/data/commodities_salvage.toml");
    economy::load("assets/data/commodity_prices.json", "assets/bases");

    const char* ids[] = { "scrap_metal", "salvaged_electronics",
                          "damaged_components", "hull_plating" };
    int ok = 0, total = 0;
    for (const char* id : ids) {
        ++total;
        const Commodity* c = commodity::find(id);
        if (!c) { std::printf("FAIL: '%s' not in catalog\n", id); continue; }
        std::printf("%-22s cat=%-8s", c->id.c_str(), c->category.c_str());
        bool sellable = false;
        for (const char* b : { "achilles", "helen" }) {
            const economy::Quote q = economy::price(b, c->id);
            std::printf("  | %s sell=%d avail=%d", b, q.sell_price, q.available_units);
            if (q.valid && q.sell_price > 0) sellable = true;
        }
        std::printf("  -> %s\n", sellable ? "SELLABLE" : "NOT SELLABLE");
        if (sellable) ++ok;
    }
    std::printf("\n%d/%d salvage commodities sellable through the exchange\n", ok, total);
    return ok == total ? 0 : 1;
}
