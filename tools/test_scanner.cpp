// -----------------------------------------------------------------------------
// tools/test_scanner.cpp — headless proof for the scanner product line (#143).
//
// Loads the REAL assets/data/equipment_prices.json catalog and drives the
// same scanner:: transactions the Equipment screen calls:
//   * the nine gamefaq 4.6.6 models exist with their canonical price +
//     colour / Target Lock / ITTS features,
//   * capability queries treat "no scanner" as hull-default range and
//     monochrome / no lock / no ITTS,
//   * a new game starts with k_starting_scanner, which resolves,
//   * fit / trade-in / downgrade / sell / refusal credit math,
//   * a hull swap keeps the fitted scanner (gamefaq: it transfers).
//   * identification (#516): once-per-second rolls against the gamefaq
//     % chance, restarting on retarget, and the UNKNOWN -> identified flip.
//
//   cmake --build build --target test_scanner && ./build/test_scanner
// (run from the repo root — asset paths are relative)
// -----------------------------------------------------------------------------

#include "armor.h"
#include "faction.h"
#include "gun.h"
#include "outfitting.h"
#include "player.h"
#include "scanner.h"
#include "shield.h"
#include "ship_class.h"

#include <cstdio>
#include <string>

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

struct Expected { const char* id; int64_t price; bool color, lock, itts; float id_pct; };

// gamefaq 4.6.6: Iris no colour, Hunter AW friend/foe colour, B&S full
// colour; tier 2 adds Target Lock, tier 3 adds ITTS; last column is the
// "% chance per second of identifying target" (#516).
constexpr Expected kFaq[] = {
    {"iris_mk1",            10000, false, false, false,  5},
    {"iris_mk2",            30000, false, true,  false, 15},
    {"iris_mk3",            60000, false, true,  true,  25},
    {"hunter_aw_6",         30000, true,  false, false,  5},
    {"hunter_aw_6i",        50000, true,  true,  false, 15},
    {"hunter_aw_infinity",  80000, true,  true,  true,  25},
    {"bs_tripwire",         40000, true,  false, false, 15},
    {"bs_eye",              70000, true,  true,  false, 25},
    {"bs_omni",            100000, true,  true,  true,  35},
};

// Scripted roll source: replays `vals` in order, counting calls.
struct ScriptedRolls {
    const float* vals; int n; int calls = 0;
    float operator()() { return vals[calls++ % n]; }
};

} // namespace

