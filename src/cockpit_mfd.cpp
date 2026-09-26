// cockpit_mfd.cpp — HUD panel placement + compact MFD readouts (#426).
//
// Why ImGui windows (and not a separate render path) for the MFDs:
// cockpit_hud's panels are already ImGui windows with live data, and the
// STATUS armaments page takes drag/drop input. Re-drawing them through
// another path would fork every panel. Instead each panel keeps its body and
// only its *window* moves: pinned to the MFD glass rect with ImGuiCond_Always,
// chrome stripped (no bg/border/title — the painted bezel is the frame), and
// ImGui's per-window clip rect == the glass, so nothing spills onto the dash.
// ImGui 1.92's dynamic fonts let us rasterise text at the glass-sized pixel
// height instead of blurry-scaling it.
//
// Flat windows (rather than perspective-warped planes) are correct because
// the art's MFD glass is frontal: measured corners sit <= 2.8 art px off an
// axis-aligned rect, inside the content padding. The harness enforces that
// per art row. If future art has genuinely angled MFDs, the least-invasive
// upgrade is a CPU homography over the slot window's ImDrawList vertices
// after End() (rect -> measured quad, clip rect = quad bbox), with the
// inverse mapping applied to the mouse for the interactive STATUS page.
//
// The one ImGui wrinkle: the centre MFD carries RADAR plus the FLIGHT and
// ordnance readouts that used to be separate floating panels. ImGui happily
// re-opens a window by name within a frame and appends to it, so those
// readouts Begin() the same "##mfd_center" window and draw into the flank
// strips beside the radar disc (cockpit_overlay::split_radar).
#include "cockpit_hud_internal.h"
#include "cockpit_overlay.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace cockpit_hud {

