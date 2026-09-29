// Interactive cockpit armaments schematic.
#include "cockpit_armaments.h"

#include "armament_loadout.h"
#include "equipment_hardpoints.h"
#include "firing.h"
#include "hud_text_fit.h"
#include "launcher_modes.h"
#include "material.h"
#include "player.h"
#include "ship.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace cockpit_armaments {
namespace {

using equipment_hardpoints::Kind;
using equipment_hardpoints::Layout;
using equipment_hardpoints::Zone;

constexpr ImU32 kArmed = IM_COL32(105, 240, 135, 245);
constexpr ImU32 kOff = IM_COL32(180, 150, 60, 220);
constexpr ImU32 kEmpty = IM_COL32(90, 100, 110, 180);
constexpr ImU32 kDrop = IM_COL32(120, 220, 255, 255);
constexpr ImU32 kTitle = IM_COL32(255, 217, 77, 255);
constexpr const char* kPayload = "COCKPIT_GUN_SLOT";

Layout g_layout;
std::string g_loaded_ship;
TextureSlot g_ship_texture;
sg_sampler g_ship_sampler{};
bool g_pointer_over = false;
bool g_weapon_drag_active = false;

void release_texture() {
    if (g_ship_texture.view.id) sg_destroy_view(g_ship_texture.view);
    if (g_ship_texture.image.id) sg_destroy_image(g_ship_texture.image);
    g_ship_texture = {};
}

void ensure_sampler() {
    if (g_ship_sampler.id) return;
    sg_sampler_desc desc{};
    desc.min_filter = SG_FILTER_LINEAR;
    desc.mag_filter = SG_FILTER_LINEAR;
    desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    desc.label = "cockpit-armaments-linear-sampler";
    g_ship_sampler = sg_make_sampler(&desc);
}

void ensure_layout(const PlayerState& player, const Ship& ship) {
    if (g_loaded_ship == player.ship_class_name) return;
    g_loaded_ship = player.ship_class_name;
    equipment_hardpoints::load(g_loaded_ship, (int)ship.mounts.size(), g_layout);
    release_texture();
    if (!g_layout.sprite.empty() && !load_texture_png(g_layout.sprite, g_ship_texture)) {
        std::fprintf(stderr, "[cockpit] armaments sprite unavailable: %s\n",
                     g_layout.sprite.c_str());
    }
}

bool is_gun_zone(const Zone& zone) {
    return zone.kind == Kind::Gun || zone.kind == Kind::Turret;
}

std::string display_name(const PlayerState& player, int slot) {
    if (slot < 0 || slot >= (int)player.gun_mounts.size() ||
        player.gun_mounts[(size_t)slot].gun_id.empty()) {
        return "EMPTY";
    }
    std::string name = player.gun_mounts[(size_t)slot].gun_id;
    for (char& c : name) {
        c = c == '_' ? ' ' : (char)std::toupper((unsigned char)c);
    }
    return name;
}

ImVec2 zone_center(const Zone& zone, ImVec2 image_lo, float side) {
    return ImVec2(image_lo.x + (zone.rect[0] + zone.rect[2] * 0.5f) * side,
                  image_lo.y + (zone.rect[1] + zone.rect[3] * 0.5f) * side);
}

void draw_background(ImDrawList* dl, ImVec2 lo, float side) {
    const ImVec2 hi(lo.x + side, lo.y + side);
    dl->AddRectFilled(lo, hi, IM_COL32(2, 7, 12, 235), 3.0f);
    if (g_ship_texture.valid && g_ship_sampler.id) {
        // Match the equipment bay's authored orientation: nose points upward.
        // The dedicated linear sampler preserves the PNG's anti-aliased alpha
        // coverage while shrinking 512px art into this ~250px monitor.
        dl->AddImageQuad(simgui_imtextureid_with_sampler(
                g_ship_texture.view, g_ship_sampler),
            lo, ImVec2(hi.x, lo.y), hi, ImVec2(lo.x, hi.y),
            ImVec2(1, 1), ImVec2(0, 1), ImVec2(0, 0), ImVec2(1, 0));
    }
}

void draw_hardpoint(PlayerState& player, Ship& ship, const Zone& zone,
                    ImVec2 image_lo, float side) {
    if (!is_gun_zone(zone) || zone.slot < 0) return;
    const size_t slot = (size_t)zone.slot;
    const bool fitted = slot < player.gun_mounts.size() &&
                        !player.gun_mounts[slot].gun_id.empty();
    const bool live = slot < ship.mounts.size();
    const bool armed = live && slot < ship.gun_armed.size() && ship.gun_armed[slot];
    // Preserve the authored normalized rectangle exactly. Scaling the whole
    // schematic scales every hardpoint uniformly; no per-axis clamping that
    // fattens narrow gun slots into generic square buttons.
    const ImVec2 lo(image_lo.x + zone.rect[0] * side,
                    image_lo.y + zone.rect[1] * side);
    const ImVec2 hi(image_lo.x + (zone.rect[0] + zone.rect[2]) * side,
                    image_lo.y + (zone.rect[1] + zone.rect[3]) * side);
    const float marker_w = hi.x - lo.x;
    const float marker_h = hi.y - lo.y;
    const ImVec2 center = zone_center(zone, image_lo, side);

    ImGui::PushID(zone.id.c_str());
    ImGui::SetCursorScreenPos(lo);
    ImGui::InvisibleButton("##hardpoint", ImVec2(marker_w, marker_h));
    const bool hovered = ImGui::IsItemHovered();
    const bool active_target = ImGui::BeginDragDropTarget();
    if (active_target) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kPayload)) {
            const int from = *static_cast<const int*>(payload->Data);
            if (payload->IsDelivery() && live &&
                armament_loadout::swap_mounted_weapons(
                    player, ship, (size_t)from, slot)) {
                firing::apply_gun_mode(ship, ship.gun_mode_idx);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (fitted && live && ImGui::BeginDragDropSource(
            ImGuiDragDropFlags_SourceNoDisableHover)) {
        const int payload_slot = (int)slot;
        ImGui::SetDragDropPayload(kPayload, &payload_slot, sizeof(payload_slot));
        ImGui::Text("MOVE %s", display_name(player, (int)slot).c_str());
        ImGui::EndDragDropSource();
    }

    const ImU32 color = active_target ? kDrop : fitted ? (armed ? kArmed : kOff) : kEmpty;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, IM_COL32(3, 13, 22, 180), 3.0f);
    dl->AddRect(lo, hi, color, 3.0f, 0, hovered ? 2.5f : 1.5f);
    char label[8];
    std::snprintf(label, sizeof label, "%d", zone.slot + 1);
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(center.x - text_size.x * 0.5f,
                       center.y - text_size.y * 0.5f), color, label);
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", zone.label.c_str());
        ImGui::TextColored(armed ? ImVec4(0.42f, 0.94f, 0.55f, 1.0f)
                                 : ImVec4(0.80f, 0.68f, 0.34f, 1.0f),
                           "%s%s", display_name(player, (int)slot).c_str(),
                           zone.kind == Kind::Turret ? "  [TURRET]" : "");
        if (fitted) ImGui::TextDisabled("Drag to another numbered hardpoint");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

