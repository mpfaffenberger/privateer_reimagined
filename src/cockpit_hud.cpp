// cockpit_hud.cpp — implementation. See header for the elevator pitch.
//
// Architectural notes:
//   * Module is *pure draw*, no state of its own. All inputs come
//     through build()'s parameters; all outputs go through ImGui's
//     foreground drawlist or short-lived ImGui windows. Restartable
//     and testable in isolation.
//   * Per-frame allocations are zero — even the kind→colour lookup
//     compares short std::strings that almost always intern to a
//     small fixed set. If kinds proliferate, swap to an enum.
//   * Two engine quirks are honoured here, both also called out in
//     main.cpp's older HUD code:
//       (1) ImGui drawlist coords are LOGICAL pixels — divide
//           sapp_width()/_height() by sapp_dpi_scale() before use.
//       (2) Our perspective pipeline maps world-up to +screen_y
//           (no NDC Y-flip), so target projection skips the
//           textbook (1 - ndc_y) inversion.
#include "cockpit_hud.h"
#include "navmap_projection.h"

#include "armor.h"
#include "camera.h"
#include "firing.h"
#include "hazards.h"
#include "perception.h"
#include "shield.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_registry.h"
#include "ship_sprite.h"
#include "system_def.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"   // simgui_imtextureid for sprite thumbnails

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace cockpit_hud {

namespace {

// ---- shared helpers ------------------------------------------------------

// Logical-pixel framebuffer dims. ImGui windows + drawlists use these,
// not raw sapp_width()/_height() which return Retina-scaled physical pixels.
struct ScreenSize { float w, h, dpi; };
ScreenSize screen_size() {
    const float dpi = sapp_dpi_scale();
    return { (float)sapp_width()  / dpi,
             (float)sapp_height() / dpi,
             dpi };
}

// Palette — a small, deliberate set of HUD colours so every panel sings
// the same tune. Amber matches our nav-target reticle; cyan/green/blue
// are radar dot colours per nav kind.
static const ImU32 kAmber     = IM_COL32(255, 217,  77, 240);
static const ImU32 kDimAmber  = IM_COL32(180, 150,  60, 200);
static const ImU32 kCyan      = IM_COL32(120, 220, 255, 240);
static const ImU32 kGreen     = IM_COL32(120, 240, 140, 240);
static const ImU32 kBlueP     = IM_COL32( 90, 160, 255, 240);
// navmap-only colours. Distinguish jump holes (bright blue circles) from
// empty nav points (green circles) and from dockable bases (squares use
// the kind colour from color_for_kind). Matches the classic Privateer
// tactical map: square = base, blue = jump hole, green = nav point.
static const ImU32 kJumpBlue  = IM_COL32( 80, 150, 255, 240);
static const ImU32 kNavGreen  = IM_COL32(120, 240, 140, 240);
// Grid + axis labels on the navmap. Faint enough that nav points still
// pop, dark enough that the grid is legible against the panel background.
static const ImU32 kGridLine  = IM_COL32( 80, 130, 180,  55);
static const ImU32 kHudWhite  = IM_COL32(220, 230, 235, 220);
static const ImU32 kPanelBg   = IM_COL32( 10,  14,  20, 220);

// Map a nav kind to its radar/MFD dot colour. String compare is fine —
// nav_points is small and this loop is dwarfed by ImGui call overhead.
ImU32 color_for_kind(const std::string& kind) {
    if (kind == "jump")    return kCyan;
    if (kind == "station") return kGreen;
    if (kind == "planet")  return kBlueP;
    return kHudWhite;
}

// Common flag set for HUD windows: locked-in-place, no chrome, no input.
constexpr ImGuiWindowFlags kHudWindowFlags =
    ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize        |
    ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoScrollbar     |
    ImGuiWindowFlags_NoCollapse         | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav           |
    ImGuiWindowFlags_NoInputs;

// Push the standard HUD window styling (dark-bg + amber border). Pair
// with pop_hud_style() — uses 2 colours + 2 vars.
void push_hud_style() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border,   kAmber);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(8.0f, 6.0f));
}
void pop_hud_style() {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ---- gun crosshair (centre) ---------------------------------------------
//
// Distinct from the nav reticle in role and colour: this one says "here
// is where the guns will fire" and never moves; the amber reticle says
// "here is where the target is" and slides around. Two pieces of data,
// two visuals — never overload one symbol with two meanings.
//
// Also doubles as the fly-by-wire dead-zone visualisation: a faint
// extra ring (radius matching Camera::mouse_dead_zone × half-screen)
// shows where the cursor stops driving turn. Inside the ring → ship
// flies straight. Outside → ship turns.
void draw_crosshair(bool fly_by_wire) {
    const auto s = screen_size();
    const float cx = s.w * 0.5f, cy = s.h * 0.5f;
    auto* dl = ImGui::GetForegroundDrawList();

    constexpr float arm = 10.0f;     // half-length of each crosshair tick
    constexpr float gap = 4.0f;      // empty space at the centre

    dl->AddLine(ImVec2(cx - arm, cy), ImVec2(cx - gap, cy), kHudWhite, 1.5f);
    dl->AddLine(ImVec2(cx + gap, cy), ImVec2(cx + arm, cy), kHudWhite, 1.5f);
    dl->AddLine(ImVec2(cx, cy - arm), ImVec2(cx, cy - gap), kHudWhite, 1.5f);
    dl->AddLine(ImVec2(cx, cy + gap), ImVec2(cx, cy + arm), kHudWhite, 1.5f);
    dl->AddCircleFilled(ImVec2(cx, cy), 1.5f, kHudWhite);

    // Dead-zone ring — only meaningful while flying. Ratio mirrors the
    // Camera::mouse_dead_zone default (0.05); keep them numerically in
    // sync if you change one. (TODO: pass dead_zone in if it ever
    // becomes per-ship tunable.)
    if (fly_by_wire) {
        const float dz_radius = std::min(cx, cy) * 0.05f;
        const ImU32 dz_col    = IM_COL32(220, 230, 235, 70);
        dl->AddCircle(ImVec2(cx, cy), dz_radius, dz_col, 0, 1.0f);
    }
}

// ---- aim cursor (mouse position, fly-by-wire mode) -----------------------
//
// In fly-by-wire mode the OS cursor is hidden (sapp_show_mouse(false)).
// We draw our own amber crosshair-cursor at the mouse position so the
// pilot has a clear visual for 'where I'm pointing the nose'. In free-
// cursor mode this draws nothing — the OS cursor is back, and rendering
// our own would just double up.
void draw_aim_cursor(float mouse_x, float mouse_y, bool fly_by_wire) {
    if (!fly_by_wire) return;
    auto* dl = ImGui::GetForegroundDrawList();
    constexpr float r = 5.0f;
    dl->AddCircle(ImVec2(mouse_x, mouse_y), r, kAmber, 0, 1.5f);
    // Tiny tick marks at N/E/S/W of the cursor — reads as 'crosshair'
    // even from peripheral vision without crowding the centre dot.
    constexpr float tick = 3.0f, gap = 1.5f;
    dl->AddLine(ImVec2(mouse_x, mouse_y - r - gap),
                ImVec2(mouse_x, mouse_y - r - gap - tick), kAmber, 1.5f);
    dl->AddLine(ImVec2(mouse_x, mouse_y + r + gap),
                ImVec2(mouse_x, mouse_y + r + gap + tick), kAmber, 1.5f);
    dl->AddLine(ImVec2(mouse_x - r - gap, mouse_y),
                ImVec2(mouse_x - r - gap - tick, mouse_y), kAmber, 1.5f);
    dl->AddLine(ImVec2(mouse_x + r + gap, mouse_y),
                ImVec2(mouse_x + r + gap + tick, mouse_y), kAmber, 1.5f);
}

// ---- nav target reticle --------------------------------------------------
//
// Floats over the projected screen position of the selected nav point.
// Edge-clamps with a chevron when off-screen / behind the camera.
void draw_nav_reticle(const Camera& cam, const StarSystem& system, int selected_nav) {
    if (selected_nav < 0 || selected_nav >= (int)system.nav_points.size()) return;

    const auto&    nav    = system.nav_points[selected_nav];
    const HMM_Vec3 target = nav.position;
    const HMM_Vec3 d      = HMM_SubV3(target, cam.position);
    const float    len    = HMM_LenV3(d);

    const auto s = screen_size();
    const float fb_w = s.w, fb_h = s.h;
    const float cx = fb_w * 0.5f, cy = fb_h * 0.5f;
    constexpr float margin = 40.0f;

    const float fwd_dot   = HMM_DotV3(d, cam.forward());
    const float right_dot = HMM_DotV3(d, cam.right());
    const float up_dot    = HMM_DotV3(d, cam.up());
    const bool  behind    = (fwd_dot <= 0.0f);

    float sx = cx, sy = cy;
    bool  clamped = behind;

    if (!behind) {
        // NOTE: NO (1 - ndc_y) flip here — engine quirk, see header.
        const float    aspect = fb_w / fb_h;
        const HMM_Mat4 vp     = HMM_MulM4(cam.projection(aspect), cam.view());
        const HMM_Vec4 ph     = { target.X, target.Y, target.Z, 1.0f };
        const HMM_Vec4 clip   = HMM_MulM4V4(vp, ph);
        const float    ndc_x  = clip.X / clip.W;
        const float    ndc_y  = clip.Y / clip.W;
        sx = (ndc_x * 0.5f + 0.5f) * fb_w;
        sy = (ndc_y * 0.5f + 0.5f) * fb_h;
        clamped = !(sx >= margin && sx <= fb_w - margin &&
                    sy >= margin && sy <= fb_h - margin);
    }

    if (clamped) {
        const float sign = behind ? -1.0f : 1.0f;
        float dx = sign * right_dot;
        float dy = sign * up_dot;          // engine Y quirk: no flip
        const float dlen = std::sqrt(dx * dx + dy * dy);
        if (dlen < 1e-6f) { dx = 0.0f; dy = 1.0f; }
        else              { dx /= dlen; dy /= dlen; }
        const float max_x = cx - margin;
        const float max_y = cy - margin;
        const float tx = std::abs(dx) > 1e-6f ? max_x / std::abs(dx) : 1e9f;
        const float ty = std::abs(dy) > 1e-6f ? max_y / std::abs(dy) : 1e9f;
        const float t  = std::min(tx, ty);
        sx = cx + dx * t;
        sy = cy + dy * t;
    }

    auto* dl = ImGui::GetForegroundDrawList();
    constexpr float r = 14.0f;
    dl->AddCircle(ImVec2(sx, sy), r, kAmber, 0, 2.0f);
    const float tick_in = r * 0.45f, tick_out = r * 0.85f;
    dl->AddLine(ImVec2(sx - tick_out, sy), ImVec2(sx - tick_in, sy), kAmber, 2.0f);
    dl->AddLine(ImVec2(sx + tick_in,  sy), ImVec2(sx + tick_out, sy), kAmber, 2.0f);
    dl->AddLine(ImVec2(sx, sy - tick_out), ImVec2(sx, sy - tick_in), kAmber, 2.0f);
    dl->AddLine(ImVec2(sx, sy + tick_in),  ImVec2(sx, sy + tick_out), kAmber, 2.0f);

    if (clamped) {
        const float sign = behind ? -1.0f : 1.0f;
        float dx = sign * right_dot, dy = sign * up_dot;
        const float dlen = std::sqrt(dx * dx + dy * dy);
        if (dlen > 1e-6f) { dx /= dlen; dy /= dlen; }
        const float arr_d = r + 6.0f, arr_t = arr_d + 9.0f;
        const float perp_x = -dy, perp_y = dx;
        const ImVec2 tip { sx + dx * arr_t, sy + dy * arr_t };
        const ImVec2 b1  { sx + dx * arr_d + perp_x * 5.0f,
                           sy + dy * arr_d + perp_y * 5.0f };
        const ImVec2 b2  { sx + dx * arr_d - perp_x * 5.0f,
                           sy + dy * arr_d - perp_y * 5.0f };
        dl->AddTriangleFilled(tip, b1, b2, kAmber);
    }

    char buf[32];
    if (len < 10000.0f) std::snprintf(buf, sizeof(buf), "%.0f u",   len);
    else                std::snprintf(buf, sizeof(buf), "%.1f k u", len * 0.001f);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const bool below_ok = (sy + r + 4.0f + ts.y) < (fb_h - 4.0f);
    const float label_y = below_ok ? sy + r + 4.0f : sy - r - 4.0f - ts.y;
    dl->AddText(ImVec2(sx - ts.x * 0.5f, label_y), kAmber, buf);
}

// ---- target MFD (bottom-right) -------------------------------------------
//
// Compact data panel — TARGET name, DIST, AZ/EL. No graphs, no
// animation; the player's reading this during combat and doesn't
// need eye-candy fighting for attention.
// Bottom-right NAV panel — info on the currently-cycled nav point
// (selected with the N key). Renamed from "TARGET" since a real ship
// target panel now sits separately (top-right) — "target" should mean
// "the ship I'm shooting at", not "the nav point I'm flying to".
void draw_nav_mfd(const Camera& cam, const StarSystem& system, int selected_nav,
                  const char* dock_prompt, bool dock_ready) {
    const auto s = screen_size();
    const bool has_prompt = dock_prompt && dock_prompt[0] != '\0';
    // Grow the panel a touch when a docking prompt is showing so the
    // extra line doesn't clip under the AZ/EL row (np-9cu.1).
    const float w = 240.0f, margin = 16.0f;
    const float h = has_prompt ? 116.0f : 96.0f;

    ImGui::SetNextWindowPos(ImVec2(s.w - w - margin, s.h - h - margin),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    push_hud_style();

    if (ImGui::Begin("##nav_mfd", nullptr, kHudWindowFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("NAV");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (selected_nav < 0 || selected_nav >= (int)system.nav_points.size()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDimAmber);
            ImGui::TextUnformatted("[ NO NAV SELECTED ]");
            ImGui::TextUnformatted("press N to cycle");
            ImGui::PopStyleColor();
        } else {
            const auto& nav = system.nav_points[selected_nav];
            const HMM_Vec3 d = HMM_SubV3(nav.position, cam.position);
            const float len  = HMM_LenV3(d);

            const float fwd_dot   = HMM_DotV3(d, cam.forward());
            const float right_dot = HMM_DotV3(d, cam.right());
            const float up_dot    = HMM_DotV3(d, cam.up());
            constexpr float kRad2Deg = 57.2957795f;
            const float az_deg = std::atan2(right_dot, fwd_dot) * kRad2Deg;
            const float horiz  = std::sqrt(fwd_dot * fwd_dot + right_dot * right_dot);
            const float el_deg = std::atan2(up_dot, horiz) * kRad2Deg;

            std::string kind_up = nav.kind;
            for (char& c : kind_up) c = (char)std::toupper((unsigned char)c);

            ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
            ImGui::Text("%s  %s", kind_up.c_str(), nav.name.c_str());
            if (len < 10000.0f) ImGui::Text("DIST  %7.0f u",   len);
            else                ImGui::Text("DIST  %6.1f k u", len * 0.001f);
            ImGui::Text("AZ %+4.0f  EL %+3.0f", az_deg, el_deg);
            ImGui::PopStyleColor();

            // Docking feedback line (np-9cu.1). Green when cleared
            // ("PRESS D TO DOCK"), amber otherwise ("DOCK: TOO FAST").
            if (has_prompt) {
                ImGui::PushStyleColor(ImGuiCol_Text, dock_ready ? kGreen : kAmber);
                ImGui::TextUnformatted(dock_prompt);
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();
    pop_hud_style();
}

// ---- radar MFD (bottom-left) ---------------------------------------------
//
// Top-down radar in the camera's local frame: player at centre as a
// triangle, radar +Y = camera forward, radar +X = starboard. World-
// space nav points project onto the camera's right/forward plane and
// scale into the disc; beyond max_range, dots clamp to the rim with
// no extra fanfare. A slow sweep line (~one revolution per 4 s)
// keeps the HUD feeling 'live' even when nothing is moving.
//
// Top-right TARGET panel — info on the currently-locked SHIP target
// (T-key cycles). Shows a per-frame thumbnail of the target sprite
// (picked via the same camera-relative az/el lookup the main render
// uses), class label, faction stance, distance, and shield/armor
// breakdown per facing as horizontal bars.
//
// Distinct from the NAV panel (bottom-right) which tracks the
// nav-point N-key cycle. Both panels can be active simultaneously.
void draw_target_mfd(const Camera& cam, const ShipRegistry& ships,
                     uint32_t target_ship_id) {
    // Resolve target id -> Ship*. Linear scan; ships count is small.
    const Ship* target = ships.find_by_id(target_ship_id);

    const auto sz = screen_size();
    constexpr float w = 280.0f, h = 152.0f, margin = 16.0f;
    ImGui::SetNextWindowPos(ImVec2(sz.w - w - margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    push_hud_style();

    if (ImGui::Begin("##target_mfd", nullptr, kHudWindowFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("TARGET");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (!target || !target->alive) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDimAmber);
            ImGui::TextUnformatted("[ NO TARGET ]");
            ImGui::TextUnformatted("press T to cycle");
            ImGui::PopStyleColor();
        } else {
            // Left column: sprite thumbnail (if available). Use the
            // camera-relative cell selector so the thumbnail matches
            // what the player sees out the window.
            constexpr float thumb_w = 80.0f, thumb_h = 80.0f;
            if (target->sprite && target->sprite->atlas) {
                const ShipSpriteFrame* f = choose_ship_sprite_frame(
                    *target->sprite->atlas, *target->sprite, cam);
                if (f && f->art) {
                    ImGui::Image(simgui_imtextureid(f->art->hull.view),
                                 ImVec2(thumb_w, thumb_h));
                } else {
                    ImGui::Dummy(ImVec2(thumb_w, thumb_h));
                }
            } else {
                ImGui::Dummy(ImVec2(thumb_w, thumb_h));
            }
            ImGui::SameLine();

            // Right column: identity + range + stance + HP bars.
            ImGui::BeginGroup();

            const char* class_name = target->klass
                ? target->klass->display_name.c_str()
                : (target->is_player ? "PLAYER" : "?");
            ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
            ImGui::Text("%s", class_name);
            ImGui::PopStyleColor();

            // Faction + stance — find the player's perception entry to
            // get the stance the AI uses (so target-panel colors match
            // the on-screen indicator + radar). Distance from the
            // contact entry too — already filtered by radar range.
            const char*  fac_name = target->klass
                ? faction::to_name(target->faction) : "?";
            float        dist_m   = 0.0f;
            Stance       stance   = Stance::Neutral;
            if (const Ship* player = ships.player(); player) {
                for (const PerceivedContact& c : player->perception.visible) {
                    if (c.ship_id == target_ship_id) {
                        dist_m = c.distance_m;
                        stance = c.stance;
                        break;
                    }
                }
            }
            const ImU32 stance_col =
                (stance == Stance::Hostile) ? IM_COL32(255,  90,  90, 255)
              : (stance == Stance::Allied)  ? IM_COL32( 90, 255, 110, 255)
              :                                IM_COL32(255, 220,  60, 255);
            const char* stance_str =
                (stance == Stance::Hostile) ? "HOSTILE"
              : (stance == Stance::Allied)  ? "ALLIED"
              :                                "NEUTRAL";

            ImGui::PushStyleColor(ImGuiCol_Text, stance_col);
            ImGui::Text("%s  %s", fac_name, stance_str);
            ImGui::PopStyleColor();

            ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
            if (dist_m < 10000.0f) ImGui::Text("DIST  %6.0f m",  dist_m);
            else                   ImGui::Text("DIST  %5.1f km", dist_m * 0.001f);
            ImGui::PopStyleColor();

            ImGui::EndGroup();

            // Bottom: per-facing shield + armor bars. F / A / S labels
            // (Fore / Aft / Side); each bar fills proportionally to
            // current vs. max for that facing. Shield max comes from
            // the fitted ShieldType, armor max from class hull +
            // fitted ArmorType.
            ImGui::Separator();
            const ShipClass* k = target->klass;
            float shield_max[3] = {0,0,0}, armor_max[3] = {0,0,0};
            if (k) {
                if (k->default_shield) {
                    shield_max[0] = k->default_shield->front_cm * target->shield_mult;
                    shield_max[1] = k->default_shield->back_cm  * target->shield_mult;
                    shield_max[2] = k->default_shield->side_cm  * target->shield_mult;
                }
                armor_max[0] = k->armor_fore_cm;
                armor_max[1] = k->armor_aft_cm;
                armor_max[2] = k->armor_side_cm;
                if (k->default_armor) {
                    armor_max[0] += k->default_armor->front_cm;
                    armor_max[1] += k->default_armor->back_cm;
                    armor_max[2] += k->default_armor->side_cm;
                }
            }
            const float shield_cur[3] = { target->shield_fore_cm,
                                          target->shield_aft_cm,
                                          target->shield_side_cm };
            const float armor_cur[3]  = { target->armor_fore_cm,
                                          target->armor_aft_cm,
                                          target->armor_side_cm };
            const char* facing_lbl[3] = { "F", "A", "S" };

            ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(20,20,30,180));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  IM_COL32(80,160,255,220));
            for (int i = 0; i < 3; ++i) {
                const float frac = shield_max[i] > 0.0f
                    ? std::clamp(shield_cur[i] / shield_max[i], 0.0f, 1.0f) : 0.0f;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "S%s %.0f/%.0f",
                              facing_lbl[i], shield_cur[i], shield_max[i]);
                ImGui::ProgressBar(frac, ImVec2(80.0f, 14.0f), buf);
                if (i < 2) ImGui::SameLine();
            }
            ImGui::PopStyleColor(2);

            ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(20,20,30,180));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  IM_COL32(255,140,60,220));
            for (int i = 0; i < 3; ++i) {
                const float frac = armor_max[i] > 0.0f
                    ? std::clamp(armor_cur[i] / armor_max[i], 0.0f, 1.0f) : 0.0f;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "A%s %.0f/%.0f",
                              facing_lbl[i], armor_cur[i], armor_max[i]);
                ImGui::ProgressBar(frac, ImVec2(80.0f, 14.0f), buf);
                if (i < 2) ImGui::SameLine();
            }
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::End();
    pop_hud_style();
}

// Top-left STATUS panel — player ship's hull integrity at a glance.
// Mirrors the TARGET panel's layout (class label + 6 progress bars
// for shield + armor per facing) but for the registry's player. No
// thumbnail since the player has no sprite. Energy displayed as a
// numeric current/max instead of a bar — energy ticks fast enough
// during sustained fire that a bar would just look noisy.
void draw_player_status(const ShipRegistry& ships) {
    const Ship* player_p = ships.player();
    if (!player_p) return;
    const Ship& player = *player_p;

    const auto sz = screen_size();
    constexpr float w = 280.0f, h = 184.0f, margin = 16.0f;
    ImGui::SetNextWindowPos(ImVec2(margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    push_hud_style();

    if (ImGui::Begin("##player_status", nullptr, kHudWindowFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("STATUS");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (!player.alive) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 80, 80, 255));
            ImGui::TextUnformatted("*** DESTROYED ***");
            ImGui::PopStyleColor();
        } else {
            // Header line: class name.
            const char* class_name = player.klass
                ? player.klass->display_name.c_str() : "PLAYER";
            ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
            ImGui::Text("%s", class_name);
            ImGui::PopStyleColor();

            // Energy bar — full width, yellow. Drains during sustained
            // fire, recharges at klass->energy_recharge per second.
            // Visible bar makes it easy to see when you're about to
            // run dry mid-burst.
            const float energy_max = player.klass ? player.klass->energy_max : 0.0f;
            const float e_frac = energy_max > 0.0f
                ? std::clamp(player.energy_gj / energy_max, 0.0f, 1.0f) : 0.0f;
            char ebuf[32];
            std::snprintf(ebuf, sizeof(ebuf), "ENERGY  %.0f / %.0f GJ",
                          player.energy_gj, energy_max);
            ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(20,20,30,180));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  IM_COL32(255,210,60,220));
            ImGui::ProgressBar(e_frac, ImVec2(-1.0f, 14.0f), ebuf);
            ImGui::PopStyleColor(2);

            ImGui::Separator();

            // Per-facing maxes (same math as target panel).
            const ShipClass* k = player.klass;
            float shield_max[3] = {0,0,0}, armor_max[3] = {0,0,0};
            if (k) {
                if (k->default_shield) {
                    shield_max[0] = k->default_shield->front_cm * player.shield_mult;
                    shield_max[1] = k->default_shield->back_cm  * player.shield_mult;
                    shield_max[2] = k->default_shield->side_cm  * player.shield_mult;
                }
                armor_max[0] = k->armor_fore_cm;
                armor_max[1] = k->armor_aft_cm;
                armor_max[2] = k->armor_side_cm;
                if (k->default_armor) {
                    armor_max[0] += k->default_armor->front_cm;
                    armor_max[1] += k->default_armor->back_cm;
                    armor_max[2] += k->default_armor->side_cm;
                }
            }
            const float shield_cur[3] = { player.shield_fore_cm,
                                          player.shield_aft_cm,
                                          player.shield_side_cm };
            const float armor_cur[3]  = { player.armor_fore_cm,
                                          player.armor_aft_cm,
                                          player.armor_side_cm };
            const char* facing_lbl[3] = { "F", "A", "S" };

            ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(20,20,30,180));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  IM_COL32(80,160,255,220));
            for (int i = 0; i < 3; ++i) {
                const float frac = shield_max[i] > 0.0f
                    ? std::clamp(shield_cur[i] / shield_max[i], 0.0f, 1.0f) : 0.0f;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "S%s %.0f/%.0f",
                              facing_lbl[i], shield_cur[i], shield_max[i]);
                ImGui::ProgressBar(frac, ImVec2(80.0f, 14.0f), buf);
                if (i < 2) ImGui::SameLine();
            }
            ImGui::PopStyleColor(2);

            ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(20,20,30,180));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  IM_COL32(255,140,60,220));
            for (int i = 0; i < 3; ++i) {
                const float frac = armor_max[i] > 0.0f
                    ? std::clamp(armor_cur[i] / armor_max[i], 0.0f, 1.0f) : 0.0f;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "A%s %.0f/%.0f",
                              facing_lbl[i], armor_cur[i], armor_max[i]);
                ImGui::ProgressBar(frac, ImVec2(80.0f, 14.0f), buf);
                if (i < 2) ImGui::SameLine();
            }
            ImGui::PopStyleColor(2);

            // Gun arm-mode (np-3dp). Line under the armor bars so the
            // player sees at a glance which mode G picked and how many
            // mounts it actually enables. Empty mounts are suppressed so
            // the line disappears for ships with zero guns (rare, but
            // happens for tutorial / scout hulls).
            const int n_mounts = (int)player.mounts.size();
            if (n_mounts > 0 && !player.gun_armed.empty()) {
                const std::vector<int>& u =
                    firing::gun_unique_types_cache(player.mounts);
                const char* label  = firing::gun_mode_label(u, player.gun_mode_idx);
                const int    armed = firing::gun_mode_armed_count(player);
                ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
                ImGui::Text("GUNS: %s  (%d of %d armed)",
                            label, armed, n_mounts);
                ImGui::PopStyleColor();
            }
        }
    }
    ImGui::End();
    pop_hud_style();
}

