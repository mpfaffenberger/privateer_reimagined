// -----------------------------------------------------------------------------
// hazards.cpp — see hazards.h for the design overview.
// -----------------------------------------------------------------------------
#include "hazards.h"

#include "system_def.h"

#include <algorithm>
#include <cmath>

namespace hazards {

// ---- base avoidance ----------------------------------------------------------
//
// Walks every nav_point with a "station-y" kind and, when the candidate
// (other_pos) is inside the warn radius, blends a repulsion unit vector
// into (out_steering). Steering strength ramps linearly from 0 at the
// warn radius to 1 at the no-fly radius, so a ship cruising past a base
// gets a small nudge and one entering the danger zone gets a hard shove.
// Caller is responsible for normalising (out_steering) and combining with
// their own base direction.
//
// Implementation detail: we sum repulsion vectors first, then normalise,
// so multiple nearby bases compose into one sensible push. That said
// stations tend to be sparse in our nav data so this rarely bites.

// Bank kinds we treat as "bases" — anything dockable or with a planet/station
// silhouette. "nav" alone is the asteroid field / generic nav point and
// doesn't have a hull to avoid.
bool is_base_kind(std::string_view k) {
    return k == "station" || k == "planet" || k == "mining" || k == "base";
}

HMM_Vec3 base_repulsion(const std::vector<NavPointDef>& navs,
                        HMM_Vec3 other_pos,
                        float   warn_radius = k_base_warn_radius_m,
                        float   no_fly_radius = k_base_no_fly_radius_m) {
    HMM_Vec3 accum{0.0f, 0.0f, 0.0f};
    for (const NavPointDef& n : navs) {
        if (!is_base_kind(n.kind)) continue;
        const HMM_Vec3 d   = HMM_SubV3(other_pos, n.position);
        const float    d2  = HMM_DotV3(d, d);
        const float    wr2 = warn_radius * warn_radius;
        if (d2 >= wr2 || d2 < 1e-6f) continue;        // out of warn zone, or coincident
        const float dist = std::sqrt(d2);
        // Linear ramp: 0 at warn_radius, 1.0 at no_fly_radius (and beyond).
        const float t      = (warn_radius - dist) / std::max(warn_radius - no_fly_radius, 1.0f);
        const float weight = std::clamp(t, 0.0f, 1.0f);
        const HMM_Vec3 dir = HMM_DivV3F(d, dist);
        accum = HMM_AddV3(accum, HMM_MulV3F(dir, weight));
    }
    return accum;
}

// ---- sun repulsion ------------------------------------------------------------
//
// Returns a unit vector pointing away from the sun with a strength that
// ramps linearly from 0 at the avoid radius (20k) to 1 at the damage
// boundary (15k), and stays at 1 inside the damage zone. The strength is
// written through (out_strength) so the caller can pick its own blend.
HMM_Vec3 sun_repulsion(HMM_Vec3 sun_pos, HMM_Vec3 pos, float* out_strength) {
    const HMM_Vec3 d = HMM_SubV3(sun_pos, pos);
    const float    d2 = HMM_DotV3(d, d);
    const float    dist = std::sqrt(std::max(d2, 1e-6f));
    // t in [0..1] : 0 at k_sun_avoid_radius, 1 at k_sun_damage_radius and beyond.
    float t = (k_sun_avoid_radius_m - dist) / (k_sun_avoid_radius_m - k_sun_damage_radius_m);
    t = std::clamp(t, 0.0f, 1.0f);
    if (out_strength) *out_strength = t;
    if (dist < 1e-3f) return HMM_V3(0, 1, 0);   // at the sun -- arbitrary up
    HMM_Vec3 dir = HMM_DivV3F(d, dist);
    return HMM_MulV3F(dir, -t);   // negate so it's "away" from the sun
}

} // namespace hazards
