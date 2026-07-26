#pragma once
// -----------------------------------------------------------------------------
// music.h — the dynamic MUSIC DIRECTOR (np-ida): Privateer's AdLib score,
// wired to game state from the user's ground-truth labels (docs/music_labels.json).
//
// Privateer's "feel" is half its FM music. The labels revealed the REAL
// system: COMBAT.ADL is not "the combat track" — it is the entire IN-FLIGHT
// music state machine. There's a calm MAIN flight theme that plays in open
// space, which escalates to a far-threat loop the moment hostiles appear on
// the scope, then to a near-threat loop as they close inside 5km, and resolves
// back to the main theme with a little outro when the sky clears. On top of
// that sit one-shot event stings (jump, landing approach, death) and, when
// you're docked, a per-base ambient tune (BASETUNE.ADL holds one per base
// archetype). This module recreates that, sitting on top of the generic audio
// mixer (audio.h) exactly like sfx.h sits on it for one-shots.
//
// ---- the label -> track mapping (the policy this module implements) ----
//   FLIGHT (combat-state machine, driven by nearest-hostile DISTANCE):
//     no hostiles            -> FlightMain    (combat_04, the main theme; loops)
//     hostiles, nearest >5km -> CombatFar     (combat_05)
//     hostiles, nearest<=5km -> CombatNear    (combat_06)
//     combat -> resolved     -> CombatResolve (combat_07, outro into main loop)
//   EVENT stings (one-shots over the loop / replacing it):
//     jump executed (Loading)-> StingJump      (combat_08)
//     ship destroyed (Dying) -> StingDeath     (combat_10)
//     [landing approach <700m-> StingLanding   (combat_09) — deferred, see np-ida]
//   LANDED (per base, by archetype):
//     agricultural (Helen)   -> BaseAgricultural (basetune_00)
//     mining (Achilles/Hect) -> BaseMining        (basetune_04)
//   MENU (title/menu)        -> Menu           (menu — custom bed, see assets/music/original/menu.wav)
//
// ---- where the audio comes from ----
// The tracks are the game's ORIGINAL OPL2/AdLib music (.ADL XMIDI + the
// game's own TIMBRES.AD FM bank), rendered offline to looping WAVs by
// tools/render_music.py (see docs/music_extraction.md + docs/music_director.md).
// Those WAVs are LOCAL-ONLY / gitignored (assets/music/original/<name>.wav)
// under the same legal model as the SFX — a clean clone simply has no music
// and every call here degrades to a silent no-op. There is deliberately NO
// committed placeholder music (unlike sfx): silence is a fine fallback.
//
// ---- the mixer relationship (separate bus, uncullable loops) ----
// The looping tracks each play on their OWN dedicated looping voice via
// audio::play_loop, so:
//   * never spatialized, never fight the 3D SFX for pan/attenuation;
//   * a LOOP, which np-3va's voice-stealer treats as un-cullable (same
//     protection the engine hum gets) — a furball can't steal the music;
//   * a separate master volume (set_master_volume) scales ONLY music — its
//     own gain "bus", independent of the SFX mix.
// The event STINGS are short one-shots played on the music bus (audio::play
// at the music gain). They overlay the loop (the death sting also fades the
// loop out); being short, np-3va's stealer never bothers them in practice
// (they fire in lulls — jump/landing/death/resolve, not mid-furball).
//
// ---- crossfade (loop layer) ----
// Switching LOOP tracks is a 2-voice ping-pong crossfade: the incoming loop
// starts on a fresh voice at gain 0 and lerps up while the outgoing voice
// lerps down and is then stopped. No clicks, no gap. At most two loop voices
// are ever live; a switch arriving mid-fade hard-stops the older outgoing one.
//
// ---- dynamic selection ----
// update() is called once per frame with the current GameMode + player
// position + the docked base id, and owns the entire state->track policy
// (documented at the impl), including the flight combat-tier state machine
// (with hysteresis so a hostile dipping across the 5km line / scope edge
// doesn't strobe the music), the resolve outro, the per-base landed tunes,
// and the mode-edge stings (jump on entering Loading, death on entering
// Dying). Call sites stay one-liners; all policy is HERE.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>

#include "game_state.h"   // GameMode

