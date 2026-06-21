#pragma once
// -----------------------------------------------------------------------------
// speech_labeler.h — F9-toggled ImGui tool to audition + hand-label every
// Privateer speech line from assets/speech/original/speech_NNNN.wav.
//
// Why this exists: the speech pack contains 544 short voice clips (player /
// NPC mission lines, ambient chatter, etc.) and the engine currently binds
// NONE of them. To wire any of them into the engine we need ground-truth
// names ("player_accept", "pirate_threaten", "merchant_bid", ...) and the
// only reliable source is a human ear. So: this tool plays each clip and
// lets you type a name for it, saving to a COMMITTABLE JSON
// (docs/speech_labels.json).
//
// Workflow: press F9. A scrollable window lists every speech_NNNN.wav found
// in assets/speech/original/ (PAK order). Each row: index + filename, a
// [Play] button, the duration, a text box for your label, and a small
// "current use: ..." tag (always "(unused)" for now — the engine doesn't
// bind speech yet, but the tag leaves room for one when it does).
// [Save] writes the JSON; labels reload on open so you can resume.
//
// Build prerequisites (run ONCE after a clean clone):
//   python3 tools/extract_speech_pak.py assets/speech/SPEECH.PAK \
//                                    --convert-wav assets/speech/original
// This extracts all 544 VOCs from the PAK and converts them to engine-
// compatible WAVs (~117MB total at 44.1kHz mono s16). Re-runs are no-ops
// for unchanged files.
//
// LEGAL: assets/speech/original/ lives next to a tracked SPEECH.PAK; per the
// project's "shove it in git" stance, both are tracked and the WAVs are
// considered a build artefact of the tracked PAK (not a derived work of any
// external IP — they ARE the original). docs/speech_labels.json is committable
// text, like docs/sound_labels.json.
//
// Lifecycle ordering relative to peers (see main.cpp): handle_event runs
// ahead of debug_panel so the F9 toggle beats ImGui focus; build runs after
// debug_panel::build (which issues simgui_new_frame) but before simgui_render
// — the same window the F4/F7/F8 tools use.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

namespace speech_labeler {

// One-time setup. No heavy work — clip scan + sample loads are LAZY (first
// time the window is opened) so a normal boot pays nothing. Logs the toggle
// key so it's discoverable from the boot log.
void init();

// Symmetric cleanup for parity with the other debug tools. No-op today
// (samples are never unloaded — audio.cpp owns them until shutdown).
void shutdown();

// Must run BEFORE debug_panel::handle_event so the F9 toggle works even when
// an ImGui window has keyboard focus. Returns true if the event was consumed
// (so main.cpp's input router stops here).
bool handle_event(const sapp_event* e);

// Per-frame UI build. Call once between simgui_new_frame() (issued by
// debug_panel::build) and the final swapchain pass that flushes ImGui draw
// calls. Safe to call when hidden — early-returns without drawing or loading.
void build();

} // namespace speech_labeler