void draw_radar_mfd(const Camera& cam, const StarSystem& system, int selected_nav,
                    const ShipRegistry& ships, uint32_t target_ship_id) {
    const auto s = screen_size();
    constexpr float w = 168.0f, h = 168.0f, margin = 16.0f;

    ImGui::SetNextWindowPos(ImVec2(margin, s.h - h - margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border,   kAmber);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.0f, 0.0f));

    if (ImGui::Begin("##radar_mfd", nullptr, kHudWindowFlags)) {
        ImVec2      p0 = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ImVec2 ctr  = ImVec2(p0.x + w * 0.5f, p0.y + h * 0.5f);
        const float  rad  = std::min(w, h) * 0.5f - 6.0f;
        // np-3dp: 15 km radar radius so local ships read clearly (matches
        // the player's detection sphere set in perception.cpp). Anything
        // past 15k clamps to the rim, so nav points 100+ km away overlap
        // at the edge — that's the intended trade: the MFD is a "where's
        // the contact NEAR me" display, not a system overview.
        constexpr float max_range = 15000.0f;       // u — beyond 15km, clamp to rim

        // Concentric range rings + crosshair. Drawn before sweep so the
        // sweep line passes over them.
        static const ImU32 grid     = IM_COL32(120, 160, 130,  90);
        static const ImU32 grid_dim = IM_COL32(120, 160, 130,  50);
        dl->AddCircle(ctr, rad,         grid,     0, 1.0f);
        dl->AddCircle(ctr, rad * 0.66f, grid_dim, 0, 1.0f);
        dl->AddCircle(ctr, rad * 0.33f, grid_dim, 0, 1.0f);
        dl->AddLine(ImVec2(ctr.x - rad, ctr.y), ImVec2(ctr.x + rad, ctr.y), grid_dim, 1.0f);
        dl->AddLine(ImVec2(ctr.x, ctr.y - rad), ImVec2(ctr.x, ctr.y + rad), grid_dim, 1.0f);

        // Sweep — 12 lines at decreasing alpha to fake a comet trail.
        // Cheap and reads as 'rotating beam' instantly.
        const float t = (float)ImGui::GetTime();
        const float sweep_a = std::fmod(t * 0.25f, 1.0f) * 6.2831853f - 1.5707963f;
        for (int i = 0; i < 12; ++i) {
            const float a   = sweep_a - i * 0.04f;
            const float ca  = std::cos(a), sa = std::sin(a);
            const int   alf = (int)(180 * (1.0f - i / 12.0f));
            const ImU32 col = IM_COL32(140, 220, 150, alf);
            dl->AddLine(ctr,
                        ImVec2(ctr.x + ca * rad, ctr.y + sa * rad),
                        col, 1.5f);
        }

        // Plot every nav point. Camera-relative (right_dot, fwd_dot) maps
        // straight onto the radar's (X, -Y) plane: forward = up on screen.
        for (int i = 0; i < (int)system.nav_points.size(); ++i) {
            const auto& nav = system.nav_points[i];
            const HMM_Vec3 d = HMM_SubV3(nav.position, cam.position);
            const float fwd_dot   = HMM_DotV3(d, cam.forward());
            const float right_dot = HMM_DotV3(d, cam.right());
            const float plane_len = std::sqrt(fwd_dot * fwd_dot + right_dot * right_dot);

            float dx_norm, dy_norm;
            if (plane_len < 1.0f) { dx_norm = 0.0f; dy_norm = 0.0f; }
            else                  { dx_norm =  right_dot / plane_len;
                                    dy_norm = -fwd_dot   / plane_len; }   // forward = up on radar

            const float r_norm = std::min(plane_len / max_range, 1.0f);
            const ImVec2 dot { ctr.x + dx_norm * r_norm * rad,
                               ctr.y + dy_norm * r_norm * rad };
            const ImU32  col = color_for_kind(nav.kind);
            const bool   sel = (i == selected_nav);
            const float  dot_r = sel ? 4.5f : 2.5f;
            dl->AddCircleFilled(dot, dot_r, col);
            if (sel) dl->AddCircle(dot, dot_r + 2.5f, kAmber, 0, 1.5f);
        }

        // Plot ship contacts from the player's perception. Same
        // camera-relative projection as the nav loop above; stance
        // colors mirror the on-screen target indicator (red=hostile,
        // green=allied, yellow=neutral) so the radar reads at a glance.
        // Ships at < ~14% of radar (35 km vs 250 km) cluster near
        // center — that's the steady-state engagement bubble; nav
        // points spread out farther because they're system-scale
        // (planets, jump points 100+ km away).
        if (const Ship* player_p = ships.player(); player_p) {
            const Ship& player = *player_p;
            for (const PerceivedContact& c : player.perception.visible) {
                // Reconstruct world position from cached unit + distance.
                const HMM_Vec3 contact_pos =
                    HMM_AddV3(player.position, HMM_MulV3F(c.to_unit, c.distance_m));
                const HMM_Vec3 d = HMM_SubV3(contact_pos, cam.position);
                const float fwd_dot   = HMM_DotV3(d, cam.forward());
                const float right_dot = HMM_DotV3(d, cam.right());
                const float plane_len = std::sqrt(fwd_dot * fwd_dot + right_dot * right_dot);

                float dx_norm, dy_norm;
                if (plane_len < 1.0f) { dx_norm = 0.0f; dy_norm = 0.0f; }
                else                  { dx_norm =  right_dot / plane_len;
                                        dy_norm = -fwd_dot   / plane_len; }

                const float r_norm = std::min(plane_len / max_range, 1.0f);
                const ImVec2 dot { ctr.x + dx_norm * r_norm * rad,
                                   ctr.y + dy_norm * r_norm * rad };

                const ImU32 col =
                    (c.stance == Stance::Hostile) ? IM_COL32(255,  90,  90, 255)
                  : (c.stance == Stance::Allied)  ? IM_COL32( 90, 255, 110, 255)
                  :                                 IM_COL32(255, 220,  60, 255);
                const bool   sel = (c.ship_id == target_ship_id);
                const float  dot_r = sel ? 4.0f : 2.5f;
                dl->AddCircleFilled(dot, dot_r, col);
                if (sel) dl->AddCircle(dot, dot_r + 2.5f, kAmber, 0, 1.5f);
            }
        }

        // Player ship — small triangle at centre pointing 'up' (forward).
        const ImVec2 p_tip { ctr.x,        ctr.y - 6.0f };
        const ImVec2 p_bl  { ctr.x - 4.0f, ctr.y + 4.0f };
        const ImVec2 p_br  { ctr.x + 4.0f, ctr.y + 4.0f };
        dl->AddTriangleFilled(p_tip, p_bl, p_br, kHudWhite);

        // Faint label so first-time players know what they're looking at.
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f), kDimAmber, "RADAR");
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ---- nav-point name labels ----------------------------------------------
//
// Floats the name of EVERY nav point next to its projected screen
// position. Distinct from draw_nav_reticle (which only marks the
// CURRENTLY-TARGETED nav with an amber crosshair): this is a permanent
// "what is this thing" overlay, useful for the mesh_showroom scene
// where one nav point is auto-generated per ship and you want to know
// which hull is which at a glance.
//
// Cheap projection: we skip anything behind the camera or far outside
// the view frustum so 100-nav scenes don't pay for off-screen draws.
// No edge-clamping — labels don't make sense floating against the
// window border the way a target reticle does.
void draw_nav_labels(const Camera& cam, const StarSystem& system) {
    if (system.nav_points.empty()) return;

    const auto     s      = screen_size();
    const float    fb_w   = s.w, fb_h = s.h;
    const float    aspect = fb_w / fb_h;
    const HMM_Mat4 vp     = HMM_MulM4(cam.projection(aspect), cam.view());
    auto*          dl     = ImGui::GetForegroundDrawList();

    for (const auto& nav : system.nav_points) {
        const HMM_Vec3 d       = HMM_SubV3(nav.position, cam.position);
        const float    fwd_dot = HMM_DotV3(d, cam.forward());
        if (fwd_dot <= 0.0f) continue;                  // behind camera

        const HMM_Vec4 ph   = { nav.position.X, nav.position.Y, nav.position.Z, 1.0f };
        const HMM_Vec4 clip = HMM_MulM4V4(vp, ph);
        if (clip.W <= 0.0f) continue;
        const float ndc_x = clip.X / clip.W;
        const float ndc_y = clip.Y / clip.W;
        // 1.15× frustum slop — labels just barely off-screen don't pop
        // in/out as the camera turns, but we cull aggressively beyond.
        if (ndc_x < -1.15f || ndc_x > 1.15f) continue;
        if (ndc_y < -1.15f || ndc_y > 1.15f) continue;

        const float sx = (ndc_x * 0.5f + 0.5f) * fb_w;
        const float sy = (ndc_y * 0.5f + 0.5f) * fb_h;

        // Tiny dot at the exact position, name floating just above.
        dl->AddCircleFilled(ImVec2(sx, sy), 2.5f, kDimAmber);
        const char*  name  = nav.name.c_str();
        const ImVec2 tsize = ImGui::CalcTextSize(name);
        dl->AddText(ImVec2(sx - tsize.x * 0.5f, sy - tsize.y - 6.0f),
                    kHudWhite, name);
    }
}

} // anonymous namespace

