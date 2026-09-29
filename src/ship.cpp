#include "ship.h"

#include "armor.h"
#include "mobility.h"
#include "ship_class.h"
#include "ship_sprite.h"
#include "shield.h"
#include "world_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

namespace {

constexpr float k_deg_to_rad = 0.017453293f;

// Per-facing shield maximum in cm, including the flat capital-ship bonus.
// Capital hulls (klass.capital) carry an extra k_capital_shield_bonus_cm on
// EVERY facing so they soak punishment befitting their size. The bonus is
// only added on top of a real shield generator's facing value -- a hull
// with no default_shield still has 0 cm of shields (matching the current
// "no generator = no shields" behavior); callers guard the null case.
constexpr float k_capital_shield_bonus_cm = 200.0f;
static float shield_max_cm(const ShipClass& k, float base_facing_cm, float mult) {
    float m = base_facing_cm * mult;
    if (k.capital) m += k_capital_shield_bonus_cm;
    return m;
}

// What direction the ship's nose is pointing, in world space. Identity
// orientation = nose along world +Z (matches the atlas-authoring
// convention; same as ship_sprite.cpp's integrator).
HMM_Vec3 forward_world(const HMM_Quat& q) {
    const HMM_Mat4 R = HMM_QToM4(q);
    const HMM_Vec4 f = HMM_MulM4V4(R, HMM_V4(0.0f, 0.0f, 1.0f, 0.0f));
    return HMM_V3(f.X, f.Y, f.Z);
}

// Rotate a world-space vector into body frame using the inverse of the
// ship's orientation. Used to convert a desired-rotation axis (which the
// controller computes in world frame from current vs desired forward)
// into body frame, since update_ship_sprite_motion expects body-frame
// angular_velocity.
HMM_Vec3 world_to_body(const HMM_Quat& q, HMM_Vec3 v_world) {
    const HMM_Quat q_inv = HMM_InvQ(q);
    const HMM_Mat4 R_inv = HMM_QToM4(q_inv);
    const HMM_Vec4 v4    = HMM_MulM4V4(R_inv, HMM_V4(v_world.X, v_world.Y, v_world.Z, 0.0f));
    return HMM_V3(v4.X, v4.Y, v4.Z);
}

// PursueTarget: aim at target_pos and ramp throttle so we ARRIVE rather
// than overshoot. Naive "throttle to cruise always" produces clover-leaf
// orbits around the target — at 400 m/s with 50°/s max yaw, the U-turn
// radius (~v / max_yaw_rad ≈ 460 m) is comparable to typical engagement
// ranges, so the ship flies past, turns, flies past again, forever. The
// fix is a distance-based throttle ramp: full cruise far out, linear
// decel through `slow_radius`, hard stop inside `stop_radius`. Radii are
// derived from cruise_speed so a faster ship gets a proportionally
// wider arrival cone (Centurion @ 500 m/s starts slowing 5 km out;
// Tarsus @ 300 starts at 3 km).
void behavior_pursue_target(Ship& s) {
    if (!s.sprite || !s.klass) return;
    const HMM_Vec3 to = HMM_SubV3(s.behavior.target_pos, s.sprite->position);
    const float    d2 = HMM_DotV3(to, to);
    if (d2 < 1e-3f) {
        // Sub-mm distance — we ARE the target. Park.
        s.controller.desired_speed = 0.0f;
        return;
    }
    const float d = std::sqrt(d2);
    s.controller.desired_forward = HMM_DivV3F(to, d);

    // Kinematic arrival: at distance `d` (minus a small arrival
    // tolerance), the highest speed we can carry and STILL decelerate
    // to zero exactly at the target is v_brake = sqrt(2·a·d). Clamp to
    // cruise_speed and that's our target throttle. This auto-scales:
    // far away -> full cruise (v_brake exceeds cruise so the clamp
    // wins), close in -> the sqrt curve smoothly ramps speed down,
    // arrival distance -> exactly zero. No tuning constants beyond the
    // arrival tolerance; the ship's own accel + cruise pick the
    // window. A previous draft used arbitrary radii (cruise·10 / cruise·1)
    // which under-shot at short range — Talon at 500m initial distance
    // got a 11 m/s target and looked stationary. The kinematic version
    // gives ~308 m/s at the same distance and a clean smooth approach.
    constexpr float k_arrival_tol_m = 25.0f;
    // speed_scale lets callers ask for a gentler cruise (civilian loiter /
    // lane traffic ambles at <1.0); defaults to 1.0 for combat/JSON users.
    const float v_max         = s.klass->cruise_speed * s.controller.speed_scale;
    const float accel         = mobility::accel_mps2(s.klass->acceleration);
    const float dist_braking  = std::max(0.0f, d - k_arrival_tol_m);
    const float v_brake       = std::sqrt(2.0f * accel * dist_braking);
    s.controller.desired_speed = std::min(v_max, v_brake);
}

// Proportional flight controller. Closes the angle from current forward
// to controller.desired_forward by writing a body-frame omega; lerps
// forward_speed toward controller.desired_speed at class accel rate.
//
// Kp = 4.0 produces a snappy-but-not-twitchy turn — at the max-rate
// clamp, a 90° gap closes in roughly π/2 / max_rate seconds (~1.5s for
// a Good-tier ship). Tune later if the AI feels sluggish/jittery.
void flight_controller_step(Ship& s, float dt) {
    if (!s.sprite || !s.klass) return;
    const HMM_Vec3 fwd  = forward_world(s.sprite->orientation);
    const HMM_Vec3 want = s.controller.desired_forward;

    // axis_world = fwd × want; |axis_world| = sin(angle); fwd·want = cos(angle).
    HMM_Vec3 axis_world = HMM_Cross(fwd, want);
    const float sin_mag2 = HMM_DotV3(axis_world, axis_world);
    if (sin_mag2 > 1e-10f) {
        const float sin_mag = std::sqrt(sin_mag2);
        const float cos_a   = std::clamp(HMM_DotV3(fwd, want), -1.0f, 1.0f);
        const float angle   = std::atan2(sin_mag, cos_a);   // 0..π, well-conditioned

        // Convert rotation axis from world to body frame for the integrator.
        HMM_Vec3 axis_body = world_to_body(s.sprite->orientation,
                                            HMM_DivV3F(axis_world, sin_mag));
        // Defensive renormalise — rotation is unitary so axis_body's
        // magnitude should already be ~1, but float drift over time can
        // accumulate if we skip this.
        const float ab_len2 = HMM_DotV3(axis_body, axis_body);
        if (ab_len2 > 1e-10f) axis_body = HMM_DivV3F(axis_body, std::sqrt(ab_len2));

        // Single combined max-rate cap. Privateer ships have similar
        // yaw/pitch tiers, so the simpler model (single rate) reads
        // identical in-flight to a per-axis controller; revisit when a
        // ship class wants asymmetric rates.
        constexpr float Kp = 4.0f;
        const float max_rate_rad = std::min(mobility::yaw_rate_deg(s.klass->max_ypr),
                                            mobility::pitch_rate_deg(s.klass->max_ypr))
                                   * s.klass->ypr_rate_multiplier
                                   * k_deg_to_rad;
        // Damaged engines/maneuvering thrusters (#141) derate the turn rate.
        const float omega_mag    = std::min(Kp * angle,
                                            max_rate_rad * ship_systems::turn_mult(s.systems));
        s.sprite->angular_velocity = HMM_MulV3F(axis_body, omega_mag);
    } else {
        // Aligned — kill any residual rotation so the integrator doesn't
        // keep nudging us off-axis. Without this, tiny numerical
        // remainders from previous frames can produce visible jitter.
        s.sprite->angular_velocity = HMM_V3(0.0f, 0.0f, 0.0f);
    }

    // Throttle: lerp forward_speed toward desired at class accel. Damaged
    // engines (#141) cap whatever speed the behavior asked for.
    const float want_speed = s.controller.desired_speed * ship_systems::speed_mult(s.systems);
    const float ds       = want_speed - s.sprite->forward_speed;
    const float max_step = mobility::accel_mps2(s.klass->acceleration) * dt;
    s.sprite->forward_speed += std::clamp(ds, -max_step, +max_step);

    // Push the afterburner flag through to the sprite snapshot so the
    // renderer can react (recolor + grow BLUE nav lights). The visual
    // doesn't drive any kinematics -- that's all desired_speed above.
    s.sprite->afterburner = s.controller.afterburner;
}

} // namespace

