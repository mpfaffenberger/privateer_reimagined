#pragma once
// -----------------------------------------------------------------------------
// cinematic_studio.h — the in-game Cinematic Studio panel (Phase B,
// docs/cinematic_studio.md §3-§5).
//
// An ImGui window (Ctrl+K to toggle) with four tabs:
//   COMPOSE  — English brief / triggers / outcome text -> writes a
//              kind:"author" request file for the out-of-game bridge.
//   REQUESTS — throttled scan of assets/cinematics/studio/{requests,
//              responses}; per-request status + one-click Reload/Play/Delete.
//   LINES    — parse any assets/cinematics/*.json (via the engine-free
//              cinematic_parse) and fine-tune its `line` cues; writes a
//              kind:"refine" request with only the overridden fields.
//   PLAYBACK — live cinematic status, Play/Stop/Reload/Seek, trigger table
//              status, and the last few "cinematic" beat events.
//
// The engine stays AI/network-free: this panel ONLY reads/writes JSON files
// (through cinematic_studio_io) and drives cinematic:: locally. Everything
// runs on the main thread (it's ImGui), so play/reload/seek go through host
// hooks that main.cpp wires to the exact same recipes the dev_remote
// /cinematic/* endpoints use (g.ships / g.player / encounter_spawn — the
// set_despawn_hook seam pattern, again).
//
// Follows the debug_panel two-phase contract: build() is called once per
// frame AFTER debug_panel::build (which owns simgui_new_frame); the draw
// commands are flushed by the existing debug_panel::render() call. No extra
// swapchain pass, no extra ImGui frame.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

#include <functional>
#include <string>

namespace cinematic::studio {

// Host recipes for the playback verbs (wired in main.cpp next to the
// dev_remote cinematic hooks — SAME bodies, so panel and HTTP behave
// identically: play is Flight-gated, reload also reloads triggers.json).
// Any unset hook = the corresponding button shows "not wired" and no-ops.
struct Hooks {
    std::function<bool(const std::string& id, std::string& err)> play;
    std::function<void()>                                        stop;
    std::function<bool(const std::string& id, std::string& err)> reload;
    std::function<bool(float t, std::string& err)>               seek;
};
void set_hooks(Hooks h);

// Toggle key: Ctrl+K ("Kinema"). Ctrl-modified like debug_panel's Ctrl+M —
// the plain function keys F2-F10 are all taken by the other dev tools.
// Returns true when the event was consumed (the toggle itself).
bool handle_event(const sapp_event* e);

// Build the window's widgets for this frame. Call once per frame after
// debug_panel::build() in BOTH frame paths (Flight + stub-mode). Cheap when
// hidden. File-system scans inside are throttled (~2s), not per-frame.
void build();

// Feed one "cinematic" beat event into the Playback tab's last-10 ring.
// main.cpp chains this off the existing cinematic::set_event_tap sink.
void note_event(const std::string& text);

} // namespace cinematic::studio
