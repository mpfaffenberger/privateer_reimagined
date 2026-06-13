// -----------------------------------------------------------------------------
// tools/test_autopilot.cpp — offline driver for the np-opa.3 nav-autopilot
// state machine. Links the REAL autopilot.cpp + camera.cpp + threat.cpp and
// walks the full lifecycle (refuse-no-nav, engage, cruise, arrive, return
// control) printing the same [autopilot] logs the live game emits. Key
// injection isn't available over dev_remote, so this is the deterministic
// proof of the controller; the live game provides the HUD-banner screenshots.
//
// Build:
//   clang++ -std=c++20 -Isrc -Ithird_party tools/test_autopilot.cpp \
//       src/autopilot.cpp src/camera.cpp src/threat.cpp -o /tmp/test_autopilot
// -----------------------------------------------------------------------------

#include "autopilot.h"
#include "camera.h"
#include "system_def.h"

#include <cstdio>

// sfx.cpp drags in the whole audio mixer; the autopilot module only calls
// this one entry point, so stub it for the offline harness.
namespace sfx { void ui_click() { std::printf("[sfx] ui_click\n"); } }

static StarSystem make_system() {
    StarSystem s;
    NavPointDef helen;
    helen.name     = "Helen Planet";
    helen.kind     = "planet";
    helen.position = HMM_V3(166667.0f, -66667.0f, 100000.0f);
    s.nav_points.push_back(helen);
    return s;
}

int main() {
    const StarSystem system = make_system();
    constexpr float dt = 1.0f / 60.0f;

    Camera    cam;
    Autopilot ap;

    // Default Camera caps (engine L0): cruise0=300, cruise1=600 u/s. A
    // beefier engine would raise these via outfitting; the autopilot
    // inherits whatever the camera carries — that's the "reuse the cruise
    // engine" contract.
    cam.position = HMM_V3(0.0f, 0.0f, 30000.0f);

    std::printf("=== np-opa.3 nav-autopilot harness ===\n\n");

    // ---- 1. refuse: no nav selected ---------------------------------------
    std::printf("-- case: NO NAV SELECTED --\n");
    autopilot::try_engage(ap, cam, system, /*selected_nav=*/-1);
    std::printf("[harness] engaged=%d  banner='%s'\n\n",
                (int)autopilot::engaged(ap), ap.msg);

    // ---- 2. engage toward Helen + fly the whole trip ----------------------
    std::printf("-- case: ENGAGE -> cruise -> arrive --\n");
    autopilot::try_engage(ap, cam, system, /*selected_nav=*/0);
    std::printf("[harness] engaged=%d  banner='%s'\n", (int)autopilot::engaged(ap), ap.msg);

    const HMM_Vec3 target = system.nav_points[0].position;
    float peak_speed = 0.0f;
    int   guard      = 0;
    while (autopilot::engaged(ap) && guard++ < 60 * 600) {  // 600s safety cap
        autopilot::tick(ap, cam, dt);
        const float spd = HMM_LenV3(cam.velocity);
        if (spd > peak_speed) peak_speed = spd;
    }

    const float final_dist = HMM_LenV3(HMM_SubV3(target, cam.position));
    std::printf("\n[harness] trip done after %.1fs sim:\n", guard * dt);
    std::printf("[harness]   final dist to nav = %.0fu (arrival radius %.0fu)\n",
                final_dist, autopilot::k_arrival_radius_m);
    std::printf("[harness]   peak speed = %.0fu/s (engine cap %.0f)\n",
                peak_speed, cam.max_speed_cruise1);
    std::printf("[harness]   final speed = %.0fu/s\n", HMM_LenV3(cam.velocity));
    std::printf("[harness]   engaged=%d  banner='%s'\n",
                (int)autopilot::engaged(ap), ap.msg);
    std::printf("[harness]   arrived within radius? %s\n",
                final_dist <= autopilot::k_arrival_radius_m ? "YES" : "NO");

    // ---- 3. mid-flight cancel (manual override style) ---------------------
    std::printf("\n-- case: CANCEL mid-flight (A again / manual input) --\n");
    cam.position = HMM_V3(0.0f, 0.0f, 30000.0f);   // far out again
    cam.velocity = HMM_V3(0.0f, 0.0f, 0.0f);
    autopilot::try_engage(ap, cam, system, /*selected_nav=*/0);
    for (int i = 0; i < 30; ++i) autopilot::tick(ap, cam, dt);  // ~0.5s of cruise
    std::printf("[harness] mid-flight engaged=%d, speed=%.0fu/s\n",
                (int)autopilot::engaged(ap), HMM_LenV3(cam.velocity));
    autopilot::disengage(ap, cam, "AUTOPILOT DISENGAGED");
    std::printf("[harness] after cancel: engaged=%d  cruise_target=%.1f  banner='%s'\n",
                (int)autopilot::engaged(ap), cam.cruise_target, ap.msg);

    std::printf("\n=== done ===\n");
    return 0;
}