namespace { uint32_t s_next_id = 1; }   // shared monotonic counter

Ship ship::spawn_player() {
    Ship s;
    s.id        = s_next_id++;
    s.is_player = true;
    s.klass     = nullptr;
    s.faction   = Faction::Civilian;   // unaligned; rep is the real currency
    s.alive     = true;
    // No sprite, no controller, no behaviour. Pose is filled in each frame
    // from the camera before perception runs. Health/energy don't apply
    // until the player ship has a class — defer to a follow-up that lets
    // the player pick a Centurion/Tarsus/etc. and inherit its stats.
    return s;
}

void ship::sync_from_sprite(Ship& s) {
    if (!s.sprite) return;
    s.position    = s.sprite->position;
    s.orientation = s.sprite->orientation;
    // World-frame velocity for NPCs: orientation * +Z * forward_speed.
    // Used by firing.cpp so projectiles inherit the shooter's motion.
    const HMM_Mat4 R = HMM_QToM4(s.orientation);
    const HMM_Vec4 f = HMM_MulM4V4(R, HMM_V4(0.0f, 0.0f, 1.0f, 0.0f));
    s.world_velocity = HMM_MulV3F(HMM_V3(f.X, f.Y, f.Z), s.sprite->forward_speed);
}

Ship ship::spawn(const ShipClass& klass) {
    Ship s;
    s.id      = s_next_id++;
    s.klass   = &klass;
    s.faction = klass.default_faction;

    // Health = full max. Base hull armor from class; shields from the
    // fitted shield generator if one is configured.
    //
    // ENEMY SURVIVABILITY BUFF (world_scale.h): every ship spawned from a
    // class here is an NPC (the player uses spawn_player + heal_to_full,
    // which never call this), so scale armor + shields to lengthen fights
    // without nerfing damage. Armor scales directly (no regen, set once);
    // shields scale via shield_mult so the dynamically-recomputed per-facing
    // max in regen_shields()/take_damage() stays consistent. Capitals use
    // their own gentler knobs — they're already sponges via the +200cm
    // capital shield bonus, so the full fighter buff over-tanked them.
    const float enemy_armor_k  = klass.capital ? world_scale::k_enemy_armor_mult_capital
                                               : world_scale::k_enemy_armor_mult;
    const float enemy_shield_k = klass.capital ? world_scale::k_enemy_shield_mult_capital
                                               : world_scale::k_enemy_shield_mult;
    s.armor_fore_cm  = klass.armor_fore_cm      * enemy_armor_k;
    s.armor_aft_cm   = klass.armor_aft_cm       * enemy_armor_k;
    s.armor_port_cm  = klass.armor_port_cm      * enemy_armor_k;
    s.armor_starboard_cm = klass.armor_starboard_cm * enemy_armor_k;
    s.shield_mult   *= enemy_shield_k;
    // No armor package at spawn — armor is a purchasable upgrade only
    // (np armor). Every ship starts on its base hull cm; fitted armor is
    // attached later (the player buys it; NPCs never get any). When
    // present it stacks additively on the base hull (see heal_to_full).
    s.fitted_armor = nullptr;
    if (klass.default_shield) {
        s.shield_fore_cm = shield_max_cm(klass, klass.default_shield->front_cm, s.shield_mult);
        s.shield_aft_cm  = shield_max_cm(klass, klass.default_shield->back_cm,  s.shield_mult);
        s.shield_port_cm     = shield_max_cm(klass, klass.default_shield->port_cm,     s.shield_mult);
        s.shield_starboard_cm = shield_max_cm(klass, klass.default_shield->starboard_cm, s.shield_mult);
    }
    s.energy_gj = klass.energy_max;

    // Mount loadout: copy the class default. gun_cooldowns sized to
    // match, all starting at 0 (fully ready to fire). Per-instance
    // copy lets the player upgrade individual ships without mutating
    // the shared ShipClass.
    s.mounts        = klass.default_guns;
    s.gun_cooldowns.assign(s.mounts.size(), 0.0f);
    // gun_armed starts ALL TRUE so ships fire normally out of spawn; the
    // player loadout then selects ALL explicitly (firing::arm_all_guns).
    s.gun_armed.assign(s.mounts.size(), true);

    // Controller idle until a behavior fills it in.
    s.controller.desired_forward = HMM_V3(0.0f, 0.0f, 1.0f);
    s.controller.desired_speed   = 0.0f;
    return s;
}

