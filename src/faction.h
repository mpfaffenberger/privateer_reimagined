#pragma once
// -----------------------------------------------------------------------------
// faction.h — who hates whom.
//
// Privateer's universe has eight factions (plus the appended Steltek — the
// campaign's drone, #146). Most NPC behaviour is gated on
// "is this other ship hostile to me?" which is a 2-axis lookup:
//
//   * NPC vs NPC: a static 8x8 stance matrix. A Pirate sees a Confed and
//     attacks; a Merchant sees another Merchant and ignores it.
//   * NPC vs PLAYER: the matrix doesn't apply because the player has no
//     faction. Instead, every faction tracks a *reputation* score with the
//     player (-100..+100), combined with that faction's *baseline* feeling
//     about strangers. Stance is the thresholded sum of the two.
//
// This split matches the original game: you can't BECOME Confed by killing
// pirates, but Confed can come to like (or hate) you individually.
//
// Stance is symmetric for v1. Privateer was actually asymmetric in places
// (Retros hated everyone but not everyone hated Retros equally) — fine to
// add when we need it. The matrix is mutable at runtime so future event
// scripts ("you destroyed the Steltek artefact, all Retros now Hostile to
// you") can poke it without recompile.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class Faction : uint8_t {
    Civilian = 0,
    Merchant,
    Confed,
    Militia,
    Hunter,
    Pirate,
    Retro,
    Kilrathi,
    Steltek,    // (#146) the drone + its makers — hostile to everything,
                // -100 baseline, no rep path. Appended (never reorder);
                // rep/kills serialize by NAME so old saves are safe.
    Count
};
constexpr int kFactionCount = (int)Faction::Count;

enum class Stance : uint8_t { Allied, Neutral, Hostile };

// 8x8 symmetric matrix, populated by faction::init(). Indexed
// [faction_a][faction_b]. Mutable by design — runtime events may flip
// individual cells (e.g. mission scripts).
extern Stance g_faction_stance[kFactionCount][kFactionCount];

// Per-player rep score with each faction. Range is [-100, +100]; defaults
// to 0 on a fresh save. Combined with `g_faction_baseline_to_player[]` to
// derive the runtime stance.
struct PlayerReputation {
    int8_t rep[kFactionCount] = {};   // zeroed
};

// Faction-specific opinion of a stranger. Confed treat newcomers neutrally
// (0), Pirates dislike them (-30), Kilrathi treat any non-Kilrathi as prey
// (-100, no rep can save you).
extern int8_t g_faction_baseline_to_player[kFactionCount];

// ---------------------------------------------------------------------------
// Reputation consequences of player kills (np-ma2.1)
// ---------------------------------------------------------------------------
// Global scope (like Stance / PlayerReputation above) — only the functions
// live in namespace faction.
//
// How one faction's opinion of the player moved as a result of a single
// player kill. Returned by faction::apply_player_kill so the comm/HUD layer
// can surface the change (rep line + taunt) and log stance-threshold flips
// without re-deriving who-cares-about-whom.
enum class KillReaction : uint8_t {
    None,     // rep moved but the faction stays quiet (e.g. outlaw shrugging off a dead comrade)
    Praise,   // player did this faction a favour (rep up) — killed their enemy
    Anger,    // player wronged this faction (rep down) — killed them or their ally
};

struct RepKillEffect {
    Faction      faction       = Faction::Civilian;
    int8_t       before        = 0;        // rep before the kill
    int8_t       after         = 0;        // rep after (clamped to [-100,100])
    Stance       stance_before = Stance::Neutral;
    Stance       stance_after  = Stance::Neutral;
    KillReaction reaction      = KillReaction::None;
};

namespace faction {

// Populate the static tables (stance matrix + player baselines). Idempotent.
// Logs one line: `[faction] 8 factions, X hostile pairs, Y allied pairs`.
void init();

// String <-> enum. Names are lowercase canonical: "civilian", "merchant",
// "confed", "militia", "hunter", "pirate", "retro", "kilrathi". Returns
// Faction::Count on unknown input (call sites should treat that as error).
Faction     from_name(std::string_view s);
const char* to_name(Faction f);

// The two queries the AI layer cares about. NPC-vs-NPC consults the static
// matrix; NPC-vs-PLAYER folds rep + baseline into thresholds.
//   eff = clamp(baseline + rep, -100, +100)
//   eff <= -25  -> Hostile
//   eff >= +25  -> Allied
//   else        -> Neutral
Stance stance_npc_vs_npc(Faction a, Faction b);
Stance stance_npc_vs_player(Faction npc, const PlayerReputation& rep);

// ---- campaign player-stance override (#114) --------------------------------
// When armed for a faction, stance_npc_vs_player returns the override
// instead of the baseline+rep math ("Pentonville pirates treat you as
// neutral while you work for Tayla"). Scoping (which flags, which system)
// is the CAMPAIGN layer's job — it re-derives and arms/clears these per
// frame from plot state. Transient: never serialized. Per-ship grudges
// (Ship::provoked_by_player, AIShipState::aggro_player) still win — the
// override only replaces the faction-level rep verdict.
void set_player_stance_override(Faction f, Stance s);
void clear_player_stance_override(Faction f);
bool player_stance_override_active(Faction f);

// Apply the reputation fallout of the player destroying a `victim`-faction
// ship. Mutates `rep` in place (clamped) and returns the per-faction
// effects (only factions whose rep actually moved are listed).
//
// Who-likes-whom is DERIVED, not hardcoded:
//   * The victim's ALLIES (g_faction_stance == Allied) resent the kill.
//   * The victim's ENEMIES (g_faction_stance == Hostile) approve of it.
//   * Whether the victim is an OUTLAW is read from
//     g_faction_baseline_to_player: a negative baseline (Pirate/Retro/
//     Kilrathi) marks a faction the lawful universe already distrusts —
//     killing them is policing (small swings). A non-negative baseline
//     (Civilian/Merchant/Confed/Militia/Hunter) marks the lawful, and
//     killing them unprovoked is a CRIME (large swings against the
//     victim + its allies). No hardcoded faction pairs.
//
// Witness model (v1): GLOBAL. The whole faction "hears about" the kill
// regardless of who was in sensor range — simple, deterministic, and
// true to the original game where rep is a single galaxy-wide number per
// faction. A perception-gated variant (only factions with a witness in
// radar range react, with smaller deltas for hearsay) is future work;
// see the np-ma2.1 close note.
std::vector<RepKillEffect> apply_player_kill(PlayerReputation& rep, Faction victim);

} // namespace faction
