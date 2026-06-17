#pragma once
// -----------------------------------------------------------------------------
// music_labeler.h — F8-toggled ImGui tool to audition + hand-label every
// rendered Privateer AdLib music track. The music sibling of the F7 sound
// labeler (src/sound_labeler.{h,cpp}); same layout, persistence, toolbar,
// auto-advance and reverse-lookup pattern.
//
// Why this exists: render_music.py (np-m96) renders the game's original
// OPL2/AdLib score to looping WAVs (basetune, combat, opening, victory,
// credits). The engine's state->track policy in music.cpp is a best-guess
// wiring (which .ADL underscores which GameMode), and the only reliable way
// to confirm "this WAV is the combat track / this one is the title tune" is
// a human ear. So: this tool plays each rendered track and lets Mike type a
// name for it, saving to a COMMITTABLE JSON (docs/music_labels.json) so we
// can later reconcile music.cpp's state->track mapping against ground truth.
//
// Workflow: press F8. A scrollable window lists every rendered track found in
// gog_extracted/music_wav/ (basetune.wav, combat.wav, ...). Each row: track
// name + filename, a [Play] button, the duration, a text box for your label,
// and a "used by: ..." tag showing what GameMode the engine CURRENTLY ties
// this track to (so you can confirm/refute it). [Save] writes the JSON;
// labels reload on open so you can resume.
//
// ---- LONG tracks + the live-music handoff (the big difference vs F7) ----
// Music tracks run 50-150s, not the ~1s SFX clips. So preview is strictly
// ONE-AT-A-TIME and controllable: [Play] on a row cuts whatever was playing,
// [Stop] kills it. Crucially, while the labeler is previewing it DUCKS the
// dynamic in-game music (music::set_muted(true)) so the preview isn't
// fighting the live BASETUNE/COMBAT bed. The previous mute state is captured
// on the first duck and RESTORED the instant the preview stops, the window
// closes (X), or F8 toggles it off — so we never leave the live music layer
// muted behind us (don't break np-m96 / np-3va).
//
// LEGAL: this LOADS the gitignored rendered WAVs from gog_extracted/music_wav/
// at runtime — local-only, exactly like music.cpp prefers the gitignored
// assets/music/original/ files. It NEVER copies audio into the repo. The only
// thing it WRITES is text (docs/music_labels.json), which is derived-facts
// and committable — same spirit as docs/sound_labels.json.
//
// Lifecycle ordering relative to peers (see main.cpp): handle_event runs
// ahead of debug_panel so the F8 toggle beats ImGui focus; build runs after
// debug_panel::build (which issues simgui_new_frame) but before simgui_render
// — the same window of opportunity the F4/F7 tools use.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

namespace music_labeler {

// One-time setup. No heavy work — track scan + sample loads are LAZY (first
// time the window is opened) so a normal boot pays nothing. Logs the toggle
// key so it's discoverable from the boot log.
void init();

// Symmetric cleanup for parity with the other debug tools. Stops any preview
// and restores the live music layer's mute state if we were ducking it.
void shutdown();

// Must run BEFORE debug_panel::handle_event so the F8 toggle works even when
// an ImGui window has keyboard focus. Returns true if the event was consumed
// (so main.cpp's input router stops here).
bool handle_event(const sapp_event* e);

// Per-frame UI build. Call once between simgui_new_frame() (issued by
// debug_panel::build) and the final swapchain pass that flushes ImGui draw
// calls. Safe to call when hidden — early-returns without drawing or loading
// (but still tears down a stray preview / un-ducks on the close transition).
void build();

} // namespace music_labeler
