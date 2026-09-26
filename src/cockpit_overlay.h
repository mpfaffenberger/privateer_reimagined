// cockpit_overlay.h — per-hull cockpit art framing the flight view (#426).
//
// Frame flow (all ImGui, logical pixels, composited after the 3D scene):
//   1. draw()      — before the HUD. Publishes this frame's fit, and on the
//                    BACKGROUND draw list paints a
//                    dark glass fill under every display.
//   2. HUD panels  — cockpit_hud lays each instrument out FLAT in its
//                    display's panel window (display_panel / window id).
//   3. finalize()  — right before ImGui renders. Moves each display window's
//                    geometry onto the background list, warped through a
//                    homography onto the skewed bezel quad, then draws the
//                    cockpit PNG ON TOP: its real alpha masks rounded glass
//                    corners and any overhang, exactly like a physical frame.
//   Everything else (crosshair, reticles, tooltips, navmap) stays above.
#pragma once

#include "cockpit_overlay_layout.h"

#include <string>

namespace cockpit_overlay {

// Step 1. No-op for hulls without art. Call once per Flight frame inside the
// ImGui frame, before the cockpit HUD panels ask for their displays.
void draw(const std::string& ship_class);

// Step 3. No-op unless draw() ran this frame. Call after ALL ImGui building,
// immediately before the frame is rendered.
void finalize();

// True when cockpit art is active this ImGui frame. Keyed on ImGui's frame
// counter so a skipped draw() (title, cinematic) can never leave the HUD
// parked in displays that are not on screen.
bool active();

// ImGui window every panel targeting `d` must draw into (shared per display,
// so later panels append; finalize() warps it).
const char* display_window_id(Display d);

// Flat panel rect (logical px) for display `d` this frame. False when no
// art is active or this art lacks that display.
bool display_panel(Display d, Rect& out);

// Inverse warp: screen point -> flat panel point, for hit-testing the
// interactive STATUS page. False when the display isn't active.
bool screen_to_panel(Display d, Vec2 screen, Vec2& out);

} // namespace cockpit_overlay
