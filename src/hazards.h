#pragma once
// -----------------------------------------------------------------------------
// hazards.h — system hazards: sun + bases (np-3dp).
//
// Three live rules:
//
//   * Sun damage: inside the 15k radius, the player (and any ship) takes
//     5 cm of armor damage per accumulated second. Accumulator-based so
//     damage reads as discrete ticks every full second, not as a per-frame
//     trickle that the player never notices.
//
//   * Autopilot sun avoidance: the autopilot (and the NPC AI) won't fly
//     inside a 20k bubble around the sun. Steering bends away from the
//     sun as the ship closes, and never enters the 15k damage zone unless
//     the player's manual flight ignores it (their problem).
//
//   * Base avoidance: NPCs and the autopilot will never fly inside the
//     7.5k base safety bubble. They bend their heading away from any
//     dockable kind (station/asteroid/etc) once they're within ~10k.
//
// See hazards.cpp for the implementations. Constants live here so callers
// (autopilot, AI, main loop, future missions) all share the same tuning.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <vector>
struct NavPointDef;

namespace hazards {

// ---- sun (single per-system star, g.sun.position) -----------------------------
constexpr float k_sun_damage_radius_m   = 15000.0f;   // hard floor; everything inside takes damage
constexpr float k_sun_damage_radius_sq  = k_sun_damage_radius_m * k_sun_damage_radius_m;
constexpr float k_sun_avoid_radius_m    = 20000.0f;   // 5km buffer outside the damage radius
constexpr float k_sun_avoid_radius_sq   = k_sun_avoid_radius_m * k_sun_avoid_radius_m;
constexpr float k_sun_damage_per_s_cm   = 5.0f;       // per accumulated second inside the 15k zone
constexpr float k_sun_tick_s            = 1.0f;       // accumulator rolls over at 1s

// True if (pos) is inside the sun damage zone. Use squared distance for
// cheap hot-loop checks; sq radius baked in to skip the mul.
inline bool inside_sun_damage(HMM_Vec3 sun_pos, HMM_Vec3 pos) {
    const HMM_Vec3 d = HMM_SubV3(sun_pos, pos);
    return HMM_DotV3(d, d) < k_sun_damage_radius_sq;
}

// True if (pos) is inside the autopilot avoidance bubble (used to start
// bending the steering vector away from the sun).
inline bool inside_sun_avoid(HMM_Vec3 sun_pos, HMM_Vec3 pos) {
    const HMM_Vec3 d = HMM_SubV3(sun_pos, pos);
    return HMM_DotV3(d, d) < k_sun_avoid_radius_sq;
}

// Returns the per-base repulsion vector summed across all dockable nav
// points within the warn radius. Magnitude of the returned vector is
// already in [0..1] (linear ramp from warn to no-fly); direction is
// unit-summed so the caller can renormalise after blending.
HMM_Vec3 base_repulsion(const std::vector<NavPointDef>& navs,
                        HMM_Vec3 other_pos,
                        float   warn_radius,
                        float   no_fly_radius);

// Strength ramp for steering repulsion from the sun. 0 outside the avoid
// radius (so distant ships see nothing); ramps linearly to 1 at the
// hard damage boundary (15k); saturates at 1 inside that.
// Returns a UNIT repulsion vector (away from sun) and the strength (0..1)
// in (out_strength). Caller blends into their steering.
HMM_Vec3 sun_repulsion(HMM_Vec3 sun_pos, HMM_Vec3 pos,
                       float *out_strength = nullptr);

// ---- bases (nav_points with kind=="station"/"planet"/etc) ---------------------
constexpr float k_base_no_fly_radius_m  = 7500.0f;    // hard floor (NPCs + autopilot never enter)
constexpr float k_base_no_fly_radius_sq = k_base_no_fly_radius_m * k_base_no_fly_radius_m;
constexpr float k_base_warn_radius_m    = 10000.0f;   // start bending steering at this distance
constexpr float k_base_warn_radius_sq  = k_base_warn_radius_m * k_base_warn_radius_m;

} // namespace hazards
