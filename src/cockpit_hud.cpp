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
#include "cockpit_hud_internal.h"
#include "cockpit_armaments.h"
#include "cockpit_overlay.h"
#include "comms_menu.h"
#include "hud_text_fit.h"
#include "navmap_projection.h"

#include "armor.h"
#include "camera.h"
#include "comm.h"
#include "faction.h"
#include "firing.h"
#include "galaxy.h"
#include "hazards.h"
#include "missions.h"
#include "perception.h"
#include "player.h"
#include "scanner.h"
#include "sfx.h"
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
#include <map>
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

// Palette, kHudWindowFlags and push/pop_hud_style live in
// cockpit_hud_internal.h (shared with cockpit_mfd.cpp).

// Map a nav kind to its radar/MFD dot colour. String compare is fine —
// nav_points is small and this loop is dwarfed by ImGui call overhead.
ImU32 color_for_kind(const std::string& kind) {
    if (kind == "jump")    return kCyan;
    if (kind == "station") return kGreen;
    if (kind == "planet")  return kBlueP;
    return kHudWhite;
}

// Which sub-screen the STATUS panel is showing. Defaults to the hull
// diagram (Ship). Mutated only on the main thread via set_status_screen.
StatusScreen g_status_screen = StatusScreen::Ship;

// Bare placeholder for a sub-panel whose underlying simulation model has not
// landed yet. Keep the lie obvious instead of drawing fake instrumentation.
void draw_status_stub(const char* title, const char* body) {
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
    ImGui::TextUnformatted(body);
    ImGui::PopStyleColor();
}

void draw_weapons_status(PlayerState* state, Ship& live_ship,
                         int selected_ordnance) {
    if (!state) {
        draw_status_stub("ARMAMENTS", "persistent loadout unavailable");
        return;
    }
    cockpit_armaments::draw(*state, live_ship, selected_ordnance);
}

constexpr float kShipDiagramIconScale = 4.15f;

void draw_ship_diagram_centerpiece(ImDrawList* dl, const Ship& ship,
                                   const ShipSpriteAtlas* atlas_override,
                                   ImVec2 center, float max_px,
                                   ImU32 fallback_fill,
                                   ImU32 fallback_outline) {
    const ShipSpriteAtlas* atlas = atlas_override;
    if (!atlas && ship.sprite) atlas = ship.sprite->atlas;

    const ShipSpriteFrame* frame = nullptr;
    if (atlas) {
        // Fixed top-down query: az=0, el=90. The atlas selector returns
        // the nearest authored frame, so exact pole cells win when present
        // and partial atlases still degrade sensibly.
        frame = choose_ship_sprite_frame_by_angles(*atlas, 0.0f, 90.0f);
    }

    if (!frame || !frame->art || frame->art->hull.view.id == SG_INVALID_ID) {
        dl->AddCircleFilled(center, max_px * 0.5f, fallback_fill, 24);
        dl->AddCircle(center, max_px * 0.5f, fallback_outline, 24, 1.25f);
        return;
    }

    const SpriteArt& art = *frame->art;
    const float src_w = (art.hull_w > 0) ? (float)art.hull_w : 1.0f;
    const float src_h = (art.hull_h > 0) ? (float)art.hull_h : 1.0f;
    float draw_w = max_px;
    float draw_h = max_px;
    if (src_w >= src_h) draw_h = max_px * (src_h / src_w);
    else                draw_w = max_px * (src_w / src_h);

    const ImVec2 tl(center.x - draw_w * 0.5f, center.y - draw_h * 0.5f);
    const ImVec2 br(center.x + draw_w * 0.5f, center.y + draw_h * 0.5f);
    // HUD atlas image needs a 180° turn relative to texture-space here.
    // Flipping both UV axes is equivalent to rotating the sampled image
    // 180° without adding custom quad math. Tiny ships, tiny crimes.
    dl->AddImage(simgui_imtextureid(art.hull.view), tl, br,
                 ImVec2(1.0f, 1.0f), ImVec2(0.0f, 0.0f),
                 IM_COL32(255, 255, 255, 245));
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
    auto* dl = cockpit_overlay::world_draw_list();

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

    auto* dl = cockpit_overlay::world_draw_list();
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

    PanelPlacement panel = place_panel(
        cockpit_overlay::Display::Right, "##nav_mfd",
        ImVec2(s.w - w - margin, s.h - h - margin), ImVec2(w, h));
    if (begin_panel(panel)) {
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
    end_panel(panel);
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
    constexpr float w = 280.0f, h = 248.0f, margin = 16.0f;
    PanelPlacement panel = place_panel(
        cockpit_overlay::Display::Right, "##target_mfd",
        ImVec2(sz.w - w - margin, margin), ImVec2(w, h));

    if (begin_panel(panel)) {
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
            // In an MFD the portrait shrinks to the 3-line identity block
            // beside it so the shield/armor diagram keeps room below.
            const float thumb_w = panel.in_display
                ? ImGui::GetTextLineHeightWithSpacing() * 3.0f : 80.0f;
            const float thumb_h = thumb_w;
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

            // Right column: identity + range + stance + HP bars. Lines fit
            // the column's width (#430): an MFD is far narrower than the
            // classic box, so long names shrink and faction/stance wraps.
            ImGui::BeginGroup();

            const char* class_name = !target->display_name.empty()
                ? target->display_name.c_str()
                : (target->klass ? target->klass->display_name.c_str()
                                 : (target->is_player ? "PLAYER" : "?"));
            hud_text_fit::text(class_name, kHudWhite);

            // Faction + stance — find the player's perception entry to
            // get the stance the AI uses (so target-panel colors match
            // the on-screen indicator + radar). Distance from the
            // contact entry too — already filtered by radar range.
            const char*  fac_name = target->klass
                ? faction::to_name(target->faction) : "?";
            float        dist_m   = 0.0f;
            Stance       stance   = Stance::Neutral;
            bool         iff      = false;   // #143: stance needs colour IFF
            if (const Ship* player = ships.player(); player) {
                iff = scanner::color_iff(player->fitted_scanner);
                for (const PerceivedContact& c : player->perception.visible) {
                    if (c.ship_id == target_ship_id) {
                        dist_m = c.distance_m;
                        stance = c.stance;
                        break;
                    }
                }
            }
            const ImU32 stance_col = contact_color(stance, iff);
            const char* stance_str =
                !iff                        ? "NO IFF"
              : (stance == Stance::Hostile) ? "HOSTILE"
              : (stance == Stance::Allied)  ? "ALLIED"
              :                                "NEUTRAL";

            hud_text_fit::pair_or_wrap(fac_name, stance_str, stance_col);

            char dist[32];
            if (dist_m < 10000.0f) std::snprintf(dist, sizeof(dist), "DIST  %6.0f m",  dist_m);
            else                   std::snprintf(dist, sizeof(dist), "DIST  %5.1f km", dist_m * 0.001f);
            hud_text_fit::text(dist, kHudWhite);

            ImGui::EndGroup();

            // Bottom: per-facing shield + armor bars. F / A / P / St
            // labels (Fore / Aft / Port / Starboard); each bar fills
            // proportionally to current vs. max for that facing.
            // Shield max comes from the fitted ShieldType, armor max
            // from class hull + fitted ArmorType. 4 bars laid out 2x2
            // so the row of labels stays narrow (port and starboard
            // were a single "side" pool pre-issue-30).
            ImGui::Separator();
            const ShipClass* k = target->klass;
            float shield_max[4] = {0,0,0,0}, armor_max[4] = {0,0,0,0};
            if (k) {
                if (k->default_shield) {
                    shield_max[0] = k->default_shield->front_cm      * target->shield_mult;
                    shield_max[1] = k->default_shield->back_cm       * target->shield_mult;
                    shield_max[2] = k->default_shield->port_cm       * target->shield_mult;
                    shield_max[3] = k->default_shield->starboard_cm  * target->shield_mult;
                }
                armor_max[0] = k->armor_fore_cm;
                armor_max[1] = k->armor_aft_cm;
                armor_max[2] = k->armor_port_cm;
                armor_max[3] = k->armor_starboard_cm;
                if (const ArmorType* fitted_armor = target->fitted_armor) {
                    armor_max[0] += fitted_armor->front_cm;
                    armor_max[1] += fitted_armor->back_cm;
                    armor_max[2] += fitted_armor->port_cm;
                    armor_max[3] += fitted_armor->starboard_cm;
                }
            }
            const float shield_cur[4] = { target->shield_fore_cm,
                                          target->shield_aft_cm,
                                          target->shield_port_cm,
                                          target->shield_starboard_cm };
            const float armor_cur[4]  = { target->armor_fore_cm,
                                          target->armor_aft_cm,
                                          target->armor_port_cm,
                                          target->armor_starboard_cm };
            // Same visual vocabulary as the player STATUS diagram:
            // two independent bars per facing, shield outside / armor
            // inside, arranged around a tiny ship circle. The old target
            // panel used stacked progress bars and clipped the final row;
            // tiny rectangle crimes, basically.
            constexpr ImU32 k_shield_col   = IM_COL32( 80, 160, 255, 220);
            constexpr ImU32 k_armor_col    = IM_COL32(255, 140,  60, 220);
            constexpr ImU32 k_bg_col       = IM_COL32( 20,  20,  30, 180);
            constexpr ImU32 k_ship_col     = IM_COL32(255, 210, 100, 230);
            constexpr ImU32 k_ship_outline = IM_COL32(220, 180, 100, 190);

            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Claim the ENTIRE remaining panel area for the diagram and
            // scale the layout to fill it (same treatment as the player
            // STATUS panel, so target + own-ship diagrams read alike).
            const ImVec2 region_tl = ImGui::GetCursorScreenPos();
            const float  avail_w   = ImGui::GetContentRegionAvail().x;
            const float  avail_h   = ImGui::GetContentRegionAvail().y;
            ImGui::Dummy(ImVec2(avail_w, avail_h));
            const float cx = region_tl.x + avail_w * 0.5f;
            const float cy = region_tl.y + avail_h * 0.5f;

            // Uniform scale to fill. Base layout was authored ~150x104 px
            // (bars + facing labels); grow it to whatever room is left below
            // the target's portrait/identity block, clamped so it can't get
            // absurd.
            // No facing labels to reserve room for, so the base height is the
            // bare bar/frame extent and the drawing fills the space below the
            // portrait/identity block.
            // Floor 0.4 (not 1.0) so the diagram still fits a cockpit MFD.
            const float s = std::clamp(std::min(avail_w / 150.0f,
                                                avail_h / 82.0f), 0.4f, 3.0f);

            const float k_circle_r         = 11.0f * s;
            const float k_bar_thick        = 6.0f  * s;
            const float k_pair_gap         = 3.0f  * s;
            const float k_h_bar_max        = 46.0f * s;
            const float k_v_shield_bar_max = 40.0f * s;
            const float k_v_armor_bar_max  = 40.0f * s;
            const float k_frame_hw         = 40.0f * s;
            const float k_frame_hh         = 35.0f * s;
            const float k_bar_round        = 2.5f  * s;

            auto draw_h_single = [&](float x, float y, float w, ImU32 col,
                                     float cur, float maxv) {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + k_bar_thick),
                                  k_bg_col, k_bar_round);
                if (maxv <= 0.0f) return;
                const float frac = std::clamp(cur / maxv, 0.0f, 1.0f);
                dl->AddRectFilled(ImVec2(x, y),
                                  ImVec2(x + w * frac, y + k_bar_thick),
                                  col, k_bar_round);
            };
            auto draw_v_single = [&](float x, float y, float h, ImU32 col,
                                     float cur, float maxv) {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + k_bar_thick, y + h),
                                  k_bg_col, k_bar_round);
                if (maxv <= 0.0f) return;
                const float frac = std::clamp(cur / maxv, 0.0f, 1.0f);
                const float fill_h = h * frac;
                dl->AddRectFilled(ImVec2(x, y + h - fill_h),
                                  ImVec2(x + k_bar_thick, y + h),
                                  col, k_bar_round);
            };

            const float pair_span = k_bar_thick * 2.0f + k_pair_gap;

            const float fore_x    = cx - k_h_bar_max * 0.5f;
            const float fore_sh_y = cy - k_frame_hh;
            const float fore_ar_y = fore_sh_y + k_bar_thick + k_pair_gap;
            draw_h_single(fore_x, fore_sh_y, k_h_bar_max, k_shield_col,
                          shield_cur[0], shield_max[0]);
            draw_h_single(fore_x, fore_ar_y, k_h_bar_max, k_armor_col,
                          armor_cur[0], armor_max[0]);

            const float aft_ar_y = cy + k_frame_hh - pair_span;
            const float aft_sh_y = aft_ar_y + k_bar_thick + k_pair_gap;
            draw_h_single(fore_x, aft_sh_y, k_h_bar_max, k_shield_col,
                          shield_cur[1], shield_max[1]);
            draw_h_single(fore_x, aft_ar_y, k_h_bar_max, k_armor_col,
                          armor_cur[1], armor_max[1]);

            const float port_sh_x = cx - k_frame_hw;
            const float port_ar_x = port_sh_x + k_bar_thick + k_pair_gap;
            const float port_sh_y = cy - k_v_shield_bar_max * 0.5f;
            const float port_ar_y = cy - k_v_armor_bar_max  * 0.5f;
            draw_v_single(port_sh_x, port_sh_y, k_v_shield_bar_max, k_shield_col,
                          shield_cur[2], shield_max[2]);
            draw_v_single(port_ar_x, port_ar_y, k_v_armor_bar_max, k_armor_col,
                          armor_cur[2], armor_max[2]);

            const float stbd_ar_x = cx + k_frame_hw - pair_span;
            const float stbd_sh_x = stbd_ar_x + k_bar_thick + k_pair_gap;
            const float stbd_sh_y = cy - k_v_shield_bar_max * 0.5f;
            const float stbd_ar_y = cy - k_v_armor_bar_max  * 0.5f;
            draw_v_single(stbd_sh_x, stbd_sh_y, k_v_shield_bar_max, k_shield_col,
                          shield_cur[3], shield_max[3]);
            draw_v_single(stbd_ar_x, stbd_ar_y, k_v_armor_bar_max, k_armor_col,
                          armor_cur[3], armor_max[3]);

            draw_ship_diagram_centerpiece(dl, *target, nullptr, ImVec2(cx, cy),
                                           k_circle_r * kShipDiagramIconScale,
                                           k_ship_col, k_ship_outline);
        }
    }
    end_panel(panel);
}

