// -----------------------------------------------------------------------------
// tools/test_missions.cpp — offline driver for np-zte.1 mission computer:
// generation + accept/deliver + bounty progress + save/load round-trip.
//
// Links the REAL missions.cpp (built -DMISSIONS_HEADLESS so the ImGui board
// is excluded), comm.cpp (-DCOMM_HEADLESS), faction/player/commodity/galaxy/
// system_def/json/savegame. It walks exactly the paths the live Mission
// Computer screen + the np-ma2.1 kill hook drive, since those call the same
// model functions this test does:
//
//   * generate() a deterministic board for Achilles (Troy) and print the
//     cargo + bounty offers,
//   * ACCEPT a cargo mission (cargo loaded into the hold) + prove an
//     over-capacity accept is REFUSED,
//   * "travel" to the destination + DELIVER (reward paid, cargo removed),
//   * ACCEPT a bounty + simulate qualifying kills via on_player_kill ->
//     progress increments -> completion auto-pays,
//   * SAVE then LOAD and confirm accepted missions survive the round-trip.
//
// Build:
//   clang++ -std=c++20 -DMISSIONS_HEADLESS -DCOMM_HEADLESS -Isrc -Ithird_party \
//       tools/test_missions.cpp src/missions.cpp src/comm.cpp src/faction.cpp \
//       src/player.cpp src/commodity.cpp src/galaxy.cpp src/system_def.cpp \
//       src/savegame.cpp src/json.cpp -o /tmp/test_missions
// -----------------------------------------------------------------------------

#include "missions.h"
#include "comm.h"
#include "commodity.h"
#include "faction.h"
#include "galaxy.h"
#include "player.h"
#include "savegame.h"
#include "ship_class.h"

