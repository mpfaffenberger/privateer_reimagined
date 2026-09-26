#pragma once

#include <cmath>

// Two cascaded first-order responses = a critically damped rate controller.
// Both stages remain inside the commanded rate envelope. Unlike filtering
// the mouse position, this models the ship's angular response, including
// smooth braking and reversals. Exact integration avoids frame-rate-dependent
// tuning and integrates the angle, not just the end-of-frame rate (#433).
struct TurnResponse {
    float drive = 0.0f;
    float rate = 0.0f;

    void reset() { drive = rate = 0.0f; }

    float acceleration(float response_seconds) const {
        return response_seconds > 0.0f ? (drive - rate) / response_seconds : 0.0f;
    }

    float step(float target, float dt, float response_seconds) {
        if (!(dt > 0.0f)) return 0.0f;
        if (!(response_seconds > 0.0f)) {
            drive = rate = target;
            return target * dt;
        }
        const double omega = 1.0 / response_seconds;
        const double elapsed = omega * dt;
        const double decay = std::exp(-elapsed);
        const double one_minus_decay = -std::expm1(-elapsed);
        const double a = drive - target;
        const double b = rate - target;
        const double angle = target * dt + b * one_minus_decay / omega
            + a * (one_minus_decay - elapsed * decay) / omega;
        drive = static_cast<float>(target + a * decay);
        rate = static_cast<float>(target + (b + a * elapsed) * decay);
        return static_cast<float>(angle);
    }
};
