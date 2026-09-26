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
#include "hazards.h"
#include "look_rotation.h"
#include "sfx.h"
#include "system_def.h"
#include "threat.h"
#include "world_scale.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// World up used to build the autopilot's target orientation. Keeping it
// world-aligned (not camera-relative) means a 180° yaw slews through
// yaw, not a barrel roll — `look_rotation::make` builds the basis with
// `up` as a reference, so the resulting pose stays right-side-up.
constexpr HMM_Vec3 k_world_up = { 0.0f, 1.0f, 0.0f };

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

EngageResult engage_check(const Camera& cam, const StarSystem& system,
                          int selected_nav) {
    if (selected_nav < 0 || selected_nav >= (int)system.nav_points.size())
        return EngageResult::NoNav;
    // Hostile gate (threat.h): live spatial query over the ship registry.
    if (threat::hostiles_near(cam.position, k_threat_radius_m))
        return EngageResult::Hostiles;
    return EngageResult::Engaged;
}

EngageResult try_engage(Autopilot& a, Camera& cam,
                        const StarSystem& system, int selected_nav) {
    // No target → nothing to fly to; hostiles near → no autopilot through
    // a furball. Flash the refusal and bail (the "no-op gracefully" path).
    switch (engage_check(cam, system, selected_nav)) {
    case EngageResult::NoNav:
        set_msg(a, "NO NAV SELECTED");
        std::printf("[autopilot] engage refused — no nav selected\n");
        return EngageResult::NoNav;
    case EngageResult::Hostiles:
        set_msg(a, "HOSTILES DETECTED - CANNOT ENGAGE");
        std::printf("[autopilot] engage refused — hostiles within %.0fu\n",
                    k_threat_radius_m);
        return EngageResult::Hostiles;
    case EngageResult::Engaged:
        break;
    }

    const NavPointDef& nav = system.nav_points[selected_nav];
    a.phase     = AutopilotPhase::Cruising;
    a.nav_index = selected_nav;
    a.nav_name  = nav.name;
    a.target    = nav.position;
    a.start_pos = cam.position;
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

void tick(Autopilot& a, Camera& cam, const StarSystem& system,
          float dt, HMM_Vec3 sun_pos) {
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

    const float traveled = HMM_LenV3(HMM_SubV3(cam.position, a.start_pos));
    if (traveled >= k_navpoint_break_after_m) {
        for (int i = 0; i < (int)system.nav_points.size(); ++i) {
            if (i == a.nav_index) continue; // reaching the chosen target is handled below
            const NavPointDef& nav = system.nav_points[i];
            const float d = HMM_LenV3(HMM_SubV3(nav.position, cam.position));
            if (d <= k_navpoint_break_m) {
                std::printf("[autopilot] within %.0fu of %s after %.0fu traveled — dropping autopilot\n",
                            k_navpoint_break_m, nav.name.c_str(), traveled);
                disengage(a, cam, "AUTOPILOT DISENGAGED - NAVPOINT");
                return;
            }
        }
    }

    const HMM_Vec3 to_t = HMM_SubV3(a.target, cam.position);
    const float    dist = HMM_LenV3(to_t);
    HMM_Vec3       dir  = dist > 1e-3f ? HMM_DivV3F(to_t, dist) : cam.forward();

    // Sun avoidance (np-3dp): bend the desired steering direction away
    // from the sun when the autopilot would otherwise fly through it.
    // Strength ramps linearly from 0 at the avoid radius (20k) to 1 at
    // the damage boundary (15k) and inside. Within the avoid radius the
    // autopilot never enters the 15k damage zone.
    if (hazards::inside_sun_avoid(sun_pos, cam.position)) {
        float sun_t = 0.0f;
        const HMM_Vec3 sun_dir = hazards::sun_repulsion(sun_pos, cam.position, &sun_t);
        if (sun_t > 1e-3f) {
            // Bend `dir` AWAY from the sun: subtract the projection of
            // sun_dir onto dir and renormalise. The bend angle scales
            // with sun_t (0..1).
            const float proj = HMM_DotV3(sun_dir, dir);
            const HMM_Vec3 lateral = HMM_SubV3(sun_dir, HMM_MulV3F(dir, proj));
            const float bend_strength = sun_t * 0.6f;  // cap so we still make progress
            dir = HMM_AddV3(HMM_MulV3F(dir, 1.0f - bend_strength),
                            HMM_MulV3F(lateral, bend_strength));
            const float dl = HMM_LenV3(dir);
            if (dl > 1e-3f) dir = HMM_DivV3F(dir, dl);
        }
    }

    // Swing the nose toward the nav. Build the target pose from a
    // (-dir, world_up) basis instead of a shortest-arc slerp so a 180°
    // yaw slews through yaw rather than rolling the ship upside-down.
    // `look_rotation::make` aligns +Z along its argument; the camera
    // looks down -Z, so we feed it -dir so -Z (camera forward) ends
    // up pointing at the target.
    const HMM_Quat want = look_rotation::make(HMM_MulV3F(dir, -1.0f), k_world_up);
    cam.orientation = HMM_NormQ(HMM_SLerp(cam.orientation, ease_k(k_turn_rate, dt),
                                          want));

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
