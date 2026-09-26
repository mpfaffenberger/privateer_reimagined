#pragma once

#include "cockpit_overlay_layout.h"

namespace cockpit_overlay {
struct LightState {
    bool powered = false;
    bool comms_active = false;
    bool autopilot_ready = false;
    bool damage_warning = false;
};
enum class Lamp { Power, Comms, Auto, Damage, Activity };
struct LampPlacement { Lamp lamp; Rect bounds; };
// Centurion art pixels: console bank plus two upper-rim heartbeat lamps.
// Renderer applies the identical fit + rigid slide as the cockpit PNG.
inline constexpr LampPlacement kCenturionLamps[] = {
    {Lamp::Power,  {742, 810, 32, 12}},
    {Lamp::Comms,  {794, 810, 32, 12}},
    {Lamp::Auto,   {846, 810, 32, 12}},
    {Lamp::Damage, {898, 810, 32, 12}},
    // Flank the arch banner on solid rim metal, clear of the glass (#456).
    {Lamp::Power,    {642, 40, 32, 12}},
    {Lamp::Activity, {998, 40, 32, 12}},
};

inline bool lamp_on(Lamp lamp, const LightState& state, double seconds) {
    if (!state.powered) return false;
    // Healthy power heartbeat every 1.6s; 2Hz comms and 1Hz warning.
    // The heartbeat is cosmetic life in normal flight, not a fault signal.
    const auto pulse = [seconds](double period) {
        return std::fmod(std::max(0.0, seconds), period) < period * 0.5;
    };
    switch (lamp) {
    case Lamp::Power: return pulse(1.6);
    case Lamp::Comms: return state.comms_active && pulse(0.5);
    case Lamp::Auto: return state.autopilot_ready;
    case Lamp::Damage: return state.damage_warning && pulse(1.0);
    case Lamp::Activity: return !pulse(1.6); // cosmetic powered activity, not a warning
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
