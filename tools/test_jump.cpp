// -----------------------------------------------------------------------------
// tools/test_jump.cpp — offline driver for the np-6al.3 jump eligibility
// oracle. Links the REAL jump.cpp + galaxy.cpp and walks every verdict the
// HUD/J-key share (Ready / TooFar / NoRoute / NotJumpNav / Hostiles) plus the
// reciprocal round-trip (Troy->Pyrenees->Troy lands back at the origin gate).
//
// Key injection isn't available over dev_remote, so this is the deterministic
// proof of the verdict logic; the live game (--dev-jump-soak) provides the
// arrival-position + teardown/build + stability logs.
//
// Build:
//   clang++ -std=c++20 -Isrc -Ithird_party tools/test_jump.cpp \
//       src/jump.cpp src/galaxy.cpp src/json.cpp -o /tmp/test_jump && /tmp/test_jump
// -----------------------------------------------------------------------------

#include "jump.h"
#include "galaxy.h"
#include "camera.h"
#include "system_def.h"

#include <cstdio>

// threat.cpp drags in the ship registry + faction tables; jump.cpp only calls
// this one entry point, so stub it (toggle to exercise the hostile gate).
namespace threat {
bool g_force_hostiles = false;
bool hostiles_near(HMM_Vec3, float) { return g_force_hostiles; }
}

// Build the Troy<->Pyrenees slice of the real galaxy.json topology in memory.
static galaxy::Galaxy make_galaxy() {
    galaxy::Galaxy g;
    g.systems.push_back({ "troy",     "Troy",     "Troy Sector", "assets/systems/troy.json",     {} });
    g.systems.push_back({ "pyrenees", "Pyrenees", "Troy Sector", "assets/systems/pyrenees.json", {} });
    g.jumps.push_back({ "troy",     "Pyrenees Jump", "pyrenees", "Troy Jump" });
    g.jumps.push_back({ "pyrenees", "Troy Jump",     "troy",     "Pyrenees Jump" });
    return g;
}

static StarSystem make_troy() {
    StarSystem s;
    s.name = "Troy";
    NavPointDef pyr;  pyr.name = "Pyrenees Jump"; pyr.kind = "jump";
    pyr.position = HMM_V3(100000.0f, 66667.0f, 133333.0f);
    pyr.links_to = "pyrenees"; pyr.links_to_nav = "Troy Jump";
    NavPointDef sta;  sta.name = "Achilles Mining"; sta.kind = "station";
    sta.position = HMM_V3(200000.0f, -133333.0f, 0.0f); sta.dockable = true;
    NavPointDef war;  war.name = "War Jump"; war.kind = "jump";   // dangling, no galaxy edge
    war.position = HMM_V3(0.0f, 0.0f, 150000.0f);
    s.nav_points = { pyr, sta, war };
    return s;
}

static const char* PASS(bool ok) { return ok ? "PASS" : "*** FAIL ***"; }

int main() {
    const galaxy::Galaxy gal = make_galaxy();
    const StarSystem     troy = make_troy();
    Camera cam;
    int fails = 0;

    std::printf("=== np-6al.3 jump eligibility harness ===\n\n");

    // ---- 1. Ready: at the Pyrenees Jump gate, no hostiles -----------------
    cam.position = troy.nav_points[0].position;   // sitting on the gate
    threat::g_force_hostiles = false;
    {
        const jump::Eligibility e = jump::evaluate(cam, troy, gal, "troy", 0);
        bool ready = false; const char* p = jump::prompt(e, &ready);
        const bool ok = e.status == jump::Status::Ready &&
                        e.dest_id == "pyrenees" && e.dest_name == "Pyrenees" &&
                        e.arrival_nav == "Troy Jump" && ready;
        std::printf("[1 Ready ]    status=%-9s dest=%s/'%s' arr='%s' prompt='%s' ready=%d  %s\n",
                    jump::status_str(e.status), e.dest_id.c_str(), e.dest_name.c_str(),
                    e.arrival_nav.c_str(), p ? p : "(null)", ready, PASS(ok));
        fails += !ok;
    }

    // ---- 2. TooFar: same gate, but 50k out --------------------------------
    cam.position = HMM_AddV3(troy.nav_points[0].position, HMM_V3(50000.0f, 0.0f, 0.0f));
    {
        const jump::Eligibility e = jump::evaluate(cam, troy, gal, "troy", 0);
        bool ready = false; const char* p = jump::prompt(e, &ready);
        const bool ok = e.status == jump::Status::TooFar && !ready;
        std::printf("[2 TooFar]    status=%-9s dist=%.0fu (trigger %.0fu) prompt='%s'  %s\n",
                    jump::status_str(e.status), e.distance_m, jump::k_trigger_range_m,
                    p ? p : "(null)", PASS(ok));
        fails += !ok;
    }

    // ---- 3. NoRoute: the dangling War Jump (no galaxy edge) ---------------
    cam.position = troy.nav_points[2].position;
    {
        const jump::Eligibility e = jump::evaluate(cam, troy, gal, "troy", 2);
        const bool ok = e.status == jump::Status::NoRoute;
        std::printf("[3 NoRoute]   status=%-9s prompt='%s'  %s\n",
                    jump::status_str(e.status),
                    jump::prompt(e, nullptr) ? jump::prompt(e, nullptr) : "(null)", PASS(ok));
        fails += !ok;
    }

    // ---- 4. NotJumpNav: a station nav -> no prompt ------------------------
    cam.position = troy.nav_points[1].position;
    {
        const jump::Eligibility e = jump::evaluate(cam, troy, gal, "troy", 1);
        const bool ok = e.status == jump::Status::NotJumpNav &&
                        jump::prompt(e, nullptr) == nullptr;
        std::printf("[4 NotJump]   status=%-9s prompt=%s  %s\n",
                    jump::status_str(e.status),
                    jump::prompt(e, nullptr) ? jump::prompt(e, nullptr) : "(null)", PASS(ok));
        fails += !ok;
    }

    // ---- 5. Hostiles: at the gate but the threat oracle trips -------------
    cam.position = troy.nav_points[0].position;
    threat::g_force_hostiles = true;
    {
        const jump::Eligibility e = jump::evaluate(cam, troy, gal, "troy", 0);
        bool ready = false; const char* p = jump::prompt(e, &ready);
        const bool ok = e.status == jump::Status::Hostiles && !ready;
        std::printf("[5 Hostile]   status=%-9s prompt='%s' ready=%d  %s\n",
                    jump::status_str(e.status), p ? p : "(null)", ready, PASS(ok));
        fails += !ok;
    }
    threat::g_force_hostiles = false;

    // ---- 6. Reciprocal round-trip via the galaxy graph --------------------
    {
        const galaxy::JumpTarget out  = gal.jump_target("troy", "Pyrenees Jump");
        const galaxy::JumpTarget back = gal.jump_target("pyrenees", out.nav);
        const bool ok = out.ok && out.system == "pyrenees" && out.nav == "Troy Jump" &&
                        back.ok && back.system == "troy" && back.nav == "Pyrenees Jump";
        std::printf("\n[6 RoundTrip] troy/'Pyrenees Jump' -> %s/'%s' -> %s/'%s'  %s\n",
                    out.system.c_str(), out.nav.c_str(),
                    back.system.c_str(), back.nav.c_str(), PASS(ok));
        fails += !ok;
    }

    std::printf("\n=== %s (%d failures) ===\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
