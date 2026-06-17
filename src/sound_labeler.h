#pragma once
// -----------------------------------------------------------------------------
// sound_labeler.h — F7-toggled ImGui tool to audition + hand-label every
// extracted original Privateer SFX clip.
//
// Why this exists: the canonical event->sound and gun->sound tables are NOT
// recoverable from the data files (GUNS.IFF has no sound field; the
// clean-room dpjudas reference never plays a firing sound — see
// docs/sfx_gun_mapping.md). The current bindings are an ACOUSTIC HEURISTIC
// and it's WRONG in places (the Mass Driver historically played the
// armor-damage / Meson sibling clip). The only reliable source of truth left
// is a human ear. So: this tool plays each of the 43 extracted clips and
// lets Mike type a name for it. The labels are saved to a COMMITTABLE JSON
// (docs/sound_labels.json) so we can later rebuild the canonical mappings
// from that ground-truth instead of from crest-factor guesswork.
//
// Workflow: press F7. A scrollable window lists every clip found in
// gog_extracted/sfx_wav/ (sfx_NN.wav, PAK order). Each row: index +
// filename, a [Play] button, the clip duration, a text box for your label,
// and a small "used by: ..." tag showing what the engine CURRENTLY binds
// this clip to (so you can spot the wrong ones). [Save] writes the JSON;
// labels reload on open so you can resume.
//
// LEGAL: this LOADS the gitignored extracted originals from gog_extracted/
// at runtime — local-only, exactly like sfx.cpp prefers the gitignored
// assets/sfx/original/ files. It NEVER copies audio into the repo. The only
// thing it WRITES is text (docs/sound_labels.json), which is derived-facts
// and committable — same spirit as docs/sfx_gun_mapping.md.
//
// Lifecycle ordering relative to peers (see main.cpp): handle_event runs
// ahead of debug_panel so the F7 toggle beats ImGui focus; build runs after
// debug_panel::build (which issues simgui_new_frame) but before simgui_render
// — the same window of opportunity the F2/F4 tools use.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

namespace sound_labeler {

// One-time setup. No heavy work — clip scan + sample loads are LAZY (first
// time the window is opened) so a normal boot pays nothing. Logs the toggle
// key so it's discoverable from the boot log.
void init();

// Symmetric cleanup for parity with the other debug tools. No-op today
// (samples are never unloaded — audio.cpp owns them until shutdown).
void shutdown();

// Must run BEFORE debug_panel::handle_event so the F7 toggle works even when
// an ImGui window has keyboard focus. Returns true if the event was consumed
// (so main.cpp's input router stops here).
bool handle_event(const sapp_event* e);

// Per-frame UI build. Call once between simgui_new_frame() (issued by
// debug_panel::build) and the final swapchain pass that flushes ImGui draw
// calls. Safe to call when hidden — early-returns without drawing or loading.
void build();

} // namespace sound_labeler
