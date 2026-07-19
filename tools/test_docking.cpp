// -----------------------------------------------------------------------------
// tools/test_docking.cpp — offline driver for the np-9cu.1 docking state
// machine. Links the REAL docking.cpp + game_state.cpp + camera.cpp and
// walks the full lifecycle (reject-too-far, reject-too-fast, accept,
// auto-approach, dock, launch) printing the same [dock] logs the live
// game emits. Key injection isn't available over dev_remote, so this is
// the deterministic proof of the controller; the live game provides the
// HUD-prompt + Landed-stub screenshots.
//
// Build (see validation notes in the bead):
//   clang++ -std=c++20 -Isrc -Ithird_party tools/test_docking.cpp \
//       src/docking.cpp src/game_state.cpp src/camera.cpp -o /tmp/test_docking
// -----------------------------------------------------------------------------

#include "docking.h"
#include "camera.h"
#include "game_state.h"
#include "player.h"
#include "system_def.h"

#include <cstdio>
#include <string>

// Audio and save IO are integration boundaries, not docking behavior. Stub
// both so this deterministic harness never touches a real user's save folder.
namespace sfx { void ui_click() { std::printf("[sfx] ui_click\n"); } }
namespace savegame {
std::string save_timestamped(const PlayerState&) { return "headless-autosave"; }
}

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
} // namespace

static NavPointDef make_base() {
    NavPointDef n;
    n.name     = "Achilles Mining";
    n.kind     = "station";
    n.position = HMM_V3(200000.0f, -133333.0f, 0.0f);
    n.dockable = true;
    n.base_id  = "achilles";
    return n;
}

int main() {
    const NavPointDef base = make_base();
    constexpr float dt = 1.0f / 60.0f;

    Camera      cam;
    GameState   gs;
    PlayerState player;
    Docking     dock;

    std::printf("=== np-9cu.1 docking state-machine harness ===\n\n");

    // ---- 1. reject: too far ------------------------------------------------
    std::printf("-- case: TOO FAR (10km out, stationary) --\n");
    cam.position = HMM_AddV3(base.position, HMM_V3(0, 0, 10000.0f));
    cam.velocity = HMM_V3(0, 0, 0);
    check(docking::request(dock, cam.position, cam.velocity, base) == DockResult::TooFar,
          "request rejects a ship outside docking range");

    // ---- 2. reject: too fast ----------------------------------------------
    std::printf("\n-- case: TOO FAST (1km out, 300 m/s) --\n");
    cam.position = HMM_AddV3(base.position, HMM_V3(0, 0, 1000.0f));
    cam.velocity = HMM_V3(0, 0, -300.0f);
    check(docking::request(dock, cam.position, cam.velocity, base) == DockResult::TooFast,
          "request rejects excessive relative speed");

    // ---- 3. accept: in range + slow ---------------------------------------
    std::printf("\n-- case: CLEARED (1.5km out, 40 m/s) --\n");
    cam.position = HMM_AddV3(base.position, HMM_V3(0, 0, 1500.0f));
    cam.velocity = HMM_MulV3F(HMM_NormV3(HMM_SubV3(base.position, cam.position)), 40.0f);
    check(docking::request(dock, cam.position, cam.velocity, base) == DockResult::Cleared,
          "request accepts an in-range slow approach");

    // ---- 4. fly the approach to completion --------------------------------
    std::printf("\n-- auto-approach + dock --\n");
    int guard = 0;
    while (player.docked == false && guard++ < 60 * 40) {  // 40s safety cap
        docking::tick(dock, cam, gs, player, dt);
        game_state::apply_pending(gs, dt);   // mirror main.cpp's frame-top apply
    }
    std::printf("[harness] player.docked=%d  last_docked_base='%s'  game.mode=%s day=%d\n",
                (int)player.docked, player.last_docked_base.c_str(),
                game_state::to_name(gs.mode), player.day);
    check(player.docked, "approach reaches the dock");
    check(player.last_docked_base == "achilles", "landing records the base");
    check(player.day == 1, "successful landing advances exactly one day");
    for (int i = 0; i < 120; ++i) docking::tick(dock, cam, gs, player, dt);
    check(player.day == 1, "remaining docked does not advance extra days");

    // ---- 5. launch back to flight -----------------------------------------
    std::printf("\n-- launch --\n");
    docking::launch(dock, cam, gs, player, HMM_V3(0.0f, 0.0f, 0.0f));
    game_state::apply_pending(gs, dt);
    const float off = HMM_LenV3(HMM_SubV3(cam.position, base.position));
    std::printf("[harness] after launch: docked=%d  mode=%s  offset_from_pad=%.0fm  "
                "speed=%.0fm/s  cooldown=%.2fs\n",
                (int)player.docked, game_state::to_name(gs.mode), off,
                HMM_LenV3(cam.velocity), dock.cooldown_s);

    // ---- 6. confirm re-dock is locked out during cooldown -----------------
    std::printf("\n-- case: BUSY (cooldown active right after launch) --\n");
    cam.position = HMM_AddV3(base.position, HMM_V3(0, 0, 1500.0f));
    cam.velocity = HMM_V3(0, 0, 0);
    check(docking::request(dock, cam.position, cam.velocity, base) == DockResult::Busy,
          "launch cooldown prevents an immediate second landing");

    std::printf("\n=== %s ===\n", failures == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED");
    return failures == 0 ? 0 : 1;
}
