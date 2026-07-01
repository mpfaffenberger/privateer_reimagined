// -----------------------------------------------------------------------------
// tools/test_campaign.cpp — headless proof for campaign mission logic
// (epic #136) + the SHIPPED fixer content in assets/data/fixers.json.
//
// Links the real campaign.cpp / fixers.cpp / plot.cpp and drives M01
// (#113) end to end without a renderer:
//
//   1. the real fixers.json: Sandoval gates at New Detroit (bare AND
//      suffixed base id), Tayla hidden until m01_delivered,
//   2. accept: hold-space refusal leaves the offer re-takeable; success
//      loads 40 iron + m01_active,
//   3. dock at Liverpool with the iron -> delivered flag, cargo removed,
//   4. dock at Liverpool WITHOUT the iron -> failure clears m01_active,
//   5. wrong-base docks are no-ops,
//   6. Tayla's done_actions grant the artifact + sandoval_done, and the
//      full-chain gate flip hides both entries afterwards.
//
// Build:
//   cmake --build build --target test_campaign && ./build/test_campaign
// Run from the repo root (loads assets/data/fixers.json + cargo.toml).
// -----------------------------------------------------------------------------

#include "campaign.h"
#include "commodity.h"
#include "fixers.h"
#include "player.h"
#include "plot.h"
#include "ship_class.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    if (!ok) ++g_fail;
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what);
}

int cargo_units(const PlayerState& p, const std::string& id) {
    int n = 0;
    for (const CargoEntry& e : p.cargo)
        if (e.commodity_id == id) n += e.units;
    return n;
}

bool fixer_here(const std::string& base, const PlayerState& p,
                const std::string& id) {
    for (const fixers::FixerDef* f : fixers::present_at(base, "", p))
        if (f->id == id) return true;
    return false;
}

} // namespace

int main() {
    std::printf("=== campaign M01 harness (#113) ===\n\n");

    // Real data: commodity catalog (for "iron") + the shipped fixer table.
    commodity::load("assets/data/privateer_db/cargo.toml");
    ship_class::load_all("assets/ships");   // tarsus hold capacity
    check(fixers::load("assets/data/fixers.json") >= 2,
          "shipped fixers.json loads (>= 2 entries)");
    campaign::init();

    PlayerState p = player::new_game("troy");   // tarsus, 100-unit hold

    // ---- 1. placement + gating off the SHIPPED table ---------------------
    check(fixer_here("new_detroit", p, "sandoval_offer"),
          "Sandoval at 'new_detroit' (bare id)");
    check(fixer_here("new_detroit_industrial", p, "sandoval_offer"),
          "Sandoval at 'new_detroit_industrial' (suffixed nav id)");
    check(!fixer_here("liverpool", p, "sandoval_offer"),
          "Sandoval absent elsewhere");
    check(!fixer_here("new_detroit", p, "tayla_artifact_handoff"),
          "Tayla hidden before delivery");

    // ---- 2. accept: hold-space refusal, then success ----------------------
    {
        // Stuff the hold so 40 units can't fit (tarsus base 100).
        PlayerState full = p;
        player::add_cargo(full, "iron", 80, 0, 100);
        const fixers::FixerDef* sand = fixers::find("sandoval_offer");
        check(sand != nullptr, "find(sandoval_offer)");
        fixers::accept(*sand, full);
        check(!plot::has_flag(full, "m01_active"),
              "full hold: accept refused, m01_active NOT set");
        check(fixer_here("new_detroit", full, "sandoval_offer"),
              "full hold: offer still on the table");
    }
    {
        const fixers::FixerDef* sand = fixers::find("sandoval_offer");
        fixers::accept(*sand, p);
        check(plot::has_flag(p, "m01_active"), "accept: m01_active set");
        check(cargo_units(p, "iron") == 40,    "accept: 40 iron aboard");
        check(!fixer_here("new_detroit", p, "sandoval_offer"),
              "offer hidden while active");
    }

    // ---- 3. wrong-base dock is a no-op ------------------------------------
    campaign::on_dock(p, "achilles");
    check(plot::has_flag(p, "m01_active") && cargo_units(p, "iron") == 40,
          "docking elsewhere changes nothing");

    // ---- 4. delivery at Liverpool (suffixed nav id) ------------------------
    campaign::on_dock(p, "liverpool_refinery");
    check(!plot::has_flag(p, "m01_active"),   "delivery clears m01_active");
    check(plot::has_flag(p, "m01_delivered"), "delivery sets m01_delivered");
    check(cargo_units(p, "iron") == 0,        "delivery removes the iron");

    // ---- 5. Tayla handoff --------------------------------------------------
    check(fixer_here("new_detroit", p, "tayla_artifact_handoff"),
          "Tayla appears after delivery");
    check(!fixer_here("new_detroit", p, "sandoval_offer"),
          "Sandoval gone after delivery");
    {
        const fixers::FixerDef* tay = fixers::find("tayla_artifact_handoff");
        check(tay != nullptr, "find(tayla_artifact_handoff)");
        fixers::dialogue_done(*tay, p);
        check(plot::has_item(p, "steltek_artifact"),
              "handoff grants steltek_artifact");
        check(plot::has_flag(p, "sandoval_done"),
              "handoff sets sandoval_done");
        check(!fixer_here("new_detroit", p, "tayla_artifact_handoff"),
              "Tayla handoff entry retires after sandoval_done");
    }

    // ---- 6. failure path: sold the consignment ----------------------------
    {
        PlayerState f = player::new_game("troy");
        const fixers::FixerDef* sand = fixers::find("sandoval_offer");
        fixers::accept(*sand, f);
        player::remove_cargo(f, "iron", 40);   // "sold" it
        campaign::on_dock(f, "liverpool_refinery");
        check(!plot::has_flag(f, "m01_active"),
              "missing cargo at Liverpool clears m01_active (failed)");
        check(!plot::has_flag(f, "m01_delivered"),
              "failed run is NOT delivered");
        check(fixer_here("new_detroit", f, "sandoval_offer"),
              "failed run: Sandoval re-offers at New Detroit");
    }

    std::printf("\n=== %s ===\n",
                g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}
