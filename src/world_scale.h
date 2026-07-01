#pragma once
// -----------------------------------------------------------------------------
// world_scale.h — global pacing knobs.
//
// A single constant tuned at the integrator layer so changing it slows
// the game uniformly across the camera, NPC ships, and projectiles —
// without touching AI tick rates, gun cooldowns, or shield regen
// (those use raw dt and stay snappy at the player-perception level).
//
// k_world_velocity_scale: multiplier on linear velocity at integration
// time. 1.0 = canonical; 0.5 halves how far everything moves per second
// while leaving turn rates, firing rhythm, and decision cadence at
// full real-time speed. Tune here once, every system picks it up.
//
// Why a header constant instead of a runtime knob: keeps the math
// inlinable by the optimizer and makes it impossible to forget to apply
// in any one integrator. Promote to runtime when the game settings UI
// wants a player-facing speed slider.
// -----------------------------------------------------------------------------

namespace world_scale {

// Linear-velocity scale — ships, projectiles, and the camera all
// move through space at this fraction of their canonical speed.
// Back to 1.0 (full speed) — the slower-pacing experiments helped
// validate the visual feedback layers (shield flashes, armor sparks,
// hit vignette), and at full speed the new effects still read
// clearly. Drop back to 0.5-0.6 if a future scenario wants the
// deliberate "time to aim" feel.
constexpr float k_world_velocity_scale = 1.0f;

// Shield-regen slowdown — separate from the velocity knob because
// regen plays a different role (combat attrition pacing, not motion
// feel). Model: PERCENTAGE of each facing's max per second (not a flat
// cm/s off the catalogue Regen). 0.04 = 4%/s, so ANY facing refills from
// empty in ~25 s regardless of shield size (SG1 10 cm or SG5 100 cm both
// take 25 s) and it auto-scales with dealer/upgrade shield_mult caps.
// Applies uniformly to player + NPC via the shared regen_shields() call
// site. (History: #106 used a flat-cm scale 0.0625 where bigger pools
// took proportionally longer; the %/s model fixes that asymmetry.)
constexpr float k_shield_regen_frac_per_s = 0.04f;

// ---- enemy survivability (combat balance) -----------------------------
// Combat was tuned around the old hard-to-aim controls; with the current
// snappy aiming + free turrets, enemies died instantly. Rather than nerf
// damage (which slows BOTH sides), we raise ENEMY effective HP so the
// player's kills take longer while enemies still hit back at full damage.
//
// Per facing:  shield_cm = base_shield x shield_mult x k_enemy_shield_mult
//              armor_cm  = base_armor                x k_enemy_armor_mult
//
// Applied NPC-only in ship::spawn (the player uses spawn_player +
// heal_to_full, which never touch these). Multiplicative (not flat +cm) so
// the light/heavy hull hierarchy is preserved. Shields are weighted heavier
// than armor because shields regen (k_shield_regen_frac_per_s) -> rewards
// sustained pressure, while keeping kills achievable once armor is exposed.
// Net effect ~2.1x enemy EHP (vs the 3x of the reverted 1/3-damage build).
constexpr float k_enemy_shield_mult = 2.5f;   // enemy shield facings x2.5
constexpr float k_enemy_armor_mult  = 1.5f;   // enemy armor  facings x1.5

// Capital hulls get their OWN (gentler) multipliers. They're already
// designed bullet-sponges via the flat +200cm/facing capital shield bonus
// (ship.cpp k_capital_shield_bonus_cm) plus heavy base armor, so stacking
// the full fighter dogfight buff on top made them tedious slogs. 1.0 = no
// extra buff -> capitals keep their canonical tankiness, which is still
// ~5x a fighter's EHP. Dial below 1.0 to actively soften them.
constexpr float k_enemy_shield_mult_capital = 1.0f;   // capitals: no shield buff
constexpr float k_enemy_armor_mult_capital  = 1.0f;   // capitals: no armor buff

// Ship-size multiplier — scales every ship's rendered length AND its
// collision hit-radius (which is derived from the rendered size).
// 1.4 makes ships 40% larger — easier to hit at the demo's combat
// distances without the AI's lead-prediction breaking down. Visual
// proportion vs the system (planets, mining bases) shifts only
// slightly because those are sized in tens of km already.
constexpr float k_ship_size_scale = 1.4f;

}