#include <cstdio>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    faction::init();
    comm::load("assets/data/comm_lines.json");
    commodity::load("docs/privateer_db/cargo.toml");

    galaxy::Galaxy gal;
    if (!galaxy::load("assets/galaxy.json", gal)) {
        std::printf("FAIL: could not load galaxy\n");
        return 1;
    }

    // A Tarsus-sized hold (100 units), fresh-start bankroll.
    ShipClass tarsus; tarsus.cargo_units = 100;
    PlayerState p = player::new_game("troy");
    p.last_docked_base = "achilles";
    const int cap = player::cargo_capacity(p, &tarsus);

    // ---- 1. generation -------------------------------------------------------
    std::printf("\n=================================================================\n");
    std::printf("1. GENERATE board for Achilles (Troy), fixed seed\n");
    std::printf("=================================================================\n");
    const std::vector<missions::Mission> board =
        missions::generate("achilles", "troy", gal, /*seed=*/0xABCDEF01u);
    for (const missions::Mission& m : board) {
        const bool cargo = (m.type == missions::MissionType::CargoDelivery);
        std::printf("  %-9s %-44s reward %lld\n", cargo ? "[CARGO]" : "[BOUNTY]",
                    m.title.c_str(), (long long)m.reward);
        if (cargo)
            std::printf("            -> %d %s to %s in %s\n", m.units,
                        m.commodity_id.c_str(), m.dest_base.c_str(),
                        m.dest_system.c_str());
    }
    check(!board.empty(), "board generated some missions");

    // Find the first cargo + first bounty offer.
    const missions::Mission* cargo  = nullptr;
    const missions::Mission* bounty = nullptr;
    for (const missions::Mission& m : board) {
        if (!cargo  && m.type == missions::MissionType::CargoDelivery) cargo  = &m;
        if (!bounty && m.type == missions::MissionType::Bounty)        bounty = &m;
    }
    check(cargo  != nullptr, "board contains a cargo-delivery mission");
    check(bounty != nullptr, "board contains a bounty mission");
    if (!cargo || !bounty) return 1;

    // ---- 2. accept cargo + capacity enforcement ------------------------------
    std::printf("\n=================================================================\n");
    std::printf("2. ACCEPT cargo mission + over-capacity refusal\n");
    std::printf("=================================================================\n");
    const long long cr_before_accept = p.credits;
    std::printf("  accepting: %s (%d %s -> %s)\n", cargo->title.c_str(),
                cargo->units, cargo->commodity_id.c_str(), cargo->dest_base.c_str());
    check(missions::accept(p, *cargo, cap), "accept loads the cargo");
    check(player::cargo_units_used(p) == cargo->units, "hold now holds the consignment");
    check(p.missions.size() == 1, "one active mission tracked");
    check(p.credits == cr_before_accept, "accepting a delivery costs nothing up front");

    // Over-capacity refusal: a tiny hold can't take the same consignment.
    ShipClass tiny; tiny.cargo_units = cargo->units - 1;
    PlayerState small = player::new_game("troy");
    const int tiny_cap = player::cargo_capacity(small, &tiny);
    check(!missions::can_accept(small, *cargo, tiny_cap), "can_accept=false when hold too small");
    check(!missions::accept(small, *cargo, tiny_cap), "accept refused when over capacity");
    check(small.missions.empty(), "refused accept tracks nothing");

    // ---- 3. travel + deliver -------------------------------------------------
    std::printf("\n=================================================================\n");
    std::printf("3. TRAVEL to destination + DELIVER\n");
    std::printf("=================================================================\n");
    // Refuse delivery at the wrong base first.
    check(!missions::complete_delivery(p, cargo->id, "achilles"),
          "deliver refused at the wrong base");

    // "Fly" to the destination (the live game sets last_docked_base on dock).
    p.current_system   = cargo->dest_system;
    p.last_docked_base = cargo->dest_base;
    const long long cr_before_deliver = p.credits;
    std::printf("  at dest '%s' (system '%s'); delivering...\n",
                p.last_docked_base.c_str(), p.current_system.c_str());
    check(missions::complete_delivery(p, cargo->id, cargo->dest_base),
          "deliver succeeds at the destination base");
    check(p.credits == cr_before_deliver + cargo->reward,
          "reward credited exactly");
    check(player::cargo_units_used(p) == 0, "consignment removed from hold");
    check(p.missions.empty(), "delivered mission dropped from active list");
    std::printf("  credits %lld -> %lld (+%lld)\n", cr_before_deliver,
                (long long)p.credits, (long long)cargo->reward);

    // ---- 4. accept bounty + simulate kills -----------------------------------
    std::printf("\n=================================================================\n");
    std::printf("4. ACCEPT bounty + simulate qualifying kills (np-ma2.1 path)\n");
    std::printf("=================================================================\n");
    check(missions::accept(p, *bounty, cap), "accept the bounty");
    check(p.missions.size() == 1, "bounty tracked active");
    const Faction target = faction::from_name(bounty->target_faction);
    check(target != Faction::Count, "bounty target faction resolves");

    const long long cr_before_bounty = p.credits;
    const int need = bounty->count_required;
    std::printf("  target '%s', need %d kills, reward %lld\n",
                bounty->target_faction.c_str(), need, (long long)bounty->reward);

    // A non-matching kill must NOT advance the bounty.
    Faction other = (target == Faction::Pirate) ? Faction::Kilrathi : Faction::Pirate;
    missions::on_player_kill(p, other);
    check(!p.missions.empty() && p.missions[0].progress == 0,
          "non-matching kill does not advance progress");

    // Matching kills advance one at a time, then auto-pay on the final one.
    for (int k = 1; k <= need; ++k) {
        const int adv = missions::on_player_kill(p, target);
        if (k < need) {
            check(adv == 1 && !p.missions.empty() && p.missions[0].progress == k,
                  "matching kill increments progress");
        } else {
            check(adv == 1 && p.missions.empty(), "final kill completes + drops the bounty");
        }
    }
    check(p.credits == cr_before_bounty + bounty->reward, "bounty payout credited exactly");
    std::printf("  credits %lld -> %lld (+%lld)\n", cr_before_bounty,
                (long long)p.credits, (long long)bounty->reward);

    // ---- 5. save / load round-trip of accepted missions ----------------------
    std::printf("\n=================================================================\n");
    std::printf("5. SAVE / LOAD round-trip of accepted missions\n");
    std::printf("=================================================================\n");
    // Re-accept both kinds so there's a non-trivial mission list to persist.
    p.missions.clear();
    missions::accept(p, *cargo, cap);     // a cargo delivery (in progress)
    missions::accept(p, *bounty, cap);    // a bounty
    p.missions[1].progress = 1;           // partial bounty progress to round-trip
    const size_t before_n   = p.missions.size();
    const std::string c_id   = p.missions[0].id;
    const std::string b_id   = p.missions[1].id;
    const int          b_prog= p.missions[1].progress;
    const int64_t      b_rew = p.missions[1].reward;

    constexpr int kSlot = 7;
    check(savegame::save(p, kSlot), "save wrote the slot");

    PlayerState loaded;
    check(savegame::load(loaded, kSlot), "load read the slot");
    check(loaded.missions.size() == before_n, "mission count survives round-trip");
    bool match = loaded.missions.size() == before_n &&
                 loaded.missions[0].id == c_id &&
                 loaded.missions[1].id == b_id &&
                 loaded.missions[1].progress == b_prog &&
                 loaded.missions[1].reward == b_rew;
    check(match, "mission ids / progress / reward all match after load");
    for (const ActiveMission& m : loaded.missions)
        std::printf("    loaded: %s type=%d progress=%d reward=%lld\n",
                    m.id.c_str(), m.type, m.progress, (long long)m.reward);

    std::printf("\n=================================================================\n");
    std::printf("RESULT: %s\n", g_fail == 0 ? "ALL CHECKS PASSED" : "FAILURES PRESENT");
    std::printf("=================================================================\n");
    return g_fail == 0 ? 0 : 1;
}
