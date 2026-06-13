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
// non-Flight modes. Cruise engage rising edge fires the windup one-shot.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>

namespace sfx {

// Load every gameplay sample + start the (silent) engine-hum loop.
// Call once, AFTER audio::init. Logs a summary line; missing files log
// individually and their events degrade to silent no-ops.
void load_all();

// ---- combat -----------------------------------------------------------------
void gun_fired(HMM_Vec3 world_pos, bool is_player);
void impact(HMM_Vec3 world_pos, bool shield);      // shield=true: absorbed thunk
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
// Fires cruise_windup automatically on the cruise rising edge.
void update_engine_hum(float speed_frac, float cruise_level,
                       bool flight_mode, float dt);

// ---- UI ----------------------------------------------------------------------
void ui_click();

// ---- jump (np-6al.3) ---------------------------------------------------------
// Hyperspace sting played when a jump engages. Reuses the cruise-windup
// sample (a rising engine swell) at full gain — close enough to a "jump
// whoosh" that authoring a bespoke sample wasn't worth a new asset; swap
// the file in load_all() if a dedicated jump sound ever lands. 2D: it's
// the player's own drive spooling into hyperspace, not a world event.
void jump();

} // namespace sfx
