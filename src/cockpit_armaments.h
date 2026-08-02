#pragma once

struct PlayerState;
struct Ship;

namespace cockpit_armaments {

// Draw the interactive top-down gun schematic into the current ImGui window.
void draw(PlayerState& player, Ship& live_ship);

} // namespace cockpit_armaments
