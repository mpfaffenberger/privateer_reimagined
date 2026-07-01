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
#include "faction.h"
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

    // Real data: commodity catalog (for "iron") + contraband set (for
    // "brilliance", Phase 2) + the shipped fixer table.
    commodity::load("assets/data/privateer_db/cargo.toml");
    commodity::load_contraband("assets/data/contraband.json");
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

    // =======================================================================
    // Phase 2: the Tayla arc (M02-M05, #114-#117)
    // =======================================================================
    std::printf("\n=== Tayla arc (M02-M05) ===\n\n");
    faction::init();

    // `p` carries on from M01: sandoval_done + artifact in hand.
    const int64_t credits_before = p.credits;

    // ---- M02: plastics to Oakham (#114) -----------------------------------
    check(fixer_here("new_detroit", p, "tayla_m02_offer"),
          "M02 offered at New Detroit after sandoval_done");
    fixers::accept(*fixers::find("tayla_m02_offer"), p);
    check(plot::has_flag(p, "m02_active"),      "M02 accept: m02_active");
    check(plot::has_flag(p, "tayla_employed"),  "M02 accept: tayla_employed");
    check(cargo_units(p, "plastics") == 30,     "M02 accept: 30 plastics aboard");

    // ---- pirate-neutrality override (#114) --------------------------------
    {
        p.rep.rep[(int)Faction::Pirate] = -100;   // pirates should HATE us
        campaign::tick(p, "pentonville");
        check(faction::stance_npc_vs_player(Faction::Pirate, p.rep) ==
                  Stance::Neutral,
              "override ON: Pentonville pirates neutral at rep -100");
        campaign::tick(p, "troy");
        check(faction::stance_npc_vs_player(Faction::Pirate, p.rep) ==
                  Stance::Hostile,
              "override OFF outside Pentonville: hostile again");
        p.rep.rep[(int)Faction::Pirate] = 0;
    }

    campaign::on_dock(p, "oakham_pirate");
    check(!plot::has_flag(p, "m02_active"),     "M02 delivery clears active");
    check(plot::has_flag(p, "m02_delivered"),   "M02 delivered flag");
    check(cargo_units(p, "plastics") == 0,      "M02 plastics removed");
    check(p.credits == credits_before + 10000,  "M02 pays 10,000 on landing");
    check(plot::has_flag(p, "tayla_1_done"),
          "M02 landing sets tayla_1_done (no return leg)");

    // ---- M03: Brilliance to Hector (#115) ---------------------------------
    check(fixer_here("oakham_pirate", p, "tayla_m03_offer"),
          "M03 offered at Oakham after tayla_1_done");
    fixers::accept(*fixers::find("tayla_m03_offer"), p);
    check(cargo_units(p, "brilliance") == 15,   "M03: 15 brilliance aboard");
    check(player::carrying_contraband(p),       "M03: open brilliance IS scannable");
    campaign::on_dock(p, "hector");
    check(plot::has_flag(p, "m03_delivered") &&
          p.credits == credits_before + 25000,  "M03 pays 15,000 at Hector");
    check(fixer_here("oakham_pirate", p, "tayla_m03_debrief"),
          "M03 debrief waiting at Oakham");
    fixers::dialogue_done(*fixers::find("tayla_m03_debrief"), p);
    check(plot::has_flag(p, "tayla_2_done"),    "M03 debrief sets tayla_2_done");

    // ---- M03 failure path: consignment sold mid-route ---------------------
    {
        PlayerState f = player::new_game("troy");
        plot::set_flag(f, "sandoval_done");
        plot::set_flag(f, "tayla_1_done");
        fixers::accept(*fixers::find("tayla_m03_offer"), f);
        player::remove_cargo(f, "brilliance", 10);   // sold most of it
        campaign::on_dock(f, "achilles");            // ANY dock detects it
        check(!plot::has_flag(f, "m03_active"),
              "M03 fail: off-destination dock with missing cargo clears active");
        check(fixer_here("oakham_pirate", f, "tayla_m03_offer"),
              "M03 fail: offer re-appears at Oakham");
    }

    // ---- M04: Brilliance to New Constantinople + compartment (#116) -------
    check(fixer_here("oakham_pirate", p, "tayla_m04_offer"),
          "M04 offered after tayla_2_done");
    fixers::accept(*fixers::find("tayla_m04_offer"), p);
    check(cargo_units(p, "brilliance") == 25,   "M04: 25 brilliance aboard");
    campaign::on_dock(p, "new_constantinople");
    check(plot::has_flag(p, "m04_delivered") &&
          p.credits == credits_before + 45000,  "M04 pays 20,000");
    fixers::dialogue_done(*fixers::find("tayla_m04_debrief"), p);
    check(plot::has_item(p, "secret_compartment"),
          "M04 debrief installs the secret compartment");
    check(plot::has_flag(p, "tayla_3_done"),    "M04 debrief sets tayla_3_done");

    // ---- secret compartment mechanics (#116) ------------------------------
    {
        PlayerState c = player::new_game("troy");
        plot::give_item(c, "secret_compartment");
        check(!player::add_compartment_cargo(c, "iron", 5),
              "compartment refuses non-contraband");
        check(player::add_compartment_cargo(c, "brilliance", 15),
              "compartment stows 15 brilliance");
        check(!player::add_compartment_cargo(c, "brilliance", 6),
              "compartment refuses overflow past 20");
        check(player::add_compartment_cargo(c, "brilliance", 5),
              "compartment tops up to exactly 20");
        check(player::compartment_units_used(c) == 20,
              "compartment usage reads 20/20");
        check(!player::carrying_contraband(c),
              "stowed contraband is INVISIBLE to scans");
        check(player::cargo_units_used(c) == 0,
              "stowed goods take no hold space");
        player::add_cargo(c, "brilliance", 5, 0, 100);
        check(player::carrying_contraband(c),
              "open brilliance alongside stowed IS scannable");
        check(player::cargo_units_used(c) == 5,
              "hold accounting counts only the open stack");
    }

    // ---- M05: final run, stowed in the compartment (#117) ------------------
    check(fixer_here("oakham_pirate", p, "tayla_m05_offer"),
          "M05 offered after tayla_3_done");
    fixers::accept(*fixers::find("tayla_m05_offer"), p);
    check(plot::has_flag(p, "m05_active"),      "M05 active");
    check(cargo_units(p, "brilliance") == 20,   "M05: 20 brilliance aboard");
    check(!player::carrying_contraband(p),
          "M05 consignment auto-stowed: scan-clean");
    check(player::compartment_units_used(p) == 20,
          "M05 fills the compartment exactly");
    campaign::on_dock(p, "new_constantinople");
    check(plot::has_flag(p, "m05_delivered") &&
          p.credits == credits_before + 55000,  "M05 pays 10,000");
    check(player::compartment_units_used(p) == 0,
          "M05 delivery empties the compartment");
    fixers::dialogue_done(*fixers::find("tayla_m05_debrief"), p);
    check(plot::has_flag(p, "tayla_done"),      "debrief sets tayla_done");
    check(!plot::has_flag(p, "tayla_employed"), "debrief ends the employment");
    campaign::tick(p, "pentonville");
    check(!faction::player_stance_override_active(Faction::Pirate),
          "pirate-neutrality override lapses with tayla_done");
    check(!fixer_here("oakham_pirate", p, "tayla_m05_offer") &&
          !fixer_here("oakham_pirate", p, "tayla_m05_debrief"),
          "Tayla's Oakham entries retire after the chain");

    std::printf("\n=== %s ===\n",
                g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}