namespace {

using cockpit_overlay::Mfd;
using cockpit_overlay::Rect;

// One shared window per MFD slot, so later panels can append (see header).
const char* slot_window_id(Mfd slot) {
    switch (slot) {
    case Mfd::Left:   return "##mfd_left";
    case Mfd::Center: return "##mfd_center";
    case Mfd::Right:  default: return "##mfd_right";
    }
}

// Font pixel height for an MFD of `glass_h` logical px. ~8.5 text rows per
// glass keeps the densest page (NAV: title + 4 rows + dock prompt) inside
// the smaller side MFDs; clamped so huge windows don't get billboard text
// and tiny ones stay legible.
float mfd_font_px(float glass_h) {
    return std::clamp(glass_h / 8.5f, 8.0f, 16.0f);
}

// Centred text on one line of a flank strip, shrunk to fit its width.
// Advances `y` by the line height actually used.
void flank_text(ImDrawList* dl, const Rect& flank, float& y,
                const char* text, ImU32 col) {
    ImFont* font = ImGui::GetFont();
    float   px   = ImGui::GetFontSize();
    ImVec2  ts   = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const float room = flank.w - 2.0f;
    if (ts.x > room && ts.x > 0.0f) {
        px *= room / ts.x;
        ts  = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    }
    dl->AddText(font, px, ImVec2(flank.x + (flank.w - ts.x) * 0.5f, y), col, text);
    y += ts.y + 1.0f;
}

float line_px() { return ImGui::GetFontSize() + 1.0f; }

// Autopilot lines are transient/situational, not an instrument, so in the
// cockpit they pop up as a small banner sitting on the dash right above the
// centre MFD rather than taking a whole screen.
void draw_autopilot_banner(const FlightStatusHudState& s, const Rect& centre) {
    if (!s.autopilot_nav && !s.autopilot_msg) return;
    const int   rows = (s.autopilot_nav ? 1 : 0) + (s.autopilot_msg ? 1 : 0);
    const float w = std::max(centre.w * 1.6f, 220.0f);
    const float h = ImGui::GetTextLineHeightWithSpacing() * rows + 12.0f;
    ImGui::SetNextWindowPos(ImVec2(centre.x + centre.w * 0.5f - w * 0.5f,
                                   centre.y - h - 6.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    push_hud_style();
    if (ImGui::Begin("##autopilot_banner", nullptr, kHudWindowFlags)) {
        if (s.autopilot_nav) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::Text("AUTOPILOT - %s", s.autopilot_nav);
            ImGui::PopStyleColor();
        }
        if (s.autopilot_msg) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextUnformatted(s.autopilot_msg);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    pop_hud_style();
}

} // namespace

void push_hud_style(ImVec2 padding) {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border,   kAmber);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    padding);
}
void pop_hud_style() {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

bool mfd_placement(Mfd slot, PanelPlacement& out) {
    Rect glass;
    if (!cockpit_overlay::mfd_rect(slot, glass)) return false;
    out.window_id = slot_window_id(slot);
    out.pos       = ImVec2(glass.x, glass.y);
    out.size      = ImVec2(glass.w, glass.h);
    out.in_mfd    = true;
    return true;
}

PanelPlacement place_panel(Mfd slot, const char* classic_id,
                           ImVec2 classic_pos, ImVec2 classic_size) {
    PanelPlacement p;
    if (mfd_placement(slot, p)) return p;
    p.window_id = classic_id;
    p.pos       = classic_pos;
    p.size      = classic_size;
    return p;
}

bool begin_panel(const PanelPlacement& p, ImGuiWindowFlags flags,
                 ImVec2 classic_padding) {
    ImGui::SetNextWindowPos(p.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(p.size, ImGuiCond_Always);
    if (p.in_mfd) {
        // Glass fill is painted under the bezel by cockpit_overlay; the
        // window itself is chrome-free so the art frames the content.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 3.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4.0f, 1.0f));
        ImGui::PushFont(nullptr, mfd_font_px(p.size.y));
        flags |= ImGuiWindowFlags_NoBackground;
    } else {
        ImGui::SetNextWindowBgAlpha(0.55f);
        push_hud_style(classic_padding);
    }
    return ImGui::Begin(p.window_id, nullptr, flags);
}

void end_panel(const PanelPlacement& p) {
    ImGui::End();
    if (p.in_mfd) {
        ImGui::PopFont();
        ImGui::PopStyleVar(3);
    } else {
        pop_hud_style();
    }
}

LockReadout lock_readout(const WeaponsHudState& w) {
    // DF (no lock) just shows its label; HS/IR show seeking/locked with an
    // IR build-up bar so the ~1.5s acquire is visible, not mysterious.
    if (!w.needs_lock)      return { w.no_lock_label,        kDimAmber, false };
    if (w.lock_state == 2)  return { "LOCKED",               kGreen,    false };
    if (w.lock_state == 1)  return { "LOCK\xE2\x80\xA6",     kCyan,     true  };  // "LOCK…"
    return                         { "NO TARGET",            kDimAmber, false };
}

bool draw_flight_flanks(const FlightStatusHudState& s) {
    PanelPlacement p;
    if (!mfd_placement(Mfd::Center, p)) return false;

    if (begin_panel(p)) {   // appends to the RADAR's centre-MFD window
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto split = cockpit_overlay::split_radar(to_rect(p));
        const float pad = 3.0f;

        // Left flank, top-down: speed + flight mode.
        float y = split.left.y + pad;
        char buf[32];
        flank_text(dl, split.left, y, "SPD", kDimAmber);
        std::snprintf(buf, sizeof(buf), "%.0f", s.speed);
        flank_text(dl, split.left, y, buf, kHudWhite);
        flank_text(dl, split.left, y, s.mode, kAmber);

        // Right flank: energy bank as a vertical reservoir gauge — full
        // grows upward, burn/fire drains it (same pool feeds both).
        if (s.energy_max > 0.0f) {
            const Rect& r = split.right;
            float ly = r.y + pad;
            flank_text(dl, r, ly, "NRG", kDimAmber);
            const float frac = std::clamp(s.energy / s.energy_max, 0.0f, 1.0f);
            const float bw   = std::min(r.w * 0.45f, 14.0f);
            const ImVec2 lo(r.x + (r.w - bw) * 0.5f, ly + 2.0f);
            const ImVec2 hi(lo.x + bw, r.y + r.h - pad - line_px());
            if (hi.y > lo.y) {
                dl->AddRectFilled(lo, hi, IM_COL32(20, 20, 30, 200), 2.0f);
                dl->AddRectFilled(ImVec2(lo.x, hi.y - (hi.y - lo.y) * frac), hi,
                                  IM_COL32(255, 210, 60, 220), 2.0f);
                dl->AddRect(lo, hi, kDimAmber, 2.0f);
            }
            float py = r.y + r.h - pad - line_px();
            std::snprintf(buf, sizeof(buf), "%.0f%%", frac * 100.0f);
            flank_text(dl, r, py, buf, kHudWhite);
        }
    }
    end_panel(p);

    draw_autopilot_banner(s, to_rect(p));
    return true;
}

bool draw_weapons_flank(const WeaponsHudState& w) {
    PanelPlacement p;
    if (!mfd_placement(Mfd::Center, p)) return false;

    if (begin_panel(p)) {   // appends to the centre-MFD window
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const Rect left = cockpit_overlay::split_radar(to_rect(p)).left;
        const LockReadout lock = lock_readout(w);

        // Bottom-anchored in the left flank, under SPD/mode: "MSL", the
        // selected type + count, then the lock state (+ IR build-up bar).
        const float bar_h = lock.show_progress ? 5.0f : 0.0f;
        float y = left.y + left.h - 3.0f - line_px() * 3.0f - bar_h;
        char buf[32];
        flank_text(dl, left, y, "MSL", kDimAmber);
        std::snprintf(buf, sizeof(buf), "%s x%d", w.missile_name, w.missile_count);
        flank_text(dl, left, y, buf, kAmber);
        flank_text(dl, left, y, lock.text, lock.col);
        if (lock.show_progress) {
            const float f = std::clamp(w.lock_progress, 0.0f, 1.0f);
            const ImVec2 lo(left.x + 4.0f, y + 1.0f);
            const ImVec2 hi(left.x + left.w - 4.0f, y + 1.0f + bar_h - 1.0f);
            dl->AddRect(lo, hi, kDimAmber);
            dl->AddRectFilled(lo, ImVec2(lo.x + (hi.x - lo.x) * f, hi.y), kCyan);
        }
    }
    end_panel(p);
    return true;
}

} // namespace cockpit_hud
