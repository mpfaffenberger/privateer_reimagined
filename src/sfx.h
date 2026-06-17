#pragma once
// -----------------------------------------------------------------------------
// sfx.h — gameplay events -> sounds, one call per event.
//
// The thin façade between gameplay code and the generic audio mixer
// (audio.h). Call sites stay one-liners — `sfx::gun_fired(pos, false)`
// — and every policy decision lives HERE in one place: which sample an
// event maps to, 2D vs 3D, gains, attenuation radii, rate limits. The
// mixer below stays ignorant of Privateer; the gameplay code above
// stays ignorant of sound design. When real produced SFX replace the
// procedural stand-ins, only load_all()'s file list changes.
//
// Player vs NPC gunfire: the player's own shots play 2D at modest gain
// — Privateer-style "my guns are always audible", immune to the
// degenerate 3D case where the source IS the listener. NPC shots are
// positional so a Talon opening up behind you is heard behind you.
// The SAMPLE also differs: the user's F7 labels expose a loud clip and a
// quieter twin per gun (sfx_00..08 / sfx_09..17). Player guns play the
// loud clip, NPC guns the quiet twin — authentic, and it keeps the
// player's own guns front-and-center over a furball's distant chatter.
//
// Impact rate limit: a global ~8 sounds/sec gate (token timestamps in
// sfx.cpp). Sustained multi-mount beam-spam otherwise stacks dozens of
// near-identical thunks per second — clipping the mixer's headroom for
// zero added information. Suppression is logged (throttled) so the
// gate is observable, not mysterious.
//
// Engine hum: a looping voice started at gain 0 by load_all and owned
// by this module. main.cpp calls update_engine_hum once per frame with
// throttle inputs; the mapping (documented at the impl) lerps gain
// toward the target to avoid zipper artifacts and force-zeroes in
// non-Flight modes. The idle bed is deliberately quiet + smooth (np-4dr
// de-buzzed it).
//
// Afterburner (np-4dr): the cruise/afterburner sound (sfx_22) is a HELD
// LOOP, not a one-shot. update_engine_hum manages its lifecycle off the
// same cruise_level it already gets: the cruise rising edge starts a
// dedicated looping voice (after a brief windup stab), held TAB sustains
// it (gain lerped to a roar), and the falling edge — or any Flight-mode
// exit — stops it. Like the hum it's a protected (uncullable) loop, and
// it layers OVER the quiet idle bed.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>
#include <cstdint>

// Forward-declared so the facade stays lean (no need to drag gun.h's
// std::string/string_view into every sfx.h includer). Must match the
// definition in gun.h exactly - enum class GunType : uint8_t.
enum class GunType : uint8_t;

namespace sfx {

// Load every gameplay sample + start the (silent) engine-hum loop.
// Call once, AFTER audio::init. Logs a summary line; missing files log
// individually and their events degrade to silent no-ops.
void load_all();

// ---- combat -----------------------------------------------------------------
// Per-gun firing sound: `type` selects the sample (each GunType binds to
// its own original Privateer SFX in load_all, falling back to the generic
// laser_fire when no per-gun sample is present). is_player picks the loud
// (player) vs quiet (NPC) twin AND the playback path: player shots play 2D
// (always audible); NPC shots are positional + coalesced (see impl).
void gun_fired(GunType type, HMM_Vec3 world_pos, bool is_player);
// Impact thunk. shield=true: absorbed (soft) thunk; false: armor crack.
// victim_is_player selects the clip variant — the user's labels give
// distinct player-vs-NPC damage sounds (armor: sfx_23/sfx_24,
// shield: sfx_25/sfx_26). The damage pass knows who took the hit.
void impact(HMM_Vec3 world_pos, bool shield, bool victim_is_player);
void ship_exploded(HMM_Vec3 world_pos, bool big);  // big: cargo/capital hulls

// ---- missiles + lock (np-zte.2) ---------------------------------------------
// missile_fired: 2D launch whoosh (the player's own rack). out_of_ammo: a
// dry click when the trigger's pulled on an empty rack / with no lock.
void missile_fired();
void out_of_ammo();
// Target-lock tones, played while the player holds a lock-requiring missile
// selected with a target in the reticle. lock_seeking is a slow beep the
// caller re-triggers on a cadence WHILE acquiring (IR build-up); lock_acquired
// is the solid one-shot fired once on the seeking->locked transition. Both 2D
// (cockpit avionics, not world events).
void lock_seeking();
void lock_acquired();

// ---- flight -----------------------------------------------------------------
// Per-frame hum control. `speed_frac` = |player velocity| / max speed
// (0..1-ish, clamped); `cruise_level` = camera.cruise_level (0..1).
// `flight_mode` false (Landed/Dying/Loading) ramps the hum to silence.
// Also owns the afterburner held-loop lifecycle (start on cruise rising
// edge, stop on falling edge / mode exit) — see the header note above.
void update_engine_hum(float speed_frac, float cruise_level,
                       bool flight_mode, float dt);

// ---- UI ----------------------------------------------------------------------
void ui_click();

// ---- jump (np-6al.3) ---------------------------------------------------------
// Hyperspace sting played when a jump engages. The user's F7 labels show
// the real jump is TWO clips in sequence: sfx_41 then sfx_42 ("plays
// immediately followed by 42 when you press j inside a jump gate"). We
// play sfx_41 (jump) immediately and queue sfx_42 (jump2) to follow.
// Distinct from the cruise-windup sample now (they were wrongly shared).
// 2D: it's the player's own drive spooling into hyperspace.
void jump();

} // namespace sfx