// Top-left STATUS panel — player ship's hull integrity at a glance.
// Mirrors the TARGET panel's layout (class label + 6 progress bars
// for shield + armor per facing) but for the registry's player. No
// thumbnail since the player has no sprite. Energy displayed as a
// numeric current/max instead of a bar — energy ticks fast enough
// during sustained fire that a bar would just look noisy.
void draw_player_status(ShipRegistry& ships,
                        const ShipSpriteAtlas* player_preview_atlas,
                        const StarSystem& system, const Ship* target,
                        PlayerState* player_state, int selected_ordnance) {
    Ship* player_p = ships.player();
    if (!player_p) return;
    Ship& player = *player_p;

    constexpr float w = 280.0f, h = 224.0f, margin = 16.0f;
    PanelPlacement panel = place_panel(
        cockpit_overlay::Display::Left, "##player_status",
        ImVec2(margin, margin), ImVec2(w, h));

    // The armaments screen is the one interactive STATUS page: its hardpoints
    // accept drag/drop. Every other page remains click-through flight HUD.
    const ImGuiWindowFlags status_flags = g_status_screen == StatusScreen::Weapons
        ? (kHudWindowFlags & ~ImGuiWindowFlags_NoInputs) : kHudWindowFlags;
    if (begin_panel(panel, status_flags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("STATUS");
        ImGui::PopStyleColor();
        ImGui::Separator();

      // Dispatch the STATUS window to its active sub-screen. Ship keeps the
      // canonical hull diagram; Comms hosts the data-driven hail menu;
      // Damage remains a stub until component health exists; Weapons reads
      // the live mount/arm/energy state.
      switch (g_status_screen) {
      case StatusScreen::Comms: {
        static const PlayerReputation kNoRep{};
        comms_menu::draw(system, target,
                         player_state ? player_state->rep : kNoRep);
        break;
      }
      case StatusScreen::Damage:
        draw_status_stub("DAMAGE CONTROL", "system damage not yet modeled");
        break;
      case StatusScreen::Weapons:
        draw_weapons_status(player_state, player, selected_ordnance);
        break;
      case StatusScreen::Ship:
      default:
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

            ImGui::Separator();

            // Per-facing maxes (same math as target panel).
            const ShipClass* k = player.klass;
            float shield_max[4] = {0,0,0,0}, armor_max[4] = {0,0,0,0};
            if (k) {
                if (k->default_shield) {
                    shield_max[0] = k->default_shield->front_cm      * player.shield_mult;
                    shield_max[1] = k->default_shield->back_cm       * player.shield_mult;
                    shield_max[2] = k->default_shield->port_cm       * player.shield_mult;
                    shield_max[3] = k->default_shield->starboard_cm  * player.shield_mult;
                }
                armor_max[0] = k->armor_fore_cm;
                armor_max[1] = k->armor_aft_cm;
                armor_max[2] = k->armor_port_cm;
                armor_max[3] = k->armor_starboard_cm;
                if (const ArmorType* fitted_armor = player.fitted_armor) {
                    armor_max[0] += fitted_armor->front_cm;
                    armor_max[1] += fitted_armor->back_cm;
                    armor_max[2] += fitted_armor->port_cm;
                    armor_max[3] += fitted_armor->starboard_cm;
                }
            }
            const float shield_cur[4] = { player.shield_fore_cm,
                                          player.shield_aft_cm,
                                          player.shield_port_cm,
                                          player.shield_starboard_cm };
            const float armor_cur[4]  = { player.armor_fore_cm,
                                          player.armor_aft_cm,
                                          player.armor_port_cm,
                                          player.armor_starboard_cm };

            // ---- Ship diagram: 8 bars around the ship icon ---------
            // Each facing gets TWO independent bars: shield (blue, outer)
            // and armor (orange, inner). Dim background = max capacity,
            // bright fill = current. With the energy bar + gun text gone,
            // the diagram now OWNS the whole panel: it claims all the
            // remaining space and scales uniformly to fill it.
            constexpr ImU32 k_shield_col   = IM_COL32( 80, 160, 255, 220);
            constexpr ImU32 k_armor_col    = IM_COL32(255, 140,  60, 220);
            constexpr ImU32 k_bg_col       = IM_COL32( 20,  20,  30, 180);
            constexpr ImU32 k_ship_col     = IM_COL32(255, 210, 100, 240);
            constexpr ImU32 k_ship_outline = IM_COL32(220, 180, 100, 200);

            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Claim the ENTIRE remaining panel area for the diagram and
            // centre the layout in it.
            const ImVec2 region_tl = ImGui::GetCursorScreenPos();
            const float  avail_w   = ImGui::GetContentRegionAvail().x;
            const float  avail_h   = ImGui::GetContentRegionAvail().y;
            ImGui::Dummy(ImVec2(avail_w, avail_h));
            const float cx = region_tl.x + avail_w * 0.5f;
            const float cy = region_tl.y + avail_h * 0.5f;

            // Uniform scale to fill the region. The base geometry below was
            // authored to occupy ~150x118 px (bars + facing labels); grow
            // it to fit whatever room the panel gives, clamped so a giant
            // window can't blow it up absurdly.
            // Base height is the bare bar/frame extent now that the F/A/P/St
            // facing labels are gone — no label margin to reserve, so the
            // drawing grows to nearly fill the panel.
            // Floor 0.4 (not 1.0) so the diagram still fits a cockpit MFD.
            const float s = std::clamp(std::min(avail_w / 150.0f,
                                                avail_h / 98.0f), 0.4f, 3.0f);

            const float k_circle_r         = 13.0f * s;  // ship icon radius
            const float k_bar_thick        = 7.0f  * s;  // each bar thickness
            const float k_pair_gap         = 3.0f  * s;  // gap between shield+armor
            const float k_h_bar_max        = 54.0f * s;  // fore/aft bar length
            const float k_v_shield_bar_max = 48.0f * s;  // side shield length
            const float k_v_armor_bar_max  = 48.0f * s;  // side armor length
            const float k_frame_hw         = 46.0f * s;  // half-width  (to side pair)
            const float k_frame_hh         = 44.0f * s;  // half-height (to top/bot pair)
            const float k_bar_round        = 2.5f  * s;  // bar corner rounding

            // Helpers draw ONE actual bar (bg track + bright fill).
            auto draw_h_single = [&](float x, float y, float w, ImU32 col,
                                     float cur, float maxv) {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + k_bar_thick),
                                  k_bg_col, k_bar_round);
                if (maxv <= 0.0f) return;
                const float frac = std::clamp(cur / maxv, 0.0f, 1.0f);
                dl->AddRectFilled(ImVec2(x, y),
                                  ImVec2(x + w * frac, y + k_bar_thick),
                                  col, k_bar_round);
            };

            auto draw_v_single = [&](float x, float y, float hh, ImU32 col,
                                     float cur, float maxv) {
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + k_bar_thick, y + hh),
                                  k_bg_col, k_bar_round);
                if (maxv <= 0.0f) return;
                const float frac = std::clamp(cur / maxv, 0.0f, 1.0f);
                const float fill_h = hh * frac;
                // Vertical health reads as a reservoir: full grows upward,
                // damage drains downward.
                dl->AddRectFilled(ImVec2(x, y + hh - fill_h),
                                  ImVec2(x + k_bar_thick, y + hh),
                                  col, k_bar_round);
            };

            // ---- Place 8 bars (2 per facing) + ship icon ------
            // Convention: shield is the OUTER shell, armor is INNER.
            const float pair_span = k_bar_thick * 2.0f + k_pair_gap;

            // Fore pair — shield outside/top, armor inside/below.
            const float fore_x = cx - k_h_bar_max * 0.5f;
            const float fore_sh_y = cy - k_frame_hh;
            const float fore_ar_y = fore_sh_y + k_bar_thick + k_pair_gap;
            draw_h_single(fore_x, fore_sh_y, k_h_bar_max, k_shield_col,
                          shield_cur[0], shield_max[0]);
            draw_h_single(fore_x, fore_ar_y, k_h_bar_max, k_armor_col,
                          armor_cur[0], armor_max[0]);

            // Aft pair — armor inside/top, shield outside/bottom.
            const float aft_ar_y = cy + k_frame_hh - pair_span;
            const float aft_sh_y = aft_ar_y + k_bar_thick + k_pair_gap;
            draw_h_single(fore_x, aft_sh_y, k_h_bar_max, k_shield_col,
                          shield_cur[1], shield_max[1]);
            draw_h_single(fore_x, aft_ar_y, k_h_bar_max, k_armor_col,
                          armor_cur[1], armor_max[1]);

            // Port pair — shield outside/left, armor inside/right.
            const float port_sh_x = cx - k_frame_hw;
            const float port_ar_x = port_sh_x + k_bar_thick + k_pair_gap;
            const float port_sh_y = cy - k_v_shield_bar_max * 0.5f;
            const float port_ar_y = cy - k_v_armor_bar_max  * 0.5f;
            draw_v_single(port_sh_x, port_sh_y, k_v_shield_bar_max, k_shield_col,
                          shield_cur[2], shield_max[2]);
            draw_v_single(port_ar_x, port_ar_y, k_v_armor_bar_max, k_armor_col,
                          armor_cur[2], armor_max[2]);

            // Starboard pair — armor inside/left, shield outside/right.
            const float stbd_ar_x = cx + k_frame_hw - pair_span;
            const float stbd_sh_x = stbd_ar_x + k_bar_thick + k_pair_gap;
            const float stbd_sh_y = cy - k_v_shield_bar_max * 0.5f;
            const float stbd_ar_y = cy - k_v_armor_bar_max  * 0.5f;
            draw_v_single(stbd_sh_x, stbd_sh_y, k_v_shield_bar_max, k_shield_col,
                          shield_cur[3], shield_max[3]);
            draw_v_single(stbd_ar_x, stbd_ar_y, k_v_armor_bar_max, k_armor_col,
                          armor_cur[3], armor_max[3]);

            // Ship icon (centrepiece). Use the atlas top-down frame when
            // available; fallback amber circle keeps HUD robust for any
            // ship/class missing sprite data.
            draw_ship_diagram_centerpiece(dl, player, player_preview_atlas, ImVec2(cx, cy),
                                           k_circle_r * kShipDiagramIconScale,
                                           k_ship_col, k_ship_outline);

        }
        break;
      }   // end switch(g_status_screen)
    }
    end_panel(panel);

}

