// Offline check for #112 model layer: sell_cargo_unit decrements + credits +
// erases at zero; item add/sell still works. Build:
//   cmake --build build --target test_inventory112 && ./build/test_inventory112
#include "inventory.h"
#include "player.h"
#include <cstdio>

int main() {
    inventory::load_prices("assets/data/loot_prices.json");
    PlayerState p = player::new_game("troy");
    p.credits = 0;
    p.cargo.push_back({ "grain", 3, 0 });
    p.cargo.push_back({ "iron",  1, 0 });

    int fails = 0;
    auto check = [&](const char* what, bool ok) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++fails;
    };

    std::printf("== sell_cargo_unit ==\n");
    // Sell 1 grain @ 22 -> grain 2, credits 22.
    inventory::sell_cargo_unit(p, 0, 22);
    check("grain decremented to 2", p.cargo.size() == 2 && p.cargo[0].units == 2);
    check("credited 22", p.credits == 22);
    // Sell the single iron unit -> entry erased.
    inventory::sell_cargo_unit(p, 1, 30);
    check("iron stack erased at 0 units", p.cargo.size() == 1);
    check("credited 22+30=52", p.credits == 52);
    // Bad index -> no-op, no mutation.
    const int64_t before = p.credits;
    check("bad index refused", !inventory::sell_cargo_unit(p, 9, 100));
    check("no phantom credits", p.credits == before);

    std::printf("== commodity-kind item add + sell ==\n");
    inventory::InventoryItem food;
    food.id = "luxury_foods"; food.kind = inventory::ItemKind::Commodity; food.qty = 5;
    const ShipClass* k = nullptr;  // capacity uses default when klass null
    player::add_item(p, food, 1000);
    check("commodity item added to items[]", !p.items.empty() &&
          p.items.back().kind == inventory::ItemKind::Commodity);
    const int64_t pre = p.credits;
    inventory::sell_item(p, (int)p.items.size() - 1);
    check("commodity item sold (credits rose)", p.credits > pre);
    (void)k;

    std::printf("\n%s (%d failures)\n", fails == 0 ? "ALL PASS" : "FAILURES", fails);
    return fails == 0 ? 0 : 1;
}
