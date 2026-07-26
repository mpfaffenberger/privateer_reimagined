#pragma once
// -----------------------------------------------------------------------------
// music_dj.h — tiny bar-music DJ panel (issue #264).
//
// Ctrl+B toggles a small ImGui window for auditioning the 14-track bar pool
// while reviewing fixer scenes: prev/next cycling, direct track buttons, and
// a Shuffle reset. Pure front-end over music::request_bar_track — the same
// override the fixers' per-scene `music` field and POST /music use, so what
// you hear in the panel is exactly what shipping a track id will sound like.
//
// Same tool-window pattern as sound_labeler (F7) / cinematic_studio (Ctrl+K):
// handle_event() beats ImGui focus for the toggle, build() is a no-op while
// hidden.
// -----------------------------------------------------------------------------

struct sapp_event;

namespace music_dj {

// Ctrl+B toggle. Returns true when the event was consumed.
bool handle_event(const sapp_event* e);

// Draw the window (no-op while hidden). Call once per frame from the ImGui
// build pass, any mode.
void build();

} // namespace music_dj