void draw_radar_mfd(const Camera& cam, const StarSystem& system, int selected_nav,
                    const ShipRegistry& ships, uint32_t target_ship_id) {
    const auto s = screen_size();
    constexpr float w = 168.0f, h = 168.0f, margin = 16.0f;

    // In the cockpit the disc takes the square middle of the centre MFD;
    // the flanks either side carry the FLIGHT/ordnance readouts.
    PanelPlacement panel = place_panel(
        cockpit_overlay::Display::Center, "##radar_mfd",
        ImVec2(margin, s.h - h - margin), ImVec2(w, h));
    const cockpit_overlay::Rect disc = panel.in_display
        ? cockpit_overlay::split_radar(to_rect(panel)).disc
        : to_rect(panel);

    if (begin_panel(panel, kHudWindowFlags, ImVec2(0.0f, 0.0f))) {
        const ImVec2 p0 = panel.pos;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ImVec2 ctr  = ImVec2(disc.x + disc.w * 0.5f, disc.y + disc.h * 0.5f);
        const float  rad  = std::min(disc.w, disc.h) * 0.5f - (panel.in_display ? 3.0f : 6.0f);
        // The disc's rim is the player's radar sphere (perception::
        // radar_range_m, #492), so a contact at the rim is exactly one that
        // is about to drop off radar and out of lock. Anything farther
        // (nav points 100+ km away) clamps to the rim: the MFD is a
        // "where's the contact NEAR me" display, not a system overview.
        const Ship* radar_owner = ships.player();
        const float max_range = radar_owner ? perception::radar_range_m(*radar_owner)
                                            : k_default_radar_range_m;

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
        // green=allied, yellow=neutral) so the radar reads at a glance —
        // on a colour-IFF scanner; monochrome ones show one tint (#143).
        // Ships cluster toward the center (the engagement bubble); nav
        // points sit at or near the rim because they're system-scale
        // (planets, jump points 100+ km away).
        if (const Ship* player_p = ships.player(); player_p) {
            const Ship& player = *player_p;
            const bool iff = scanner::color_iff(player.fitted_scanner);
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

                const ImU32 col = contact_color(c.stance, iff);
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
        // The cockpit's flank gauges own that corner; the bezel says RADAR.
        if (!panel.in_display)
            dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f), kDimAmber, "RADAR");
    }
    end_panel(panel);
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
    auto*          dl     = cockpit_overlay::world_draw_list();

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

// ---- mission objective markers + readout (#18) ---------------------------
//
// Read-only surfacing of accepted missions (PlayerState::missions). For
// active jobs whose objective sits in the CURRENT system we float a cyan
// diamond over the targeted nav/base (so it's never confused with the amber
// nav-target reticle), and we list a compact per-type progress string for
// every active job. Every value is read straight off ActiveMission — the
// HUD touches the mission model, never mutates it.

// Mission-objective colour — a deliberate RED so nav objectives read
// distinctly from the amber selected-nav reticle, the cyan jump holes,
// and the green regular navs. Keep it its own constant (not an alias of
// kCyan) so repurposing cyan elsewhere never bleeds into objectives.
static const ImU32 kObjective    = IM_COL32(255,  80,  80, 240);  // objective markers + in-system lines
static const ImU32 kObjectiveDim = IM_COL32(255,  80,  80,  85);  // surveyed patrol navs (route progress)

// Cyan objective diamond + optional label, floating at a world position.
// `col` defaults to the bright objective cyan; pass kObjectiveDim for
// already-surveyed patrol navs so the player reads route progress at a
// glance without a second draw routine.
void draw_objective_marker(const Camera& cam, HMM_Vec3 world,
                           const char* label, ImU32 col = kObjective,
                           int label_stagger_idx = 0) {
    float sx, sy;
    if (!project_world_point(cam, world, sx, sy)) return;
    auto* dl = cockpit_overlay::world_draw_list();
    constexpr float r = 11.0f;
    dl->AddQuad(ImVec2(sx, sy - r), ImVec2(sx + r, sy),
                ImVec2(sx, sy + r), ImVec2(sx - r, sy), col, 2.0f);
    dl->AddCircleFilled(ImVec2(sx, sy), 2.0f, col);
    if (label && label[0]) {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        // Issue #25: stagger overlapping labels. Multiple missions can share
        // a single nav point (e.g. three cargo deliveries all bound for
        // Tarsus). Stack their captions vertically above the diamond so the
        // player can still read each one without them colliding.
        const float y_off = (float)label_stagger_idx * (ts.y + 4.0f);
        dl->AddText(ImVec2(sx - ts.x * 0.5f, sy - r - ts.y - 3.0f - y_off),
                    col, label);
    }
}

