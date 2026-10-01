#pragma once
// -----------------------------------------------------------------------------
// voice.h — voiced comm line playback over the audio mixer (np-ma3).
//
// A thin voice-playback layer that sits OVER the generic audio mixer
// (audio.h) and is driven by the comm system (comm.h). Same pattern as
// sfx.h and music.h: gameplay code stays one-liners
// (`voice::say(faction, Category::Hostile, pos, true)`), all policy
// lives HERE — which voice_id to play for a given (faction, category),
// whether a line is player-directed or world, and the one-voice-at-a-time
// contract below.
//
// Loads a voice-bank manifest (default: `assets/data/voice_bank.json`,
// built by tools/build_voice_bank.py) that maps (faction, category) ->
// a list of voice_ids for that line. Each voice_id resolves through the
// bank to a list of MP3 paths in `assets/voice/...` that audio::load
// pulls once at boot — so runtime playback is a mixer play() /
// play_world(), no I/O on the hot path.
//
// ----- playback path (the 2D-vs-3D policy this module owns) -----
// Player-directed lines (`to_player == true`, i.e. anyone hailing the
// player) play 2D via audio::play() — the "radio" feel: the player
// hears the line centered in their head, immune to the degenerate 3D
// case where the source IS the listener, and the gain is stable no
// matter where the speaker is relative to the cockpit.
//
// Ambient lines (`to_player == false`, NPC-to-NPC chatter in earshot)
// play 3D via audio::play_world() at `world_pos` against the listener
// pose — properly attenuated and panned so a Talon behind the player
// hails from behind, and a distant NPC fades with range.
//
// ----- one-voice-at-a-time (player-directed lines only) -----
// Player-directed hails are queued: when a new player-directed line is
// requested while a previous one is still playing, the new line waits
// behind it rather than stacking on top. NPC-to-NPC world voices are
// independent and never interrupted; they're cheap ambient flavor, not
// radio.
//
// Phase 0 keeps the contract small: load() + two say() overloads.
// The .cpp will land in a follow-up issue (the impl isn't done here;
// this is the API surface the comm layer wires against).
// -----------------------------------------------------------------------------

#include "faction.h"

#include <HandmadeMath.h>
#include <cstdint>
#include <string>

namespace voice {

// The voice category — the gameplay meaning of a line. Drives which
// pool of (faction, category) -> voice_ids we pick from, and lets the
// debug panel force-trigger a specific category for testing.
enum class Category : uint8_t {
    Greeting,   // friendly hail ("Confed: 'Nice shooting, civilian.'")
    Hostile,    // aggressive bark ("Pirate: 'You're flying Confed colors...'")
    LowHp,      // hull critical ("Militia: 'We're taking damage!'")
    Kill,       // kill confirmation ("Pirate: 'Target down.'")
    Demand,     // surrender / dock demand ("Confed: 'Heave to, civilian.'")
    Rumor,      // bar-patron/merchant rumor line
    Search,     // contraband search ("Militia: 'Stand by, we need a scan.'")
    Clear,      // clean scan / search passed ("Militia: 'You're clear, proceed.'")
};

// Whether faction `f` has a generic in-flight VOICE at all (barks, hail
// replies, kill reactions). Voiceless factions:
//   * Kilrathi (#640): their comm TEXT still shows in the feed, only the
//     audio is dropped.
//   * Steltek (#650): not a real faction — just the mute drone and the
//     scout, whose interrogation is AUTHORED in scripted encounters (a
//     separate path this predicate doesn't touch).
// Inline (no voice.cpp link dependency) so headless harnesses compiling
// comm.cpp can call it too. Civilian is "voiced" here — it simply has no
// bank of its own (see bank_faction).
inline bool speaks(Faction f) {
    return f != Faction::Kilrathi && f != Faction::Steltek;
}

// Single source of truth for faction -> voice/response-bank key. Hunter is
// the one outlier vs faction::to_name() ("bounty_hunter"). Returns nullptr
// for factions with no bank: Civilian (bases stay silent) and any faction
// that doesn't speak() — so every lookup keyed by it resolves to no line.
const char* bank_faction(Faction f);

// Load the voice-bank manifest from `path` (default
// `assets/data/voice_bank.json`). Idempotent — replaces any prior table.
// Missing/unparseable file is NON-fatal: say() then degrades to a silent
// no-op (one log line) so the rest of the game keeps running without
// voice. Mirrors the comm::load() policy on missing lines.
bool load(const std::string& path);

// Play the next voice line for (speaker, cat), at `world_pos`, addressed
// to the player when `to_player` is true (radio/2D path) or as ambient
// NPC flavor when false (positional/play_world). Selects a voice_id
// deterministically-ish from the bank (random within the pool) so
// repeated calls of the same (speaker, cat) don't always play the same
// clip. Falls back to a silent no-op if no (speaker, cat) entry exists
// in the bank.
void say(Faction speaker, Category cat, HMM_Vec3 world_pos, bool to_player);

// Overload for callers that already have a concrete voice_id (e.g. the
// scripted-encounter / scenario engine from Phase 2 — see the plan —
// which authors a specific voice_id for a given line). Same 2D / 3D
// rule via `to_player`. Used today by the debug panel's "force bark"
// button.
//
// Resolution order: alias (e.g. confed_f) -> by_voice_category[voice_id]
// [cat] (a category-appropriate line in THIS voice) -> by_voice[voice_id]
// (any line in this voice) -> silent no-op.
void say(const std::string& voice_id, Category cat,
         HMM_Vec3 world_pos, bool to_player);

// Resolve the STABLE voice_id a given entity speaks with. Maps `f` through
// bank_faction() and, if that faction has a non-empty `faction_voices`
// list, returns the `entity_id % size`-th id — so a given ship/base ALWAYS
// speaks with the same voice across its lifetime. Returns "" when the
// faction has no bank or no voices (caller falls back to the faction-level
// say(), which is then a silent no-op for a faction with no bank).
std::string voice_for(Faction f, uint32_t entity_id);

// Per-entity voiced line: speak `cat` in `entity_id`'s OWN stable voice
// (via voice_for). Falls back to the faction-level say() when the entity
// has no resolved voice. This is the entry point gameplay should use so
// each NPC barks consistently in its own voice.
void say_ship(Faction f, uint32_t entity_id, Category cat,
              HMM_Vec3 world_pos, bool to_player);

// Stop any voice line(s) currently playing FOR `entity_id` and forget them
// (issue #105). Call this the instant a ship is destroyed (and on player
// death) so a dead ship can't keep yelling over the radio to its natural
// clip end. Tolerates an unknown entity_id (no-op) and stale voice handles
// (audio::stop ignores finished/stolen ids). Only voices spawned via
// say_ship() are tracked per-entity; faction-/voice-level say() calls with
// no entity context are untracked (UI, scenarios) and unaffected.
void stop_for(uint32_t entity_id);

} // namespace voice
