// hud_text_fit.h — keep dense HUD text lines inside narrow MFD glass (#430).
//
// Panel layouts were authored for the ~280 px classic free-floating HUD box;
// cockpit MFDs are ~140-190 px. These helpers measure against the width left
// on the current ImGui line, so a wide classic panel draws exactly what it
// always did and only a cramped panel falls back: compact phrasing or a
// wrapped line first, and as a last resort a font shrunk just enough to fit.
// Nothing ever runs past the right edge of the window.
#pragma once

#include "imgui.h"

namespace hud_text_fit {

// One way to phrase a "HEAD tail" readout: `head` in the accent colour, then
// `tail` after it on the same line in the body colour.
struct Phrasing { const char* head; const char* tail; };

// Which phrasing a line() call drew, and the font scale it needed (1 = none).
struct Fit { int option; float scale; };

// Single line of text, shrunk to fit if it doesn't.
void text(const char* s, ImU32 col);

// First of `options` (ordered roomiest -> most compact) that fits; if none
// does, the last one with the font shrunk to fit.
Fit line(const Phrasing* options, int count, ImU32 head_col, ImU32 tail_col);

// "head  tail" on one line when it fits, else `head` then `tail` on their own
// lines (each still shrunk if needed). Returns the number of lines drawn.
int pair_or_wrap(const char* head, const char* tail, ImU32 col);

} // namespace hud_text_fit
