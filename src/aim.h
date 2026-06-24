#pragma once
// -----------------------------------------------------------------------------
// aim.h — projectile lead / intercept prediction (single source of truth).
//
// Given a shooter, a moving target, and a finite-speed projectile, "where
// do I aim so the bullet and the target arrive at the same place at the
// same time?" This is the classic first-order intercept problem and it
// shows up in three places: the AI brain (ai_brain.cpp computes c.lead_pos
// to point the nose), the player's ITTS reticle (main.cpp), and now NPC
// turrets (firing.cpp), which need a per-mount aim vector.
//
// The math: let r = target_pos - shooter_pos, v = target_vel, s = proj
// speed. We want the time t at which the projectile (travelling distance
// s*t from the shooter) reaches the target's future position:
//
//     |r + v*t|^2 = (s*t)^2
//   → (v·v - s^2) t^2 + 2(r·v) t + (r·r) = 0
//
// Solve the quadratic for the SMALLEST POSITIVE root (the soonest moment
// the shot can connect). When the quadratic has no positive solution —
// target outrunning the projectile, or degenerate inputs — fall back to
// the linear approximation t ≈ |r| / s that ai_brain.cpp has always used.
// The returned point is the target's predicted position at t.
//
// Header-only + inline: the function is a handful of dot products, called
// once per turret mount per frame. Inlining keeps it out of a translation
// unit of its own (no CMakeLists churn) and lets the optimiser fold it
// into the firing loop.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>

#include <cmath>

namespace aim {

// World-space point to aim a projectile of `proj_speed` (m/s) at, so it
// intercepts a target now at `target_pos` moving at `target_vel` (world
// m/s), fired from `shooter_pos`. Degrades to the straight-line lead
// (dist/speed) when no positive intercept time exists. Never returns a
// NaN — proj_speed ≤ 0 just yields target_pos unchanged.
inline HMM_Vec3 lead_point(HMM_Vec3 shooter_pos,
                           HMM_Vec3 target_pos,
                           HMM_Vec3 target_vel,
                           float    proj_speed) {
    const HMM_Vec3 r = HMM_SubV3(target_pos, shooter_pos);
    if (proj_speed <= 1.0f) return target_pos;

    const float a = HMM_DotV3(target_vel, target_vel) - proj_speed * proj_speed;
    const float b = 2.0f * HMM_DotV3(r, target_vel);
    const float c = HMM_DotV3(r, r);

    float t = -1.0f;
    if (std::fabs(a) < 1e-6f) {
        // Linear case (target speed ≈ projectile speed): b*t + c = 0.
        if (std::fabs(b) > 1e-6f) t = -c / b;
    } else {
        const float disc = b * b - 4.0f * a * c;
        if (disc >= 0.0f) {
            const float sq = std::sqrt(disc);
            const float t0 = (-b - sq) / (2.0f * a);
            const float t1 = (-b + sq) / (2.0f * a);
            // Smallest strictly-positive root.
            if (t0 > 0.0f && t1 > 0.0f) t = std::fmin(t0, t1);
            else if (t0 > 0.0f)         t = t0;
            else if (t1 > 0.0f)         t = t1;
        }
    }

    // Fallback: straight-line time-of-flight (matches ai_brain's original
    // approximation) when the quadratic gives nothing usable.
    if (!(t > 0.0f)) {
        const float dist = std::sqrt(std::fmax(c, 1e-6f));
        t = dist / proj_speed;
    }

    return HMM_AddV3(target_pos, HMM_MulV3F(target_vel, t));
}

} // namespace aim
