#pragma once
// -----------------------------------------------------------------------------
// ship_systems.h — per-component ship damage (issue #141, GAP_ANALYSIS P1.3).
//
// Original Privateer damaged individual ship systems whenever a hit got
// through the armor, and made you pay per-system at the base repair desk.
// This is that model, reduced to its essentials:
//
//   * Seven systems, each with an INTEGRITY in [0, 1] (1 = pristine,
//     0 = destroyed) and an INSTALLED bit (a Tarsus without a jump drive
//     can't have its jump drive shot out).
//   * Every armor-penetrating hit damages exactly ONE installed system,
//     picked at random from the hit facing's candidate list (nose hits
//     wreck guns/radar, tail hits wreck engines/jump drive, flank hits wreck
//     launchers/shield gen). Integrity loss is proportional to the cm that
//     got through, so the model is frame-rate independent (sixty 0.1 cm sun
//     ticks cost the same as one 6 cm neutron bolt).
//   * Effects are pure functions of integrity (speed/turn/radar/regen
//     multipliers) or a destroyed/operational gate (guns, launchers, jump
//     drive, tractor). Callers apply them; this header never touches Ship.
//
// Header-only and dependency-free (like gun_modes.h / launcher_modes.h) so
// savegame, repair, the HUD and the pure harness (tools/test_ship_systems)
// can share it without dragging extra TUs into every link line. The caller
// owns the randomness: ship::take_damage rolls, this code only maps a roll.
// -----------------------------------------------------------------------------

#include "hit_facing.h"

#include <algorithm>
#include <array>
#include <cstdint>

enum class ShipSystem : uint8_t {
    Guns = 0,
    Launchers,
    Engines,     // engines + maneuvering thrusters: speed AND turn rate
    ShieldGen,
    Radar,
    JumpDrive,
    Tractor,
    Count
};

inline constexpr int kShipSystemCount = (int)ShipSystem::Count;

using SystemIntegrity = std::array<float, kShipSystemCount>;

namespace ship_systems {
inline constexpr SystemIntegrity k_pristine = { 1, 1, 1, 1, 1, 1, 1 };
} // namespace ship_systems

struct ShipSystems {
    SystemIntegrity                      integrity = ship_systems::k_pristine;
    std::array<bool, kShipSystemCount>   installed = { true, true, true, true,
                                                       true, true, true };
};
static_assert(kShipSystemCount == 7, "update the ShipSystems initialisers");

