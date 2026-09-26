#pragma once

#include "cockpit_overlay_layout.h"
#include "turn_response.h"

// Cosmetic rigid cockpit slide (#444), replacing the rejected shear (#441). The world camera, firing rays and
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
        const float unit = std::min(viewport.w, viewport.h);
        const float dx = std::clamp(lateral.rate, -1.0f, 1.0f) * unit * 0.006f * strength;
        const float dy = std::clamp(vertical.rate, -1.0f, 1.0f) * unit * 0.004f * strength;
        const float cx = viewport.x + viewport.w * 0.5f;
        const float cy = viewport.y + viewport.h * 0.5f;
        // Fixed uniform overscan reserves 0.8% at every edge, more than the
        // maximum slide. It never changes with acceleration: no breathing,
        // shear, rotation or changing proportions. Only dx/dy move over time.
        const float zoom = 1.0f + 0.016f * strength;
        Homography h;
        h.m[0] = h.m[4] = zoom;
        h.m[2] = cx * (1.0f - zoom) + dx;
        h.m[5] = cy * (1.0f - zoom) + dy;
        return h;
    }
};
} // namespace cockpit_overlay
