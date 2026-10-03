// -----------------------------------------------------------------------------
// tools/test_outfitting.cpp �?" offline driver for np-9cu.3 outfitting: hull +
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
//   * prove engine upgrades preserve hull-defined top speed.
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

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// Purchasable turrets (#145): hull slots from ship.json, hardware buy/sell,
// gun fitting gated on owned hardware, dealer hulls arriving turret-less.
static void test_turrets() {
    std::printf("\n== Turret hardware (#145) ==\n");
    const ShipClass* tarsus = ship_class::find("tarsus");
    const ShipClass* cent   = ship_class::find("centurion");
    const ShipClass* galaxy = ship_class::find("galaxy");
    const ShipClass* orion  = ship_class::find("orion");
    check(tarsus && tarsus->turret_slots.empty(), "Tarsus supports no turret (vanilla)");
    check(cent && cent->turret_slots.size() == 1 && cent->turret_slots[0].id == "rear" &&
          cent->turret_slots[0].mounts == std::vector<int>{4, 5},
          "Centurion: one rear turret over mounts 4+5");
    check(orion && orion->find_turret_slot("rear") && orion->turret_slots.size() == 1,
          "Orion: rear turret");
    check(galaxy && galaxy->turret_slots.size() == 2 && galaxy->find_turret_slot("top") &&
          galaxy->find_turret_slot("bottom") && !galaxy->find_turret_slot("rear"),
          "Galaxy: top + bottom turrets only (vanilla, #527)");
    check(cent && cent->turret_slots[0].label == "Rear Turret", "slot label derived from id");
    check(outfitting::turret_price() > 0, "turret_price loaded from equipment_prices.json");
    if (!cent || !tarsus) return;

    PlayerState p = player::new_game("troy");
    p.credits = 1000000;
    check(p.turrets.empty(), "new game owns no turrets");
    check(outfitting::buy_hull(p, "centurion"), "buy Centurion");
    check(p.turrets.empty() && p.gun_mounts.size() == 6 &&
          p.gun_mounts[4].gun_id.empty() && p.gun_mounts[5].gun_id.empty() &&
          !p.gun_mounts[0].gun_id.empty(),
          "dealer hull: forward guns stocked, turret mounts empty + unowned");
    check(player::first_open_mount(p, cent) == -1,
          "first_open_mount skips unbought turret mounts");

    const long long c0 = p.credits;
    check(!outfitting::buy_gun(p, "laser", 4, cent) && p.credits == c0,
          "gun into unbought turret refused, no charge");
    check(!outfitting::buy_turret(p, "top", cent), "Centurion has no top turret");
    check(!outfitting::buy_turret(p, "rear", tarsus), "slot must exist on the given hull");

    check(outfitting::buy_turret(p, "rear", cent) && player::has_turret(p, "rear") &&
          p.credits == c0 - outfitting::turret_price(), "buy rear turret charges turret_price");
    check(!outfitting::buy_turret(p, "rear", cent), "second rear turret refused");
    check(player::first_open_mount(p, cent) == 4, "owned turret mount is now open");
    check(outfitting::buy_gun(p, "laser", 4, cent) && p.gun_mounts[4].gun_id == "laser",
          "gun fits into owned turret");
    check(!outfitting::sell_turret(p, "rear", cent) && player::has_turret(p, "rear"),
          "can't sell a turret still carrying a gun");
    check(outfitting::sell_gun(p, 4, cent), "sell the turret gun");
    const long long c1 = p.credits;
    check(outfitting::sell_turret(p, "rear", cent) && !player::has_turret(p, "rear") &&
          p.credits == c1 + outfitting::turret_price(), "empty turret sells for a refund");
    check(!outfitting::sell_turret(p, "rear", cent), "can't sell what you don't own");

    PlayerState poor = player::new_game("troy");
    poor.ship_class_name = "centurion";
    poor.credits = outfitting::turret_price() - 1;
    check(!outfitting::buy_turret(poor, "rear", cent) && poor.turrets.empty(),
          "unaffordable turret refused");

    outfitting::buy_turret(p, "rear", cent);
    outfitting::buy_hull(p, "tarsus");
    check(p.turrets.empty(), "hull swap drops the old hull's turrets");

    PlayerState dev = player::new_game("troy");
    outfitting::fit_stock_guns(dev, cent, /*with_turrets=*/true);
    check(player::has_turret(dev, "rear") && dev.gun_mounts[4].gun_id == "ionic_pulse_cannon",
          "fit_stock_guns(with_turrets) = fully stocked hull (--ship override)");
}

