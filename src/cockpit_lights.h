#pragma once

#include "cockpit_overlay_layout.h"

namespace cockpit_overlay {
struct LightState {
    bool powered = false;
    bool comms_active = false;
    bool autopilot_ready = false;
    bool damage_warning = false;
};
enum class Lamp { Power, Comms, Auto, Damage };
struct LampPlacement { Lamp lamp; Rect bounds; };
// Centurion art pixels: small hardware bank on solid metal below radar.
// Renderer applies the identical fit + rigid slide as the cockpit PNG.
inline constexpr LampPlacement kCenturionLamps[] = {
    {Lamp::Power,  {749, 814, 18, 7}},
    {Lamp::Comms,  {801, 814, 18, 7}},
    {Lamp::Auto,   {853, 814, 18, 7}},
    {Lamp::Damage, {905, 814, 18, 7}},
};

inline bool lamp_on(Lamp lamp, const LightState& state, double seconds) {
    if (!state.powered) return false;
    // Restrained, deterministic cadences: 2Hz activity and 1Hz warning.
    const auto pulse = [seconds](double period) {
        return std::fmod(std::max(0.0, seconds), period) < period * 0.5;
    };
    switch (lamp) {
    case Lamp::Power: return true;
    case Lamp::Comms: return state.comms_active && pulse(0.5);
    case Lamp::Auto: return state.autopilot_ready;
    case Lamp::Damage: return state.damage_warning && pulse(1.0);
    }
    return false;
}

// Warn if any actual armor facing is at/below 25% of fitted capacity.
// A missing capacity is not damage (e.g. unavailable ship data).
inline bool low_armor(const float (&current)[4], const float (&capacity)[4]) {
    for (int i = 0; i < 4; ++i)
        if (capacity[i] > 0 && current[i] <= capacity[i] * 0.25f) return true;
    return false;
}
} // namespace cockpit_overlay
