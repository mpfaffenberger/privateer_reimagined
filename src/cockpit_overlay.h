// cockpit_overlay.h — per-hull cockpit art framing the flight view (#426).
//
// Layering (all ImGui, bottom to top):
//   1. ImGui BACKGROUND draw list: dark MFD "glass" fills, then the cockpit
//      PNG. The PNG's real alpha lets space through the canopy; the glass
//      fills sit under the MFD holes so the bezel art overlaps their edges.
//   2. Regular ImGui windows: cockpit_hud's STATUS / RADAR / NAV (TARGET)
//      panels, parked exactly inside the three MFD holes (see mfd_rect).
//   3. ImGui FOREGROUND draw list: crosshair, reticles, warnings.
//
// Everything is in ImGui logical pixels, so it composites after the 3D
// scene without touching the render passes.
#pragma once

#include "cockpit_overlay_layout.h"

#include <string>

namespace cockpit_overlay {

// Draw the art for `ship_class` this frame (no-op for hulls without art).
// Call once per Flight frame, inside the ImGui frame, BEFORE the cockpit HUD
// panels so they can ask for their MFD rects.
void draw(const std::string& ship_class);

// True when cockpit art was drawn during the current ImGui frame. Keyed on
// ImGui's frame counter so a skipped draw() (title, cinematic) can never
// leave the HUD parked in holes that are not on screen.
bool active();

// Screen rect (ImGui logical px) of an MFD's glass this frame. False when no
// cockpit art is active — callers fall back to their classic placement.
bool mfd_rect(Mfd which, Rect& out);

} // namespace cockpit_overlay
