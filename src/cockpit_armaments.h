#pragma once

struct PlayerState;
struct Ship;

namespace cockpit_armaments {

// Draw the interactive top-down gun schematic into the current ImGui window.
void draw(PlayerState& player, Ship& live_ship);

// True when the pointer is merely hovering this MFD, not dragging a weapon.
// Flight input may pass through in that state; an active drag still captures it.
bool allows_flight_mouse_passthrough();

// Release the cached schematic texture and its dedicated filtered sampler.
void shutdown();

} // namespace cockpit_armaments