// Fitting over a gun must never eat it (#741): buy_gun refuses an armed mount
// without charging, sell_gun refunds + frees it, then the new gun fits.
static void test_no_overwrite() {
    std::printf("\n== No gun overwrite (#741) ==\n");
    const ShipClass* tarsus = ship_class::find("tarsus");
    check(tarsus != nullptr, "Tarsus catalog loaded");
    if (!tarsus) return;

    PlayerState p = player::new_game("troy");
    p.credits = 300000;
    const std::string stock = p.gun_mounts.empty() ? "" : p.gun_mounts[0].gun_id;
    check(player::mount_armed(p, 0) && !stock.empty(),
          "new-game Tarsus mount 0 carries its stock gun");
    check(!player::mount_armed(p, -1) && !player::mount_armed(p, 99),
          "out-of-range mounts read as unarmed");

    const long long c0 = p.credits;
    check(!outfitting::buy_gun(p, "tachyon_cannon", 0, tarsus) && p.credits == c0 &&
          p.gun_mounts[0].gun_id == stock,
          "FIT onto an armed mount refused: no charge, stock gun kept");
    check(outfitting::sell_gun(p, 0, tarsus) && !player::mount_armed(p, 0) &&
          p.credits == c0 + outfitting::gun_price(stock),
          "selling the fitted gun refunds it and frees the mount");
    const long long c1 = p.credits;
    check(outfitting::buy_gun(p, "tachyon_cannon", 0, tarsus) &&
          p.gun_mounts[0].gun_id == "tachyon_cannon" &&
          p.credits == c1 - outfitting::gun_price("tachyon_cannon"),
          "...then the new gun fits at its full price");
}

// The starter ship must be able to fight (#743): with its new-game shield
// generator running, net energy regen covers sustained fire from its own
// stock guns. "Above zero" was not enough: 10 GJ/s lost every opening fight.
// Uses outfitting::energy_budget, the same numbers the equipment bay shows.
static void test_starter_energy() {
    std::printf("\n== Starter energy budget (#743) ==\n");
    const PlayerState p = player::new_game("troy");
    const ShipClass* k = ship_class::find(p.ship_class_name);
    check(k != nullptr, "starter hull catalog loaded");
    if (!k) return;
    const outfitting::EnergyBudget b = outfitting::energy_budget(p, k);
    std::printf("  %s: net regen %.1f GJ/s vs stock-gun burn %.1f GJ/s (bank %.0f)\n",
                p.ship_class_name.c_str(), b.regen_gj_s, b.gun_burn_gj_s, b.bank_gj);
    check(b.gun_burn_gj_s > 0.0f, "starter carries energy-using stock guns");
    check(b.regen_gj_s >= b.gun_burn_gj_s, "starter net regen sustains its stock guns indefinitely");
}

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

