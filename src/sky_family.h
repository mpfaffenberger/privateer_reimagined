#pragma once
// -----------------------------------------------------------------------------
// sky_family.h — canonical skybox-colour family + sun-preset pairing.
//
// The mapping table (k_sky_families) ties a skybox's nebula tint to a
// compatible sun preset. The goal: when the player visits a system with a
// purple nebula, the sun MUST be purple / blue / yellow (never red or
// green); when the nebula is green, the sun MUST be green or yellow; etc.
// Yellow is the universal fallback — allowed for any skybox — but never
// dominant, so the rotation still has variety.
//
// 5 families cover the catalog (warm/yellow/green/blue/purple). The
// engine picks one based on a hash of the skybox seed, then picks a sun
// preset from the family's allowed list. Both are deterministic per
// seed so the same skybox seed always produces the same look.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"
#include <cstdint>
#include <string>

enum class SkyFamily {
    Warm,    // red/orange nebula; sun = red/orange/yellow
    Yellow,  // yellow nebula; sun = yellow only (canonical Sol)
    Green,   // green/teal nebula; sun = green or yellow
    Blue,    // blue nebula; sun = blue or yellow
    Purple,  // purple nebula; sun = purple/blue/yellow
};
constexpr int kSkyFamilyCount = 5;

struct SkyFamilyConfig {
    float        warmth;           // nebula tint bias [-1, +1]
    HMM_Vec3     target_a;         // primary nebula tint target
    HMM_Vec3     target_b;         // alternate tint target (for variety)
    int          sun_choice_count; // 1..3 sun preset names follow
    const char*  sun_choices[3];   // sun preset names
};

extern const SkyFamilyConfig k_sky_families[kSkyFamilyCount];

// Pick a SkyFamily from a 64-bit hash (e.g. FNV-1a of the skybox seed).
// Deterministic: same hash -> same family. Distributes ~equally across
// the 5 families.
SkyFamily sky_family_for_hash(uint64_t h);

// Pick a sun preset name from a SkyFamily using a 64-bit hash.
// Uses different bits than sky_family_for_hash so the pick is independent.
const char* sky_family_pick_sun(SkyFamily fam, uint64_t h);

// Convenience for callers that need the warmth alone.
float sky_family_warmth(SkyFamily fam);

// Map a family name ("warm"/"yellow"/"green"/"blue"/"purple", case-
// insensitive) to a SkyFamily. Returns false if unknown so the caller can
// keep its seed-derived default. Used for per-system art-direction overrides.
bool sky_family_from_name(const std::string& name, SkyFamily& out);

// FNV-1a string hash. Same algorithm skybox_gen uses for its seed,
// so re-hashing the same skybox_seed gives the same value here.
uint64_t sky_family_hash(const std::string& s);
