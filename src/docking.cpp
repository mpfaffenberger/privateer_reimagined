// -----------------------------------------------------------------------------
// docking.cpp — request-landing + autodock approach state machine.
//
// See docking.h for the design rationale. The interesting bit is tick():
// a deliberately simple autopilot. No path planning, no obstacle
// avoidance — at the demo's scale the pad is in open space and a
// proportional "steer the nose at the pad, ease speed to zero as you
// arrive" controller reads as a smooth, hands-off landing. If a base
// ever sits inside an asteroid belt this grows a clearance leg; until
// then YAGNI.
// -----------------------------------------------------------------------------

#include "docking.h"

#include "camera.h"
#include "game_state.h"
#include "player.h"
#include "savegame.h"
#include "sfx.h"
#include "system_def.h"
#include "world_scale.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

// Shortest-arc orientation that points the camera's default forward
// (-Z) at `dir`, using world +Y as the up reference. Mirrors the spawn
// look_at math in main.cpp — kept local because it's a one-off the
// autopilot slerps toward, not general camera API.
HMM_Quat facing_quat(HMM_Vec3 dir) {
    const HMM_Vec3 def_fwd = HMM_V3(0.0f, 0.0f, -1.0f);
    const HMM_Vec3 axis    = HMM_Cross(def_fwd, dir);
    const float    sin2    = HMM_DotV3(axis, axis);
    if (sin2 <= 1e-10f) {
        // dir is (anti)parallel to default forward; identity is close
        // enough for the parallel case and the slerp smooths the rest.
        return HMM_Q(0.0f, 0.0f, 0.0f, 1.0f);
    }
    const float    sin_a = std::sqrt(sin2);
    const float    cos_a = std::fmax(-1.0f, std::fmin(1.0f, HMM_DotV3(def_fwd, dir)));
    const float    angle = std::atan2(sin_a, cos_a);
    const HMM_Vec3 unit  = HMM_DivV3F(axis, sin_a);
    return HMM_QFromAxisAngle_RH(unit, angle);
}

// Frame-rate-independent approach factor: lerp toward target by this
// each frame, asymptotic. Same shape as Camera::integrate's cruise lerp.
float ease_k(float rate, float dt) {
    return 1.0f - std::exp(-rate * dt);
}

// Campaign clearance gate (#126). Consulted by every landing path;
// nullptr / false = clear. The gate owns its own messaging.
std::function<bool(const std::string&)> g_clearance_gate;
std::function<void(PlayerState&, const std::string&)> g_commit_handler;

bool clearance_refused(const NavPointDef& nav) {
    return g_clearance_gate && !nav.base_id.empty() &&
           g_clearance_gate(nav.base_id);
}

void commit_landing(Docking& d, GameState& gs, PlayerState& player) {
    player.docked           = true;
    player.last_docked_base = d.base_id;
    if (player.day < std::numeric_limits<int>::max()) ++player.day;
    if (g_commit_handler) g_commit_handler(player, d.base_id);
    game_state::request_mode(gs, GameMode::Landed);
    if (!savegame::save_timestamped(player).empty()) {
        std::printf("[save] autosaved at %s (%lld cr)\n",
                    d.base_id.c_str(), (long long)player.credits);
    }
}

} // namespace