void ship::make_ace(Ship& s) {
    if (!s.klass) return;
    // ---- 1) hot-rod the guns ------------------------------------------
    // Swap every FIXED forward gun to a high-tier weapon and stamp it with
    // legendary-grade mods (20% faster, 20% cheaper to fire — the
    // inventory.h Legendary tier). Turrets keep their type; they're already
    // a free-firing threat. Mounts are cycled through the elite pool so a
    // multi-gun ace fields a varied, nasty loadout.
    static const GunType kAceGuns[] = {
        GunType::TachyonCannon, GunType::PlasmaGun,
        GunType::IonicPulseCannon, GunType::ParticleCannon,
    };
    constexpr size_t kAceGunCount = sizeof(kAceGuns) / sizeof(kAceGuns[0]);
    s.mount_mods.assign(s.mounts.size(), inventory::WeaponMods{});
    size_t fixed_idx = 0;
    for (size_t i = 0; i < s.mounts.size(); ++i) {
        if (s.mounts[i].is_turret) continue;
        s.mounts[i].type = kAceGuns[fixed_idx % kAceGunCount];
        s.mount_mods[i].fire_rate_mult = 1.2f;   // Legendary tier
        s.mount_mods[i].energy_mult    = 0.8f;
        ++fixed_idx;
    }
    // ---- 2) bigger shields --------------------------------------------
    // Bump shield_mult and refresh the live facings so the dynamically
    // recomputed per-facing max (regen_shields / take_damage) stays in
    // sync. Aces are fighters, so default_shield is set and capital==false.
    constexpr float k_ace_shield_mult = 1.6f;
    s.shield_mult *= k_ace_shield_mult;
    if (s.klass->default_shield) {
        const auto& sh = *s.klass->default_shield;
        s.shield_fore_cm      = shield_max_cm(*s.klass, sh.front_cm,     s.shield_mult);
        s.shield_aft_cm       = shield_max_cm(*s.klass, sh.back_cm,      s.shield_mult);
        s.shield_port_cm      = shield_max_cm(*s.klass, sh.port_cm,      s.shield_mult);
        s.shield_starboard_cm = shield_max_cm(*s.klass, sh.starboard_cm, s.shield_mult);
    }
}

