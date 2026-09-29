// -----------------------------------------------------------------------------
// tools/test_mission_tracker.cpp — offline driver for #13 in-flight tracking.
//
// Links the REAL mission_tracker.cpp + missions.cpp (-DMISSIONS_HEADLESS) +
// comm.cpp (-DCOMM_HEADLESS) + threat.cpp + player/faction/system_def, and
// drives mission_tracker::tick() against a hand-built PlayerState + nav list:
//
//   * SCOUT: one nav at a known position. tick with player FAR -> not done,
//     mission still active. tick with player AT the nav -> nav_done flips,
//     the mission COMPLETES and credits the reward.
//   * PATROL: two navs. Reaching one flips only its flag (no completion);
//     reaching the second completes + credits.
//
// No world is wired into threat::, so threat::hostiles_near() returns its
// safe always-false fallback — the Scout/Patrol paths never touch it. Build:
//   clang++ -std=c++20 -DMISSIONS_HEADLESS -DCOMM_HEADLESS -Isrc -Ithird_party \
//       tools/test_mission_tracker.cpp src/mission_tracker.cpp src/missions.cpp \
//       src/comm.cpp src/threat.cpp src/player.cpp src/faction.cpp \
//       src/commodity.cpp src/galaxy.cpp src/system_def.cpp src/json.cpp \
//       src/savegame.cpp src/savegame_read.cpp src/savegame_write.cpp \
//       src/ship_registry.cpp src/mobility.cpp src/gun.cpp \
//       src/armor.cpp src/shield.cpp -o /tmp/test_mission_tracker
// -----------------------------------------------------------------------------

#include "mission_tracker.h"
#include "missions.h"
#include "comm.h"
#include "player.h"
#include "system_def.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++g_fail; }        \
        else         { std::printf("  ok:   %s\n", msg); }                  \
    } while (0)

static NavPointDef make_nav(const std::string& name, HMM_Vec3 pos) {
    NavPointDef n;
    n.name = name;
    n.position = pos;
    return n;
}

static bool has_mission(const PlayerState& p, const std::string& id) {
    for (const ActiveMission& m : p.missions) if (m.id == id) return true;
    return false;
}

int main() {
    const std::string SYS = "troy";
    int rc = 0;

    // ---- SCOUT: far -> not done; at-nav -> complete + credited ------------
    {
        std::printf("[scout]\n");
        std::vector<NavPointDef> navs = {
            make_nav("Survey Alpha", HMM_V3(1000.0f, 0.0f, 0.0f)),
            make_nav("Decoy",        HMM_V3(9.0e5f,  0.0f, 0.0f)),
        };

        PlayerState p;
        p.credits = 100;
        ActiveMission am;
        am.id            = "scout-1";
        am.type          = (int)missions::MissionType::Scout;
        am.title         = "Scout Survey Alpha";
        am.reward        = 5000;
        am.target_system = SYS;
        am.nav_targets   = { "Survey Alpha" };
        am.nav_done.assign(1, (uint8_t)0);
        p.missions.push_back(am);

        // Far from the nav: nothing flips, mission stays active.
        mission_tracker::tick(p, SYS, navs, HMM_V3(5.0e5f, 0.0f, 0.0f));
        CHECK(has_mission(p, "scout-1"), "far tick leaves scout active");
        CHECK(p.missions[0].nav_done[0] == 0, "far tick: nav NOT done");
        CHECK(p.credits == 100, "far tick: no reward paid");

        // Right on the nav (within k_nav_reach_m): reach + auto-complete.
        mission_tracker::tick(p, SYS, navs, HMM_V3(1000.0f, 0.0f, 0.0f));
        CHECK(!has_mission(p, "scout-1"), "at-nav tick completes + drops scout");
        CHECK(p.credits == 100 + 5000, "at-nav tick: reward credited");
    }

    // ---- PATROL: completes only when ALL navs reached ---------------------
    {
        std::printf("[patrol]\n");
        std::vector<NavPointDef> navs = {
            make_nav("Picket A", HMM_V3(0.0f,    0.0f, 0.0f)),
            make_nav("Picket B", HMM_V3(50000.0f, 0.0f, 0.0f)),
        };

        PlayerState p;
        p.credits = 0;
        ActiveMission am;
        am.id            = "patrol-1";
        am.type          = (int)missions::MissionType::Patrol;
        am.title         = "Patrol the pickets";
        am.reward        = 8000;
        am.target_system = SYS;
        am.nav_targets   = { "Picket A", "Picket B" };
        am.nav_done.assign(2, (uint8_t)0);
        p.missions.push_back(am);

        // Reach A only: one flag set, still active (B outstanding).
        mission_tracker::tick(p, SYS, navs, HMM_V3(0.0f, 0.0f, 0.0f));
        CHECK(has_mission(p, "patrol-1"), "patrol still active after 1/2 navs");
        CHECK(p.missions[0].nav_done[0] == 1, "patrol nav A reached");
        CHECK(p.missions[0].nav_done[1] == 0, "patrol nav B still outstanding");
        CHECK(p.credits == 0, "patrol: no reward at 1/2");

        // Reach B: all navs done -> complete + credit.
        mission_tracker::tick(p, SYS, navs, HMM_V3(50000.0f, 0.0f, 0.0f));
        CHECK(!has_mission(p, "patrol-1"), "patrol completes at 2/2");
        CHECK(p.credits == 8000, "patrol: reward credited on full clear");
    }

    // ---- cross-system guard ----------------------------------------------
    {
        std::printf("[cross-system]\n");
        std::vector<NavPointDef> navs = {
            make_nav("Elsewhere", HMM_V3(0.0f, 0.0f, 0.0f)),
        };
        PlayerState p;
        ActiveMission am;
        am.id            = "scout-x";
        am.type          = (int)missions::MissionType::Scout;
        am.title         = "Scout in another system";
        am.reward        = 1000;
        am.target_system = "pyrenees";   // NOT the current system
        am.nav_targets   = { "Elsewhere" };
        am.nav_done.assign(1, (uint8_t)0);
        p.missions.push_back(am);

        // Player sits right on a same-named nav, but the mission's system
        // is elsewhere -> tracker must ignore it.
        mission_tracker::tick(p, SYS, navs, HMM_V3(0.0f, 0.0f, 0.0f));
        CHECK(has_mission(p, "scout-x"), "cross-system mission untouched");
        CHECK(p.missions[0].nav_done[0] == 0, "cross-system nav NOT flipped");
    }

    if (g_fail) { std::printf("\nFAILED (%d checks)\n", g_fail); rc = 1; }
    else        { std::printf("\nALL PASS\n"); }
    return rc;
}