int main() {
    std::printf("=== #143 scanner catalog + transactions ===\n");
    faction::init();
    gun::load_table("assets/data/privateer_ship_data.json");
    shield::load_table("assets/data/privateer_ship_data.json");
    armor::load_table("assets/data/privateer_ship_data.json");
    ship_class::load_all("assets/ships");
    outfitting::load("assets/data/ship_prices.json", "assets/data/equipment_prices.json");
    const int n = scanner::load("assets/data/equipment_prices.json");

    std::printf("\n-- catalog matches gamefaq 4.6.6 --\n");
    check(n == 9, "nine scanner models loaded");
    for (const Expected& e : kFaq) {
        const ScannerType* s = scanner::find(e.id);
        char what[128];
        std::snprintf(what, sizeof what, "%s: price/colour/lock/ITTS/identify%%", e.id);
        check(s && s->price == e.price && s->color_iff == e.color &&
              s->target_lock == e.lock && s->itts == e.itts && s->range_m > 0.0f &&
              s->identify_pct_per_s == e.id_pct, what);
    }
    const ScannerType* mk1  = scanner::find("iris_mk1");
    const ScannerType* omni = scanner::find("bs_omni");
    check(mk1 && mk1->range_m == k_default_radar_range_m,
          "Iris Mk I range == the 15 km hull default NPCs keep");
    check(mk1 && omni && omni->range_m > mk1->range_m, "top tier out-ranges the base tier");

    std::printf("\n-- capability queries --\n");
    check(scanner::range_m(nullptr, 12345.0f) == 12345.0f, "no scanner -> hull default range");
    check(!scanner::color_iff(nullptr) && !scanner::target_lock(nullptr) && !scanner::itts(nullptr),
          "no scanner -> monochrome, no lock, no ITTS");
    check(omni && scanner::range_m(omni, 12345.0f) == omni->range_m, "fitted scanner overrides hull range");
    check(!scanner::find("") && !scanner::find("nope"), "empty / unknown id resolves to none");

    std::printf("\n-- new game --\n");
    PlayerState p = player::new_game("troy");
    check(p.scanner_id == player::k_starting_scanner && scanner::find(p.scanner_id),
          "new game fits k_starting_scanner and it resolves");

    std::printf("\n-- fit / trade-in / refusals --\n");
    p.credits = 45000;
    check(scanner::buy(p, "hunter_aw_6i") && p.scanner_id == "hunter_aw_6i" && p.credits == 5000,
          "Hunter AW 6i over Iris Mk I charges 50000 - 10000");
    check(!scanner::buy(p, "hunter_aw_6i") && p.credits == 5000, "re-buying the fitted model refused");
    check(!scanner::buy(p, "bogus") && p.scanner_id == "hunter_aw_6i", "unknown model refused");
    check(!scanner::buy(p, "bs_omni") && p.scanner_id == "hunter_aw_6i" && p.credits == 5000,
          "unaffordable net cost refused without mutation");
    check(scanner::buy(p, "iris_mk1") && p.credits == 45000, "downgrade refunds the difference");

    std::printf("\n-- sell --\n");
    check(scanner::sell(p) && p.scanner_id.empty() && p.credits == 55000, "sell refunds full price");
    check(!scanner::sell(p) && p.credits == 55000, "selling with nothing fitted refused");
    check(scanner::buy(p, "bs_tripwire") && p.credits == 15000, "buying into an empty bay charges full price");

    std::printf("\n-- hull swap keeps the scanner --\n");
    p.credits = 1000000;
    check(outfitting::buy_hull(p, "centurion") && p.scanner_id == "bs_tripwire",
          "buy_hull leaves the fitted scanner in place");

    std::printf("\n-- identification (#516) --\n");
    {
        const ScannerType* iris = scanner::find("iris_mk1");   // 5%/s
        check(!scanner::identify_roll(nullptr, 0.0f), "no scanner never identifies");
        check(iris && scanner::identify_roll(iris, 0.049f) && !scanner::identify_roll(iris, 0.05f),
              "Iris Mk I succeeds on a roll under 5%, fails at 5%");
        check(omni && scanner::identify_roll(omni, 0.349f) && !scanner::identify_roll(omni, 0.35f),
              "B&S Omni succeeds under 35%");

        scanner::IdentifyTimer t;
        check(scanner::identify_rolls_due(t, 7, 0.6f) == 0, "no roll before a full second");
        check(scanner::identify_rolls_due(t, 7, 0.6f) == 1, "roll falls due once 1 s accrues");
        check(scanner::identify_rolls_due(t, 7, 2.3f) == 2, "a long frame owes one roll per whole second");
        check(scanner::identify_rolls_due(t, 9, 0.9f) == 0, "retargeting restarts the clock");
        check(scanner::identify_rolls_due(t, 9, 0.1f) == 1, "...then rolls after 1 s on the new target");
        check(scanner::identify_rolls_due(t, 0, 5.0f) == 0, "no target -> no rolls");

        // Reveal state: contact reads UNKNOWN until a roll lands.
        const float misses_then_hit[] = {0.9f, 0.5f, 0.01f};
        ScriptedRolls rolls{misses_then_hit, 3};
        scanner::IdentifyTimer tt;
        bool identified = false;
        int seconds = 0;
        for (; seconds < 10 && !identified; ++seconds)
            identified = scanner::tick_identify(tt, 42, iris, 1.0f, rolls);
        check(identified && seconds == 3 && rolls.calls == 3,
              "stays UNKNOWN through two misses, identified on the 3rd second");

        const float always_hit[] = {0.0f};
        ScriptedRolls hit{always_hit, 1};
        scanner::IdentifyTimer tn;
        bool none_id = false;
        for (int i = 0; i < 10; ++i) none_id |= scanner::tick_identify(tn, 42, nullptr, 1.0f, hit);
        check(!none_id, "no scanner: contact stays UNKNOWN forever");

        // Statistical sanity: 100k one-second rolls land near the gamefaq %.
        unsigned lcg = 12345u;
        auto u01 = [&lcg] { lcg = lcg * 1664525u + 1013904223u; return (float)(lcg >> 8) / 16777216.0f; };
        int hits = 0;
        for (int i = 0; i < 100000; ++i) hits += scanner::identify_roll(omni, u01()) ? 1 : 0;
        check(hits > 34000 && hits < 36000, "B&S Omni identifies ~35% of seconds");
    }

    std::printf("\n=== %s ===\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}
