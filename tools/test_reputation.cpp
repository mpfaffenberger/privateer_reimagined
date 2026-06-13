// -----------------------------------------------------------------------------
// tools/test_reputation.cpp — offline driver for np-ma2.1 reputation
// consequences from kills + comm taunts. Links the REAL faction.cpp +
// comm.cpp (built with -DCOMM_HEADLESS so the ImGui feed draw is excluded)
// + player.cpp + json.cpp, then exercises the exact path the live game's
// damage pass drives: comm::report_player_kill(player, victim).
//
// This is the deterministic numeric proof the spec asks for — the rep
// math is pure, so the printed deltas / stance flips / taunts here are
// identical to what the running game logs when a real projectile kill
// (or the debug "simulate player kill" button) fires.
//
// Build:
//   clang++ -std=c++20 -DCOMM_HEADLESS -Isrc -Ithird_party \
//       tools/test_reputation.cpp src/comm.cpp src/faction.cpp \
//       src/player.cpp src/json.cpp -o /tmp/test_reputation
// -----------------------------------------------------------------------------

#include "comm.h"
#include "faction.h"
#include "player.h"

#include <cstdio>

static void dump_rep(const PlayerState& p) {
    std::printf("    rep: ");
    for (int i = 0; i < kFactionCount; ++i) {
        std::printf("%s=%d ", faction::to_name((Faction)i), (int)p.rep.rep[i]);
    }
    std::printf("\n");
}

int main() {
    faction::init();
    comm::load("assets/data/comm_lines.json");

    PlayerState player = player::new_game("troy");

    std::printf("\n=================================================================\n");
    std::printf("SCENARIO 1: player kills PIRATES (lawful universe approves)\n");
    std::printf("=================================================================\n");
    std::printf("BEFORE:\n");
    dump_rep(player);
    // Five pirate kills — enough for the lawful factions (+5 each) to cross
    // the +25 Allied threshold, and for the pirates themselves to slide
    // toward Hostile.
    for (int k = 0; k < 5; ++k) {
        std::printf("\n-- pirate kill #%d --\n", k + 1);
        comm::report_player_kill(player, Faction::Pirate);
    }
    std::printf("\nAFTER:\n");
    dump_rep(player);

    std::printf("\n=================================================================\n");
    std::printf("SCENARIO 2: player MURDERS a MERCHANT (the crime path)\n");
    std::printf("=================================================================\n");
    std::printf("BEFORE:\n");
    dump_rep(player);
    comm::report_player_kill(player, Faction::Merchant);
    std::printf("\n-- second merchant murder (push merchants Hostile) --\n");
    comm::report_player_kill(player, Faction::Merchant);
    std::printf("\nAFTER:\n");
    dump_rep(player);

    std::printf("\n=================================================================\n");
    std::printf("SCENARIO 3: player kills a KILRATHI (everyone but Kilrathi cheers)\n");
    std::printf("=================================================================\n");
    comm::report_player_kill(player, Faction::Kilrathi);
    std::printf("\nFINAL:\n");
    dump_rep(player);

    return 0;
}