// Top-centre FLIGHT panel. Mirrors STATUS / TARGET in style (dark bg +
// amber border, amber title, separator) so the data-at-the-top reads as
// part of the same HUD vocabulary instead of free-floating text. Public
// (not in the anon namespace) so main.cpp can drive it with the live
// camera + autopilot snapshot every flight frame.
void draw_flight_status_mfd(const FlightStatusHudState& s) {
    const auto sz = screen_size();
    constexpr float w = 280.0f, margin = 16.0f;
    // Height grows if autopilot rows are present so the box always frames
    // the content snugly.
    float h = 110.0f;
    if (s.autopilot_nav) h += 18.0f;
    if (s.autopilot_msg) h += 18.0f;
    ImGui::SetNextWindowPos(ImVec2(sz.w * 0.5f - w * 0.5f, margin),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    push_hud_style();

    if (ImGui::Begin("##flight_status", nullptr, kHudWindowFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("FLIGHT");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Text("SPEED  %7.0f u/s", s.speed);
        ImGui::Text("MODE   %s",        s.mode);
        ImGui::Text("D(SUN) %7.0f u",   s.d_sun);
        ImGui::Text("POS    %5.0f %5.0f %5.0f", s.pos_x, s.pos_y, s.pos_z);
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

void build(const Camera& cam, const StarSystem& system, int selected_nav,
           float mouse_x, float mouse_y, bool fly_by_wire,
           const ShipRegistry& ships, uint32_t target_ship_id,
           const char* dock_prompt, bool dock_ready) {
    draw_crosshair(fly_by_wire);
    draw_aim_cursor(mouse_x, mouse_y, fly_by_wire);
    draw_nav_reticle(cam, system, selected_nav);
    draw_nav_labels (cam, system);
    draw_player_status(ships);
    draw_nav_mfd   (cam, system, selected_nav, dock_prompt, dock_ready);
    draw_target_mfd(cam, ships, target_ship_id);
    draw_radar_mfd (cam, system, selected_nav, ships, target_ship_id);
}

void build_weapons_status(const WeaponsHudState& w) {
    const ScreenSize ss = screen_size();
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Anchored above the bottom-left radar MFD, left edge. Three short
    // lines + a gauge; foreground drawlist so it never steals input.
    const float x = 24.0f;
    float       y = ss.h - 280.0f;
    if (y < 70.0f) y = 70.0f;   // tiny windows: don't collide with the top

    char line[64];

    // ---- missile readout: "MSL  IR x3" ---------------------------------
    const ImU32 ammo_col = (w.missile_count > 0) ? kAmber : kDimAmber;
    std::snprintf(line, sizeof(line), "MSL  %s x%d", w.missile_name, w.missile_count);
    dl->AddText(ImVec2(x, y), ammo_col, line);
    y += 18.0f;

    // ---- lock state -----------------------------------------------------
    // DF (no lock) just shows "DUMBFIRE"; HS/IR show seeking/locked with
    // an IR build-up bar so the ~1.5s acquire is visible, not mysterious.
    if (!w.needs_lock) {
        dl->AddText(ImVec2(x, y), kDimAmber, "DUMBFIRE");
    } else if (w.lock_state == 2) {
        dl->AddText(ImVec2(x, y), kGreen, "LOCKED");
    } else if (w.lock_state == 1) {
        dl->AddText(ImVec2(x, y), kCyan, "LOCK\xE2\x80\xA6");   // "LOCK…"
        // Build-up bar to the right of the label.
        const float bx = x + 64.0f, bw = 80.0f, bh = 8.0f;
        dl->AddRect(ImVec2(bx, y + 2.0f), ImVec2(bx + bw, y + 2.0f + bh), kDimAmber);
        const float f = std::clamp(w.lock_progress, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(bx + 1, y + 3.0f),
                          ImVec2(bx + 1 + (bw - 2) * f, y + 1.0f + bh), kCyan);
    } else {
        dl->AddText(ImVec2(x, y), kDimAmber, "NO TARGET");
    }
    y += 22.0f;

    // Afterburner fuel gauge removed (np-zte.2 merged pool). The STATUS
    // panel's ENERGY bar is the burner gauge now — same pool, one
    // readout. Don't re-add a bar here unless we re-split the resources.
    (void)y;
}

// ---- big navmap overlay --------------------------------------------------
//
// Centered fullscreen-ish window with a top-down projection of the
// system. Visual style follows classic Privateer (np-7gr):
//
//   * 7x7 tactical grid   - drawn behind the markers, faint blue
//   * Squares = BASES     - any dockable nav point (stations/planets)
//   * Blue circles = JUMP - kind=="jump" nav points
//   * Green circles = NAV - empty waypoints (kind=="nav")
//   * Player marker       - small white triangle, nose-aligned
//
// World-space (X, Z) -> map (X, -Z). World X runs across the map;
// world Z runs vertically but INVERTED so a larger Z moves a nav UP
// (scientific axes, not image top-left origin). NB: the system JSON
// has wcpedia columns 2 & 3 swapped, so the designer's intended
// vertical coordinate lives in position.Z, which is why we plot Z
// (not Y) on the vertical axis. World-up (cam.up) ignored — this is a
// system-overhead map, not a 3D viewport.
void build_navmap(const Camera& cam, const StarSystem& system,
                  int& selected_nav_in_out,
                  const ShipRegistry& ships,
                  bool& shown_in_out) {
    if (!shown_in_out) return;

    const auto sz = screen_size();
    // Square window (np-7gr.2): use min(sz.w, sz.h) so the navmap
    // is always square. 92% of the min dimension so it fills more of
    // the screen than the old 85%-of-each-axis sizing. Capped at 1100
    // so it doesn't get unwieldy on huge monitors.
    const float edge_raw = std::min(sz.w, sz.h) * 0.92f;
    const float edge     = std::min(edge_raw, 1100.0f);
    ImGui::SetNextWindowPos(ImVec2((sz.w - edge) * 0.5f,
                                   (sz.h - edge) * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(edge, edge), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    push_hud_style();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    if (ImGui::Begin("NAVIGATION MAP   (N cycles, Esc to close)", &open, flags)) {

        // Optional authored 2D navmap layout. Real gameplay keeps using
        // NavPointDef::position; this map can use NavPointDef::map_position
        // when the legacy source image is a hand layout instead of a literal
        // 3D projection (hello, Troy, you beautiful little fraud).
        auto nav_map_pos = [](const NavPointDef& nv) -> HMM_Vec2 {
            return navmap_project_nav(nv);
        };
        auto world_map_pos = [](const HMM_Vec3& p) -> HMM_Vec2 {
            return navmap_project_world(p);
        };

        // Compute a square bbox around the MAP ORIGIN. Origin-centred
        // framing avoids the old centroid-fit bug where x=0 drifted to an
        // edge when all coordinates were positive. Map-Y is scientific:
        // larger values render UP, handled by the minus in to_screen().
        HMM_Vec2 cam_mp = world_map_pos(cam.position);
        float min_x = cam_mp.X, max_x = cam_mp.X;
        float min_y = cam_mp.Y, max_y = cam_mp.Y;
        for (const auto& nv : system.nav_points) {
            const HMM_Vec2 mp = nav_map_pos(nv);
            if (mp.X < min_x) min_x = mp.X;
            if (mp.X > max_x) max_x = mp.X;
            if (mp.Y < min_y) min_y = mp.Y;
            if (mp.Y > max_y) max_y = mp.Y;
        }
        float half = 1.0f;
        half = std::max(half, std::fabs(min_x));
        half = std::max(half, std::fabs(max_x));
        half = std::max(half, std::fabs(min_y));
        half = std::max(half, std::fabs(max_y));
        half *= 1.05f;                                      // 5% edge buffer
        min_x = -half; max_x = half;
        min_y = -half; max_y = half;
        const float span = half * 2.0f;                     // now square

        // Square map area: take the smaller content-region dimension
        // as the edge so the area is always square, then center it
        // within whatever space remains.
        const ImVec2 area_p0 = ImGui::GetCursorScreenPos();
        const ImVec2 area_sz = ImGui::GetContentRegionAvail();
        const float  edge = std::min(area_sz.x, area_sz.y);
        const float  sq_ox = (area_sz.x - edge) * 0.5f;
        const float  sq_oy = (area_sz.y - edge) * 0.5f;
        const ImVec2 sq_p0 { area_p0.x + sq_ox, area_p0.y + sq_oy };
        const ImVec2 sq_sz { edge, edge };
        // Uniform scale against the now-square map span.
        const float scale = 0.92f * edge / span;
        const float cx_w = 0.5f * (min_x + max_x);
        const float cy_w = 0.5f * (min_y + max_y);
        const ImVec2 ctr { sq_p0.x + sq_sz.x * 0.5f,
                           sq_p0.y + sq_sz.y * 0.5f };
        // NOTE the MINUS on the vertical term: screen-Y grows downward,
        // but map-Y is scientific and grows upward.
        auto to_screen = [&](float wx, float wy) {
            return ImVec2(ctr.x + (wx - cx_w) * scale,
                          ctr.y - (wy - cy_w) * scale);
        };

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background panel (the window bg already fills it; this is
        // the actual map area outline). Square frame within the
        // content region.
        dl->AddRect(sq_p0,
                    ImVec2(sq_p0.x + sq_sz.x, sq_p0.y + sq_sz.y),
                    IM_COL32(80, 100, 120, 200), 0.0f, 0, 1.0f);

        // Privateer-style tactical grid (np-7gr). 7x7 cells mirrors
        // the classic navmap; each axis is divided into 7 segments so
        // there are 8 grid lines per axis. Drawn behind the markers
        // and the ship-contact pips so it reads as a tactical overlay
        // rather than competing with the dots. After squaring the bbox
        // both axes share the SAME span (`span` = padded_longest), so
        // cell_w == cell_h — the grid is square in both world and
        // screen space, and the stretched axis naturally determines
        // the cell size for both.
        constexpr int kGridDivs = 7;
        const float cell_w = span / float(kGridDivs);
        const float cell_h = span / float(kGridDivs);
        for (int i = 0; i <= kGridDivs; ++i) {
            const float wx = min_x + i * cell_w;
            const ImVec2 a = to_screen(wx, min_y);
            const ImVec2 b = to_screen(wx, max_y);
            dl->AddLine(a, b, kGridLine, 1.0f);
        }
        for (int j = 0; j <= kGridDivs; ++j) {
            const float wy = min_y + j * cell_h;
            const ImVec2 a = to_screen(min_x, wy);
            const ImVec2 b = to_screen(max_x, wy);
            dl->AddLine(a, b, kGridLine, 1.0f);
        }

        // Ship contacts under nav points so navs don't get hidden by
        // densely packed ship dots.
        if (const Ship* player_p = ships.player(); player_p) {
            const Ship& player = *player_p;
            for (const PerceivedContact& c : player.perception.visible) {
                const HMM_Vec3 p = HMM_AddV3(
                    player.position, HMM_MulV3F(c.to_unit, c.distance_m));
                const HMM_Vec2 mp = world_map_pos(p);
                const ImVec2 sp = to_screen(mp.X, mp.Y);
                const ImU32 col =
                    (c.stance == Stance::Hostile) ? IM_COL32(255,  90,  90, 230)
                  : (c.stance == Stance::Allied)  ? IM_COL32( 90, 255, 110, 230)
                  :                                  IM_COL32(255, 220,  60, 230);
                dl->AddCircleFilled(sp, 3.0f, col, 8);
            }
        }

        // Nav points — clickable. dockable things render as filled
        // SQUARES (bases); kind=="jump" renders as a blue CIRCLE (jump
        // hole); anything else renders as a green CIRCLE (nav point).
        // Squares use the kind colour for fill (so stations read green
        // and planets read blue) and get a white outline that flips to
        // amber when selected. Hit-test shape matches the marker.
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool mouse_in_panel =
            mouse.x >= area_p0.x && mouse.x <= area_p0.x + area_sz.x &&
            mouse.y >= area_p0.y && mouse.y <= area_p0.y + area_sz.y;
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                          && mouse_in_panel;
        for (int i = 0; i < (int)system.nav_points.size(); ++i) {
            const auto& nv = system.nav_points[i];
            const HMM_Vec2 mp = nav_map_pos(nv);
            const ImVec2 sp = to_screen(mp.X, mp.Y);
            const bool   sel = (i == selected_nav_in_out);

            const bool is_square = nv.dockable;
            // Squares sit at ~7/9 px and circles at ~5/8 px so the
            // selected/unselected pair reads at a glance.
            const float r   = sel ? (is_square ? 9.0f : 8.0f)
                                  : (is_square ? 7.0f : 5.0f);
            ImU32       fill;
            if      (is_square)            fill = color_for_kind(nv.kind);
            else if (nv.kind == "jump")   fill = kJumpBlue;
            else                            fill = kNavGreen;

            if (is_square) {
                const ImVec2 a { sp.x - r, sp.y - r };
                const ImVec2 b { sp.x + r, sp.y + r };
                dl->AddRectFilled(a, b, fill);
                // Outline: amber when selected (thicker), white otherwise.
                dl->AddRect(a, b, sel ? kAmber : kHudWhite, 0.0f, 0,
                            sel ? 2.0f : 1.0f);
            } else {
                dl->AddCircleFilled(sp, r, fill, 16);
                if (sel) dl->AddCircle(sp, r + 3.0f, kAmber, 0, 1.5f);
            }

            // Label — same offset regardless of marker shape; text
            // hovers to the right of whatever the centre is.
            dl->AddText(ImVec2(sp.x + r + 4.0f, sp.y - 7.0f),
                        kHudWhite, nv.name.c_str());

            // Click hit-test. Square uses a bounding-box test against
            // the marker; circle uses the existing radial test.
            if (clicked) {
                const float dxs = mouse.x - sp.x;
                const float dys = mouse.y - sp.y;
                if (is_square) {
                    if (std::abs(dxs) <= r + 4.0f && std::abs(dys) <= r + 4.0f) {
                        selected_nav_in_out = i;
                    }
                } else {
                    if (dxs * dxs + dys * dys < (r + 8.0f) * (r + 8.0f)) {
                        selected_nav_in_out = i;
                    }
                }
            }
        }

        // Player marker — small triangle at camera position. Position and
        // heading both pass through world_map_pos(), keeping the live marker
        // on the same X/Z projection as navs and ship contacts.
        const HMM_Vec2 player_mp = world_map_pos(cam.position);
        const ImVec2 pp = to_screen(player_mp.X, player_mp.Y);
        const HMM_Vec3 cf = cam.forward();
        const HMM_Vec3 ahead_world = HMM_AddV3(cam.position, HMM_MulV3F(cf, 1000.0f));
        const HMM_Vec2 ahead_mp = world_map_pos(ahead_world);
        const ImVec2 ahead_sp = to_screen(ahead_mp.X, ahead_mp.Y);
        const float fx = ahead_sp.x - pp.x;
        const float fy = ahead_sp.y - pp.y;
        const float fl = std::sqrt(fx * fx + fy * fy);
        const float ux = (fl > 1e-3f) ? (fx / fl) : 0.0f;
        const float uy = (fl > 1e-3f) ? (fy / fl) : 1.0f;
        constexpr float kSize = 9.0f;
        const ImVec2 tip { pp.x + ux * kSize,           pp.y + uy * kSize };
        const ImVec2 bl  { pp.x - ux * kSize * 0.4f - uy * kSize * 0.6f,
                            pp.y - uy * kSize * 0.4f + ux * kSize * 0.6f };
        const ImVec2 br  { pp.x - ux * kSize * 0.4f + uy * kSize * 0.6f,
                            pp.y - uy * kSize * 0.4f - ux * kSize * 0.6f };
        dl->AddTriangleFilled(tip, bl, br, kHudWhite);

        // Footer help.
        ImGui::SetCursorScreenPos(ImVec2(area_p0.x, area_p0.y + area_sz.y + 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kDimAmber);
        ImGui::TextUnformatted("Click a nav point to select it.  N cycles; Esc / X to close.");
        ImGui::PopStyleColor();
    }
    ImGui::End();
    pop_hud_style();

    // ESC closes too. Title-bar X also flips `open` to false.
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        shown_in_out = false;
    }
}

// -----------------------------------------------------------------------------
// draw_sun_warning -- centre-screen banner for the sun proximity rules.
// No-op when outside the 20k avoid bubble; yellow "WARNING" between 15-20k,
// big red "DESTRUCTION IMMINENT" inside the 15k damage zone with a pulsing
// border + flashing text. Drawn LAST so it overlays everything else.
// -----------------------------------------------------------------------------
void draw_sun_warning(const Camera& cam, HMM_Vec3 sun_pos) {
    // Cheap out when outside the avoid bubble entirely.
    if (!hazards::inside_sun_avoid(sun_pos, cam.position)) return;
    const float dist = HMM_LenV3(HMM_SubV3(sun_pos, cam.position));
    const bool  danger = (dist < hazards::k_sun_damage_radius_m);
    if (!danger && dist >= hazards::k_sun_avoid_radius_m) return;   // outside ring

    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x;
    const float h = io.DisplaySize.y;

    // Use the foreground draw list so we paint over EVERY ImGui panel.
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // Per-frame "pulse" so the red flash feels alive. Cheap sin on time.
    const float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4.5f);

    // Colour picks: big yellow "WARNING - APPROACHING SUN" between 15-20k,
    // big red "DESTRUCTION IMMINENT" inside 15k.
    ImU32 col_text, col_border, col_block;
    const char* line1;
    char        line2_buf[64];
    const char* line2;

    if (danger) {
        // INSIDE 15k -- destructive. Pulsing red on red.
        col_text   = IM_COL32(255,  60,  60, (int)(220 + 35 * pulse));   // pulsing bright red
        col_border = IM_COL32(255,  40,  40, (int)(180 + 75 * pulse));   // pulsing red border
        col_block  = IM_COL32( 90,   0,   0, (int)(140 +  80 * pulse));   // pulsing dark fill
        line1     = "!!! DESTRUCTION IMMINENT !!!";
        std::snprintf(line2_buf, sizeof(line2_buf),
                      "%.1f km FROM STAR  --  EVACUATE", dist / 1000.0f);
        line2 = line2_buf;
    } else {
        // 15k..20k -- caution. Yellow/orange.
        col_text   = IM_COL32(255, 200,  40, 230);                         // amber text
        col_border = IM_COL32(255, 160,  40, 200);                         // orange border
        col_block  = IM_COL32( 80,  60,   0, 140);                         // dark orange fill
        line1     = "WARNING  --  APPROACHING STAR";
        std::snprintf(line2_buf, sizeof(line2_buf),
                      "%.1f km  --  TURN AWAY", dist / 1000.0f);
        line2 = line2_buf;
    }

    // Layout: centred around y = h * 0.32 so the banner sits below the
    // top edge and well clear of the FLIGHT panel + centre reticle.
    const float cx = w * 0.5f;
    const float cy = h * 0.32f;

    // Big + sub font sizes. Both fed to the (font,size) AddText overload
    // so the rendered pixels match the size we ask for. CalcTextSize uses
    // the active font's size (default ~13 px), so we measure at default
    // and scale to the target size proportionally.
    constexpr float kBig = 38.0f;        // big line at this pixel size
    constexpr float kSub = 20.0f;        // sub-line at this pixel size
    constexpr float kDefFontPx = 13.0f;   // approx default ImGui font px
    const float kBigScale = kBig / kDefFontPx;
    const float kSubScale = kSub / kDefFontPx;
    const ImVec2 big_at_default = ImGui::CalcTextSize(line1);
    const ImVec2 sub_at_default = ImGui::CalcTextSize(line2);
    const ImVec2 big_sz = ImVec2(big_at_default.x * kBigScale,
                                  big_at_default.y * kBigScale);
    const ImVec2 sub_sz = ImVec2(sub_at_default.x * kSubScale,
                                  sub_at_default.y * kSubScale);

    // Filled panel backing the text (size to the text with padding).
    const float pad_x = 28.0f, pad_y = 16.0f;
    const float block_w = std::max(big_sz.x, sub_sz.x) + pad_x * 2.0f;
    // Block height: padded top + big line + gap + sub line + padded bottom.
    const float block_h = pad_y + kBig + 10.0f + kSub + pad_y;
    const float block_x0 = cx - block_w * 0.5f;
    const float block_y0 = cy - block_h * 0.5f;
    dl->AddRectFilled(ImVec2(block_x0, block_y0),
                      ImVec2(block_x0 + block_w, block_y0 + block_h),
                      col_block, 8.0f);
    dl->AddRect(ImVec2(block_x0, block_y0),
               ImVec2(block_x0 + block_w, block_y0 + block_h),
               col_border, 8.0f, 0, 3.0f);

    // Line 1 -- big. Draw at the requested font size so layout matches.
    const float line1_x = cx - big_sz.x * 0.5f;
    const float line1_y  = block_y0 + pad_y + kBig;   // baseline at bottom
    dl->AddText(NULL, kBig, ImVec2(line1_x, line1_y), col_text, line1);

    // Sub line.
    const float sub_x = cx - sub_sz.x * 0.5f;
    const float sub_y  = line1_y + 10.0f + kSub;     // baseline at bottom
    dl->AddText(NULL, kSub, ImVec2(sub_x, sub_y), col_text, line2);
}

} // namespace cockpit_hud
// 1781715641501136000