namespace ship_systems {

// ---- tuning knobs ------------------------------------------------------------
// Integrity lost per cm of armor-penetrating damage. A 1.8 cm laser bolt that
// gets through knocks ~14% off one system; a missile that punches through
// usually destroys one outright.
inline constexpr float k_integrity_loss_per_cm = 0.08f;
// Floor multipliers at 0 integrity: dead engines still limp home, a dead
// radar still sees a knife-fight.
inline constexpr float k_min_speed_mult = 0.40f;
inline constexpr float k_min_turn_mult  = 0.50f;
inline constexpr float k_min_radar_mult = 0.25f;

// ---- names -------------------------------------------------------------------
// HUD / repair-desk label.
inline const char* label(ShipSystem s) {
    switch (s) {
        case ShipSystem::Guns:      return "GUNS";
        case ShipSystem::Launchers: return "LAUNCHERS";
        case ShipSystem::Engines:   return "ENGINES";
        case ShipSystem::ShieldGen: return "SHIELD GEN";
        case ShipSystem::Radar:     return "RADAR";
        case ShipSystem::JumpDrive: return "JUMP DRIVE";
        case ShipSystem::Tractor:   return "TRACTOR";
        case ShipSystem::Count:     break;
    }
    return "?";
}

// Stable on-disk key. NEVER rename one: savegame reads by these strings so an
// enum reorder can't scramble old saves (same discipline as faction names).
inline const char* key(ShipSystem s) {
    switch (s) {
        case ShipSystem::Guns:      return "guns";
        case ShipSystem::Launchers: return "launchers";
        case ShipSystem::Engines:   return "engines";
        case ShipSystem::ShieldGen: return "shield_gen";
        case ShipSystem::Radar:     return "radar";
        case ShipSystem::JumpDrive: return "jump_drive";
        case ShipSystem::Tractor:   return "tractor";
        case ShipSystem::Count:     break;
    }
    return "?";
}

inline constexpr ShipSystem at(int i) { return (ShipSystem)i; }

// ---- state queries -----------------------------------------------------------
inline float integrity(const ShipSystems& ss, ShipSystem s) {
    return ss.integrity[(int)s];
}
inline bool installed(const ShipSystems& ss, ShipSystem s) {
    return ss.installed[(int)s];
}
// An uninstalled system is not "operational" — but callers that gate on
// ownership (jump drive, launchers) already check that separately; this
// answers "is the fitted hardware still working?".
inline bool operational(const ShipSystems& ss, ShipSystem s) {
    return ss.integrity[(int)s] > 0.0f;
}
inline bool damaged(const ShipSystems& ss, ShipSystem s) {
    return installed(ss, s) && ss.integrity[(int)s] < 1.0f;
}
inline bool any_damaged(const ShipSystems& ss) {
    for (int i = 0; i < kShipSystemCount; ++i)
        if (damaged(ss, at(i))) return true;
    return false;
}

// ---- mutation ----------------------------------------------------------------
// Fitting/removing hardware. Removing it also resets integrity: whatever you
// buy next comes out of the crate pristine.
inline void set_installed(ShipSystems& ss, ShipSystem s, bool on) {
    ss.installed[(int)s] = on;
    if (!on) ss.integrity[(int)s] = 1.0f;
}

inline void repair(ShipSystems& ss, ShipSystem s) { ss.integrity[(int)s] = 1.0f; }
inline void repair_all(ShipSystems& ss)            { ss.integrity = k_pristine; }

// Load a persisted integrity snapshot, clamping junk into [0, 1].
inline void restore(ShipSystems& ss, const SystemIntegrity& snap) {
    for (int i = 0; i < kShipSystemCount; ++i)
        ss.integrity[i] = std::clamp(snap[i], 0.0f, 1.0f);
}

// Which systems sit behind each facing's armor. Every system is reachable
// from at least one facing.
struct FacingCandidates { ShipSystem list[4]; int n; };
inline FacingCandidates candidates(HitFacing f) {
    switch (f) {
        case HitFacing::Fore:
            return { { ShipSystem::Guns, ShipSystem::Radar, ShipSystem::Tractor }, 3 };
        case HitFacing::Aft:
            return { { ShipSystem::Engines, ShipSystem::JumpDrive, ShipSystem::ShieldGen }, 3 };
        case HitFacing::Port:
        case HitFacing::Starboard:
            break;
    }
    return { { ShipSystem::Launchers, ShipSystem::ShieldGen,
               ShipSystem::Guns, ShipSystem::Engines }, 4 };
}

// Pick the system a penetrating hit lands on. roll01 in [0, 1). Only
// INSTALLED candidates are eligible; returns Count when none are (the hit
// is pure hull damage).
inline ShipSystem pick(const ShipSystems& ss, HitFacing f, float roll01) {
    const FacingCandidates c = candidates(f);
    ShipSystem eligible[4];
    int n = 0;
    for (int i = 0; i < c.n; ++i)
        if (installed(ss, c.list[i])) eligible[n++] = c.list[i];
    if (n == 0) return ShipSystem::Count;
    const int idx = std::clamp((int)(roll01 * (float)n), 0, n - 1);
    return eligible[idx];
}

// Apply one armor-penetrating hit of `penetrating_cm`. Returns the system
// that took the damage, or Count if nothing did (no eligible system / no
// damage). Deterministic given the roll — the caller supplies randomness.
inline ShipSystem apply_hit(ShipSystems& ss, HitFacing f,
                            float penetrating_cm, float roll01) {
    if (penetrating_cm <= 0.0f) return ShipSystem::Count;
    const ShipSystem s = pick(ss, f, roll01);
    if (s == ShipSystem::Count) return s;
    float& v = ss.integrity[(int)s];
    v -= penetrating_cm * k_integrity_loss_per_cm;
    // Snap float dust to a clean zero so "destroyed" is exact.
    if (v < 1e-4f) v = 0.0f;
    return s;
}

// ---- gameplay effects --------------------------------------------------------
inline float lerp_floor(float floor, float i) { return floor + (1.0f - floor) * i; }

// Top-speed multiplier from engine damage.
inline float speed_mult(const ShipSystems& ss) {
    return lerp_floor(k_min_speed_mult, integrity(ss, ShipSystem::Engines));
}
// Turn-rate multiplier from engine (maneuvering) damage.
inline float turn_mult(const ShipSystems& ss) {
    return lerp_floor(k_min_turn_mult, integrity(ss, ShipSystem::Engines));
}
// Radar/sensor range multiplier.
inline float radar_mult(const ShipSystems& ss) {
    return lerp_floor(k_min_radar_mult, integrity(ss, ShipSystem::Radar));
}
// Shield regen multiplier: a destroyed generator doesn't recharge at all.
inline float shield_regen_mult(const ShipSystems& ss) {
    return integrity(ss, ShipSystem::ShieldGen);
}

} // namespace ship_systems
