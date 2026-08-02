#pragma once

struct PlayerState;
struct Ship;

namespace cockpit_armaments {

// Draw the interactive top-down gun + launcher schematic into the current
// ImGui window. selected_ordnance is a MissileType index.
void draw(PlayerState& player, Ship& live_ship, int selected_ordnance);

// True when the pointer is merely hovering this MFD, not dragging a weapon.
// Flight input may pass through in that state; an active drag still captures it.
bool allows_flight_mouse_passthrough();

// Release the cached schematic texture and its dedicated filtered sampler.
void shutdown();

} // namespace cockpit_armaments
