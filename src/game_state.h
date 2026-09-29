#pragma once
// -----------------------------------------------------------------------------
// game_state.h — top-level game-mode state machine.
//
// The game is always in exactly ONE of four modes:
//
//   Flight  — the normal sim: camera physics, AI, projectiles, rendering.
//             Everything the game has done since Stage 1 lives here.
//   Landed  — docked at a base/planet: the concourse and its sub-screens
//             (base_screens.h).
//   Dying   — the player ship was destroyed: a timed death cinematic over
//             the still-running sim, then back to the title screen.
//   Loading — a system is being swapped in (the jump flash). There is no
//             progress screen; non-jump loads show a one-line label.
//
// Why a deferred-transition API instead of just assigning `mode`?
// A mode flip mid-frame is a footgun: half the frame's systems would run
// under the old mode and half under the new one (e.g. AI ticks in Flight,
// then rendering suddenly thinks we're Landed and skips the scene the AI
// just updated). So writers call game_state::request_mode() — which only
// records intent — and main.cpp calls game_state::apply_pending() exactly
// once at the top of frame_cb(), BEFORE any sim or render work. Every
// system inside a frame therefore sees one consistent mode, always.
//
// Multiple requests in the same frame: last writer wins. That's the
// simplest rule and matches how the original Privateer behaved — if you
// somehow dock and die in the same instant, whichever event fired last
// gets the screen. Requesting the current mode is a no-op (no spurious
// prev_mode churn, no time_in_mode_s reset).
//
// `time_in_mode_s` accumulates inside apply_pending() so callers don't
// need a second per-frame call; it resets to zero on every transition.
// Useful for "fade in the concourse over 0.5s" type effects later.
// -----------------------------------------------------------------------------

#include <cstdint>

enum class GameMode : uint8_t {
    Flight = 0,
    Landed,
    Dying,
    Loading,
    Menu,           // title / main menu (np-3dp.6). Music director plays
                   // the menu bed; no sim ticks, no input other than the
                   // chrome's own action handlers.
    Count
};
constexpr int kGameModeCount = (int)GameMode::Count;

struct GameState {
    GameMode mode           = GameMode::Flight;
    GameMode prev_mode      = GameMode::Flight;
    float    time_in_mode_s = 0.0f;

    // Deferred-transition slot. Private by convention — write via
    // game_state::request_mode(), consumed by game_state::apply_pending().
    GameMode pending     = GameMode::Flight;
    bool     has_pending = false;
};

namespace game_state {

// Record intent to switch modes. Takes effect at the start of the NEXT
// apply_pending() call — never mid-frame. Requesting the current mode
// clears any earlier pending request (last writer wins, and "stay put"
// is a legal last word). Safe to call from anywhere on the main thread.
void request_mode(GameState& gs, GameMode m);

// Apply any pending transition and advance time_in_mode_s by dt.
// Call exactly once per frame, at the top of frame_cb(), before any
// sim or render work. Returns true when a transition actually happened
// this frame — main.cpp uses that to clear held-key state so a key
// held across a mode flip doesn't ghost-thrust on return to Flight.
// Logs one line per transition: `[mode] flight -> landed`.
bool apply_pending(GameState& gs, float dt);

// Lowercase canonical name, mirroring faction::to_name. "flight",
// "landed", "dying", "loading"; "?" for out-of-range input.
const char* to_name(GameMode m);

} // namespace game_state
