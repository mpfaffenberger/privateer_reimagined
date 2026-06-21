#include "sky_family.h"

#include <cstdint>

// Canonical skybox-colour family catalog. Each entry pairs a skybox
// "warmth" with a target tint and a list of compatible sun presets.
// Yellow appears in EVERY family's choice list so a yellow sun can pair
// with any skybox; non-yellow presets are listed in roughly descending
// order of how often we want them. The Yellow family is locked to the
// yellow preset (canonical Sol-like skybox).
const SkyFamilyConfig k_sky_families[kSkyFamilyCount] = {
    // Warm: red/orange nebula from a red/orange/yellow sun. Hot gas
    // glows red (H-alpha); a yellow sun's continuum + reddened dust
    // also reads warm. Sun choices: red/orange/yellow in ~equal mix.
    {
        /*warmth*/    +0.85f,
        /*target_a*/  { 1.00f, 0.55f, 0.20f },   // orange
        /*target_b*/  { 1.00f, 0.30f, 0.40f },   // rose
        /*n*/         3,
        /*opts*/     { "red", "orange", "yellow" },
    },
    // Yellow: yellow nebula. The Sol-canonical pairing; no other sun
    // colour makes physical sense (a yellow nebula around a blue star
    // is artistic, not astrophysical).
    {
        /*warmth*/    +0.45f,
        /*target_a*/  { 1.00f, 0.85f, 0.30f },   // yellow
        /*target_b*/  { 1.00f, 0.70f, 0.40f },   // amber
        /*n*/         1,
        /*opts*/     { "yellow" },
    },
    // Green: green/teal nebula. Real blackbodies don't peak green; sci-
    // fi gives the alien option. Yellow is the fallback so green suns
    // are roughly twice as likely as yellow-sun-with-green-nebula.
    {
        /*warmth*/    +0.15f,
        /*target_a*/  { 0.30f, 0.90f, 0.50f },   // green
        /*target_b*/  { 0.45f, 0.95f, 0.30f },   // lime
        /*n*/         2,
        /*opts*/     { "green", "yellow" },
    },
    // Blue: blue nebula from a hot O/B star (or a Sol-like yellow star
    // over a blue-reflection nebula). Yellow suns pick up a slight blue
    // dust tint; blue O/B stars pair with blue OIII emission.
    {
        /*warmth*/    -0.60f,
        /*target_a*/  { 0.30f, 0.55f, 1.00f },   // blue
        /*target_b*/  { 0.20f, 0.40f, 0.95f },   // cobalt
        /*n*/         2,
        /*opts*/     { "blue", "yellow" },
    },
    // Purple: purple nebula. Pulsar (purple) or extreme O/B (blue) or a
    // yellow sun over reddened + blued dust. Sun choices weighted to
    // yellow (~50%) since purple star systems are lore-rare; blue and
    // purple split the remainder.
    {
        /*warmth*/    -0.70f,
        /*target_a*/  { 0.55f, 0.30f, 1.00f },   // purple
        /*target_b*/  { 0.40f, 0.20f, 0.85f },   // violet
        /*n*/         3,
        /*opts*/     { "purple", "blue", "yellow" },
    },
};

SkyFamily sky_family_for_hash(uint64_t h) {
    // Mod 5 with a defensive cast so an unsigned wrap doesn't go
    // negative in the signed reduction.
    const int idx = (int)(h % (uint64_t)kSkyFamilyCount);
    return (SkyFamily)idx;
}

const char* sky_family_pick_sun(SkyFamily fam, uint64_t h) {
    const SkyFamilyConfig& cfg = k_sky_families[(int)fam];
    if (cfg.sun_choice_count <= 0) return "yellow";
    // Use bits 8..15 for the choice index so it doesn't correlate with
    // the family-index bits (0..2) used by sky_family_for_hash. Gives
    // roughly uniform distribution across the per-family choice list.
    const uint64_t k = (h >> 8) ^ (h >> 24);
    const int idx = (int)(k % (uint64_t)cfg.sun_choice_count);
    return cfg.sun_choices[idx];
}

float sky_family_warmth(SkyFamily fam) {
    return k_sky_families[(int)fam].warmth;
}

uint64_t sky_family_hash(const std::string& s) {
    // FNV-1a 64-bit, identical to skybox_gen's hash_seed so a given
    // skybox_seed produces the same family for a fresh process and for
    // a re-load.
    uint64_t h = 14695981039346656037ull;       // FNV-1a offset basis
    for (unsigned char c : s) {
        h ^= (uint64_t)c;
        h *= 1099511628211ull;                  // FNV-1a prime
    }
    return h ? h : 0x9E3779B97F4A7C15ull;      // skybox_gen convention
}
