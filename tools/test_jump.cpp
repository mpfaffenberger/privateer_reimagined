// -----------------------------------------------------------------------------
// tools/test_jump.cpp — offline driver for the jump eligibility oracle
// (np-6al.3, nearby-gate discovery #380). Links the REAL jump.cpp +
// galaxy.cpp and walks every verdict the HUD/J-key share (Ready / NotJumpNav
// / NoDrive / NoRoute / Locked), the nearest-gate choice, and the reciprocal
// round-trip (a Troy->Pyrenees arrival is immediately jumpable back to Troy).
//
// The oracle takes NO nav selection: the gate is whichever jump nav sits
// nearest within trigger range. The "station selected" cases below pin that
// the old failure mode (J dead unless the gate was selected) can't return.
//
// Key injection isn't available over dev_remote, so this is the deterministic
// proof of the verdict logic; the live game (--dev-jump-soak) provides the
// arrival-position + teardown/build + stability logs.
//
// Build:  cmake --build build --target test_jump && build/test_jump
// -----------------------------------------------------------------------------

#include "jump.h"
#include "galaxy.h"
#include "camera.h"
#include "system_def.h"

#include <cstdio>
#include <string>

namespace {

// Troy nav indices in make_troy().
constexpr int k_pyr_gate = 0;
constexpr int k_station  = 1;
constexpr int k_war_gate = 2;

// Mirrors main.cpp execute_jump's k_arrival_offset: the player drops in this
// far from the arrival gate, toward the system origin.
constexpr float k_arrival_offset_m = 1500.0f;
static_assert(k_arrival_offset_m < jump::k_trigger_range_m,
              "arrival must land inside the reciprocal gate's trigger range");

// Build the Troy<->Pyrenees slice of the real galaxy.json topology in memory.
galaxy::Galaxy make_galaxy() {
    galaxy::Galaxy g;
    g.systems.push_back({ "troy",     "Troy",     "Troy Sector", "assets/systems/troy.json",     {} });
    g.systems.push_back({ "pyrenees", "Pyrenees", "Troy Sector", "assets/systems/pyrenees.json", {} });
    g.jumps.push_back({ "troy",     "Pyrenees Jump", "pyrenees", "Troy Jump" });
    g.jumps.push_back({ "pyrenees", "Troy Jump",     "troy",     "Pyrenees Jump" });
    return g;
}

NavPointDef make_nav(const char* name, const char* kind, HMM_Vec3 pos) {
    NavPointDef n;
    n.name = name; n.kind = kind; n.position = pos;
    return n;
}

StarSystem make_troy() {
    StarSystem s;
    s.name = "Troy";
    NavPointDef sta = make_nav("Achilles Mining", "station",
                               HMM_V3(200000.0f, -133333.0f, 0.0f));
    sta.dockable = true;
    s.nav_points = {
        make_nav("Pyrenees Jump", "jump", HMM_V3(100000.0f, 66667.0f, 133333.0f)),
        sta,
        make_nav("War Jump", "jump", HMM_V3(0.0f, 0.0f, 150000.0f)),  // dangling, no galaxy edge
    };
    return s;
}

StarSystem make_pyrenees() {
    StarSystem s;
    s.name = "Pyrenees";
    s.nav_points = { make_nav("Troy Jump", "jump", HMM_V3(83333.0f, 50000.0f, 0.0f)) };
    return s;
}

HMM_Vec3 offset(HMM_Vec3 from, float dx) { return HMM_AddV3(from, HMM_V3(dx, 0.0f, 0.0f)); }

int g_fails = 0;

// Evaluate at `pos` and print a one-line verdict; returns the eligibility so
// the caller can assert on whatever fields matter for that case.
jump::Eligibility eval_at(const StarSystem& sys, const galaxy::Galaxy& gal,
                          const char* system_id, HMM_Vec3 pos, bool drive) {
    Camera cam;
    cam.position = pos;
    return jump::evaluate(cam, sys, gal, system_id, drive);
}

void check(const char* label, const jump::Eligibility& e, bool ok) {
    bool ready = false;
    const char* p = jump::prompt(e, &ready);
    std::printf("[%-22s] status=%-20s gate=%2d dest=%-9s arr='%s' prompt='%s'  %s\n",
                label, jump::status_str(e.status), e.nav_index, e.dest_id.c_str(),
                e.arrival_nav.c_str(), p ? p : "(null)", ok ? "PASS" : "*** FAIL ***");
    g_fails += !ok;
}

bool prompt_ready(const jump::Eligibility& e) {
    bool ready = false;
    (void)jump::prompt(e, &ready);
    return ready;
}

} // namespace

