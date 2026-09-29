// cockpit_damage.cpp — DAMAGE CONTROL MFD (#141). See cockpit_damage.h.
#include "cockpit_damage.h"
#include "cockpit_hud_internal.h"

#include "ship.h"
#include "ship_systems.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace cockpit_damage {
namespace {

using cockpit_hud::kAmber;
using cockpit_hud::kDimAmber;
using cockpit_hud::kGreen;
using cockpit_hud::kHudWhite;

constexpr ImU32 kRed = IM_COL32(255, 80, 80, 255);

ImU32 color_for(float integrity) {
    if (integrity >= 1.0f) return kGreen;
    return integrity > 0.0f ? kAmber : kRed;
}

// One system row: label, a horizontal integrity bar, then a status word.
void draw_row(ImDrawList* dl, ShipSystem sys, const ShipSystems& ss,
              float label_w, float bar_w) {
    const bool  fitted = ship_systems::installed(ss, sys);
    const float v      = ship_systems::integrity(ss, sys);
    const ImU32 col    = fitted ? color_for(v) : kDimAmber;
    const float row_x  = ImGui::GetCursorPosX();

    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(ship_systems::label(sys));
    ImGui::SameLine(row_x + label_w);

    const float  line_h = ImGui::GetTextLineHeight();
    const ImVec2 p0     = ImGui::GetCursorScreenPos();
    const float  inset  = line_h * 0.2f;
    const ImVec2 a(p0.x, p0.y + inset), b(p0.x + bar_w, p0.y + line_h - inset);
    if (fitted && v > 0.0f)
        dl->AddRectFilled(a, ImVec2(a.x + bar_w * v, b.y), col);
    dl->AddRect(a, b, kDimAmber);
    ImGui::Dummy(ImVec2(bar_w, line_h));
    ImGui::SameLine();

    char status[16];
    if (!fitted)         std::snprintf(status, sizeof status, "--");
    else if (v >= 1.0f)  std::snprintf(status, sizeof status, "OK");
    else if (v > 0.0f)   std::snprintf(status, sizeof status, "%d%%",
                                       std::max(1, (int)(v * 100.0f)));
    else                 std::snprintf(status, sizeof status, "DESTROYED");
    ImGui::TextUnformatted(status);
    ImGui::PopStyleColor();
}

} // namespace

void draw(const Ship& ship) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted("DAMAGE CONTROL");
    ImGui::PopStyleColor();
    ImGui::Separator();

    if (!ship.alive) {
        ImGui::PushStyleColor(ImGuiCol_Text, kRed);
        ImGui::TextUnformatted("*** DESTROYED ***");
        ImGui::PopStyleColor();
        return;
    }

    // Column widths from the widest label/status so the bars line up at any
    // font size (classic corner panel or a cockpit MFD's glass-sized font).
    const float spacing  = ImGui::GetStyle().ItemSpacing.x;
    const float label_w  = ImGui::CalcTextSize("SHIELD GEN").x + spacing;
    const float status_w = ImGui::CalcTextSize("DESTROYED").x + spacing;
    const float bar_w    = std::max(24.0f, ImGui::GetContentRegionAvail().x
                                           - label_w - status_w);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < kShipSystemCount; ++i)
        draw_row(dl, ship_systems::at(i), ship.systems, label_w, bar_w);

    if (!ship_systems::any_damaged(ship.systems)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
        ImGui::TextUnformatted("ALL SYSTEMS NOMINAL");
        ImGui::PopStyleColor();
    }
}

} // namespace cockpit_damage