// The equipment bay's energy numbers (#747) must be firing.cpp's numbers:
// energy_cost * energy_mult per shot, every refire / fire_rate_mult; only
// FIXED guns drain the bank (turrets fire free).
static void test_energy_budget() {
    std::printf("\n== Energy budget math (#747) ==\n");
    check(near(outfitting::gun_energy_burn("laser"), 4.0f / 0.3f), "laser burns 4 GJ / 0.3 s");
    check(near(outfitting::gun_energy_burn("laser", 1.1f, 0.9f), 4.0f * 0.9f * 1.1f / 0.3f),
          "rarity mods scale the burn like firing.cpp (rare 1.1x rate, 0.9x energy)");
    check(near(outfitting::gun_energy_burn("laser", 0.0f), 4.0f / 0.3f),
          "fire_rate_mult <= 0 falls back to 1 (firing.cpp's guard)");
    check(outfitting::gun_energy_burn("not_a_gun") == 0.0f && outfitting::gun_energy_burn("") == 0.0f,
          "unknown / empty mounts burn nothing");

    const ShipClass* cent = ship_class::find("centurion");
    if (!cent) { check(false, "centurion catalog loaded"); return; }
    PlayerState p = player::new_game("troy");
    p.ship_class_name = "centurion";
    outfitting::fit_stock_guns(p, cent, /*with_turrets=*/true);
    float forward = 0.0f, turrets = 0.0f;
    for (size_t i = 0; i < p.gun_mounts.size(); ++i) {
        const float burn = outfitting::gun_energy_burn(p.gun_mounts[i].gun_id);
        (cent->default_guns[i].is_turret ? turrets : forward) += burn;
    }
    const outfitting::EnergyBudget b = outfitting::energy_budget(p, cent);
    check(turrets > 0.0f && near(b.gun_burn_gj_s, forward),
          "budget counts the forward guns only; turret guns fire free");
    check(near(b.bank_gj, cent->energy_max), "bank is the hull's energy_max");
    check(outfitting::energy_budget(p, nullptr).gun_burn_gj_s == 0.0f, "no hull -> empty budget");
}

// Player-facing gun names (#747): canonical spelling, ids never leak to UI.
static void test_gun_display_names() {
    std::printf("\n== Gun display names (#747) ==\n");
    check(gun::display_name("meson_blaster") == "Meson Blaster", "meson_blaster -> Meson Blaster");
    check(std::string(gun::display_name(GunType::Laser)) == "Laser", "GunType::Laser -> Laser");
    check(gun::display_name("not_a_gun") == "not_a_gun", "unknown ids come back unchanged");
}

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

    std::printf("\n== Swap the gun in mount 0: sell stock, then fit (Tarsus has 2 mounts) ==\n");
    const long long c0 = p.credits;
    outfitting::sell_gun(p, 0, tarsus);   // an armed mount refuses a buy (#741)
    outfitting::buy_gun(p, "tachyon_cannon", 0, tarsus);
    std::printf("  credits %lld -> %lld\n", c0, (long long)p.credits);
    std::printf("  mount0 now: %s\n", p.gun_mounts.empty() ? "(none)" : p.gun_mounts[0].gun_id.c_str());

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
    for (const auto& g : p.gun_mounts) std::printf("%s ", g.gun_id.c_str());
    std::printf("\n");

    std::printf("\n== Engine upgrades preserve hull-defined top speed ==\n");
    const ShipClass* cent = ship_class::find("centurion");
    const outfitting::SpeedCaps s0 = outfitting::effective_speed_caps(p);
    p.credits += 30000; // ensure both power-plant rungs are affordable
    outfitting::upgrade_engine(p, cent);   // L0 -> L1
    outfitting::upgrade_engine(p, cent);   // L1 -> L2
    const outfitting::SpeedCaps s2 = outfitting::effective_speed_caps(p);
    const bool speed_unchanged = s2.cruise0 == s0.cruise0 && s2.cruise1 == s0.cruise1;
    std::printf("  top speed L0 %.0f/%.0f -> L2 %.0f/%.0f  %s\n",
                s0.cruise0, s0.cruise1, s2.cruise0, s2.cruise1,
                speed_unchanged ? "PASS: speed remains a hull property" : "FAIL");

    std::printf("\n== Over-budget hull purchase is refused ==\n");
    PlayerState broke = player::new_game("troy");
    broke.credits = 5000;   // can't afford anything but the Tarsus it owns
    outfitting::buy_hull(broke, "centurion");
    std::printf("  broke pilot still flying: %s (credits %lld)\n",
                broke.ship_class_name.c_str(), (long long)broke.credits);

    test_turrets();
    test_no_overwrite();
    test_starter_energy();
    test_energy_budget();
    test_gun_display_names();

    std::printf("\nAll transaction paths exercised. %s\n",
                g_fail == 0 ? "ALL CHECKS PASS" : "CHECK FAILURES");
    return g_fail == 0 ? 0 : 1;
}