int main() {
    const galaxy::Galaxy gal  = make_galaxy();
    const StarSystem     troy = make_troy();
    const HMM_Vec3       pyr_gate = troy.nav_points[k_pyr_gate].position;

    std::printf("=== jump eligibility harness (np-6al.3 / #380) ===\n\n");

    // ---- Ready with nothing selected: flying up to the gate is enough ------
    {
        const auto e = eval_at(troy, gal, "troy", offset(pyr_gate, 1000.0f), true);
        check("unselected ready", e,
              e.status == jump::Status::Ready && e.nav_index == k_pyr_gate &&
              e.dest_id == "pyrenees" && e.dest_name == "Pyrenees" &&
              e.arrival_nav == "Troy Jump" && prompt_ready(e));
    }

    // ---- Station selected in the nav computer: selection is not an input,
    //      so the verdict names the gate, never the selected station. -------
    {
        const auto e = eval_at(troy, gal, "troy", pyr_gate, true);
        check("station-selected ready", e,
              e.status == jump::Status::Ready && e.nav_index == k_pyr_gate &&
              e.nav_index != k_station);
    }

    // ---- Nearest gate wins when two are in range --------------------------
    {
        StarSystem twin = troy;   // park the dangling War Jump 2000u off Pyrenees Jump
        twin.nav_points[k_war_gate].position = offset(pyr_gate, 2000.0f);
        const auto near_pyr = eval_at(twin, gal, "troy", offset(pyr_gate, 500.0f), true);
        check("nearest: pyrenees", near_pyr,
              near_pyr.status == jump::Status::Ready && near_pyr.nav_index == k_pyr_gate);
        const auto near_war = eval_at(twin, gal, "troy", offset(pyr_gate, 1500.0f), true);
        check("nearest: war", near_war,
              near_war.status == jump::Status::NoRoute && near_war.nav_index == k_war_gate);
    }

    // ---- No gate nearby: no verdict and no refusal prompt at all ----------
    {
        const auto far = eval_at(troy, gal, "troy", offset(pyr_gate, 50000.0f), true);
        check("no gate: 50k out", far,
              far.status == jump::Status::NotJumpNav && far.nav_index == -1 &&
              jump::prompt(far, nullptr) == nullptr);
        const auto edge = eval_at(troy, gal, "troy",
                                  offset(pyr_gate, jump::k_trigger_range_m + 1.0f), true);
        check("no gate: just outside", edge,
              edge.status == jump::Status::NotJumpNav && jump::prompt(edge, nullptr) == nullptr);
        const auto at_sta = eval_at(troy, gal, "troy", troy.nav_points[k_station].position, true);
        check("no gate: at station", at_sta,
              at_sta.status == jump::Status::NotJumpNav && jump::prompt(at_sta, nullptr) == nullptr);
    }

    // ---- NoDrive: a valid nearby gate still requires jump hardware --------
    {
        const auto e = eval_at(troy, gal, "troy", pyr_gate, false);
        check("no drive", e,
              e.status == jump::Status::NoDrive && e.nav_index == k_pyr_gate &&
              !prompt_ready(e) && jump::prompt(e, nullptr) != nullptr);
    }

    // ---- NoRoute: the dangling War Jump (no galaxy edge) ------------------
    {
        const auto e = eval_at(troy, gal, "troy", troy.nav_points[k_war_gate].position, true);
        check("no route", e,
              e.status == jump::Status::NoRoute && e.nav_index == k_war_gate &&
              !prompt_ready(e));
    }

    // ---- Locked: the campaign route gate (#130) still refuses -------------
    {
        jump::set_route_gate([](const std::string& from, const std::string& to) {
            return from == "troy" && to == "pyrenees";
        });
        const auto e = eval_at(troy, gal, "troy", pyr_gate, true);
        jump::set_route_gate(nullptr);   // restore the sandbox default
        check("locked route", e,
              e.status == jump::Status::Locked && e.nav_index == k_pyr_gate &&
              !prompt_ready(e));
    }

    // ---- Reciprocal: arriving in Pyrenees drops us inside the Troy Jump's
    //      trigger range, and J from there resolves straight back home. -----
    {
        const StarSystem pyr = make_pyrenees();
        const HMM_Vec3   gate = pyr.nav_points[0].position;
        const HMM_Vec3   into = HMM_NormV3(HMM_MulV3F(gate, -1.0f));
        const HMM_Vec3   arrival = HMM_AddV3(gate, HMM_MulV3F(into, k_arrival_offset_m));
        const auto e = eval_at(pyr, gal, "pyrenees", arrival, true);
        check("reciprocal route", e,
              e.status == jump::Status::Ready && e.nav_index == 0 &&
              e.dest_id == "troy" && e.arrival_nav == "Pyrenees Jump" && prompt_ready(e));
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fails ? "FAILED" : "ALL PASS", g_fails);
    return g_fails ? 1 : 0;
}
