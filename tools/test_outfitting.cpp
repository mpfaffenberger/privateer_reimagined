// -----------------------------------------------------------------------------
// tools/test_outfitting.cpp — offline driver for np-9cu.3 outfitting: hull +
// equipment pricing and the buy/upgrade transaction path. Links the REAL
// outfitting.cpp (built with -DOUTFITTING_HEADLESS so the ImGui shop screens
// are excluded), ship_class.cpp, gun.cpp, shield.cpp, armor.cpp, player.cpp,
// mobility.cpp, faction.cpp and json.cpp, then drives exactly the transaction
// functions the live Equipment / Ship Dealer screens call:
//
//   * buy a gun into a mount (and prove an out-of-range mount is refused),
//   * upgrade shield + engine (and prove the hull cap refuses the next step),
//   * buy cargo expansion and verify cargo_capacity() grows,
//   * buy a new hull with trade-in math (and prove over-budget is refused),
//   * show the engine_level -> effective top speed wiring changing.
//
// Key injection isn't available over dev_remote, so this is the deterministic
// numeric proof; the live game provides the docked-screen screenshots.
//
// Build (from repo root):
//   clang++ -std=c++20 -DOUTFITTING_HEADLESS -Isrc -Ithird_party \
//       tools/test_outfitting.cpp src/outfitting.cpp src/ship_class.cpp \
//       src/gun.cpp src/shield.cpp src/armor.cpp src/player.cpp \
//       src/mobility.cpp src/faction.cpp src/json.cpp -o /tmp/test_outfitting
// -----------------------------------------------------------------------------

#include "outfitting.h"
#include "player.h"
#include "ship_class.h"
#include "gun.h"
#include "shield.h"
#include "armor.h"
#include "faction.h"

#include <cstdio>

static void show(const PlayerState& p, const char* tag) {
    const ShipClass* k = ship_class::find(p.ship_class_name);
    const outfitting::SpeedCaps caps = outfitting::effective_speed_caps(p);
    std::printf("  [%s] ship=%s credits=%lld shield=L%d engine=L%d cargo_exp=%d "
                "cap=%d speed=%.0f/%.0f\n",
                tag, p.ship_class_name.c_str(), (long long)p.credits,
                p.shield_level, p.engine_level, (int)p.cargo_expansion,
                player::cargo_capacity(p, k), caps.cruise0, caps.cruise1);
}

int main() {
    // Load the catalogs the model resolves against.
    faction::init();
    gun::load_table("assets/data/privateer_ship_data.json");
    shield::load_table("assets/data/privateer_ship_data.json");
    armor::load_table("assets/data/privateer_ship_data.json");
    ship_class::load_all("assets/ships");
    const int n = outfitting::load("assets/data/ship_prices.json",
                                   "assets/data/equipment_prices.json");
    if (n <= 0) { std::printf("FAIL: no hulls priced\n"); return 1; }

    PlayerState p = player::new_game("troy");
    p.credits = 300000;   // give the test wallet room to exercise every path
    const ShipClass* tarsus = ship_class::find(p.ship_class_name);

    std::printf("\n== Start ==\n");
    show(p, "start");

    std::printf("\n== Buy a gun into mount 0 (Tarsus has 2 mounts) ==\n");
    const long long c0 = p.credits;
    outfitting::buy_gun(p, "tachyon_cannon", 0, tarsus);
    std::printf("  credits %lld -> %lld\n", c0, (long long)p.credits);
    std::printf("  mount0 now: %s\n", p.gun_mounts.empty() ? "(none)" : p.gun_mounts[0].c_str());

    std::printf("\n== Refuse a gun into an out-of-range mount (slot 9) ==\n");
    outfitting::buy_gun(p, "laser", 9, tarsus);

    std::printf("\n== Upgrade shield + engine (Tarsus caps: shield 2, engine 1) ==\n");
    outfitting::upgrade_shield(p, tarsus);   // L0 -> L1
    outfitting::upgrade_shield(p, tarsus);   // L1 -> L2
    outfitting::upgrade_shield(p, tarsus);   // L2 -> L3 REFUSED (cap 2)
    show(p, "shields");
    outfitting::upgrade_engine(p, tarsus);   // L0 -> L1
    outfitting::upgrade_engine(p, tarsus);   // L1 -> L2 REFUSED (cap 1)
    show(p, "engine");

    std::printf("\n== Buy cargo expansion (verify capacity grows) ==\n");
    const int cap_before = player::cargo_capacity(p, tarsus);
    outfitting::buy_cargo_expansion(p);
    const int cap_after = player::cargo_capacity(p, tarsus);
    std::printf("  cargo capacity %d -> %d  %s\n", cap_before, cap_after,
                cap_after > cap_before ? "PASS" : "FAIL");

    std::printf("\n== Buy a new hull (Centurion) with trade-in math ==\n");
    const long long before = p.credits;
    const long long price  = outfitting::hull_price("centurion");
    const long long tradein= outfitting::hull_trade_in("tarsus");
    const long long net    = outfitting::hull_net_cost("centurion", "tarsus");
    std::printf("  centurion price=%lld  tarsus trade-in=%lld  net=%lld\n",
                price, tradein, net);
    outfitting::buy_hull(p, "centurion");
    std::printf("  credits %lld -> %lld (delta %lld, expected -%lld)\n",
                before, (long long)p.credits, (long long)(p.credits - before), net);
    show(p, "centurion");
    std::printf("  loadout reset to %zu Centurion default mounts: ", p.gun_mounts.size());
    for (const auto& g : p.gun_mounts) std::printf("%s ", g.c_str());
    std::printf("\n");

    std::printf("\n== Engine -> speed on the Centurion (caps engine L2) ==\n");
    const ShipClass* cent = ship_class::find("centurion");
    const outfitting::SpeedCaps s0 = outfitting::effective_speed_caps(p);
    outfitting::upgrade_engine(p, cent);   // L0 -> L1
    outfitting::upgrade_engine(p, cent);   // L1 -> L2
    const outfitting::SpeedCaps s2 = outfitting::effective_speed_caps(p);
    std::printf("  top speed L0 %.0f/%.0f -> L2 %.0f/%.0f  %s\n",
                s0.cruise0, s0.cruise1, s2.cruise0, s2.cruise1,
                s2.cruise0 > s0.cruise0 ? "PASS: engine_level raises speed"
                                        : "FAIL");

    std::printf("\n== Over-budget hull purchase is refused ==\n");
    PlayerState broke = player::new_game("troy");
    broke.credits = 5000;   // can't afford anything but the Tarsus it owns
    outfitting::buy_hull(broke, "centurion");
    std::printf("  broke pilot still flying: %s (credits %lld)\n",
                broke.ship_class_name.c_str(), (long long)broke.credits);

    std::printf("\nAll transaction paths exercised.\n");
    return 0;
}
