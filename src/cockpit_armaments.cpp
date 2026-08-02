// Interactive cockpit armaments schematic.
#include "cockpit_armaments.h"

#include "armament_loadout.h"
#include "equipment_hardpoints.h"
#include "firing.h"
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

constexpr ImU32 kGrid = IM_COL32(42, 68, 82, 80);
constexpr ImU32 kArmed = IM_COL32(105, 240, 135, 245);
constexpr ImU32 kOff = IM_COL32(180, 150, 60, 220);
constexpr ImU32 kEmpty = IM_COL32(90, 100, 110, 180);
constexpr ImU32 kDrop = IM_COL32(120, 220, 255, 255);
constexpr const char* kPayload = "COCKPIT_GUN_SLOT";

Layout g_layout;
std::string g_loaded_ship;
TextureSlot g_ship_texture;

void release_texture() {
    if (g_ship_texture.view.id) sg_destroy_view(g_ship_texture.view);
    if (g_ship_texture.image.id) sg_destroy_image(g_ship_texture.image);
    g_ship_texture = {};
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
    for (int i = 1; i < 6; ++i) {
        const float offset = side * (float)i / 6.0f;
        dl->AddLine(ImVec2(lo.x + offset, lo.y), ImVec2(lo.x + offset, hi.y), kGrid);
        dl->AddLine(ImVec2(lo.x, lo.y + offset), ImVec2(hi.x, lo.y + offset), kGrid);
    }
    if (g_ship_texture.valid) {
        // Match the equipment bay's authored orientation: nose points upward.
        dl->AddImageQuad(simgui_imtextureid(g_ship_texture.view),
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
    const ImVec2 center = zone_center(zone, image_lo, side);
    const float marker_w = std::clamp(zone.rect[2] * side, 18.0f, 28.0f);
    const float marker_h = std::clamp(zone.rect[3] * side, 18.0f, 28.0f);
    const ImVec2 lo(center.x - marker_w * 0.5f, center.y - marker_h * 0.5f);
    const ImVec2 hi(center.x + marker_w * 0.5f, center.y + marker_h * 0.5f);

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

} // namespace

void draw(PlayerState& player, Ship& live_ship) {
    ensure_layout(player, live_ship);
    const auto& unique = firing::gun_unique_types_cache(live_ship.mounts);
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.30f, 1.0f), "ARMAMENTS");
    ImGui::SameLine();
    ImGui::TextDisabled("%s  %d/%zu",
        firing::gun_mode_label(unique, live_ship.gun_mode_idx),
        firing::gun_mode_armed_count(live_ship), live_ship.mounts.size());

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float side = std::max(64.0f, std::min(avail.x, avail.y));
    const ImVec2 image_lo(cursor.x + (avail.x - side) * 0.5f, cursor.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_background(dl, image_lo, side);
    for (const Zone& zone : g_layout.zones)
        draw_hardpoint(player, live_ship, zone, image_lo, side);

    ImGui::SetCursorScreenPos(cursor);
    ImGui::Dummy(ImVec2(avail.x, side));
}

} // namespace cockpit_armaments
