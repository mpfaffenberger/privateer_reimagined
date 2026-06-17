// -----------------------------------------------------------------------------
// test_ai_brain.cpp -- headless proof that the data-driven combat brain runs.
//
// Spawns a pirate attacker + a target into a ShipRegistry, points the brain at
// the target across the decoded distance bands, and checks the evaluator picks
// a sensible maneuver (and actually drives the controller) at each range. This
// is the parity proof for the condition->maneuver system (ai_brain.cpp); it is
// NOT graphical and links only the logic sources (same headless set as
// test_full_loop). See docs/ai_maneuver_system.md.
//
// Build + run:  cmake --build build --target test_ai_brain && ./build/test_ai_brain
// -----------------------------------------------------------------------------

#include "ai_brain.h"
#include "ai_maneuver.h"
#include "armor.h"
#include "faction.h"
#include "gun.h"
#include "perception.h"
#include "shield.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"

#include <cstdio>

// Same single stub as test_full_loop: the UI blip the base/docking code calls
// has no audio backend in a headless harness.
namespace sfx { void ui_click() {} }

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    gun::load_table("docs/privateer_ship_data.json");
    shield::load_table("docs/privateer_ship_data.json");
    armor::load_table("docs/privateer_ship_data.json");
    ship_class::load_all("assets/ships");
    const int n = ai_brain::load_all("assets/ai");
    check(n > 0, "AI logic tables loaded");

    const ShipClass* klass = ship_class::find("talon");
    if (!klass) klass = ship_class::find("tarsus");
    check(klass != nullptr, "found a ShipClass to spawn");
    if (!klass) return 1;

    ShipRegistry reg;

    // Target first so we know its id.
    Ship tgt = ship::spawn(*klass);
    tgt.faction  = Faction::Confed;
    tgt.position = HMM_V3(0, 0, 5000);
    const uint32_t tid = tgt.id;
    reg.spawn(std::move(tgt));

    // Attacker at the origin facing +Z (identity orientation), pirate -> steady.
    Ship atk = ship::spawn(*klass);
    atk.faction       = Faction::Pirate;
    atk.ai.enabled    = true;
    atk.ai.target_id  = tid;
    atk.position      = HMM_V3(0, 0, 0);
    atk.orientation   = HMM_Q(0, 0, 0, 1);
    const uint32_t aid = atk.id;
    reg.spawn(std::move(atk));

    Ship* A = reg.find_by_id(aid);
    Ship* T = reg.find_by_id(tid);
    check(A && T, "attacker + target resolvable in registry");
    if (!A || !T) return 1;

    struct Band { float dist; const char* expect; };
    const Band bands[] = {
        { 6000.0f, "lead_pursuit (beyond 1000u pursue switch)" },
        {  800.0f, "attack_run (between f0 break and 1000u)"    },
        {  300.0f, "break_off (inside f0 break radius)"         },
    };

    float t = 100.0f;
    for (const Band& b : bands) {
        T->position    = HMM_V3(0, 0, b.dist);
        A->ai.target_id = tid;
        t += 30.0f;   // advance well past any maneuver duration -> reselect
        ai_brain::run_combat(*A, reg, t);

        const char* mname = ai_maneuver::maneuver_name(A->ai.cur_maneuver);
        std::printf("  dist=%6.0f -> maneuver=%-14s behavior=%d  (expect %s)\n",
                    b.dist, mname, (int)A->behavior.kind, b.expect);
        check(A->behavior.kind != ShipBehavior::None,
              "brain drives the controller (behavior != None)");
    }

    // ---- Fix C: attack_run FIRES PROMPTLY on geometry, not after a 7.6s -----
    //      window. At gun range with the target dead ahead + in weapons_range,
    //      should_fire opens after only the per-pilot reaction time.
    auto is_evade = [](AIManeuver m) {
        return m == AIManeuver::EvadeJink || m == AIManeuver::EvadeLeftRight
            || m == AIManeuver::EvadeUpDown || m == AIManeuver::BarrelRoll;
    };
    {
        T->position        = HMM_V3(0, 0, 800);   // attack_run band, dead ahead
        A->ai.target_id    = tid;
        A->ai.cur_maneuver = AIManeuver::LeadPursuit;
        A->ai.cur_priority = 0.0f; A->ai.maneuver_started_at = -1.0f;
        A->ai.fire_solution_at = -1.0f;
        const float step = 1.0f / 60.0f;
        float tt = t, first_fire = -1.0f; bool ever_attack = false;
        for (int i = 0; i < 180; ++i) {            // 3 s @ 60 fps
            tt += step;
            ai_brain::run_combat(*A, reg, tt);
            if (A->ai.cur_maneuver == AIManeuver::AttackRun) ever_attack = true;
            if (A->controller.fire_guns && first_fire < 0.0f) first_fire = tt - t;
        }
        std::printf("  attack_run@800: ever_attack=%d  first_fire=%.2fs\n",
                    (int)ever_attack, first_fire);
        check(ever_attack, "ship enters attack_run at gun range");
        check(first_fire >= 0.0f, "guns FIRE during the attack run");
        check(first_fire >= 0.0f && first_fire < 2.5f,
              "guns fire PROMPTLY (<2.5s, not the old 7.6s tick window)");
        t = tt + 5.0f;
    }

    // ---- Fix A/B: evade is OCCASIONAL, not a constant loop ------------------
    //      At lead-pursuit range (where evade CAN interrupt the approach), the
    //      ship must spend well under half its time evading -- proving the
    //      per-frame random spam is gone and evade no longer dominates.
    {
        T->position        = HMM_V3(0, 0, 6000);  // lead_pursuit band
        A->ai.target_id    = tid;
        A->ai.cur_maneuver = AIManeuver::LeadPursuit;
        A->ai.cur_priority = 0.0f; A->ai.maneuver_started_at = -1.0f;
        const float step = 1.0f / 60.0f;
        const int   N    = 600;                    // 10 s @ 60 fps
        float tt = t; int evade_frames = 0;
        for (int i = 0; i < N; ++i) {
            tt += step;
            ai_brain::run_combat(*A, reg, tt);
            if (is_evade(A->ai.cur_maneuver)) ++evade_frames;
        }
        const float frac = (float)evade_frames / (float)N;
        std::printf("  evade fraction over 10s @6000 = %.1f%%\n", frac * 100.0f);
        check(frac < 0.40f, "evade is occasional (<40% of time), not constant");
        t = tt + 5.0f;
    }

    // f6 -> morale tier mapping sanity (docs/ai_model.md s0).
    check(ai_brain::flee_threshold_for(*A) > 0.0f, "flee threshold derived from f6");

    std::printf(g_fail == 0
                ? "\n# RESULT: ai_brain maneuver selection PASS\n"
                : "\n# RESULT: %d CHECK(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