void ship::heal_to_full(Ship& s) {
    // Full reset of the durability state. Mirrors the health-from-class
    // init in spawn(); kept here so the player-spawn block and respawn
    // share one definition (see header). Clears the regen pauses too —
    // a fresh hull shouldn't inherit a suppressed-shield timer from the
    // wreck it's replacing.
    s.alive             = true;
    s.shield_pause_fore = 0.0f;
    s.shield_pause_aft  = 0.0f;
    s.shield_pause_port = 0.0f;
    s.shield_pause_starboard = 0.0f;
    // Fresh components too (#141). The installed mask is loadout-owned and
    // survives; callers that must NOT fix components (repair_hull) save and
    // restore s.systems around this call.
    ship_systems::repair_all(s.systems);
    if (!s.klass) return;   // class-less player: health doesn't apply yet
    const ShipClass& k = *s.klass;
    s.armor_fore_cm = k.armor_fore_cm;
    s.armor_aft_cm  = k.armor_aft_cm;
    s.armor_port_cm     = k.armor_port_cm;
    s.armor_starboard_cm = k.armor_starboard_cm;
    // Stack the per-instance fitted armor (if any) on top of base hull cm.
    // null = no package bought = base hull only.
    if (const ArmorType* fitted_armor = s.fitted_armor) {
        s.armor_fore_cm += fitted_armor->front_cm;
        s.armor_aft_cm  += fitted_armor->back_cm;
        s.armor_port_cm     += fitted_armor->port_cm;
        s.armor_starboard_cm += fitted_armor->starboard_cm;
    }
    s.shield_fore_cm = k.default_shield ? shield_max_cm(k, k.default_shield->front_cm, s.shield_mult) : 0.0f;
    s.shield_aft_cm  = k.default_shield ? shield_max_cm(k, k.default_shield->back_cm,  s.shield_mult) : 0.0f;
    s.shield_port_cm     = k.default_shield ? shield_max_cm(k, k.default_shield->port_cm,     s.shield_mult) : 0.0f;
    s.shield_starboard_cm = k.default_shield ? shield_max_cm(k, k.default_shield->starboard_cm, s.shield_mult) : 0.0f;
    s.energy_gj      = k.energy_max;
}

