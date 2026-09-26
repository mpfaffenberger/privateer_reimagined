// cockpit_hud_internal.h — shared guts of the cockpit HUD TUs (#426).
//
// NOT a public API: included only by cockpit_hud.cpp and cockpit_mfd.cpp.
// Holds the one HUD palette, the HUD window styling, and the panel
// placement seam that lets every panel land either in its classic screen
// corner or inside a cockpit-art MFD hole (see cockpit_overlay.h).
#pragma once

#include "cockpit_hud.h"
#include "cockpit_overlay_layout.h"

#include "imgui.h"

namespace cockpit_hud {

// ---- palette ---------------------------------------------------------------
// A small, deliberate set of HUD colours so every panel sings the same tune.
// Amber matches the nav-target reticle; cyan/green/blue are radar dot colours
// per nav kind.
inline constexpr ImU32 kAmber     = IM_COL32(255, 217,  77, 240);
inline constexpr ImU32 kDimAmber  = IM_COL32(180, 150,  60, 200);
inline constexpr ImU32 kCyan      = IM_COL32(120, 220, 255, 240);
inline constexpr ImU32 kGreen     = IM_COL32(120, 240, 140, 240);
inline constexpr ImU32 kBlueP     = IM_COL32( 90, 160, 255, 240);
// navmap-only colours. Distinguish jump holes (bright blue circles) from
// empty nav points (green circles) and from dockable bases (squares use the
// kind colour from color_for_kind). Matches the classic Privateer tactical
// map: square = base, blue = jump hole, green = nav point.
inline constexpr ImU32 kJumpBlue  = IM_COL32( 80, 150, 255, 240);
inline constexpr ImU32 kNavGreen  = IM_COL32(120, 240, 140, 240);
// Grid + axis labels on the navmap. Faint enough that nav points still pop,
// dark enough that the grid is legible against the panel background.
inline constexpr ImU32 kGridLine  = IM_COL32( 80, 130, 180,  55);
inline constexpr ImU32 kHudWhite  = IM_COL32(220, 230, 235, 220);
inline constexpr ImU32 kPanelBg   = IM_COL32( 10,  14,  20, 220);

// Common flag set for HUD windows: locked-in-place, no chrome, no input.
inline constexpr ImGuiWindowFlags kHudWindowFlags =
    ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize        |
    ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoScrollbar     |
    ImGuiWindowFlags_NoCollapse         | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav           |
    ImGuiWindowFlags_NoInputs;

// Standard free-floating HUD box (dark bg + amber border). Pair with
// pop_hud_style() — pushes 2 colours + 2 vars.
void push_hud_style(ImVec2 padding = ImVec2(8.0f, 6.0f));
void pop_hud_style();

// ---- panel placement ---------------------------------------------------------
// Where a HUD panel draws this frame. Classic: its own screen-corner window
// in the amber HUD box. In an MFD: the shared window of that MFD slot, sized
// to the glass, chrome-free (the painted bezel IS the frame) and with a font
// sized to the glass so the same content fits a much smaller screen.
struct PanelPlacement {
    const char* window_id = "";
    ImVec2      pos, size;
    bool        in_mfd = false;
};

// MFD placement for `slot`; false when no cockpit art is on screen.
bool mfd_placement(cockpit_overlay::Mfd slot, PanelPlacement& out);

// MFD placement when cockpit art is up, else the classic rect.
PanelPlacement place_panel(cockpit_overlay::Mfd slot, const char* classic_id,
                           ImVec2 classic_pos, ImVec2 classic_size);

// Begin/End the panel's window with the matching styling. ALWAYS pair with
// end_panel(), whatever begin_panel() returned (ImGui Begin/End contract).
// Calling begin_panel twice for the same MFD slot appends to its window.
bool begin_panel(const PanelPlacement& p, ImGuiWindowFlags flags = kHudWindowFlags,
                 ImVec2 classic_padding = ImVec2(8.0f, 6.0f));
void end_panel(const PanelPlacement& p);

inline cockpit_overlay::Rect to_rect(const PanelPlacement& p) {
    return { p.pos.x, p.pos.y, p.size.x, p.size.y };
}

// ---- shared readouts -------------------------------------------------------
// Missile lock line, identical wording in the classic HUD and the MFD flank.
struct LockReadout { const char* text; ImU32 col; bool show_progress; };
LockReadout lock_readout(const WeaponsHudState& w);

// Compact centre-MFD flank readouts (speed/mode + energy; ordnance). Each
// returns false — drawing nothing — when cockpit art is not active, so the
// caller falls through to its classic free-floating panel.
bool draw_flight_flanks(const FlightStatusHudState& s);
bool draw_weapons_flank(const WeaponsHudState& w);

} // namespace cockpit_hud
