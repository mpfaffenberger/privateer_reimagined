#pragma once
// -----------------------------------------------------------------------------
// look_rotation.h — quaternion that points +Z along `dir` with `up` reference.
//
// Why this exists: a shortest-arc slerp between two arbitrary orientations is
// happy to roll the ship upside-down to take a shortcut (e.g. an "optimal"
// 180° yaw that flips world-up). For autopilot + chase-cam we want world-up
// to remain world-up across the whole slew, so we build the target pose
// explicitly from a (forward, reference-up) pair — same trick used by every
// FPS-style "look at" routine.
//
// Convention: +Z is forward, +Y up. `dir` need not be unit length; we
// normalise. `up` is only a reference — if `dir` is parallel to `up` we
// fall back to world +X for `right` so the basis still resolves.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

namespace look_rotation {

// Build a unit quaternion that points the local +Z axis along normalised
// `dir`, with `up` as the reference up vector. Use world +Y when in doubt.
inline HMM_Quat make(HMM_Vec3 dir, HMM_Vec3 up = HMM_V3(0.0f, 1.0f, 0.0f)) {
    const HMM_Vec3 f = HMM_NormV3(dir);
    HMM_Vec3 r = HMM_Cross(up, f);
    const float rlen = HMM_LenV3(r);
    if (rlen < 1e-4f) {
        // `dir` parallel to `up` (straight up/down at the player).
        // Fall back to a stable axis perpendicular to `up` for `right`.
        r = HMM_V3(1.0f, 0.0f, 0.0f);
    } else {
        r = HMM_DivV3F(r, rlen);
    }
    const HMM_Vec3 u = HMM_Cross(f, r);
    // Column-major basis [right up forward] -> rotation matrix -> quat.
    HMM_Mat4 m = HMM_M4D(1.0f);
    m.Columns[0] = HMM_V4(r.X, r.Y, r.Z, 0.0f);
    m.Columns[1] = HMM_V4(u.X, u.Y, u.Z, 0.0f);
    m.Columns[2] = HMM_V4(f.X, f.Y, f.Z, 0.0f);
    return HMM_M4ToQ_RH(m);
}

} // namespace look_rotation