// ---- speaker indicator --------------------------------------------------
// "Who's talking to me" HUD marker. When comm::speaker_id() is non-zero
// we draw four amber corner-brackets framing that ship plus a labelled
// glyph "((  )) FACTION" just above it. No-op when no speaker is set,
// the ship isn't in the registry any more, or the camera can't project
// the position (behind us / off-screen). Caller (build()) gates on
// draw_world normally — autopilot/navmap will hide it via the gate.
void draw_speaker_indicator(const Camera& cam, const ShipRegistry& ships) {
    if (comm::speaker_id() == 0) return;
    const Ship* s = ships.find_by_id(comm::speaker_id());
    if (!s || !s->alive) return;
    auto* dl = ImGui::GetForegroundDrawList();
    const ImU32 col = kAmber;

    // Always-visible "who is talking" banner. The on-ship brackets below only
    // show when the speaker is in front of us and on-screen; this fixed pill
    // guarantees the player always sees an incoming-transmission cue (and the
    // faction) even when the speaker is off to the side or behind.
    {
        const char* fn = faction::to_name(comm::speaker_faction());
        char banner[80];
        std::snprintf(banner, sizeof(banner), "(( o )) INCOMING  -  %s", fn);
        for (char* p = banner; *p; ++p) *p = (char)std::toupper((unsigned char)*p);
        const auto  sz = screen_size();
        const ImVec2 ts = ImGui::CalcTextSize(banner);
        const float  bx = sz.w * 0.5f - ts.x * 0.5f;
        const float  by = sz.h * 0.215f;
        dl->AddRectFilled(ImVec2(bx - 8, by - 4), ImVec2(bx + ts.x + 8, by + ts.y + 4),
                          IM_COL32(0, 0, 0, 150), 3.0f);
        dl->AddRect(ImVec2(bx - 8, by - 4), ImVec2(bx + ts.x + 8, by + ts.y + 4),
                    col, 3.0f);
        dl->AddText(ImVec2(bx, by), col, banner);
    }

    float sx, sy;
    if (!project_world_point(cam, s->position, sx, sy)) return;
    const auto ss = screen_size();
    if (sx < 0 || sx > ss.w || sy < 0 || sy > ss.h) return;  // off-screen: banner only
    dl = cockpit_overlay::world_draw_list(); // world brackets, unlike the incoming UI banner
    // Four corner brackets framing a ~22 px box. Each is an "L" of two
    // short lines. arm = 5 px so the brackets read clearly even at the
    // default Retina scale.
    constexpr float r   = 11.0f;
    constexpr float arm = 5.0f;
    // top-left
    dl->AddLine(ImVec2(sx - r, sy - r), ImVec2(sx - r + arm, sy - r), col, 1.5f);
    dl->AddLine(ImVec2(sx - r, sy - r), ImVec2(sx - r,       sy - r + arm), col, 1.5f);
    // top-right
    dl->AddLine(ImVec2(sx + r, sy - r), ImVec2(sx + r - arm, sy - r), col, 1.5f);
    dl->AddLine(ImVec2(sx + r, sy - r), ImVec2(sx + r,       sy - r + arm), col, 1.5f);
    // bottom-left
    dl->AddLine(ImVec2(sx - r, sy + r), ImVec2(sx - r + arm, sy + r), col, 1.5f);
    dl->AddLine(ImVec2(sx - r, sy + r), ImVec2(sx - r,       sy + r - arm), col, 1.5f);
    // bottom-right
    dl->AddLine(ImVec2(sx + r, sy + r), ImVec2(sx + r - arm, sy + r), col, 1.5f);
    dl->AddLine(ImVec2(sx + r, sy + r), ImVec2(sx + r,       sy + r - arm), col, 1.5f);
    // Label "((  )) CONFED" — the comm glyph + uppercased faction name.
    // We assemble into one buffer so it's a single AddText call (one
    // string -> one draw call vs two).
    const char* fname = faction::to_name(comm::speaker_faction());
    char        label[64];
    std::snprintf(label, sizeof(label), "((  )) %s", fname);
    for (char* p = label; *p; ++p) *p = (char)std::toupper((unsigned char)*p);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    // Sit just above the top bracket with a 4 px gap; centre on the ship.
    dl->AddText(ImVec2(sx - ts.x * 0.5f, sy - r - ts.y - 4.0f), col, label);
}

// Find a nav point in this system by its display name (Patrol/Scout/Attack
// store nav-point NAMES in nav_targets). nullptr = not in this system.
const NavPointDef* nav_by_name(const StarSystem& sys, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const NavPointDef& n : sys.nav_points)
        if (n.name == name) return &n;
    return nullptr;
}

// Find a base nav point by base_id (DefendBase/Cargo store base_ids), with a
// fallback to a name match for data that stores the base under either key.
const NavPointDef* nav_by_base(const StarSystem& sys, const std::string& base_id) {
    if (base_id.empty()) return nullptr;
    for (const NavPointDef& n : sys.nav_points)
        if (n.base_id == base_id) return &n;
    return nav_by_name(sys, base_id);
}

// Resolve the in-system nav points a mission paints markers for. The HUD
// is read-only on the model: this only reads nav_targets / nav_done.
//
// Per-type contract:
//   * Patrol      : EVERY unsurveyed nav_targets entry that resolves via
//                   nav_by_name in this system — a Patrol is a multi-nav
//                   route, so the whole remaining route must read at once
//                   (not just the first unsurveyed nav, which is what the
//                   legacy single-nav resolver returned).
//   * Scout/Attack: the single nav_targets.front() that resolves, if any.
//   * DefendBase  : nav_by_base(target_base).
//   * CargoDelivery: nav_by_base(dest_base).
//   * default (Bounty + anything new): empty — bounties are faction hunts
//                   across systems with no single nav target.
//
// `surveyed_out` (if non-null) collects the in-system Patrol navs that are
// already done, so a draw site can paint route progress (a dimmer marker /
// ring) without re-walking nav_targets. Left empty for non-Patrol types.
static std::vector<const NavPointDef*>
objective_navs_in_system(const ActiveMission& am, const StarSystem& sys,
                        std::vector<const NavPointDef*>* surveyed_out = nullptr) {
    using MT = missions::MissionType;
    if (surveyed_out) surveyed_out->clear();
    const MT type = (MT)am.type;
    switch (type) {
    case MT::Patrol: {
        std::vector<const NavPointDef*> unsurveyed;
        for (size_t i = 0; i < am.nav_targets.size(); ++i) {
            const bool done = (i < am.nav_done.size()) && am.nav_done[i];
            if (const NavPointDef* n = nav_by_name(sys, am.nav_targets[i])) {
                if (done) { if (surveyed_out) surveyed_out->push_back(n); }
                else      { unsurveyed.push_back(n); }
            }
        }
        return unsurveyed;
    }
    case MT::Scout:
    case MT::Attack:
        if (!am.nav_targets.empty())
            if (const NavPointDef* n = nav_by_name(sys, am.nav_targets.front()))
                return { n };
        return {};
    case MT::DefendBase:
        if (const NavPointDef* n = nav_by_base(sys, am.target_base)) return { n };
        return {};
    case MT::CargoDelivery:
        if (const NavPointDef* n = nav_by_base(sys, am.dest_base)) return { n };
        return {};
    default:
        return {};            // Bounty + any future type without a nav
    }
}

// Resolve the SINGLE in-system nav point a mission points at. Shared
// between the in-system half of resolve_nav_for_mission (click-to-target)
// and the legacy single-nav draw paths. Delegates to objective_navs_in_system
// so the two resolvers can't drift — for Patrol this is the first
// unsurveyed nav (or nullptr when the whole route is done / has no targets).
static const NavPointDef* objective_nav_in_system(const ActiveMission& am,
                                                  const StarSystem& sys) {
    const std::vector<const NavPointDef*> navs = objective_navs_in_system(am, sys);
    return navs.empty() ? nullptr : navs.front();
}

// A display label for a base objective: prefer the nav point's human name,
// fall back to the raw id when the base isn't in this system.
std::string base_label(const StarSystem& sys, const std::string& base_id) {
    if (const NavPointDef* n = nav_by_base(sys, base_id)) return n->name;
    return base_id;
}

} // anonymous namespace

uint32_t contact_color(Stance stance, bool color_iff, uint8_t alpha) {
    if (!color_iff) return IM_COL32(190, 205, 215, alpha);   // monochrome radar tint
    switch (stance) {
        case Stance::Hostile: return IM_COL32(255,  90,  90, alpha);
        case Stance::Allied:  return IM_COL32( 90, 255, 110, alpha);
        case Stance::Neutral: break;
    }
    return IM_COL32(255, 220, 60, alpha);
}

// STATUS sub-screen accessors. File-static g_status_screen lives in the
// anonymous namespace above; these are the public seam main.cpp + the
// dev_remote /panel endpoint poke.
void set_status_screen(StatusScreen s) { g_status_screen = s; }
StatusScreen status_screen()           { return g_status_screen; }

// Camera-relative world->screen projection (same engine quirk as the nav
// reticle: NO ndc-Y flip). Returns false when the point is behind the
// camera so the caller can simply skip drawing rather than smear a marker
// across the wrong half of the screen. Declared in cockpit_hud.h so other
// TUs (loot.cpp, #84 DRY) can share this helper instead of re-deriving
// the projection matrix.
bool project_world_point(const Camera& cam, HMM_Vec3 world,
                         float& sx, float& sy) {
    const HMM_Vec3 d = HMM_SubV3(world, cam.position);
    if (HMM_DotV3(d, cam.forward()) <= 0.0f) return false;   // behind camera
    const auto s = screen_size();
    const float    aspect = s.w / s.h;
    const HMM_Mat4 vp     = HMM_MulM4(cam.projection(aspect), cam.view());
    const HMM_Vec4 ph     = { world.X, world.Y, world.Z, 1.0f };
    const HMM_Vec4 clip   = HMM_MulM4V4(vp, ph);
    if (clip.W <= 0.0f) return false;
    const float ndc_x = clip.X / clip.W;
    const float ndc_y = clip.Y / clip.W;     // engine quirk: no Y flip
    sx = (ndc_x * 0.5f + 0.5f) * s.w;
    sy = (ndc_y * 0.5f + 0.5f) * s.h;
    return true;
}