namespace music {

// The tracks we render + use. None = "fade everything out / silence".
// Names mirror the gitignored WAV stems in assets/music/original/ (see the
// label mapping in the header + k_track_file in the impl).
enum class Track {
    None = 0,
    // ---- flight combat-state machine (COMBAT.ADL sub-songs) ----
    FlightMain,       // combat_04 — main flight theme, no hostiles (loops)
    CombatFar,        // combat_05 — hostiles present, nearest > 5km
    CombatNear,       // combat_06 — hostiles present, nearest <= 5km
    CombatResolve,    // combat_07 — combat ended, outro that feeds the main loop
    // ---- event stings (one-shots) ----
    StingJump,        // combat_08 — jump executed
    StingLanding,     // combat_09 — entering automatic landing zone (<700m)
    StingDeath,       // combat_10 — game over, your ship is cooked
    // ---- landed per-base tunes (BASETUNE.ADL sub-songs) ----
    BaseAgricultural, // basetune_00 — agricultural base (Helen)
    BaseMining,       // basetune_04 — mining base (Achilles, Hector)
    BaseOxford,       // oxford_theme  — Oxford (academic hub); overrides the bar pool
    BaseBar,          // bar_music_01..14 — Freelancer bar pool (shuffled, non-looping)
    // ---- menu (BASETUNE.ADL sub-song 01) ----
    Menu,             // menu       — title/menu loop (custom bed, assets/music/original/menu.wav)
    Count
};

// Load every available track WAV from the gitignored assets/music/original/.
// Call once, AFTER audio::init (so the device rate is known for resampling).
// Missing files are normal (clean clone) — those tracks stay unavailable and
// any request for them is a silent no-op. Logs a one-line summary + the
// per-track bindings (flight_main <- combat_04, ...).
void load_all();

// Crossfade the LOOP layer to `t` (no-op if it's already the target, or if
// `t` is unavailable). Track::None fades the loop to silence. The actual fade
// is driven by update(); this just records the new target. (Event stings go
// through update()'s mode edges, not this.)
void play_track(Track t);

// Bar-pool music direction, two priority levels (issue #265):
//   request_bar_track — SOFT, game-state request (the Bar screen asks for
//       the present fixer's authored track every frame; idempotent).
//   pin_bar_track     — HARD, manual pin (DJ panel / POST /music); wins
//       over the soft request until released with 0.
// 1..14 = bar_music_NN, held tracks repeat on expiry; 0 = release that
// level (shuffle resumes only when both levels are 0). No-op when the
// files are missing (clean clone) or the player isn't in the Bar.
void request_bar_track(int idx);
void pin_bar_track(int idx);
int  bar_track_override();   // effective: pin > 0 ? pin : soft request
int  bar_track_pin();        // the manual pin only (DJ panel display)

// Fire the 'entering automatic landing zone' sting (combat_09) once
// (np-3dp.22). One-shot over whatever bed is playing; no-op if the track
// isn't available in this build. Called by the docking proximity check.
void landing_approach();

// Fade all music out (equivalent to play_track(Track::None)).
void stop();

// Per-frame driver. Owns the state->track policy AND advances the crossfade
// lerps. Call once per frame in EVERY mode (flight path + the non-Flight
// stub) so the music keeps playing/fading across mode transitions.
//   mode        — the current GameMode (post apply_pending).
//   player_pos  — the audio listener / ship position (camera position).
//   base_id     — player.last_docked_base (lowercase "achilles"/"helen"/...);
//                 only consulted in Landed mode to pick the base tune. "" ok.
//   dt          — frame seconds (for gain lerps + combat-tier hysteresis).
void update(GameMode mode, HMM_Vec3 player_pos, const char* base_id, float dt);

// Master music volume (its own "bus", separate from SFX). 0..1, clamped.
// Default 0.55 — a touch under the SFX so the score underscores the action.
void  set_master_volume(float v);
float master_volume();

// Mute toggle for the debug panel. Muting fades music to silence but keeps
// the current track + selection logic running, so unmuting fades it back in.
void set_muted(bool m);
bool muted();

// Introspection for the debug panel / logs: the LOOP track currently playing
// into (the crossfade TARGET), and whether any track is loaded at all.
Track current();
bool  any_loaded();
const char* to_name(Track t);

} // namespace music
