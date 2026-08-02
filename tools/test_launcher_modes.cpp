#include "launcher_modes.h"

#include <cstdio>

namespace {
int checks = 0;
int fails = 0;

void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++fails;
        std::printf("[FAIL] %s\n", label);
    } else {
        std::printf("[ OK ] %s\n", label);
    }
}
} // namespace

int main() {
    PlayerState player;
    player.missile_launcher_left = false;
    player.missile_launcher_right = false;
    player.torpedo_launcher_left = false;
    player.torpedo_launcher_right = false;
    player.missiles[0] = 5;
    player.missiles[1] = 4;
    player.missiles[2] = 3;
    player.torpedoes = 2;

    check(!launcher_modes::has_compatible_hardware(player, 0),
          "missile ammo without launcher is unavailable");
    check(!launcher_modes::has_compatible_hardware(
              player, (int)MissileType::TORPEDO),
          "torpedo ammo without tube is unavailable");
    check(launcher_modes::next_selection(player, 1) == 1,
          "cycle stays stable when no launcher is installed");

    player.missile_launcher_left = true;
    player.missiles[0] = 0;
    check(launcher_modes::next_selection(player, 0) == 1,
          "cycle skips empty dumbfire and selects loaded heat-seeker");
    check(launcher_modes::next_selection(player, 1) == 2,
          "cycle advances to loaded image-recognition missile");
    check(launcher_modes::next_selection(player, 2) == 1,
          "cycle skips unavailable torpedo hardware and wraps");

    player.torpedo_launcher_right = true;
    check(launcher_modes::next_selection(player, 2)
              == (int)MissileType::TORPEDO,
          "cycle includes loaded torpedo with installed tube");
    check(launcher_modes::next_selection(
              player, (int)MissileType::TORPEDO) == 1,
          "cycle wraps from torpedo to loaded missile");
    check(launcher_modes::missile_launcher_count(player) == 1,
          "missile launcher count reflects installed sides");
    check(launcher_modes::torpedo_launcher_count(player) == 1,
          "torpedo launcher count reflects installed sides");
    const launcher_modes::SideState left_missile =
        launcher_modes::side_state(player, 0, (int)MissileType::HS);
    check(left_missile.missile_installed && left_missile.active,
          "HUD marks installed left missile launcher active for HS");
    const launcher_modes::SideState right_missile =
        launcher_modes::side_state(player, 1, (int)MissileType::HS);
    check(right_missile.torpedo_installed && !right_missile.active,
          "HUD leaves torpedo-only right launcher inactive for HS");
    const launcher_modes::SideState right_torpedo =
        launcher_modes::side_state(player, 1, (int)MissileType::TORPEDO);
    check(right_torpedo.active,
          "HUD marks installed right torpedo launcher active for torpedo");

    player.missiles[1] = player.missiles[2] = player.torpedoes = 0;
    check(launcher_modes::next_selection(player, 2) == 2,
          "empty installed launchers keep current HUD selection");

    std::printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