// Top-centre FLIGHT panel. Mirrors STATUS / TARGET in style (dark bg +
// amber border, amber title, separator) so the data-at-the-top reads as
// part of the same HUD vocabulary instead of free-floating text. Public
// (not in the anon namespace) so main.cpp can drive it with the live
// camera + autopilot snapshot every flight frame.
void draw_flight_status_mfd(const FlightStatusHudState& s) {
    // Cockpit art up: speed/mode/energy flank the radar instead.
    if (draw_flight_flanks(s)) return;
    const auto sz = screen_size();
    constexpr float w = 280.0f, margin = 16.0f;
    // Height grows for the energy gauge + any autopilot rows so the box
    // always frames the content snugly.
    float h = 110.0f;
    if (s.energy_max > 0.0f) h += 22.0f;
    if (s.autopilot_nav)     h += 18.0f;
    if (s.autopilot_msg)     h += 18.0f;
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
        // Energy bank — full-width yellow gauge (relocated here from the
        // STATUS panel). Drives both guns and the afterburner; watch it
        // dip during sustained fire / burn.
        if (s.energy_max > 0.0f) {
            const float e_frac = std::clamp(s.energy / s.energy_max, 0.0f, 1.0f);
            char ebuf[32];
            std::snprintf(ebuf, sizeof(ebuf), "ENERGY %.0f / %.0f GJ",
                          s.energy, s.energy_max);
            ImGui::PushStyleColor(ImGuiCol_FrameBg,       IM_COL32(20,20,30,180));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, IM_COL32(255,210,60,220));
            ImGui::ProgressBar(e_frac, ImVec2(-1.0f, 14.0f), ebuf);
            ImGui::PopStyleColor(2);
        }
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
           ShipRegistry& ships, uint32_t target_ship_id,
           const ShipSpriteAtlas* player_preview_atlas,
           const char* dock_prompt, bool dock_ready, bool draw_world,
           PlayerState* player_state, int selected_ordnance) {
    // Crosshair + aim cursor are HUD overlays that distract or fight input
    // when the navmap is up (it covers the screen centre) or autopilot owns
    // the ship (the camera is on rails, no manual aiming to assist).
    if (draw_world) {
        draw_crosshair(fly_by_wire);
        draw_aim_cursor(mouse_x, mouse_y, fly_by_wire);
    }
    // Nav reticle (yellow T crosshair) and mission-objective diamonds are
    // world glyphs that distract when autopilot owns the ship or the
    // navmap overlay is up (where the same info is rendered textually).
    if (draw_world)
        draw_nav_reticle(cam, system, selected_nav);
    // The STATUS panel's Comms sub-screen needs the current target ship and
    // the player's reputation to resolve friendly-vs-hostile hail lines.
    const Ship* status_target = target_ship_id
        ? ships.find_by_id(target_ship_id) : nullptr;
    draw_player_status(ships, player_preview_atlas, system, status_target,
                       player_state, selected_ordnance);
    if (!cockpit_overlay::active()) {
        draw_nav_mfd   (cam, system, selected_nav, dock_prompt, dock_ready);
        draw_target_mfd(cam, ships, target_ship_id);
    } else if (status_target && status_target->alive && !dock_ready) {
        // Cockpit art has ONE right-hand MFD, shared Privateer-VDU style:
        // a live ship lock shows TARGET, otherwise NAV. Cleared-to-dock/jump
        // always wins so the D/J prompt can never hide behind a lock.
        draw_target_mfd(cam, ships, target_ship_id);
    } else {
        draw_nav_mfd(cam, system, selected_nav, dock_prompt, dock_ready);
    }
    draw_radar_mfd (cam, system, selected_nav, ships, target_ship_id);
    // "Who's speaking" speaker marker. Always on when set — it's the
    // HUD's only way to anchor a comm-bark ship visually, so even when
    // the world is hidden (autopilot / navmap) we still want to see
    // which ship is talking at us.
    draw_speaker_indicator(cam, ships);
}

