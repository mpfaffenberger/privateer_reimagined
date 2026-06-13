// -----------------------------------------------------------------------------
// game_state.cpp — game-mode state machine implementation.
//
// Deliberately tiny. The machine itself is just "apply the pending mode
// at frame start, tick a timer"; the interesting policy (what each mode
// renders, which inputs it accepts) lives at the call sites in main.cpp
// where the systems being gated actually are. See game_state.h for the
// full deferred-transition rationale.
// -----------------------------------------------------------------------------

#include "game_state.h"

#include <cstdio>

namespace game_state {

void request_mode(GameState& gs, GameMode m) {
    if (m == gs.mode) {
        // "Stay where you are" — also cancels any earlier request this
        // frame (last writer wins).
        gs.has_pending = false;
        return;
    }
    gs.pending     = m;
    gs.has_pending = true;
}

bool apply_pending(GameState& gs, float dt) {
    if (!gs.has_pending) {
        gs.time_in_mode_s += dt;
        return false;
    }
    std::printf("[mode] %s -> %s\n", to_name(gs.mode), to_name(gs.pending));
    gs.prev_mode      = gs.mode;
    gs.mode           = gs.pending;
    gs.has_pending    = false;
    gs.time_in_mode_s = 0.0f;
    return true;
}

const char* to_name(GameMode m) {
    switch (m) {
        case GameMode::Flight:  return "flight";
        case GameMode::Landed:  return "landed";
        case GameMode::Dying:   return "dying";
        case GameMode::Loading: return "loading";
        default:                return "?";
    }
}

} // namespace game_state
