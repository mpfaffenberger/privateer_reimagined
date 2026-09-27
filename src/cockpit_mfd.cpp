// cockpit_mfd.cpp — HUD panel placement + compact cockpit readouts (#426).
//
// Each HUD panel keeps its ImGui body; only its *window* moves. With cockpit
// art up, a panel draws into its display's shared window, laid out flat over
// the display's panel rect (ImGuiCond_Always), chrome stripped (the painted
// bezel is the frame), font rasterised at a glass-sized height (ImGui 1.92
// dynamic fonts: crisp, not scaled). cockpit_overlay::finalize() then warps
// the finished window geometry onto the skewed bezel quad and draws the art
// over it — see cockpit_overlay.cpp for why that beats a parallel renderer.
//
// The centre display carries RADAR plus the FLIGHT and ordnance readouts
// that used to be separate floating panels: ImGui re-opens a window by name
// within a frame and appends, so those readouts Begin() the same window and
// draw into the flank strips beside the disc (cockpit_overlay::split_radar).
#include "cockpit_hud_internal.h"
#include "cockpit_overlay.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

namespace cockpit_hud {

namespace {

using cockpit_overlay::Display;
using cockpit_overlay::Rect;

// Font pixel height for a display `glass_h` logical px tall. Full MFDs get
// ~8.5 text rows (the densest page, NAV, is title + 4 rows + dock prompt);
// single-line strips (< 60 px) fill ~60% of their height. Clamped so huge
// windows don't get billboard text and tiny ones stay legible.
float display_font_px(float glass_h) {
    const float px = glass_h < 60.0f ? glass_h * 0.6f : glass_h / 8.5f;
    return std::clamp(px, 8.0f, 16.0f);
}

// Centred text on one line of a strip, shrunk to fit its width.
// Advances `y` by the line height actually used.
void strip_text(ImDrawList* dl, const Rect& strip, float& y,
                const char* text, ImU32 col) {
    ImFont* font = ImGui::GetFont();
    float   px   = ImGui::GetFontSize();
    ImVec2  ts   = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    const float room = strip.w - 2.0f;
    if (ts.x > room && ts.x > 0.0f) {
        px *= room / ts.x;
        ts  = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
    }
    dl->AddText(font, px, ImVec2(strip.x + (strip.w - ts.x) * 0.5f, y), col, text);
    y += ts.y + 1.0f;
}

// One line, vertically centred in the whole strip.
void strip_line(const Rect& strip, const char* text, ImU32 col) {
    float y = strip.y + (strip.h - ImGui::GetFontSize()) * 0.5f;
    strip_text(ImGui::GetWindowDrawList(), strip, y, text, col);
}

float line_px() { return ImGui::GetFontSize() + 1.0f; }

// Autopilot state. Art with a banner strip shows it there (message wins over
// the standing "AUTOPILOT - nav" line — the strip has one row); otherwise a
// small boxed banner sits on the dash right above the centre MFD.
void draw_autopilot(const FlightStatusHudState& s, const Rect& centre) {
    if (!s.autopilot_nav && !s.autopilot_msg) return;
    PanelPlacement strip;
    if (display_placement(Display::Banner, strip)) {
        if (begin_panel(strip)) {
            char line[96];
            if (s.autopilot_msg) std::snprintf(line, sizeof(line), "%s", s.autopilot_msg);
            else                 std::snprintf(line, sizeof(line), "AUTOPILOT - %s", s.autopilot_nav);
            strip_line(to_rect(strip), line, s.autopilot_msg ? kAmber : kGreen);
        }
        end_panel(strip);
        return;
    }
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

// Unlit indicator lamp (AUTO when autopilot can't engage).
constexpr ImU32 kUnlit = IM_COL32(110, 84, 40, 150);

// One-line strip: `left` flush left, `right` flush right, both shrunk
// together if they'd collide.
void strip_pair(const Rect& strip, const char* left, ImU32 left_col,
                const char* right, ImU32 right_col) {
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImFont*     font = ImGui::GetFont();
    float px = ImGui::GetFontSize();
    const float pad = 3.0f, gap = 6.0f;
    const float need = font->CalcTextSizeA(px, FLT_MAX, 0.0f, left).x + gap +
                       font->CalcTextSizeA(px, FLT_MAX, 0.0f, right).x;
    if (need > strip.w - 2.0f * pad && need > 0.0f) px *= (strip.w - 2.0f * pad) / need;
    const float rw = font->CalcTextSizeA(px, FLT_MAX, 0.0f, right).x;
    const float y  = strip.y + (strip.h - px) * 0.5f;
    dl->AddText(font, px, ImVec2(strip.x + pad, y), left_col, left);
    dl->AddText(font, px, ImVec2(strip.x + strip.w - pad - rw, y), right_col, right);
}

// The art's small speed strips, when it has them — all live, never baked:
// SET = commanded (+/-) speed; KPS = actual speed + the AUTO lamp, lit
// when pressing A would engage the autopilot (nav selected, no hostiles).
void draw_speed_strips(const FlightStatusHudState& s) {
    char buf[32];
    PanelPlacement set;
    if (display_placement(Display::SetSpeed, set)) {
        if (begin_panel(set)) {
            std::snprintf(buf, sizeof(buf), "%.0f", s.set_speed);
            strip_pair(to_rect(set), "SET", kDimAmber, buf, kAmber);
        }
        end_panel(set);
    }
    PanelPlacement kps;
    if (display_placement(Display::Velocity, kps)) {
        if (begin_panel(kps)) {
            std::snprintf(buf, sizeof(buf), "KPS %.0f", s.speed);
            strip_pair(to_rect(kps), buf, kAmber, "AUTO",
                       s.autopilot_ready ? kGreen : kUnlit);
        }
        end_panel(kps);
    }
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

bool display_placement(Display d, PanelPlacement& out) {
    Rect panel;
    if (!cockpit_overlay::display_panel(d, panel)) return false;
    out.window_id  = cockpit_overlay::display_window_id(d);
    out.pos        = ImVec2(panel.x, panel.y);
    out.size       = ImVec2(panel.w, panel.h);
    out.in_display = true;
    out.display    = d;
    return true;
}

PanelPlacement place_panel(Display d, const char* classic_id,
                           ImVec2 classic_pos, ImVec2 classic_size) {
    PanelPlacement p;
    if (display_placement(d, p)) return p;
    p.window_id = classic_id;
    p.pos       = classic_pos;
    p.size      = classic_size;
    return p;
}

bool begin_panel(PanelPlacement& p, ImGuiWindowFlags flags, ImVec2 classic_padding) {
    ImGui::SetNextWindowPos(p.pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(p.size, ImGuiCond_Always);
    if (!p.in_display) {
        ImGui::SetNextWindowBgAlpha(0.55f);
        push_hud_style(classic_padding);
        return ImGui::Begin(p.window_id, nullptr, flags);
    }
    // Glass is painted under the bezel by cockpit_overlay; the window itself
    // is chrome-free so the art frames the content.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4.0f, 1.0f));
    ImGui::PushFont(nullptr, display_font_px(p.size.y));
    const bool open = ImGui::Begin(p.window_id, nullptr,
                                   flags | ImGuiWindowFlags_NoBackground);
    // The window is drawn flat but SHOWN warped: interactive panels must
    // hit-test in flat space, so hand them the inverse-warped mouse.
    ImGuiIO& io = ImGui::GetIO();
    cockpit_overlay::Vec2 flat;
    p.mouse_remapped = !(flags & ImGuiWindowFlags_NoInputs) &&
                       ImGui::IsMousePosValid(&io.MousePos) &&
                       cockpit_overlay::screen_to_panel(
                           p.display, { io.MousePos.x, io.MousePos.y }, flat);
    if (p.mouse_remapped) {
        p.saved_mouse = io.MousePos;
        io.MousePos   = ImVec2(flat.x, flat.y);
    }
    return open;
}

void end_panel(PanelPlacement& p) {
    if (p.mouse_remapped) {
        ImGui::GetIO().MousePos = p.saved_mouse;
        p.mouse_remapped = false;
    }
    ImGui::End();
    if (p.in_display) {
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
    if (!display_placement(Display::Center, p)) return false;

    if (begin_panel(p)) {   // appends to the RADAR's centre-display window
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto split = cockpit_overlay::split_radar(to_rect(p));
        const float pad = 3.0f;

        // Left flank, top-down: speed + flight mode. Art with its own KPS
        // strip already shows speed there, so the flank keeps just the mode.
        float y = split.left.y + pad;
        char buf[32];
        Rect kps_strip;
        if (!cockpit_overlay::display_panel(Display::Velocity, kps_strip)) {
            strip_text(dl, split.left, y, "SPD", kDimAmber);
            std::snprintf(buf, sizeof(buf), "%.0f", s.speed);
            strip_text(dl, split.left, y, buf, kHudWhite);
        }
        strip_text(dl, split.left, y, s.mode, kAmber);

        // Right flank: energy bank as a vertical reservoir gauge — full
        // grows upward, burn/fire drains it (same pool feeds both).
        if (s.energy_max > 0.0f) {
            const Rect& r = split.right;
            float ly = r.y + pad;
            strip_text(dl, r, ly, "NRG", kDimAmber);
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
            strip_text(dl, r, py, buf, kHudWhite);
        }
    }
    end_panel(p);

    draw_speed_strips(s);
    draw_autopilot(s, to_rect(p));
    return true;
}

bool draw_weapons_flank(const WeaponsHudState& w) {
    PanelPlacement p;
    if (!display_placement(Display::Center, p)) return false;

    if (begin_panel(p)) {   // appends to the centre-display window
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const Rect left = cockpit_overlay::split_radar(to_rect(p)).left;
        const LockReadout lock = lock_readout(w);

        // Bottom-anchored in the left flank, under SPD/mode: "MSL", the
        // selected type + count, then the lock state (+ IR build-up bar).
        const float bar_h = lock.show_progress ? 5.0f : 0.0f;
        float y = left.y + left.h - 3.0f - line_px() * 3.0f - bar_h;
        char buf[32];
        strip_text(dl, left, y, "MSL", kDimAmber);
        std::snprintf(buf, sizeof(buf), "%s x%d", w.missile_name, w.missile_count);
        strip_text(dl, left, y, buf, kAmber);
        strip_text(dl, left, y, lock.text, lock.col);
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