void ship::tick(Ship& s, float dt) {
    if (!s.alive)     return;
    if (s.is_player)  return;   // player flies via camera input, not the controller
    switch (s.behavior.kind) {
    case ShipBehavior::None:
        // Controller is idle — leave sprite kinematics alone. This is
        // the path the existing demo runs on (JSON-set angular_velocity
        // + forward_speed, integrator advances them). Adding the Ship
        // struct to a system MUST stay backwards-compatible with this
        // case or every existing scene starts behaving differently.
        return;

    case ShipBehavior::PursueTarget:
        behavior_pursue_target(s);
        flight_controller_step(s, dt);
        return;

    case ShipBehavior::ChaseTarget: {
        // "Fly toward this point at full cruise, never slow." The dogfight
        // counterpart to PursueTarget: PursueTarget is for *arriving at*
        // a waypoint and stopping; ChaseTarget is for *attacking through*
        // a target — we want overshoot, not docking, so the kinematic-
        // arrival ramp would actively hurt (it'd drop speed below the
        // fleer's cruise and produce the orbital tail-chase pattern).
        //
        // Honors the controller.afterburner flag: AI's BreakOff state
        // sets it true so extensions plow out fast; Engage / Flee leave
        // it false for cruise-speed pursuit / weave. afterburner_speed=0
        // means "no afterburner fitted" (the JSON default for ships
        // missing one); fall back to cruise in that case.
        if (!s.sprite || !s.klass) return;
        const HMM_Vec3 to = HMM_SubV3(s.behavior.target_pos, s.sprite->position);
        const float d2 = HMM_DotV3(to, to);
        if (d2 > 1e-3f) {
            s.controller.desired_forward = HMM_DivV3F(to, std::sqrt(d2));
        }
        const float ab = s.klass->afterburner_speed;
        const float base =
            (s.controller.afterburner && ab > 0.0f) ? ab : s.klass->cruise_speed;
        s.controller.desired_speed = base * s.controller.speed_scale;
        flight_controller_step(s, dt);
        return;
    }
    }
}

// ----------------------------------------------------------------------------
// Damage pipeline — see ship.h for declarations.
// ----------------------------------------------------------------------------

