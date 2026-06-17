// -----------------------------------------------------------------------------
// autopilot.cpp — nav-point cruise autopilot.
//
// See autopilot.h for the design rationale. The meat is tick(): a
// proportional "swing the nose at the nav, wind up the cruise engine,
// barrel over, ease to a stop" controller. No path planning — at the
// system's open-space scale the straight line is the whole job (and if a
// nav ever sits inside a hazard, that's the encounter director's problem,
// not the autopilot's: YAGNI until then).
// -----------------------------------------------------------------------------

#include "autopilot.h"

#include "camera.h"
#include "sfx.h"
#include "system_def.h"
#include "threat.h"
#include "world_scale.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Shortest-arc orientation that points the camera's default forward (-Z)
// at `dir`, world +Y up. Twin of docking.cpp's facing_quat — kept local
// because it's a 6-line one-off the slerp smooths anyway; promoting it to
// shared camera API is a fine future refactor but not worth the coupling
// for two callers today.
HMM_Quat facing_quat(HMM_Vec3 dir) {
    const HMM_Vec3 def_fwd = HMM_V3(0.0f, 0.0f, -1.0f);
    const HMM_Vec3 axis    = HMM_Cross(def_fwd, dir);
    const float    sin2    = HMM_DotV3(axis, axis);
    if (sin2 <= 1e-10f) {
        return HMM_Q(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const float    sin_a = std::sqrt(sin2);
    const float    cos_a = std::fmax(-1.0f, std::fmin(1.0f, HMM_DotV3(def_fwd, dir)));
    const float    angle = std::atan2(sin_a, cos_a);
    const HMM_Vec3 unit  = HMM_DivV3F(axis, sin_a);
    return HMM_QFromAxisAngle_RH(unit, angle);
}

// Frame-rate-independent asymptotic lerp factor (same shape as
// Camera::integrate's cruise smoothing).
float ease_k(float rate, float dt) {
    return 1.0f - std::exp(-rate * dt);
}

void set_msg(Autopilot& a, const char* text) {
    std::snprintf(a.msg, sizeof(a.msg), "%s", text);
    a.msg_timer_s = autopilot::k_msg_secs;
}

} // namespace

namespace autopilot {

bool controls_locked(const Autopilot& a) { return a.phase != AutopilotPhase::Idle; }
bool engaged(const Autopilot& a)         { return a.phase != AutopilotPhase::Idle; }

EngageResult try_engage(Autopilot& a, Camera& cam,
                        const StarSystem& system, int selected_nav) {
    // No target → nothing to fly to. Flash and bail (the bead's
    // "no-op gracefully" path).
    if (selected_nav < 0 || selected_nav >= (int)system.nav_points.size()) {
        set_msg(a, "NO NAV SELECTED");
        std::printf("[autopilot] engage refused — no nav selected\n");
        return EngageResult::NoNav;
    }

    // Hostile gate (threat.h). Stubbed false today; live with np-ma2.3.
    if (threat::hostiles_near(cam.position, k_threat_radius_m)) {
        set_msg(a, "HOSTILES DETECTED - CANNOT ENGAGE");
        std::printf("[autopilot] engage refused — hostiles within %.0fu\n",
                    k_threat_radius_m);
        return EngageResult::Hostiles;
    }

    const NavPointDef& nav = system.nav_points[selected_nav];
    a.phase     = AutopilotPhase::Cruising;
    a.nav_index = selected_nav;
    a.nav_name  = nav.name;
    a.target    = nav.position;
    a.log_accum = 0.0f;
    // Stash the player's normal afterburn cap and bump it to the high
    // autopilot cruising speed. Restored in disengage so manual flight
    // afterward still tops out at the engine-level-respecting limit.
    a.saved_cruise1       = cam.max_speed_cruise1;
    cam.max_speed_cruise1 = k_cruise_speed;
    set_msg(a, "AUTOPILOT ENGAGED");
    sfx::ui_click();   // engage blip; the cruise windup rides the cruise edge
    const float dist = HMM_LenV3(HMM_SubV3(nav.position, cam.position));
    std::printf("[autopilot] ENGAGED -> %s (%.0fu out)\n",
                nav.name.c_str(), dist);
    return EngageResult::Engaged;
}

void disengage(Autopilot& a, Camera& cam, const char* reason) {
    cam.cruise_target = 0.0f;             // let the engine wind back down
    cam.set_forward_input(0.0f);          // and command a stop
    if (a.saved_cruise1 > 0.0f) {
        cam.max_speed_cruise1 = a.saved_cruise1;
        a.saved_cruise1 = 0.0f;
    }
    a.phase     = AutopilotPhase::Idle;
    a.nav_index = -1;
    a.log_accum = 0.0f;
    set_msg(a, reason);
}

void tick(Autopilot& a, Camera& cam, float dt) {
    // Banner decay runs unconditionally so a refusal flash ("NO NAV
    // SELECTED") fades even though we never left Idle.
    if (a.msg_timer_s > 0.0f) {
        a.msg_timer_s -= dt;
        if (a.msg_timer_s < 0.0f) a.msg_timer_s = 0.0f;
    }
    if (a.phase == AutopilotPhase::Idle) return;

    // Hostile gate, re-checked every engaged frame. Stub returns false
    // today (np-ma2.3 seam) so this never trips yet — but it's wired:
    // the moment the director spawns hostiles in range we drop out and
    // hand control back, exactly like the OG.
    if (threat::hostiles_near(cam.position, k_threat_radius_m)) {
        std::printf("[autopilot] HOSTILES detected — dropping autopilot\n");
        disengage(a, cam, "AUTOPILOT DISENGAGED - HOSTILES");
        return;
    }

    const HMM_Vec3 to_t = HMM_SubV3(a.target, cam.position);
    const float    dist = HMM_LenV3(to_t);
    const HMM_Vec3 dir  = dist > 1e-3f ? HMM_DivV3F(to_t, dist) : cam.forward();

    // Swing the nose toward the nav (shortest-arc slerp, same feel as
    // the docking approach).
    cam.orientation = HMM_NormQ(HMM_SLerp(cam.orientation, ease_k(k_turn_rate, dt),
                                          facing_quat(dir)));

    if (a.phase == AutopilotPhase::Cruising) {
        // Reuse the camera's cruise engine: drive the throttle to full
        // and set the desired forward speed to k_cruise_speed. integrate()
        // ramps v_fwd toward it at accel rate, clamped to the autopilot-
        // bumped max_speed_cruise1, so a beefier engine autopilots faster
        // for free (within the bump).
        cam.cruise_target = 1.0f;
        cam.set_forward_input(k_cruise_speed);
        cam.integrate(dt);

        // Progress log, throttled to ~2/s (matches docking's cadence).
        a.log_accum += dt;
        if (a.log_accum >= 0.5f) {
            a.log_accum = 0.0f;
            std::printf("[autopilot] cruise -> %s — dist %.0fu, spd %.0fu/s\n",
                        a.nav_name.c_str(), dist, HMM_LenV3(cam.velocity));
        }

        if (dist <= k_arrival_radius_m) {
            a.phase = AutopilotPhase::Arriving;
            set_msg(a, "AUTOPILOT - ARRIVING");
            std::printf("[autopilot] within %.0fu of %s — easing to a stop\n",
                        k_arrival_radius_m, a.nav_name.c_str());
        }
        return;
    }

    // Arriving: cruise cut, velocity easing to zero, integrate for the
    // last of the coast. Disengage (hand control back) once stopped.
    cam.cruise_target = 0.0f;
    cam.set_forward_input(0.0f);          // command a stop
    cam.velocity = HMM_LerpV3(cam.velocity, ease_k(k_brake_rate, dt),
                              HMM_V3(0.0f, 0.0f, 0.0f));
    cam.integrate(dt);

    if (HMM_LenV3(cam.velocity) <= k_stop_speed) {
        cam.brake();   // crisp full stop
        std::printf("[autopilot] ARRIVED at %s — control returned\n",
                    a.nav_name.c_str());
        disengage(a, cam, "AUTOPILOT DISENGAGED - ARRIVED");
    }
}

} // namespace autopilot
