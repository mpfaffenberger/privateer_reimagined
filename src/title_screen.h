#pragma once
// -----------------------------------------------------------------------------
// title_screen.h — the Wing-Commander-Privateer-style title/menu screen.
//
// A classic WCPrivateer-style chrome plate sits centered on a black
// background. At the top is a small "PRIVATEER" wordmark + subtitle; in
// the middle is a viewport frame (decorative only — no scene yet); at
// the bottom is a row of NEW / LOAD / OPTIONS / QUIT buttons.
//
// The module doesn't own any game state. draw_title_screen() draws the
// chrome and reports clicks back to main via an Action enum; main.cpp
// polls the action and dispatches (start new game, load save, etc.).
// -----------------------------------------------------------------------------

namespace title_screen {

enum class Action {
    None,      // no click yet this frame
    NewGame,   // NEW  clicked
    LoadGame,  // LOAD clicked
    Options,   // OPTIONS clicked
    Quit,      // QUIT clicked
};

// Draw the title screen chrome centered on the current framebuffer.
// Subsequent calls within the same frame are idempotent. Closes the
// frame on `ImGui::End` so the call site is `draw_title_screen()`.
//
// Returns the latest click this frame (or Action::None if the user
// hasn't picked anything). Callers typically gate a state transition
// on the result — e.g. "if action==NewGame, hide the overlay and
// enter free flight".
Action draw();

} // namespace title_screen