namespace {

// How long shield regen pauses on the hit facing after taking damage.
// Privateer-canonical "shields can't recover under sustained fire" feel —
// continuous fire keeps the timer pegged so shields stay down until the
// shooter lays off. 3 seconds is the original game's number; tune
// per-class later (capships might pause longer, ace shields shorter).
constexpr float k_shield_pause_after_hit = 3.0f;

// Map a HitFacing to the right (shield, armor, pause) triplet on a Ship.
// References-by-pointer because there's no clean way to return references
// to a varying-trio in C++ without a helper struct; the pointers are
// always non-null after the switch.
struct HitTarget { float* shield; float* armor; float* pause;
                   float shield_max; };
HitTarget hit_target(Ship& s, HitFacing f) {
    const float sh_max_fore = s.klass && s.klass->default_shield
                            ? shield_max_cm(*s.klass, s.klass->default_shield->front_cm, s.shield_mult) : 0.0f;
    const float sh_max_aft  = s.klass && s.klass->default_shield
                            ? shield_max_cm(*s.klass, s.klass->default_shield->back_cm,  s.shield_mult) : 0.0f;
    const float sh_max_port = s.klass && s.klass->default_shield
                            ? shield_max_cm(*s.klass, s.klass->default_shield->port_cm,     s.shield_mult) : 0.0f;
    const float sh_max_stbd = s.klass && s.klass->default_shield
                            ? shield_max_cm(*s.klass, s.klass->default_shield->starboard_cm, s.shield_mult) : 0.0f;
    switch (f) {
        case HitFacing::Fore: return { &s.shield_fore_cm, &s.armor_fore_cm,
                                       &s.shield_pause_fore, sh_max_fore };
        case HitFacing::Aft:  return { &s.shield_aft_cm,  &s.armor_aft_cm,
                                       &s.shield_pause_aft,  sh_max_aft  };
        case HitFacing::Port: return { &s.shield_port_cm, &s.armor_port_cm,
                                       &s.shield_pause_port, sh_max_port };
        case HitFacing::Starboard: return { &s.shield_starboard_cm, &s.armor_starboard_cm,
                                            &s.shield_pause_starboard, sh_max_stbd };
    }
    // Unreachable (enum exhausted), but the compiler wants a return.
    return { &s.shield_fore_cm, &s.armor_fore_cm, &s.shield_pause_fore, sh_max_fore };
}

// Component-hit roll (#141). Module-local fixed-seed generator, the same
// pattern loot.cpp / encounters.cpp use: reproducible feel, no shared state.
float component_roll() {
    static std::mt19937 rng{ 0x5C0FFEE1u };
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

const char* facing_name(HitFacing f) {
    switch (f) { case HitFacing::Fore: return "fore";
                 case HitFacing::Aft:  return "aft";
                 case HitFacing::Port: return "port";
                 case HitFacing::Starboard: return "starboard"; }
    return "?";
}

} // namespace

void ship::take_damage(Ship& s, float damage_cm, HitFacing facing,
                       GunType source_gun) {
    if (!s.alive || damage_cm <= 0.0f) return;
    // Damage immunity (campaign M21 drone / --dev-invuln): the ONE gate
    // every damage source funnels through — guns, missiles, collisions,
    // sun damage. The M23 weapon whitelist (immune_bypass_gun) pierces it
    // for exactly one gun type; sources that don't know their gun pass
    // GunType::Count and stay gated.
    if (s.damage_immune &&
        !(s.immune_bypass_gun != GunType::Count &&
          source_gun == s.immune_bypass_gun))
        return;
    HitTarget t = hit_target(s, facing);

    // Reset regen pause on the affected facing — sustained fire keeps
    // shields suppressed until the shooter lays off.
    *t.pause = k_shield_pause_after_hit;

    // Shield first (with implicit effect_pct = 100% for v1; the per-shield
    // effectiveness multiplier is loaded but not yet folded in — tracked as
    // np-3ca). NB: the np-9cu.3 brief called this the "engine multiplier" —
    // that's a different concern and is now wired (engine_level -> player
    // speed) in outfitting::effective_speed_caps; this remains a SHIELD TODO.
    if (*t.shield > 0.0f) {
        const float absorbed = std::min(*t.shield, damage_cm);
        *t.shield  -= absorbed;
        damage_cm  -= absorbed;
    }
    // Armor (spillover). Goes negative on a kill blow; we clamp at 0
    // for display but flag the kill.
    if (damage_cm > 0.0f) {
        *t.armor -= damage_cm;
        if (*t.armor <= 0.0f) {
            *t.armor    = 0.0f;
            s.alive     = false;
            // Hide the visual instantly. world_size = 0 makes the
            // billboard collapse; explosion FX is a future feature.
            if (s.sprite) s.sprite->world_size = 0.001f;
            std::printf("[ship] killed: id=%u (%s hit)\n",
                        s.id, facing_name(facing));
        } else {
            // It got through the armor and we're still flying: something
            // behind that plating just took the hit (#141).
            const SystemIntegrity before = s.systems.integrity;
            const ShipSystem hit = ship_systems::apply_hit(
                s.systems, facing, damage_cm, component_roll());
            if (hit != ShipSystem::Count) {
                const float left = ship_systems::integrity(s.systems, hit);
                // Latch for the component-damage audio cue (#519).
                s.pending_system_hit = std::max(s.pending_system_hit,
                    ship_systems::classify_hit(before[(int)hit], left));
                if (s.is_player) {
                    if (left > 0.0f)
                        std::printf("[damage] %s hit: %s at %.0f%%\n",
                                    facing_name(facing), ship_systems::label(hit),
                                    left * 100.0f);
                    else
                        std::printf("[damage] %s hit: %s DESTROYED\n",
                                    facing_name(facing), ship_systems::label(hit));
                }
            }
        }
    }
}

void ship::regen_shields(Ship& s, float dt) {
    if (!s.alive || !s.klass || !s.klass->default_shield) return;
    // PERCENTAGE-based regen (world_scale.h): each facing recovers a fixed
    // fraction of ITS OWN max per second, so every shield (small or huge)
    // refills from empty in the same wall-clock (~25 s at 4%/s) and the
    // rate auto-scales with shield_mult upgrades. The catalogue Regen field
    // is intentionally no longer consulted. Separate from velocity scale —
    // regen sets attrition pacing, not movement feel.
    auto tick_quad = [&](float& q, float& pause, float max_cm) {
        if (pause > 0.0f) {
            pause -= dt;
            if (pause < 0.0f) pause = 0.0f;
            return;          // suppressed this frame
        }
        if (q < max_cm) {
            // A damaged generator (#141) recharges proportionally slower;
            // a destroyed one not at all.
            const float regen_rate = max_cm * world_scale::k_shield_regen_frac_per_s
                                   * ship_systems::shield_regen_mult(s.systems);
            q = std::min(q + regen_rate * dt, max_cm);
        }
    };
    tick_quad(s.shield_fore_cm, s.shield_pause_fore, shield_max_cm(*s.klass, s.klass->default_shield->front_cm, s.shield_mult));
    tick_quad(s.shield_aft_cm,  s.shield_pause_aft,  shield_max_cm(*s.klass, s.klass->default_shield->back_cm,  s.shield_mult));
    tick_quad(s.shield_port_cm, s.shield_pause_port, shield_max_cm(*s.klass, s.klass->default_shield->port_cm,     s.shield_mult));
    tick_quad(s.shield_starboard_cm, s.shield_pause_starboard, shield_max_cm(*s.klass, s.klass->default_shield->starboard_cm, s.shield_mult));
}

float ship::hit_radius_m(const Ship& s) {
    // NPCs: half the rendered length feels right as a hit sphere — most
    // sprite ships are roughly as wide as they are tall, and capturing a
    // hit anywhere inside that sphere reads correctly with our bullet
    // sizes. Note: ship sprites are spawned with world_size already
    // multiplied by k_ship_size_scale (see main.cpp), so this read
    // automatically picks up the global ship-scale.
    //
    // Player without a visible ship: 30 m typical-fighter default,
    // also scaled by k_ship_size_scale so the player's collision
    // matches the same global knob NPCs respect.
    if (s.sprite)    return s.sprite->world_size * 0.5f;
    if (s.is_player) return 30.0f * world_scale::k_ship_size_scale;
    return 0.0f;
}

HitFacing ship::facing_of_hit(const Ship& s, const HMM_Vec3& hit_pos_world) {
    // Take hit position into body frame and look at which axis dominates.
    // Body +Z forward = Fore, -Z = Aft, +X = starboard, -X = port.
    // |Y| dominant -> port or starboard too (top/bottom hits are
    // sides in this game's accounting) — see the Y-fallback below.
    HMM_Vec3 to_hit = HMM_SubV3(hit_pos_world, s.position);
    const HMM_Mat4 R_inv = HMM_QToM4(HMM_InvQ(s.orientation));
    const HMM_Vec4 v = HMM_MulM4V4(R_inv, HMM_V4(to_hit.X, to_hit.Y, to_hit.Z, 0.0f));

    // Player uses camera convention (forward = -Z), so flip Z axis to
    // match ship convention before classifying the facing.
    float bz = s.is_player ? -v.Z : v.Z;
    const float bx = v.X, by = v.Y;

    // Threshold: pick fore/aft only if the Z-axis component is at least
    // half the magnitude of the lateral axes. Otherwise it's a side hit
    // and we pick port vs starboard by which way bx leans (X axis);
    // a Y-only hit (pure top/bottom) is bucketed to starboard as a
    // tie-breaker — both port and starboard here share the same
    // original "Side" pool, so the split is cosmetic for now.
    const float lateral = std::sqrt(bx * bx + by * by);
    if (std::fabs(bz) > lateral) {
        return (bz > 0.0f) ? HitFacing::Fore : HitFacing::Aft;
    }
    return bx >= 0.0f ? HitFacing::Starboard : HitFacing::Port;
}
