#pragma once

#include "cockpit_overlay_layout.h"
#include "turn_response.h"

// Cosmetic cockpit-only head lag (#439). The world camera, firing rays and
// crosshair remain unchanged: this suggests the pilot moving in the seat,
// rather than moving the aim point. Zero strength disables it completely.
namespace cockpit_overlay {
struct PilotHeadMotion {
    TurnResponse lateral{}, vertical{};
    void reset() { lateral.reset(); vertical.reset(); }

    void update(float yaw_accel, float pitch_accel, float dt) {
        // Inputs are angular acceleration divided by the hull's max rate,
        // in 1/s. Oppose acceleration, not rate: steady turns settle to rest.
        lateral.step(std::clamp(-yaw_accel / 3.0f, -1.0f, 1.0f), dt, 0.075f);
        vertical.step(std::clamp(-pitch_accel / 3.0f, -1.0f, 1.0f), dt, 0.075f);
    }

    Homography transform(const Rect& viewport, float strength = 1.0f) const {
        strength = std::clamp(strength, 0.0f, 1.0f);
        const float shear = lateral.rate * 0.035f * strength;
        const float pitch_scale = 1.0f + vertical.rate * 0.018f * strength;
        const float cx = viewport.x + viewport.w * 0.5f;
        const float cy = viewport.y + viewport.h * 0.5f;
        const float bottom = viewport.y + viewport.h;
        // Extra horizontal coverage exactly compensates shear at viewport
        // top/bottom. Head lean must never reintroduce the side gaps (#436).
        const float pitch_guard = std::max(1.0f, 2.0f / pitch_scale - 1.0f);
        const float widen = 1.0f + std::abs(shear) * viewport.h / viewport.w * pitch_guard;
        Homography h;
        h.m[0] = widen;
        h.m[1] = shear;
        h.m[2] = cx * (1.0f - widen) - shear * cy;
        h.m[4] = pitch_scale;
        h.m[5] = bottom * (1.0f - pitch_scale);
        return h;
    }
};
} // namespace cockpit_overlay