void draw_launcher(const PlayerState& player, const Zone& zone,
                   int selected_ordnance, ImVec2 image_lo, float side) {
    if (zone.kind != Kind::Launcher || zone.slot < 0 || zone.slot > 1) return;
    const launcher_modes::SideState state =
        launcher_modes::side_state(player, zone.slot, selected_ordnance);
    const bool missile_installed = state.missile_installed;
    const bool torpedo_installed = state.torpedo_installed;
    const bool active = state.active;
    const bool fitted = missile_installed || torpedo_installed;

    const ImVec2 lo(image_lo.x + zone.rect[0] * side,
                    image_lo.y + zone.rect[1] * side);
    const ImVec2 hi(image_lo.x + (zone.rect[0] + zone.rect[2]) * side,
                    image_lo.y + (zone.rect[1] + zone.rect[3]) * side);
    const ImVec2 center = zone_center(zone, image_lo, side);
    const ImU32 color = !fitted ? kEmpty : active ? kArmed : kOff;
    static constexpr const char* kNames[kMissileTypeCount] = {
        "DF", "HS", "IR", "T"
    };
    const char* label = active ? kNames[selected_ordnance]
                      : missile_installed && torpedo_installed ? "M/T"
                      : missile_installed ? "M" : torpedo_installed ? "T" : "--";

    ImGui::PushID(zone.id.c_str());
    ImGui::SetCursorScreenPos(lo);
    ImGui::InvisibleButton("##launcher", ImVec2(hi.x - lo.x, hi.y - lo.y));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(lo, hi, IM_COL32(3, 13, 22, 180), 3.0f);
    dl->AddRect(lo, hi, color, 3.0f, 0, hovered ? 2.5f : 1.5f);
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(center.x - text_size.x * 0.5f,
                       center.y - text_size.y * 0.5f), color, label);
    if (hovered) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", zone.label.c_str());
        ImGui::TextColored(missile_installed ? ImVec4(0.42f, 0.94f, 0.55f, 1.0f)
                                             : ImVec4(0.50f, 0.50f, 0.50f, 1.0f),
                           "MISSILE LAUNCHER  %s", missile_installed ? "INSTALLED" : "EMPTY");
        ImGui::TextColored(torpedo_installed ? ImVec4(0.42f, 0.94f, 0.55f, 1.0f)
                                             : ImVec4(0.50f, 0.50f, 0.50f, 1.0f),
                           "TORPEDO LAUNCHER  %s", torpedo_installed ? "INSTALLED" : "EMPTY");
        ImGui::Separator();
        ImGui::Text("DF %d   HS %d   IR %d   TORP %d",
                    player.missiles[0], player.missiles[1], player.missiles[2],
                    player.torpedoes);
        if (active) ImGui::TextColored(ImVec4(0.42f, 0.94f, 0.55f, 1.0f), "ACTIVE");
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

} // namespace

