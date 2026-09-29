#pragma once
// cockpit_damage.h — the STATUS panel's DAMAGE CONTROL sub-screen (#141).

struct Ship;

namespace cockpit_damage {

// Draw live per-component integrity for `ship` into the current ImGui
// window: one row per system (label, bar, OK / nn% / DESTROYED), with
// unfitted hardware dimmed. Pure draw, no state.
void draw(const Ship& ship);

} // namespace cockpit_damage