void build_mission_objectives(const Camera& cam, const StarSystem& system,
                              const std::string& current_system,
                              const PlayerState& player, bool draw_world) {
    using MT = missions::MissionType;
    if (player.missions.empty()) return;

    // One readout line per active mission. We list EVERY active job (so the
    // cargo "on the way" vs "ready" distinction reads), but only draw the
    // floating objective marker when the objective resolves to a nav/base in
    // THIS system. `in_current_system` just tints the line brighter — the
    // status TEXT itself comes from missions::mission_status so the navmap
    // panel (np-19.3) and this readout can't drift.
    struct Row { std::string text; bool in_system; };
    std::vector<Row> rows;
    rows.reserve(player.missions.size());

    // Issue #25: stagger overlapping label captions. Multiple missions can
    // share a nav (e.g. three cargo deliveries all bound for the same base)
    // so we count how many labels have already been assigned to each nav and
    // pass that as the stagger index into draw_objective_marker — each one
    // above gets offset higher so they stack instead of overlapping.
    std::map<const NavPointDef*, int> stagger_idx;
    auto stagger_for = [&](const NavPointDef* n) -> int {
        if (!n) return 0;
        return stagger_idx[n]++;
    };

    for (const ActiveMission& am : player.missions) {
        const missions::MissionStatus s =
            missions::mission_status(am, system, current_system);

        // Marker logic stays here (the 3D draw is per-call-site concern).
        // Only in-system rows produce a marker; cross-system ones are still
        // listed in the readout but get no floating diamond. The label
        // string mirrors the type so the in-world diamond carries the
        // same caption as the row text. The whole marker block is hidden
        // when `draw_world` is false (autopilot / navmap open) so the
        // world glyphs don't fight the autopilot HUD or double up with
        // the navmap panel that renders the same info textually.
        if (draw_world && s.in_current_system) {
            const MT type = (MT)am.type;
            const char* label = nullptr;
            switch (type) {
            case MT::Patrol:     label = "PATROL";  break;
            case MT::Scout:      label = "SCOUT";   break;
            case MT::Attack:     label = "ATTACK";  break;
            case MT::DefendBase: label = "DEFEND";  break;
            case MT::CargoDelivery: label = "DELIVER"; break;
            default: break;
            }
            if (label) {
                if (type == MT::Patrol) {
                    // Multi-nav route: mark EVERY unsurveyed nav target that
                    // resolves in this system, not just the first. Surveyed
                    // navs get a dimmer diamond so the player sees route
                    // progress. One "PATROL" caption on the first marker
                    // keeps the HUD readable (no label soup).
                    std::vector<const NavPointDef*> surveyed;
                    const std::vector<const NavPointDef*> unsurveyed =
                        objective_navs_in_system(am, system, &surveyed);
                    bool first = true;
                    for (const NavPointDef* n : unsurveyed) {
                        draw_objective_marker(cam, n->position,
                                             first ? label : nullptr,
                                             kObjective,
                                             first ? stagger_for(n) : 0);
                        first = false;
                    }
                    for (const NavPointDef* n : surveyed)
                        draw_objective_marker(cam, n->position, nullptr,
                                             kObjectiveDim, 0);
                } else if (const NavPointDef* n =
                               objective_nav_in_system(am, system)) {
                    draw_objective_marker(cam, n->position, label,
                                          kObjective, stagger_for(n));
                }
            }
        }

        rows.push_back({ s.text, s.in_current_system });
    }

    // ---- readout panel: top-left, tucked under the STATUS block ----------
    constexpr float w = 280.0f, margin = 16.0f;
    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    const float h = 26.0f + row_h * (float)rows.size() + 6.0f;
    // STATUS panel is 224 tall at (16,16); sit just below it — unless the
    // cockpit art has moved STATUS into the left MFD, freeing the corner.
    constexpr float kStatusPanelH = 224.0f;
    const float y = cockpit_overlay::active()
        ? margin : margin + kStatusPanelH + 8.0f;
    ImGui::SetNextWindowPos(ImVec2(margin, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    push_hud_style();
    if (ImGui::Begin("##mission_objectives", nullptr, kHudWindowFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("OBJECTIVES");
        ImGui::PopStyleColor();
        ImGui::Separator();
        for (const Row& r : rows) {
            ImGui::PushStyleColor(ImGuiCol_Text, r.in_system ? kObjective : kDimAmber);
            ImGui::TextUnformatted(r.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    pop_hud_style();
}

void draw_pause_overlay() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const ImVec2 screen_lo = viewport->WorkPos;
    const ImVec2 screen_hi(screen_lo.x + viewport->WorkSize.x,
                           screen_lo.y + viewport->WorkSize.y);
    fg->AddRectFilled(screen_lo, screen_hi, IM_COL32(0, 0, 0, 55));

    const ImVec2 box_size(400.0f, 112.0f);
    const ImVec2 box_lo(screen_lo.x + (viewport->WorkSize.x - box_size.x) * 0.5f,
                        screen_lo.y + (viewport->WorkSize.y - box_size.y) * 0.38f);
    const ImVec2 box_hi(box_lo.x + box_size.x, box_lo.y + box_size.y);
    fg->AddRectFilled(box_lo, box_hi, IM_COL32(20, 16, 8, 235), 7.0f);
    fg->AddRect(box_lo, box_hi, IM_COL32(255, 200, 60, 255),
                7.0f, 0, 2.5f);

    ImFont* font = ImGui::GetFont();
    constexpr const char* kTitle = "GAME PAUSED";
    constexpr const char* kInstruction = "Press P to unpause";
    const float title_px = ImGui::GetFontSize() * 1.55f;
    const float instruction_px = ImGui::GetFontSize() * 1.08f;
    const ImVec2 title_size = font->CalcTextSizeA(title_px, 1000.0f, 0.0f, kTitle);
    const ImVec2 instruction_size =
        font->CalcTextSizeA(instruction_px, 1000.0f, 0.0f, kInstruction);
    fg->AddText(font, title_px,
                ImVec2(box_lo.x + (box_size.x - title_size.x) * 0.5f,
                       box_lo.y + 24.0f),
                IM_COL32(255, 220, 120, 255), kTitle);
    fg->AddText(font, instruction_px,
                ImVec2(box_lo.x + (box_size.x - instruction_size.x) * 0.5f,
                       box_lo.y + 70.0f),
                IM_COL32(230, 210, 155, 255), kInstruction);
}

void build_weapons_status(const WeaponsHudState& w) {
    // Suppress the whole panel when the player has zero ammo — both lines
    // become noise (DF always shows "DUMBFIRE", HS/IR always shows the
    // lock-state legend which is meaningless without missiles to fire).
    if (w.missile_count <= 0) return;
    // Cockpit art up: ordnance lives in the centre MFD's left flank.
    if (draw_weapons_flank(w)) return;

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

    // ---- lock state (wording shared with the MFD flank) -----------------
    const LockReadout lock = lock_readout(w);
    dl->AddText(ImVec2(x, y), lock.col, lock.text);
    if (lock.show_progress) {
        // Build-up bar to the right of the label.
        const float bx = x + 64.0f, bw = 80.0f, bh = 8.0f;
        dl->AddRect(ImVec2(bx, y + 2.0f), ImVec2(bx + bw, y + 2.0f + bh), kDimAmber);
        const float f = std::clamp(w.lock_progress, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(bx + 1, y + 3.0f),
                          ImVec2(bx + 1 + (bw - 2) * f, y + 1.0f + bh), kCyan);
    }
    y += 22.0f;

    // Afterburner fuel gauge removed (np-zte.2 merged pool). The STATUS
    // panel's ENERGY bar is the burner gauge now — same pool, one
    // readout. Don't re-add a bar here unless we re-split the resources.
    (void)y;
}

// ---- resolve_nav_for_mission (click-to-target, T5) -------------------------
//
// On a mission-panel row click, resolve the nav point to select. Pure: no
// ImGui, no audio; nav_by_name/nav_by_base handle the in-system resolution,
// and a galaxy-graph hops_between() lookup picks a jump nav for cross-system
// cargo/bounty. Returns the nav index into system.nav_points, or -1 if no
// nav can be resolved (the caller leaves the previous selection alone).
static int resolve_nav_for_mission(const ActiveMission& am,
                                   const StarSystem& sys,
                                   const galaxy::Galaxy& galaxy,
                                   const std::string& current_system_id) {
    using MT = missions::MissionType;
    const MT type = (MT)am.type;
    const bool in_system =
        (type == MT::CargoDelivery ? am.dest_system == current_system_id
                                   : am.target_system == current_system_id);

    // ---- in-system: the objective resolves to a real nav/base in THIS system ----
    if (in_system) {
        // Patrol has a documented fallback to the first nav when every
        // target has already been surveyed (so a "completed patrol" click
        // isn't a silent no-op). Capture that fallback BEFORE the helper
        // skips done navs — then prefer the helper, then the fallback.
        const NavPointDef* first_nav = nullptr;
        if (type == MT::Patrol && !am.nav_targets.empty())
            first_nav = nav_by_name(sys, am.nav_targets[0]);

        const NavPointDef* def = objective_nav_in_system(am, sys);
        if (!def) def = first_nav;        // patrol all-done fallback

        if (def) {
            for (size_t k = 0; k < sys.nav_points.size(); ++k)
                if (&sys.nav_points[k] == def) return (int)k;
        }
        return -1;   // couldn't resolve in-system target
    }

    // ---- cross-system: pick a jump nav on the shortest path -----------
    // Only Cargo cross-system and Bounty with no in-region nav hit this
    // branch — anything else with an in-system target was handled above.
    // The target system id is the Cargo destination or the Bounty search
    // region (we use the first system in the region as the destination).
    std::string target_sys;
    if (type == MT::CargoDelivery) target_sys = am.dest_system;
    else if (type == MT::Bounty) {
        if (!am.bounty_region.empty()) target_sys = am.bounty_region.front();
        else target_sys = am.target_system;
    } else {
        target_sys = am.target_system;
    }
    if (target_sys.empty()) return -1;
    // Pre-compute the current->target distance so any nav that strictly
    // reduces it is a valid hop nav. We pick the first one that does.
    const int target_hops = missions::hops_between(galaxy, current_system_id, target_sys);
    int best_k = -1;
    int best_d = target_hops;   // lower is better; "no improvement" == current
    for (size_t i = 0; i < sys.nav_points.size(); ++i) {
        const NavPointDef& n = sys.nav_points[i];
        // Need to be a jump nav with a real link out of this system.
        const std::string& to_sys =
            n.links_to.empty() ? current_system_id : n.links_to;
        const int d = missions::hops_between(galaxy, to_sys, target_sys);
        if (d < 0) continue;       // unreachable branch
        if (d < best_d) {
            best_d = d;
            best_k = (int)i;
        }
    }
    return best_k;
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
                  const PlayerState& player,
                  const std::string& current_system_id,
                  const galaxy::Galaxy& galaxy,
                  bool& shown_in_out,
                  bool sector_pane_open) {
    if (!shown_in_out) return;

    const auto sz = screen_size();
    // Two-pane layout: full-screen-ish navmap window (np-19.3). 98% wide,
    // 94% tall, centred — fits both the map AND a right-side mission
    // panel without forcing the user to resize the window. Title says it
    // out loud so the panel isn't a surprise.
    float win_w = sz.w * 0.98f;
    const float win_h = sz.h * 0.94f;
    float win_x  = (sz.w - win_w) * 0.5f;
    // When the sector map is open (M inside the navmap), tighten the
    // navmap to the left so both maps are visible side-by-side.
    if (sector_pane_open) {
        win_w = sz.w * 0.55f;
        win_x = sz.w * 0.01f;
    }
    ImGui::SetNextWindowPos(ImVec2(win_x,
                                   (sz.h - win_h) * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
    // Full opacity — the map panel is opaque so navmap navpoints aren't
    // muddied by the cockpit backing through it (np-19.3 was 0.92).
    ImGui::SetNextWindowBgAlpha(1.0f);

    push_hud_style();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    if (ImGui::Begin("NAVIGATION MAP \xE2\x80\x94 MISSION STATUS  (N cycles, Esc to close)",
                     &open, flags)) {

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

        // ---- two-pane layout (np-19.3) -------------------------------------
        // 62 / 38 split of the window content with a small visual gap. Both
        // children use the same vertical extent so the bottom aligns — the
        // map pane owns the left, the mission-status panel owns the right.
        const ImVec2 win_sz = ImGui::GetContentRegionAvail();
        constexpr float kGap = 8.0f;
        const float left_w  = std::max(0.0f, win_sz.x * 0.62f - kGap * 0.5f);
        const float right_w = std::max(0.0f, win_sz.x - left_w - kGap);

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

        // ==== LEFT pane: top-down navmap =================================
        ImGui::BeginChild("##navmap_map",
                          ImVec2(left_w, win_sz.y - 24.0f),
                          /*border=*/false,
                          ImGuiWindowFlags_None);
        // Square map area: take the smaller content-region dimension of
        // the LEFT child (not the whole window) as the edge so the map is
        // always square, then centre it within whatever space remains.
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
            const bool iff = scanner::color_iff(player.fitted_scanner);
            for (const PerceivedContact& c : player.perception.visible) {
                const HMM_Vec3 p = HMM_AddV3(
                    player.position, HMM_MulV3F(c.to_unit, c.distance_m));
                const HMM_Vec2 mp = world_map_pos(p);
                const ImVec2 sp = to_screen(mp.X, mp.Y);
                const ImU32 col = contact_color(c.stance, iff, 230);
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

        // ---- 2D objective rings (np-19.3 / T6) ---------------------------
        // Thin cyan ring on top of the nav point an objective points at, so
        // jobs read at a glance on the 2D map. Distinct from the amber
        // selection outline (which is thicker + on selected navs only).
        // Mirrors the 3D diamond draw_objective_marker() paints in the world
        // — same resolver logic, same colours.
        auto draw_ring = [&](const NavPointDef& n,
                             ImU32 col = kObjective, float thickness = 1.5f) {
            const HMM_Vec2 mp = nav_map_pos(n);
            const ImVec2 sp2 = to_screen(mp.X, mp.Y);
            // radius slightly bigger than the marker so it frames the dot
            // without colliding with neighbour rings at this scale.
            dl->AddCircle(sp2, 14.0f, col, 24, thickness);
        };
        for (const ActiveMission& am : player.missions) {
            using MT = missions::MissionType;
            const MT type = (MT)am.type;
            const std::string& tgt_sys =
                (type == MT::CargoDelivery) ? am.dest_system : am.target_system;
            if (tgt_sys != current_system_id) continue;     // only this system
            if (type == MT::Patrol) {
                // Multi-nav route: ring every unsurveyed nav target in this
                // system; surveyed navs get a dimmer, thinner ring for route
                // progress. Mirrors the 3D diamond draw in
                // build_mission_objectives. Other types stay single-nav.
                std::vector<const NavPointDef*> surveyed;
                const std::vector<const NavPointDef*> unsurveyed =
                    objective_navs_in_system(am, system, &surveyed);
                for (const NavPointDef* n : unsurveyed) draw_ring(*n);
                for (const NavPointDef* n : surveyed)    draw_ring(*n, kObjectiveDim, 1.0f);
            } else if (const NavPointDef* n =
                           objective_nav_in_system(am, system)) {
                draw_ring(*n);
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

        // Tiny footer for the LEFT pane only — reads as part of the map
        // chrome rather than the window chrome.
        ImGui::SetCursorScreenPos(ImVec2(area_p0.x, area_p0.y + area_sz.y + 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kDimAmber);
        ImGui::TextUnformatted("Click a nav point to select it.  N cycles; Esc / X to close.");
        ImGui::PopStyleColor();

        ImGui::EndChild();     // ##navmap_map

        // ==== RIGHT pane: mission status ==================================
        // SameLine() resets the cursor X so the second child sits flush
        // against the right edge of the left one. The small gap is what
        // makes the two-pane layout read as two panels, not one blob.
        ImGui::SameLine();
        ImGui::BeginChild("##navmap_missions",
                          ImVec2(right_w, win_sz.y - 24.0f),
                          /*border=*/true,
                          ImGuiWindowFlags_None);
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            char hdr[80];
            std::snprintf(hdr, sizeof(hdr),
                          "MISSION STATUS (%d active)",
                          (int)player.missions.size());
            ImGui::TextUnformatted(hdr);
            ImGui::PopStyleColor();
            ImGui::Separator();

            if (player.missions.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDimAmber);
                ImGui::TextUnformatted("No active missions.");
                ImGui::PopStyleColor();
            } else {
                ImGui::BeginChild("##navmap_missions_scroll",
                                  ImVec2(0, 0), false,
                                  ImGuiWindowFlags_None);

                // Display name for a system id (galaxy catalog); raw id if
                // unknown. Used by the destination / bounty-search lines.
                auto sys_disp = [&](const std::string& id) -> std::string {
                    if (id.empty()) return std::string();
                    if (const galaxy::SystemEntry* se = galaxy.find(id))
                        return se->display_name;
                    return id;
                };
                // Per-type accent colour for the badge + left rail so the
                // job type reads at a glance instead of buried in text.
                auto type_accent = [](missions::MissionType t) -> ImVec4 {
                    using MT = missions::MissionType;
                    switch (t) {
                        case MT::Bounty:        return ImVec4(1.00f, 0.47f, 0.35f, 1.0f);
                        case MT::Attack:        return ImVec4(1.00f, 0.38f, 0.38f, 1.0f);
                        case MT::DefendBase:    return ImVec4(0.40f, 0.68f, 1.00f, 1.0f);
                        case MT::Patrol:        return ImVec4(0.38f, 0.82f, 1.00f, 1.0f);
                        case MT::Scout:         return ImVec4(0.50f, 0.90f, 0.58f, 1.0f);
                        case MT::CargoDelivery: return ImVec4(1.00f, 0.80f, 0.38f, 1.0f);
                        default:                return ImVec4(0.78f, 0.78f, 0.78f, 1.0f);
                    }
                };

                ImDrawList* dl    = ImGui::GetWindowDrawList();
                const ImVec4 dimV   = ImVec4(0.62f, 0.58f, 0.42f, 1.0f);
                const ImVec4 whiteV = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
                const ImVec4 goldV  = ImVec4(1.00f, 0.85f, 0.40f, 1.0f);
                const ImVec4 hereV  = ImVec4(0.55f, 0.95f, 0.60f, 1.0f);

                for (size_t mi = 0; mi < player.missions.size(); ++mi) {
                    const ActiveMission& am = player.missions[mi];
                    const auto type = (missions::MissionType)am.type;
                    const missions::MissionStatus st =
                        missions::mission_status(am, system, current_system_id);
                    const ImVec4 accent = type_accent(type);

                    ImGui::PushID((int)mi);

                    const ImVec2 card_tl = ImGui::GetCursorScreenPos();
                    const float  card_w  = ImGui::GetContentRegionAvail().x;
                    constexpr float pad  = 8.0f;

                    // Content on channel 1; card background + accent rail on
                    // channel 0 once we know the laid-out height.
                    dl->ChannelsSplit(2);
                    dl->ChannelsSetCurrent(1);

                    ImGui::BeginGroup();
                    ImGui::Indent(pad + 4.0f);
                    ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));

                    // -- header: type badge + right-aligned reward --
                    ImGui::TextColored(accent, "%s", missions::type_label(type));
                    char rew[40];
                    std::snprintf(rew, sizeof(rew), "%lld cr", (long long)am.reward);
                    ImGui::SameLine();
                    {
                        const float rw   = ImGui::CalcTextSize(rew).x;
                        const float room = ImGui::GetContentRegionAvail().x;
                        if (room > rw + pad) {
                            ImGui::Dummy(ImVec2(room - rw - pad, 0.0f));
                            ImGui::SameLine();
                        }
                        ImGui::TextColored(goldV, "%s", rew);
                    }

                    // -- concise status line (includes progress counters) --
                    ImGui::TextColored(whiteV, "%s", st.text.c_str());

                    // -- destination / search line(s), per type --
                    if (type == missions::MissionType::Bounty) {
                        // THE point of this panel for a bounty: which systems
                        // to hunt in. List the posted region, highlighting the
                        // one you're currently in.
                        ImGui::TextColored(dimV, "Search:");
                        bool any = false;
                        for (const std::string& rid : am.bounty_region) {
                            const std::string nm = sys_disp(rid);
                            if (nm.empty()) continue;
                            if (any) { ImGui::SameLine(0.0f, 0.0f); ImGui::TextColored(dimV, ","); }
                            ImGui::SameLine(0.0f, any ? 4.0f : 6.0f);
                            const bool here = (rid == current_system_id);
                            ImGui::TextColored(here ? hereV : whiteV, "%s%s",
                                               nm.c_str(), here ? " (here)" : "");
                            any = true;
                        }
                        if (!any) {
                            const std::string ls = sys_disp(am.last_seen_system);
                            ImGui::SameLine(0.0f, 6.0f);
                            ImGui::TextColored(whiteV, "%s",
                                ls.empty() ? "anywhere in range" : ls.c_str());
                        }
                    } else if (type == missions::MissionType::CargoDelivery) {
                        const std::string sys = sys_disp(am.dest_system);
                        ImGui::TextColored(dimV, "Deliver:");
                        ImGui::SameLine();
                        ImGui::TextColored(whiteV, "%s%s%s",
                            am.dest_base.c_str(),
                            (!am.dest_base.empty() && !sys.empty()) ? "  \xC2\xB7  " : "",
                            sys.c_str());
                    } else {
                        if (!st.in_current_system && !st.target_system.empty()) {
                            ImGui::TextColored(dimV, "Travel to:");
                            ImGui::SameLine();
                            ImGui::TextColored(whiteV, "%s",
                                               sys_disp(st.target_system).c_str());
                        } else {
                            ImGui::TextColored(hereV, "In this system");
                        }
                    }

                    // -- footer: issuing guild --
                    ImGui::TextColored(dimV, "%s",
                        missions::source_label((missions::MissionSource)am.source));

                    ImGui::Unindent(pad + 4.0f);
                    ImGui::Dummy(ImVec2(0.0f, pad * 0.5f));
                    ImGui::EndGroup();

                    // Whole-card click routes to the mission's nav (non-bounty;
                    // a bounty has no single nav, only a search region).
                    const ImVec2 rmin = ImGui::GetItemRectMin();
                    const ImVec2 rmax = ImGui::GetItemRectMax();
                    const ImVec2 bg0(card_tl.x, rmin.y);
                    const ImVec2 bg1(card_tl.x + card_w, rmax.y);

                    bool hovered = false, clicked = false;
                    const bool clickable = (type != missions::MissionType::Bounty);
                    if (clickable) {
                        ImGui::SetCursorScreenPos(bg0);
                        ImGui::InvisibleButton("##card_hit",
                            ImVec2(card_w, rmax.y - rmin.y));
                        hovered = ImGui::IsItemHovered();
                        clicked = ImGui::IsItemClicked();
                    }

                    dl->ChannelsSetCurrent(0);
                    const ImU32 bgc = hovered ? IM_COL32(255, 255, 255, 24)
                                              : IM_COL32(255, 255, 255, 10);
                    dl->AddRectFilled(bg0, bg1, bgc, 5.0f);
                    dl->AddRect(bg0, bg1, IM_COL32(255, 255, 255, 32), 5.0f);
                    dl->AddRectFilled(bg0, ImVec2(bg0.x + 3.0f, bg1.y),
                                      ImGui::GetColorU32(accent), 5.0f);
                    dl->ChannelsMerge();

                    if (clicked) {
                        const int idx = resolve_nav_for_mission(
                            am, system, galaxy, current_system_id);
                        if (idx >= 0) {
                            selected_nav_in_out = idx;
                            sfx::ui_click();
                            std::printf("[nav] target → %s\n",
                                system.nav_points[idx].name.c_str());
                        }
                    }

                    ImGui::Dummy(ImVec2(0.0f, 6.0f));   // gap between cards
                    ImGui::PopID();
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();     // ##navmap_missions
    }
    ImGui::End();
    pop_hud_style();

    // N cycles the selected nav while the map is up. Polled here (not in
    // event_cb) so the cycle still works when ImGui has keyboard focus
    // because the mouse is hovering the navmap — event_cb's branch on
    // N lives after ImGui and an ImGui-focused window would otherwise
    // eat the keypress. Same edge-triggered idiom as the Esc close below.
    if (ImGui::IsKeyPressed(ImGuiKey_N)) {
        if (!system.nav_points.empty()) {
            const int n = (int)system.nav_points.size();
            selected_nav_in_out = (selected_nav_in_out + 1) % n;
            sfx::ui_click();
            std::printf("[nav] target → %s\n",
                        system.nav_points[selected_nav_in_out].name.c_str());
        }
    }

    // ESC closes too. Title-bar X also flips `open` to false.
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        shown_in_out = false;
    }
}

void build_sector_navmap(const galaxy::Galaxy& galaxy,
                         const std::string& current_system_id,
                         bool& shown_in_out,
                         bool beside_navmap) {
    if (!shown_in_out || galaxy.empty()) return;

    const auto screen = screen_size();
    // Standalone: a big centred overlay. When opened from within the
    // navmap (M while N is up), sit in the RIGHT pane so the local
    // navmap (left) and sector map (right) are both visible "also".
    float win_w = screen.w * 0.96f;
    const float win_h = screen.h * 0.94f;
    float win_x = (screen.w - win_w) * 0.5f;
    if (beside_navmap) {
        win_w = screen.w * 0.43f;
        win_x = screen.w - win_w - screen.w * 0.01f;
    }
    ImGui::SetNextWindowPos(ImVec2(win_x, (screen.h - win_h) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(1.0f);

    push_hud_style();
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                                 | ImGuiWindowFlags_NoResize
                                 | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    if (!ImGui::Begin("SECTOR NAVIGATION MAP", &open, flags)) {
        ImGui::End();
        pop_hud_style();
        if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) shown_in_out = false;
        return;
    }

    const ImVec2 content_p0 = ImGui::GetCursorScreenPos();
    const ImVec2 content_sz = ImGui::GetContentRegionAvail();
    constexpr float kHeaderH = 34.0f;
    constexpr float kFooterH = 30.0f;
    constexpr float kGap = 6.0f;
    const float map_h = fmaxf(100.0f, content_sz.y - kHeaderH - kFooterH - 2.0f * kGap);
    const float map_w = fminf(content_sz.x, map_h);
    const ImVec2 map_p0(content_p0.x + (content_sz.x - map_w) * 0.5f,
                        content_p0.y + kHeaderH + kGap);
    const ImVec2 map_p1(map_p0.x + map_w, map_p0.y + map_h);
    const ImVec2 map_ctr((map_p0.x + map_p1.x) * 0.5f,
                         (map_p0.y + map_p1.y) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 green = IM_COL32(20, 255, 55, 255);
    const ImU32 green_dim = IM_COL32(20, 170, 50, 120);
    const ImU32 yellow = IM_COL32(255, 225, 20, 220);
    const ImU32 red = IM_COL32(235, 45, 30, 210);
    const ImU32 chart_text = IM_COL32(190, 255, 170, 245);
    constexpr float kChartFont = 12.0f;

    // Dedicated header band: nothing from the map is allowed to intrude here.
    const char* title = "GEMINI SECTOR NAVIGATION CHART";
    const char* controls = "M: CLOSE   ESC: CLOSE";
    dl->AddText(nullptr, 15.0f,
                ImVec2(content_p0.x + 8.0f, content_p0.y + 4.0f),
                chart_text, title);
    const ImVec2 controls_sz = ImGui::CalcTextSize(controls);
    dl->AddText(nullptr, kChartFont,
                ImVec2(content_p0.x + content_sz.x - controls_sz.x - 8.0f,
                       content_p0.y + 7.0f), chart_text, controls);
    dl->AddLine(ImVec2(content_p0.x, content_p0.y + kHeaderH),
                ImVec2(content_p0.x + content_sz.x, content_p0.y + kHeaderH),
                green, 1.5f);

    dl->AddRectFilled(map_p0, map_p1, IM_COL32(3, 6, 10, 255));
    dl->PushClipRect(map_p0, map_p1, true);

    // Stable decorative stars; this is ambience, not graph data.
    uint32_t star = 0x6d2b79f5u;
    for (int i = 0; i < 180; ++i) {
        star ^= star << 13; star ^= star >> 17; star ^= star << 5;
        const float x = float(star & 0xffffu) / 65535.0f;
        star ^= star << 13; star ^= star >> 17; star ^= star << 5;
        const float y = float(star & 0xffffu) / 65535.0f;
        dl->AddCircleFilled(ImVec2(map_p0.x + x * map_w, map_p0.y + y * map_h),
                            (i % 9 == 0) ? 1.1f : 0.65f,
                            IM_COL32(180, 200, 225, (i % 9 == 0) ? 100 : 55));
    }

    // Privateer-style four-quadrant grid with lighter subdivisions.
    for (int i = 0; i <= 4; ++i) {
        const float t = float(i) / 4.0f;
        const ImU32 color = (i == 0 || i == 2 || i == 4) ? green : green_dim;
        const float width = (i == 0 || i == 2 || i == 4) ? 1.8f : 0.8f;
        const float x = map_p0.x + t * map_w;
        const float y = map_p0.y + t * map_h;
        dl->AddLine(ImVec2(x, map_p0.y), ImVec2(x, map_p1.y), color, width);
        dl->AddLine(ImVec2(map_p0.x, y), ImVec2(map_p1.x, y), color, width);
    }
    dl->AddRect(map_p0, map_p1, green, 0.0f, 0, 2.5f);

    // QUADRANT.IFF coordinates are local to each quadrant. Scale each chart
    // axis to its own display axis so systems fill the available quadrant.
    // The stored canonical coordinates themselves remain untouched.
    struct Bounds {
        float min_x = 1e9f, max_x = -1e9f;
        float min_y = 1e9f, max_y = -1e9f;
    };
    auto quadrant_for = [](const galaxy::SystemEntry& system) {
        if (system.sector.find("Fariss") != std::string::npos) return 0;
        if (system.sector.find("Clarke") != std::string::npos) return 1;
        if (system.sector.find("Humboldt") != std::string::npos) return 2;
        return 3; // Potter; all catalog entries use one of the four quadrants.
    };
    // Temporary survey policy: these systems are not chart-visible before the
    // Taryn Cross missions. Gamma/Delta/Delta Prime/Beta still define Fariss's
    // stable fit bounds; Eden is omitted from both drawing and fit bounds.
    auto is_hidden = [](const galaxy::SystemEntry& system) {
        return system.id == "eden" || system.id == "gamma" ||
               system.id == "delta" || system.id == "delta_prime" ||
               system.id == "beta";
    };
    auto contributes_to_bounds = [](const galaxy::SystemEntry& system) {
        return system.id != "eden";
    };
    Bounds bounds[4];
    for (const auto& system : galaxy.systems) {
        if (!contributes_to_bounds(system)) continue;
        Bounds& b = bounds[quadrant_for(system)];
        b.min_x = fminf(b.min_x, system.galaxy_position.X);
        b.max_x = fmaxf(b.max_x, system.galaxy_position.X);
        b.min_y = fminf(b.min_y, system.galaxy_position.Y);
        b.max_y = fmaxf(b.max_y, system.galaxy_position.Y);
    }
    const float quadrant_w = map_w * 0.5f;
    const float quadrant_h = map_h * 0.5f;
    constexpr float inset_x = 48.0f;
    constexpr float inset_y = 38.0f;
    auto to_screen = [&](const galaxy::SystemEntry& system) {
        const int q = quadrant_for(system);
        const Bounds& b = bounds[q];
        const float source_w = fmaxf(1.0f, b.max_x - b.min_x);
        const float source_h = fmaxf(1.0f, b.max_y - b.min_y);
        const float plot_w = quadrant_w - 2.0f * inset_x;
        const float plot_h = quadrant_h - 2.0f * inset_y;
        const float scale_x = plot_w / source_w;
        const float scale_y = plot_h / source_h;
        const float origin_x = map_p0.x + (q % 2) * quadrant_w + inset_x;
        const float origin_y = map_p0.y + (q / 2) * quadrant_h + inset_y;
        return ImVec2(origin_x + (system.galaxy_position.X - b.min_x) * scale_x,
                      origin_y + (system.galaxy_position.Y - b.min_y) * scale_y);
    };

    // Lines are local only when both authored systems share a sector.
    for (const auto& jump : galaxy.jumps) {
        if (!(jump.from < jump.to)) continue;
        const galaxy::SystemEntry* a = galaxy.find(jump.from);
        const galaxy::SystemEntry* b = galaxy.find(jump.to);
        if (!a || !b || is_hidden(*a) || is_hidden(*b)) continue;
        dl->AddLine(to_screen(*a), to_screen(*b),
                    a->sector == b->sector ? yellow : red, 1.1f);
    }

    // Put quadrant names in protected corner strips, not over the title.
    struct QuadrantLabel { const char* name; ImVec2 anchor; bool right; bool bottom; };
    const QuadrantLabel quadrant_labels[] = {
        {"FARISS QUADRANT",    ImVec2(map_p0.x + 8.0f, map_p0.y + 7.0f), false, false},
        {"CLARKE QUADRANT",    ImVec2(map_p1.x - 8.0f, map_p0.y + 7.0f), true,  false},
        {"HUMBOLDT QUADRANT",  ImVec2(map_p0.x + 8.0f, map_p1.y - 20.0f), false, true},
        {"POTTER QUADRANT",    ImVec2(map_p1.x - 8.0f, map_p1.y - 20.0f), true,  true},
    };
    for (const auto& q : quadrant_labels) {
        const ImVec2 size = ImGui::CalcTextSize(q.name);
        const float x = q.right ? q.anchor.x - size.x : q.anchor.x;
        dl->AddRectFilled(ImVec2(x - 3.0f, q.anchor.y - 2.0f),
                          ImVec2(x + size.x + 3.0f, q.anchor.y + 15.0f),
                          IM_COL32(3, 6, 10, 220));
        dl->AddText(nullptr, kChartFont, ImVec2(x, q.anchor.y), green, q.name);
    }

    // Nodes first, then labels. Labels try eight placements and choose the
    // candidate with the least overlap against labels already accepted.
    for (const auto& system : galaxy.systems) {
        if (is_hidden(system)) continue;
        const ImVec2 p = to_screen(system);
        const bool current = system.id == current_system_id;
        dl->AddCircleFilled(p, current ? 5.0f : 2.5f,
                            current ? IM_COL32(170, 250, 255, 255)
                                    : IM_COL32(245, 245, 225, 255));
        dl->AddCircleFilled(p, current ? 9.0f : 5.0f,
                            current ? IM_COL32(40, 220, 255, 70)
                                    : IM_COL32(255, 245, 180, 45));
        if (current) dl->AddCircle(p, 12.0f, IM_COL32(50, 235, 255, 255), 24, 1.8f);
    }

    std::vector<ImVec4> occupied;
    occupied.reserve(galaxy.systems.size());
    constexpr float kPad = 2.0f;
    for (const auto& system : galaxy.systems) {
        if (is_hidden(system)) continue;
        const ImVec2 node = to_screen(system);
        const ImVec2 text_size = ImGui::CalcTextSize(system.display_name.c_str());
        const ImVec2 candidates[] = {
            {node.x + 7.0f, node.y - text_size.y * 0.5f},
            {node.x - text_size.x - 7.0f, node.y - text_size.y * 0.5f},
            {node.x - text_size.x * 0.5f, node.y - text_size.y - 7.0f},
            {node.x - text_size.x * 0.5f, node.y + 7.0f},
            {node.x + 6.0f, node.y - text_size.y - 5.0f},
            {node.x - text_size.x - 6.0f, node.y - text_size.y - 5.0f},
            {node.x + 6.0f, node.y + 5.0f},
            {node.x - text_size.x - 6.0f, node.y + 5.0f},
        };

        float best_score = 1e30f;
        ImVec2 best = candidates[0];
        for (const ImVec2& candidate : candidates) {
            const ImVec4 box(candidate.x - kPad, candidate.y - kPad,
                             candidate.x + text_size.x + kPad,
                             candidate.y + text_size.y + kPad);
            float score = 0.0f;
            if (box.x < map_p0.x + 3.0f || box.z > map_p1.x - 3.0f ||
                box.y < map_p0.y + 24.0f || box.w > map_p1.y - 24.0f) {
                score += 100000.0f;
            }
            for (const ImVec4& other : occupied) {
                const float overlap_w = fmaxf(0.0f, fminf(box.z, other.z) - fmaxf(box.x, other.x));
                const float overlap_h = fmaxf(0.0f, fminf(box.w, other.w) - fmaxf(box.y, other.y));
                score += overlap_w * overlap_h;
            }
            if (score < best_score) { best_score = score; best = candidate; }
        }
        occupied.emplace_back(best.x - kPad, best.y - kPad,
                              best.x + text_size.x + kPad,
                              best.y + text_size.y + kPad);
        dl->AddText(nullptr, kChartFont, best,
                    system.id == current_system_id ? IM_COL32(120, 250, 255, 255)
                                                   : IM_COL32(225, 235, 225, 240),
                    system.display_name.c_str());
    }
    dl->PopClipRect();

    // Dedicated footer band contained inside the window.
    const float footer_y = map_p1.y + kGap + 5.0f;
    float x = map_ctr.x - 230.0f;
    dl->AddLine(ImVec2(x, footer_y + 5.0f), ImVec2(x + 22.0f, footer_y + 5.0f), yellow, 1.5f);
    dl->AddText(nullptr, kChartFont, ImVec2(x + 28.0f, footer_y), chart_text, "LOCAL ROUTE");
    x += 150.0f;
    dl->AddLine(ImVec2(x, footer_y + 5.0f), ImVec2(x + 22.0f, footer_y + 5.0f), red, 1.5f);
    dl->AddText(nullptr, kChartFont, ImVec2(x + 28.0f, footer_y), chart_text, "INTERQUADRANT ROUTE");
    x += 220.0f;
    dl->AddCircle(ImVec2(x + 5.0f, footer_y + 5.0f), 7.0f, IM_COL32(50, 235, 255, 255), 16, 1.5f);
    dl->AddText(nullptr, kChartFont, ImVec2(x + 18.0f, footer_y), chart_text, "CURRENT SYSTEM");

    ImGui::End();
    pop_hud_style();
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) shown_in_out = false;
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