void draw(PlayerState& player, Ship& live_ship, int selected_ordnance) {
    ensure_sampler();
    ensure_layout(player, live_ship);
    const auto& unique = firing::gun_unique_types_cache(live_ship.mounts);
    static constexpr const char* kNames[kMissileTypeCount] = {
        "DF", "HS", "IR", "TORP"
    };
    const int selected = std::clamp(selected_ordnance, 0, kMissileTypeCount - 1);

    // Header readouts, full wording in the classic box. A cockpit MFD is
    // ~half as wide (#430), so compact phrasings drop only what the
    // schematic below already shows: the page title and launcher counts.
    char gun[64], gun_short[64], launch[64], launch_short[32];
    const char* mode = firing::gun_mode_label(unique, live_ship.gun_mode_idx);
    const int armed = firing::gun_mode_armed_count(live_ship);
    std::snprintf(gun, sizeof gun, "GUN %s  %d/%zu", mode, armed, live_ship.mounts.size());
    std::snprintf(gun_short, sizeof gun_short, "%s  %d/%zu", mode, armed, live_ship.mounts.size());
    const int ammo = launcher_modes::ammo_count(player, selected);
    std::snprintf(launch, sizeof launch, "%s x%d  [MSL %d / TORP %d]",
        kNames[selected], ammo,
        launcher_modes::missile_launcher_count(player),
        launcher_modes::torpedo_launcher_count(player));
    std::snprintf(launch_short, sizeof launch_short, "%s x%d", kNames[selected], ammo);
    const hud_text_fit::Phrasing gun_line[] = { { "ARMAMENTS", gun }, { "GUN", gun_short } };
    const hud_text_fit::Phrasing launch_line[] = { { "LAUNCH", launch }, { "LAUNCH", launch_short } };
    const ImU32 detail = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    hud_text_fit::line(gun_line, IM_ARRAYSIZE(gun_line), kTitle, detail);
    hud_text_fit::line(launch_line, IM_ARRAYSIZE(launch_line), kTitle, detail);

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(64.0f, std::min(avail.x, avail.y));
    const ImVec2 image_lo(cursor.x + (avail.x - side) * 0.5f, cursor.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_background(dl, image_lo, side);
    for (const Zone& zone : g_layout.zones) {
        if (is_gun_zone(zone))
            draw_hardpoint(player, live_ship, zone, image_lo, side);
        else if (zone.kind == Kind::Launcher)
            draw_launcher(player, zone, selected, image_lo, side);
    }

    ImGui::SetCursorScreenPos(cursor);
    ImGui::Dummy(ImVec2(avail.x, side));

    // ImGui claims the mouse for an interactive window even on plain hover.
    // Cache enough detail for main's fly-by-wire path to distinguish a harmless
    // MFD hover from an actual weapon drag that genuinely owns the pointer.
    g_pointer_over = ImGui::IsWindowHovered(
        ImGuiHoveredFlags_ChildWindows |
        ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    g_weapon_drag_active = payload && payload->IsDataType(kPayload);
}

bool allows_flight_mouse_passthrough() {
    return g_pointer_over && !g_weapon_drag_active;
}

void shutdown() {
    release_texture();
    if (g_ship_sampler.id) sg_destroy_sampler(g_ship_sampler);
    g_ship_sampler = {};
    g_loaded_ship.clear();
    g_layout = {};
    g_pointer_over = false;
    g_weapon_drag_active = false;
}

} // namespace cockpit_armaments