namespace docking {

void set_clearance_gate(std::function<bool(const std::string&)> gate) {
    g_clearance_gate = std::move(gate);
}

void set_commit_handler(
    std::function<void(PlayerState&, const std::string&)> handler) {
    g_commit_handler = std::move(handler);
}

bool controls_locked(const Docking& d) {
    return d.state == DockingState::Approaching ||
           d.state == DockingState::Docking;
}

DockResult can_request(const Docking& d, HMM_Vec3 player_pos,
                       HMM_Vec3 player_vel, const NavPointDef& nav) {
    if (!nav.dockable || nav.base_id.empty()) return DockResult::NotDockable;
    // Campaign blockade (#126): the tower answers, and the answer is no.
    if (clearance_refused(nav))               return DockResult::Refused;
    // Mid-approach or freshly launched: don't offer a new clearance.
    if (d.state != DockingState::None || d.cooldown_s > 0.0f) {
        return DockResult::Busy;
    }
    const float dist = HMM_LenV3(HMM_SubV3(nav.position, player_pos));
    if (dist > k_dock_range_m)            return DockResult::TooFar;
    // Nav points are static, so relative speed is just our own speed.
    if (HMM_LenV3(player_vel) > k_dock_speed_max) return DockResult::TooFast;
    return DockResult::Cleared;
}

const char* result_str(DockResult r) {
    switch (r) {
        case DockResult::Cleared:     return "DOCKING CLEARED";
        case DockResult::TooFar:      return "TOO FAR";
        case DockResult::TooFast:     return "TOO FAST";
        case DockResult::Busy:        return "STAND BY";
        case DockResult::Refused:     return "DOCKING REFUSED";
        case DockResult::NotDockable: return "";
        default:                      return "";
    }
}

DockResult request(Docking& d, HMM_Vec3 player_pos, HMM_Vec3 player_vel,
                   const NavPointDef& nav) {
    const DockResult r = can_request(d, player_pos, player_vel, nav);
    if (r != DockResult::Cleared) {
        std::printf("[dock] request to %s REJECTED: %s\n",
                    nav.base_id.empty() ? nav.name.c_str() : nav.base_id.c_str(),
                    result_str(r));
        return r;
    }
    d.state     = DockingState::Requested;
    d.base_id   = nav.base_id;
    d.base_name = nav.name;
    d.pad_pos   = nav.position;
    d.timer_s   = 0.0f;
    d.log_accum = 0.0f;
    sfx::ui_click();                 // request accepted blip
    std::printf("[dock] request to %s ACCEPTED — auto-approach engaged\n",
                d.base_id.c_str());
    return r;
}

void begin_auto(Docking& d, const NavPointDef& nav) {
    // Already docking or just launched? Leave it be.
    if (d.state != DockingState::None || d.cooldown_s > 0.0f) return;
    if (clearance_refused(nav)) return;    // blockade: fly on through
    d.state     = DockingState::Requested;
    d.base_id   = nav.base_id;
    d.base_name = nav.name;
    d.pad_pos   = nav.position;
    d.timer_s   = 0.0f;
    d.log_accum = 0.0f;
    sfx::ui_click();
    std::printf("[dock] AUTO-LAND zone -> approach to %s\n",
                d.base_id.empty() ? nav.name.c_str() : d.base_id.c_str());
}

void land_now(Docking& d, GameState& gs, PlayerState& player,
              const NavPointDef& nav) {
    // Instant land (np-3dp.22): the auto-land zone doesn't fly an approach
    // — it commits to Landed the moment you cross the threshold. Mirrors
    // the Docking->Docked transition in tick(): set the docked flags,
    // request Landed, and autosave (a new timestamped file every landing).
    if (d.state != DockingState::None || d.cooldown_s > 0.0f) return;
    if (clearance_refused(nav)) {
        std::printf("[dock] landing at %s REFUSED (clearance gate)\n",
                    nav.base_id.c_str());
        return;
    }
    d.state                 = DockingState::Docked;
    d.base_id               = nav.base_id;
    d.base_name             = nav.name;
    d.pad_pos               = nav.position;
    d.timer_s               = 0.0f;
    commit_landing(d, gs, player);
    sfx::ui_click();
    std::printf("[dock] AUTO-LAND zone -> instant land at %s\n",
                d.base_id.empty() ? nav.name.c_str() : d.base_id.c_str());
}

void tick(Docking& d, Camera& cam, GameState& gs, PlayerState& player, float dt) {
    // Bleed the post-launch lockout regardless of state.
    if (d.cooldown_s > 0.0f) {
        d.cooldown_s -= dt;
        if (d.cooldown_s < 0.0f) d.cooldown_s = 0.0f;
    }

    switch (d.state) {
    case DockingState::Requested:
        // One-frame handshake → start flying. Fall through so the first
        // approach step happens the same frame the request landed.
        d.state   = DockingState::Approaching;
        d.timer_s = 0.0f;
        [[fallthrough]];

    case DockingState::Approaching: {
        d.timer_s += dt;
        // Autopilot owns the engine: no cruise winding up mid-dock.
        cam.cruise_target = 0.0f;

        const HMM_Vec3 to_pad = HMM_SubV3(d.pad_pos, cam.position);
        const float    dist   = HMM_LenV3(to_pad);
        const HMM_Vec3 dir    = dist > 1e-3f ? HMM_DivV3F(to_pad, dist)
                                             : cam.forward();

        // Swing the nose toward the pad (smooth, ~2/s time constant).
        const HMM_Quat want = facing_quat(dir);
        cam.orientation = HMM_NormQ(HMM_SLerp(cam.orientation, ease_k(2.0f, dt), want));

        // Ease speed to zero at the pad: desired speed scales with the
        // distance remaining (so we decelerate into the dock), capped at
        // the approach cruise.
        const float desired = std::fmin(k_approach_speed_max, dist * 0.7f);
        const HMM_Vec3 target_vel = HMM_MulV3F(dir, desired);
        cam.velocity = HMM_LerpV3(cam.velocity, ease_k(3.0f, dt), target_vel);
        cam.position = HMM_AddV3(cam.position,
            HMM_MulV3F(cam.velocity, dt * world_scale::k_world_velocity_scale));

        // Progress log, throttled to ~2/s so it's readable, not spam.
        d.log_accum += dt;
        if (d.log_accum >= 0.5f) {
            d.log_accum = 0.0f;
            std::printf("[dock] approach %s — dist %.0fm, spd %.0fm/s\n",
                        d.base_id.c_str(), dist, HMM_LenV3(cam.velocity));
        }

        if (dist < k_arrive_dist_m) {
            cam.velocity = HMM_V3(0.0f, 0.0f, 0.0f);
            cam.position = d.pad_pos;            // settle exactly on the pad
            d.state      = DockingState::Docking;
            d.timer_s    = 0.0f;
            std::printf("[dock] arrived at %s — securing (%.1fs)\n",
                        d.base_id.c_str(), k_docking_pause_s);
        }
        break;
    }

    case DockingState::Docking: {
        d.timer_s += dt;
        cam.velocity = HMM_V3(0.0f, 0.0f, 0.0f);   // parked
        if (d.timer_s >= k_docking_pause_s) {
            d.state = DockingState::Docked;
            commit_landing(d, gs, player);
            std::printf("[dock] docked at %s\n", d.base_id.c_str());
        }
        break;
    }

    case DockingState::Docked:
    case DockingState::None:
    default:
        break;
    }
}

void launch(Docking& d, Camera& cam, GameState& gs, PlayerState& player,
            HMM_Vec3 sun_pos) {
    // Launch INWARD: place the ship k_launch_offset_m off the pad toward
    // the system centre (the sun), facing that way (np-3dp.22). You start
    // ~3.5km out in open space pointed at where the traffic is, instead
    // of nose-to-the-pad. Falls back to the pad->ship vector (then a
    // default axis) if the sun direction is degenerate.
    HMM_Vec3 out = HMM_SubV3(sun_pos, d.pad_pos);
    if (HMM_LenV3(out) < 1.0f) out = HMM_SubV3(cam.position, d.pad_pos);
    if (HMM_LenV3(out) < 1.0f) out = HMM_V3(0.0f, 0.0f, 1.0f);
    out = HMM_NormV3(out);

    cam.position    = HMM_AddV3(d.pad_pos, HMM_MulV3F(out, k_launch_offset_m));
    cam.velocity    = HMM_MulV3F(out, k_launch_speed);
    cam.orientation = facing_quat(out);   // look inward, toward the centre
    cam.cruise_target = 0.0f;
    cam.cruise_level  = 0.0f;

    player.docked = false;

    d.state      = DockingState::None;
    d.timer_s    = 0.0f;
    d.log_accum  = 0.0f;
    d.cooldown_s = k_relaunch_cooldown_s;   // don't instantly re-dock

    game_state::request_mode(gs, GameMode::Flight);
    std::printf("[dock] launch from %s — back to flight (%.0fm off pad)\n",
                d.base_id.c_str(), k_launch_offset_m);
}

} // namespace docking
