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
#include "gun.h"
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

// Archetype-placement variant (Goodin, #134: "any mining base").
bool fixer_here_arch(const std::string& base, const std::string& arch,
                     const PlayerState& p, const std::string& id) {
    for (const fixers::FixerDef* f : fixers::present_at(base, arch, p))
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

    // =======================================================================
    // Phase 3: the Lynch arc (M06-M09, #118-#121)
    // =======================================================================
    std::printf("\n=== Lynch arc (M06-M09) ===\n\n");

    const int64_t lynch_base = p.credits;

    // ---- M06: the Seelig message (#118) ------------------------------------
    check(fixer_here("new_constantinople", p, "lynch_m06_offer"),
          "M06 offered at NC after tayla_done");
    fixers::accept(*fixers::find("lynch_m06_offer"), p);
    check(plot::has_flag(p, "m06_active"), "M06 active");
    // The Seelig scenario's on_dialogue_done sets the delivered flag
    // (director path is live-verified; this is the settle-logic proxy).
    plot::set_flag(p, "m06_message_delivered");
    check(fixer_here("new_constantinople", p, "lynch_m06_debrief"),
          "M06 debrief waiting at NC");
    fixers::dialogue_done(*fixers::find("lynch_m06_debrief"), p);
    check(p.credits == lynch_base + 10000,  "M06 debrief pays 10,000 (pay: token)");
    check(plot::has_flag(p, "lynch_1_done") && !plot::has_flag(p, "m06_active"),
          "M06 settles: lynch_1_done, active cleared");

    // ---- M07: weapons to Siva (#119) ---------------------------------------
    check(fixer_here("new_constantinople", p, "lynch_m07_offer"),
          "M07 offered after lynch_1_done");
    fixers::accept(*fixers::find("lynch_m07_offer"), p);
    check(cargo_units(p, "weaponry") == 20, "M07: 20 weaponry aboard");
    campaign::on_dock(p, "siva");
    check(p.credits == lynch_base + 25000,  "M07 pays 15,000 at Siva");
    check(plot::has_flag(p, "lynch_2_done"), "M07 delivery sets lynch_2_done");

    // ---- M08: the cousin (passenger, #120) ----------------------------------
    check(fixer_here("new_constantinople", p, "lynch_m08_offer"),
          "M08 offered after lynch_2_done");
    fixers::accept(*fixers::find("lynch_m08_offer"), p);
    check(plot::has_item(p, "lynch_cousin"), "M08 accept: cousin aboard (plot item)");
    check(player::cargo_units_used(p) == 0,  "M08 passenger takes no hold space");
    campaign::on_dock(p, "achilles");
    check(plot::has_flag(p, "m08_active") && plot::has_item(p, "lynch_cousin"),
          "M08 passenger can't be lost at a wrong dock");
    campaign::on_dock(p, "romulus");
    check(p.credits == lynch_base + 55000,  "M08 pays 30,000 on landing");
    check(!plot::has_item(p, "lynch_cousin"), "M08 cousin disembarks at Romulus");
    check(plot::has_flag(p, "lynch_3_done"), "M08 sets lynch_3_done");

    // ---- M09: the Miggs betrayal (#121) -------------------------------------
    check(fixer_here("new_constantinople", p, "lynch_m09_offer"),
          "M09 offered after lynch_3_done");
    fixers::accept(*fixers::find("lynch_m09_offer"), p);
    check(plot::has_flag(p, "m09_active"), "M09 active (no payload)");
    // Docking at Liverpool (the fake pickup) settles NOTHING.
    campaign::on_dock(p, "liverpool_refinery");
    check(plot::has_flag(p, "m09_active"), "M09 still active at Liverpool - no Smythe");
    // The Miggs scenario's on_dialogue_done sets the reveal (proxy).
    plot::set_flag(p, "m09_reveal");
    campaign::on_dock(p, "oxford");
    check(!plot::has_flag(p, "m09_active"), "M09 resolves on landing at Oxford");
    check(plot::has_flag(p, "lynch_done"),  "M09 sets lynch_done");
    check(p.credits == lynch_base + 55000,  "M09 pays NOTHING (the mob, folks)");
    check(!fixer_here("new_constantinople", p, "lynch_m09_offer"),
          "Lynch offers retire after lynch_done");

    // =======================================================================
    // Phase 4: the Masterson arc (M10-M13, #122-#125 + escort infra #140)
    // =======================================================================
    std::printf("\n=== Masterson arc (M10-M13) ===\n\n");

    const int64_t mast_base = p.credits;

    // ---- M10: escort settle + landing-order gate (#122) --------------------
    check(fixer_here("oxford", p, "masterson_m10_offer"),
          "M10 offered at Oxford after lynch_done");
    fixers::accept(*fixers::find("masterson_m10_offer"), p);
    check(plot::has_flag(p, "m10_active"), "M10 active");
    // Docking before the meet (not underway): a no-op, mission continues.
    campaign::on_dock(p, "oxford");
    check(plot::has_flag(p, "m10_active"),
          "M10 docking before the meet changes nothing");
    // Escort landed (escort.cpp proxy), THEN the player lands: success.
    plot::set_flag(p, "m10_underway");
    plot::set_flag(p, "m10_escortee_landed");
    campaign::on_dock(p, "oxford");
    check(p.credits == mast_base + 10000, "M10 pays 10,000");
    check(plot::has_flag(p, "masterson_1_done") &&
          !plot::has_flag(p, "m10_active") &&
          !plot::has_flag(p, "m10_escortee_landed"),
          "M10 settles clean (flags consumed)");

    // Landing-order violation: player lands while the Drayman is still up.
    {
        PlayerState v = player::new_game("troy");
        plot::set_flag(v, "lynch_done");
        fixers::accept(*fixers::find("masterson_m10_offer"), v);
        plot::set_flag(v, "m10_underway");   // met the Drayman, it's flying
        campaign::on_dock(v, "oxford");
        check(!plot::has_flag(v, "m10_active"),
              "landing-order violation FAILS the escort");
        check(!plot::has_flag(v, "masterson_1_done"),
              "violation pays nothing");
        check(fixer_here("oxford", v, "masterson_m10_offer"),
              "violation: Masterson re-offers");
    }

    // ---- M11: the Black Rhombus hunt (#123) ---------------------------------
    check(fixer_here("oxford", p, "masterson_m11_offer"),
          "M11 offered after masterson_1_done");
    fixers::accept(*fixers::find("masterson_m11_offer"), p);
    plot::set_flag(p, "m11_found");            // scenario reveal proxy
    campaign::on_dock(p, "achilles");          // docked mid-hunt, no kill
    check(plot::has_flag(p, "m11_active") && !plot::has_flag(p, "m11_found"),
          "M11 docking mid-hunt re-arms the reveal");
    plot::set_flag(p, "killed:black_rhombus"); // kill-memory proxy
    campaign::on_dock(p, "oxford");
    check(p.credits == mast_base + 20000, "M11 pays 10,000");
    check(plot::has_flag(p, "masterson_2_done"), "masterson_2_done");

    // ---- M12 + M13: remaining escorts -> the library (#124/#125) -----------
    fixers::accept(*fixers::find("masterson_m12_offer"), p);
    plot::set_flag(p, "m12_underway");
    plot::set_flag(p, "m12_escortee_landed");
    campaign::on_dock(p, "oxford");
    check(plot::has_flag(p, "masterson_3_done") &&
          p.credits == mast_base + 30000, "M12 settles (+10,000)");
    fixers::accept(*fixers::find("masterson_m13_offer"), p);
    plot::set_flag(p, "m13_underway");
    plot::set_flag(p, "m13_escortee_landed");
    campaign::on_dock(p, "oxford");
    check(plot::has_flag(p, "masterson_done") &&
          p.credits == mast_base + 40000, "M13 settles (+10,000)");

    // ---- the library scene (#125 reward) ------------------------------------
    check(fixer_here("oxford", p, "oxford_library_scene"),
          "library scene unlocked after masterson_done");
    fixers::dialogue_done(*fixers::find("oxford_library_scene"), p);
    check(plot::has_flag(p, "library_access"),
          "library scene grants library_access (Steltek + Monkhouse lead)");
    check(!fixer_here("oxford", p, "oxford_library_scene"),
          "library scene retires after access granted");

    // ---- Phase 5: the Murphy blockade arc (#126-#128) -----------------------
    check(campaign::palan_blockaded(p),
          "Palan blockaded once the Murphy arc opens (masterson_done)");
    {
        PlayerState sandbox = player::new_game("troy");
        check(!campaign::palan_blockaded(sandbox),
              "sandbox players never see the blockade");
    }
    check(fixer_here("basra_refinery", p, "murphy_m14_offer"),
          "M14 offered at Basra after masterson_done");
    fixers::accept(*fixers::find("murphy_m14_offer"), p);
    check(plot::has_flag(p, "m14_active"), "m14 active");
    plot::set_flag(p, "m14_cleared");          // scenario on_cleared proxy
    const int64_t murphy_base = p.credits;
    check(fixer_here("basra_refinery", p, "murphy_m14_debrief"),
          "M14 debrief appears once the waves die");
    plot::clear_flag(p, "m14_active");
    fixers::dialogue_done(*fixers::find("murphy_m14_debrief"), p);
    check(p.credits == murphy_base + 15000 &&
          plot::has_flag(p, "murphy_1_done"), "M14 debrief pays 15,000");

    fixers::accept(*fixers::find("murphy_m15_offer"), p);
    plot::set_flag(p, "m15_cleared");
    plot::clear_flag(p, "m15_active");
    fixers::dialogue_done(*fixers::find("murphy_m15_debrief"), p);
    check(p.credits == murphy_base + 25000 &&
          plot::has_flag(p, "murphy_2_done"), "M15 debrief pays 10,000");

    // M16: the blockade holds until the final wave dies, then landing pays.
    fixers::accept(*fixers::find("murphy_m16_offer"), p);
    check(campaign::palan_blockaded(p), "blockade still up mid-M16");
    plot::set_flag(p, "palan_blockade_lifted");   // wave on_cleared proxy
    check(!campaign::palan_blockaded(p), "blockade lifts with the last wave");
    campaign::on_dock(p, "palan");
    check(p.credits == murphy_base + 40000 &&
          plot::has_flag(p, "murphy_done"), "M16 landing pays 15,000 + murphy_done");

    // ---- M17: Monkhouse to Basra (#129) --------------------------------------
    check(fixer_here("palan", p, "monkhouse_m17_offer"),
          "Monkhouse waits in the Palan bar");
    fixers::accept(*fixers::find("monkhouse_m17_offer"), p);
    check(plot::has_item(p, "dr_monkhouse"), "the doctor boards");
    campaign::on_dock(p, "basra_refinery");
    check(!plot::has_item(p, "dr_monkhouse") &&
          p.credits == murphy_base + 45000, "M17 delivers the doctor (+5,000)");
    check(fixer_here("basra_refinery", p, "monkhouse_m17_debrief"),
          "the lab scene awaits at Basra");
    fixers::dialogue_done(*fixers::find("monkhouse_m17_debrief"), p);
    check(plot::has_item(p, "steltek_map") &&
          plot::has_flag(p, "monkhouse_done"),
          "artifact pieces merge into the steltek_map; Cross is next");

    // ---- Phase 6: the Cross arc (#130-#133) ----------------------------------
    check(!campaign::frontier_locked(p, "delta"),
          "frontier OPEN once monkhouse_done is set");
    {
        PlayerState sandbox = player::new_game("troy");
        check(campaign::frontier_locked(sandbox, "delta") &&
              campaign::frontier_locked(sandbox, "delta_prime"),
              "frontier LOCKED for sandbox players");
        check(!campaign::frontier_locked(sandbox, "troy"),
              "core systems never lock");
    }
    check(fixer_here("rygannon", p, "cross_m18_offer"),
          "Cross recruits at Rygannon after monkhouse_done");
    fixers::accept(*fixers::find("cross_m18_offer"), p);
    for (const char* f : { "m18_nav1", "m18_nav2", "m18_nav3", "m18_nav4" })
        plot::set_flag(p, f);              // survey scenario proxies
    const int64_t cross_base = p.credits;
    check(fixer_here("rygannon", p, "cross_m18_debrief"),
          "M18 debrief gates on all four survey flags");
    fixers::dialogue_done(*fixers::find("cross_m18_debrief"), p);
    check(p.credits == cross_base + 10000 && plot::has_flag(p, "cross_1_done"),
          "M18 pays 10,000 + cross_1_done");

    fixers::accept(*fixers::find("cross_m19_offer"), p);
    check(!fixer_here("rygannon", p, "cross_m19_debrief"),
          "M19 debrief waits for Garrovick's death");
    plot::set_flag(p, "killed:garrovick");
    fixers::dialogue_done(*fixers::find("cross_m19_debrief"), p);
    check(p.credits == cross_base + 20000 && plot::has_flag(p, "cross_2_done"),
          "M19 pays 10,000 + cross_2_done");

    fixers::accept(*fixers::find("cross_m20_offer"), p);
    for (const char* f : { "m20_nav1", "m20_nav2", "m20_nav3", "m20_nav4" })
        plot::set_flag(p, f);
    fixers::dialogue_done(*fixers::find("cross_m20_debrief"), p);
    check(p.credits == cross_base + 30000 && plot::has_flag(p, "cross_3_done"),
          "M20 pays 10,000 + cross_3_done");

    // M21: the gun + the drone.
    fixers::accept(*fixers::find("cross_m21_offer"), p);
    campaign::on_dock(p, "rygannon");
    check(!plot::has_flag(p, "cross_done"),
          "docking WITHOUT the gun does not settle M21");
    plot::run_action(p, "m21:take_gun");
    check(plot::has_item(p, "steltek_gun") &&
          plot::has_flag(p, "steltek_gun_owned") &&
          plot::has_flag(p, "drone_active"),
          "taking the gun mounts it + wakes the drone");
    bool mounted = false;
    for (const MountSlot& m : p.gun_mounts)
        if (m.gun_id == "steltek_gun") mounted = true;
    check(mounted, "steltek_gun occupies a mount slot");
    plot::run_action(p, "m21:take_gun");   // idempotent re-run
    int steltek_mounts = 0;
    for (const MountSlot& m : p.gun_mounts)
        if (m.gun_id == "steltek_gun") ++steltek_mounts;
    check(steltek_mounts == 1, "take_gun is idempotent");
    campaign::on_dock(p, "rygannon");
    check(p.credits == cross_base + 40000 && plot::has_flag(p, "cross_done"),
          "M21 settles at Rygannon with the gun (+10,000)");

    // ---- Phase 7: Goodin + the Terrell finale (#134, #135) -------------------
    check(fixer_here_arch("achilles", "mining", p, "goodin_offer"),
          "Goodin appears at ANY mining base once cross_done");
    check(!fixer_here_arch("rygannon", "mining", p, "goodin_offer"),
          "Goodin never at Rygannon (excluded)");
    check(!fixer_here_arch("perry_naval", "mining", p, "goodin_offer"),
          "Goodin never at Perry itself (excluded)");
    check(!fixer_here_arch("new_detroit", "industrial", p, "goodin_offer"),
          "Goodin absent off the mining archetype");
    fixers::accept(*fixers::find("goodin_offer"), p);
    check(plot::has_flag(p, "m22_active"), "M22 accepted (the summons)");
    const int64_t pre_m22 = p.credits;
    campaign::on_dock(p, "achilles");
    check(plot::has_flag(p, "m22_active"), "wrong-base dock is a no-op");
    campaign::on_dock(p, "perry_naval");
    check(!plot::has_flag(p, "m22_active") &&
          plot::has_flag(p, "goodin_done") && p.credits == pre_m22,
          "M22 settles at Perry Naval Base, pays NOTHING");

    // M23: the boost + the drone kill + the office debrief.
    check(fixer_here("perry_naval", p, "terrell_offer"),
          "Terrell's office opens behind goodin_done");
    fixers::accept(*fixers::find("terrell_offer"), p);
    check(plot::has_flag(p, "m23_active"), "M23 accepted");
    check(!fixer_here("perry_naval", p, "terrell_debrief"),
          "debrief waits for the drone kill");

    // The Steltek boost: flag lands via the action grammar; the stat
    // mutation re-derives from plot state in campaign::tick.
    gun::load_table("assets/data/privateer_ship_data.json");
    const float dmg_stock = g_gun_stats[(int)GunType::SteltekGun].damage_cm;
    check(dmg_stock == 10.0f, "stock Steltek gun loads complete at 10 cm");
    plot::run_action(p, "m23:boost_gun");
    check(plot::has_flag(p, "steltek_gun_boosted"), "boost flag set");
    campaign::tick(p, "nitir");
    check(g_gun_stats[(int)GunType::SteltekGun].damage_cm == 19.0f,
          "boost re-derives Mega Steltek stats (19 cm)");
    {   // and it reverts for an unboosted player in the same session
        PlayerState sandbox = player::new_game("troy");
        campaign::tick(sandbox, "troy");
        check(g_gun_stats[(int)GunType::SteltekGun].damage_cm == dmg_stock,
              "unboosted plot state reverts the stat table");
        campaign::tick(p, "nitir");   // restore for the finale below
    }

    plot::set_flag(p, "killed:steltek_drone");   // the Tango kill (live-path
                                                 // proof is the smoke run)
    const int64_t pre_m23 = p.credits;
    check(fixer_here("perry_naval", p, "terrell_debrief"),
          "debrief gates open on the kill-memory flag");
    fixers::dialogue_done(*fixers::find("terrell_debrief"), p);
    check(p.credits == pre_m23 + 30000, "M23 pays 30,000");
    check(plot::has_flag(p, "terrell_done") &&
          plot::has_flag(p, "campaign_complete"),
          "terrell_done + campaign_complete set");
    check(!plot::has_flag(p, "drone_active") &&
          !plot::has_flag(p, "m23_active"),
          "the pursuit ends: drone_active + m23_active cleared");
    check(fixer_here("perry_naval", p, "terrell_epilogue"),
          "the famous office epilogue is waiting");
    fixers::dialogue_done(*fixers::find("terrell_epilogue"), p);
    check(!fixer_here("perry_naval", p, "terrell_epilogue"),
          "epilogue plays once");

    std::printf("\n=== %s ===\n",
                g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return g_fail == 0 ? 0 : 1;
}
