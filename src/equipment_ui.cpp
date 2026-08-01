// Visual ship-hardpoint equipment bay and in-game zone editor.
#include "equipment_ui_internal.h"

#include "base_screens.h"
#include "material.h"
#include "player.h"
#include "ship_class.h"
#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace outfitting::equipment_ui {
namespace {

using equipment_hardpoints::Kind;
using equipment_hardpoints::Layout;
using equipment_hardpoints::Zone;

constexpr ImVec4 kAccent(1.00f, 0.72f, 0.22f, 1.0f);
constexpr ImVec4 kDim(0.55f, 0.58f, 0.65f, 1.0f);
constexpr ImVec4 kGood(0.48f, 0.92f, 0.56f, 1.0f);
constexpr ImVec4 kBad(1.00f, 0.40f, 0.30f, 1.0f);

Layout g_layout;
std::string g_loaded_ship;
std::string g_selected_id;
std::string g_editor_ship;
TextureSlot g_ship_texture;
bool g_editing = false;
bool g_category_selected = true;
Kind g_category = Kind::Service;
enum class DragMode { None, Move, Resize };
DragMode g_drag = DragMode::None;

void release_texture() {
    if (g_ship_texture.view.id) sg_destroy_view(g_ship_texture.view);
    if (g_ship_texture.image.id) sg_destroy_image(g_ship_texture.image);
    g_ship_texture = {};
}

Zone* selected_zone() {
    for (Zone& zone : g_layout.zones)
        if (zone.id == g_selected_id) return &zone;
    if (g_layout.zones.empty()) return nullptr;
    g_selected_id = g_layout.zones.front().id;
    return &g_layout.zones.front();
}

void ensure_layout(const std::string& ship, int mounts) {
    if (g_loaded_ship == ship) return;
    g_loaded_ship = ship;
    equipment_hardpoints::load(ship, mounts, g_layout);
    g_selected_id = g_layout.zones.empty() ? std::string() : g_layout.zones.front().id;
    g_category_selected = true;
    g_category = Kind::Service;
    g_drag = DragMode::None;
    release_texture();
    if (!g_layout.sprite.empty() && !load_texture_png(g_layout.sprite, g_ship_texture))
        std::fprintf(stderr, "[equipment] top-down sprite unavailable: %s\n",
                     g_layout.sprite.c_str());
}

ImVec4 kind_color(Kind kind) {
    switch (kind) {
        case Kind::Gun:      return ImVec4(1.00f, 0.38f, 0.28f, 1.0f);
        case Kind::Turret:   return ImVec4(1.00f, 0.58f, 0.26f, 1.0f);
        case Kind::Launcher: return ImVec4(0.78f, 0.42f, 1.00f, 1.0f);
        case Kind::Armor:    return ImVec4(0.55f, 0.62f, 0.72f, 1.0f);
        case Kind::Shield:   return ImVec4(0.32f, 0.74f, 1.00f, 1.0f);
        case Kind::Engine:   return ImVec4(0.28f, 0.94f, 0.76f, 1.0f);
        case Kind::Cargo:    return ImVec4(0.92f, 0.78f, 0.28f, 1.0f);
        case Kind::Systems:  return ImVec4(0.45f, 0.90f, 0.45f, 1.0f);
        case Kind::Service:  return ImVec4(0.92f, 0.48f, 0.64f, 1.0f);
    }
    return kAccent;
}

std::string zone_state(const PlayerState& player, const Zone& zone,
                       const ShipClass* schematic_ship) {
    switch (zone.kind) {
        case Kind::Gun:
        case Kind::Turret:
            if (g_editing && schematic_ship) {
                if (zone.slot >= 0 && zone.slot < (int)schematic_ship->default_guns.size())
                    return gun::to_name(schematic_ship->default_guns[(size_t)zone.slot].type);
                return "EMPTY";
            }
            if (zone.slot >= 0 && zone.slot < (int)player.gun_mounts.size() &&
                !player.gun_mounts[(size_t)zone.slot].gun_id.empty())
                return player.gun_mounts[(size_t)zone.slot].gun_id;
            return "EMPTY";
        case Kind::Launcher: {
            if (g_editing) return "LAUNCHER HARDPOINT";
            const bool left = zone.slot == 0;
            const bool missile = left ? player.missile_launcher_left
                                      : player.missile_launcher_right;
            const bool torpedo = left ? player.torpedo_launcher_left
                                      : player.torpedo_launcher_right;
            return missile ? "MISSILE" : torpedo ? "TORPEDO" : "EMPTY";
        }
        case Kind::Armor:   return player.armor_name.empty() ? "STOCK" : player.armor_name;
        case Kind::Shield:  return "LEVEL " + std::to_string(player.shield_level);
        case Kind::Engine:  return "LEVEL " + std::to_string(player.engine_level);
        case Kind::Cargo:   return player.cargo_expansion ? "EXPANDED" : "STOCK";
        case Kind::Systems: return player.has_jump_drive ? "JUMP READY" : "AVIONICS";
        case Kind::Service: return "SERVICE";
    }
    return {};
}

ImVec2 rect_min(const Zone& zone, const ImVec2& lo, const ImVec2& size) {
    return ImVec2(lo.x + zone.rect[0] * size.x, lo.y + zone.rect[1] * size.y);
}

ImVec2 rect_max(const Zone& zone, const ImVec2& lo, const ImVec2& size) {
    return ImVec2(lo.x + (zone.rect[0] + zone.rect[2]) * size.x,
                  lo.y + (zone.rect[1] + zone.rect[3]) * size.y);
}

bool contains(const ImVec2& p, const ImVec2& lo, const ImVec2& hi) {
    return p.x >= lo.x && p.y >= lo.y && p.x <= hi.x && p.y <= hi.y;
}

void draw_zone_overlay(PlayerState& player, const ShipClass* schematic_ship,
                       const ImVec2& image_lo, const ImVec2& image_size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;

    // Last-authored zone wins overlaps, matching normal immediate-mode hit order.
    Zone* hovered = nullptr;
    for (Zone& zone : g_layout.zones) {
        const ImVec2 lo = rect_min(zone, image_lo, image_size);
        const ImVec2 hi = rect_max(zone, image_lo, image_size);
        if (contains(mouse, lo, hi)) hovered = &zone;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_selected_id = hovered->id;
        g_category_selected = false;
        const ImVec2 hi = rect_max(*hovered, image_lo, image_size);
        const ImVec2 handle_lo(hi.x - 18.0f, hi.y - 18.0f);
        g_drag = g_editing && contains(mouse, handle_lo, hi)
               ? DragMode::Resize : g_editing ? DragMode::Move : DragMode::None;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_drag = DragMode::None;

    Zone* selected = selected_zone();
    if (g_editing && selected && g_drag != DragMode::None &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const float dx = io.MouseDelta.x / image_size.x;
        const float dy = io.MouseDelta.y / image_size.y;
        if (g_drag == DragMode::Move) {
            selected->rect[0] = std::clamp(selected->rect[0] + dx, 0.0f,
                                           1.0f - selected->rect[2]);
            selected->rect[1] = std::clamp(selected->rect[1] + dy, 0.0f,
                                           1.0f - selected->rect[3]);
        } else {
            selected->rect[2] = std::clamp(selected->rect[2] + dx, 0.035f,
                                           1.0f - selected->rect[0]);
            selected->rect[3] = std::clamp(selected->rect[3] + dy, 0.035f,
                                           1.0f - selected->rect[1]);
            equipment_hardpoints::set_category_dimensions(
                g_layout, selected->kind, selected->rect[2], selected->rect[3]);
        }
    }

    // Blueprint callouts: markers stay on the hull; labels live at the margins
    // and connect with leader lines, so long equipment names never overflow.
    std::vector<Zone*> left, right;
    for (Zone& zone : g_layout.zones) {
        const ImVec2 lo = rect_min(zone, image_lo, image_size);
        const ImVec2 hi = rect_max(zone, image_lo, image_size);
        const bool is_selected = !g_category_selected && selected && selected->id == zone.id;
        const bool is_hovered = hovered && hovered->id == zone.id;
        ImVec4 color = kind_color(zone.kind);
        color.w = is_selected ? 1.0f : is_hovered ? 0.88f : 0.64f;
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(color);
        dl->AddRectFilled(lo, hi, IM_COL32(3, 13, 22, is_selected ? 145 : 85), 3.0f);
        dl->AddRect(lo, hi, col, 3.0f, 0, is_selected ? 3.0f : 1.5f);
        if (g_editing && is_selected)
            dl->AddRectFilled(ImVec2(hi.x - 18.0f, hi.y - 18.0f), hi, col, 2.0f);
        const float center_x = (lo.x + hi.x) * 0.5f;
        (center_x < image_lo.x + image_size.x * 0.5f ? left : right).push_back(&zone);
    }

    auto draw_callouts = [&](std::vector<Zone*>& zones, bool on_right) {
        std::sort(zones.begin(), zones.end(), [&](const Zone* a, const Zone* b) {
            return rect_min(*a, image_lo, image_size).y < rect_min(*b, image_lo, image_size).y;
        });
        float next_y = image_lo.y + 18.0f;
        const float label_x = on_right ? image_lo.x + image_size.x - 205.0f
                                       : image_lo.x + 12.0f;
        for (Zone* zone : zones) {
            const ImVec2 lo = rect_min(*zone, image_lo, image_size);
            const ImVec2 hi = rect_max(*zone, image_lo, image_size);
            const ImVec2 anchor((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            const float label_y = std::max(anchor.y - 13.0f, next_y);
            next_y = label_y + 42.0f;
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(kind_color(zone->kind));
            const float line_end = on_right ? label_x - 7.0f : label_x + 193.0f;
            // Single straight leader from the hardpoint marker to the label,
            // rather than a two-segment elbow that reads as separate lines.
            dl->AddLine(anchor, ImVec2(line_end, label_y + 12.0f), col, 1.5f);
            dl->AddCircleFilled(anchor, 3.5f, col);
            std::string state = zone_state(player, *zone, schematic_ship);
            for (char& c : state) {
                if (c == '_') c = ' ';
                else c = (char)std::toupper((unsigned char)c);
            }
            dl->AddText(ImVec2(label_x, label_y), col, zone->label.c_str());
            dl->AddText(ImVec2(label_x, label_y + 18.0f),
                        IM_COL32(214, 226, 238, 245), state.c_str());
        }
    };
    draw_callouts(left, false);
    draw_callouts(right, true);
}

void draw_ship_schematic(PlayerState& player, const ShipClass* ship,
                         const ImVec2& panel_size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.012f, 0.018f, 0.028f, 0.96f));
    if (ImGui::BeginChild("##ship_schematic", panel_size, true,
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextColored(kAccent, "%s LOADOUT",
                           ship ? ship->display_name.c_str() : g_layout.ship.c_str());
        ImGui::SameLine();
        ImGui::TextColored(kDim, "Top-down hardpoint schematic");
        ImGui::SameLine(ImGui::GetWindowWidth() - 210.0f);
        ImGui::TextColored(g_editing ? kBad : kDim, "%s", g_editing ? "EDITOR ACTIVE" : "H: EDIT ZONES");

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float side = std::max(100.0f, std::min(avail.x - 20.0f, avail.y - 18.0f));
        const ImVec2 image_lo(ImGui::GetCursorScreenPos().x + (avail.x - side) * 0.5f,
                              ImGui::GetCursorScreenPos().y + (avail.y - side) * 0.5f);
        const ImVec2 image_hi(image_lo.x + side, image_lo.y + side);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(image_lo, image_hi, IM_COL32(2, 5, 10, 235));
        for (int i = 1; i < 8; ++i) {
            const float v = side * i / 8.0f;
            dl->AddLine(ImVec2(image_lo.x + v, image_lo.y), ImVec2(image_lo.x + v, image_hi.y),
                        IM_COL32(30, 55, 72, 90));
            dl->AddLine(ImVec2(image_lo.x, image_lo.y + v), ImVec2(image_hi.x, image_lo.y + v),
                        IM_COL32(30, 55, 72, 90));
        }
        if (g_ship_texture.valid)
            // The source +90 captures face nose-down. Rotate the UVs 180° so
            // every loadout blueprint reads naturally with the nose upward.
            dl->AddImageQuad(simgui_imtextureid(g_ship_texture.view),
                image_lo, ImVec2(image_hi.x, image_lo.y), image_hi,
                ImVec2(image_lo.x, image_hi.y),
                ImVec2(1, 1), ImVec2(0, 1), ImVec2(0, 0), ImVec2(1, 0));
        else
            dl->AddText(ImVec2(image_lo.x + 20.0f, image_lo.y + 20.0f),
                        IM_COL32(240, 100, 80, 255), "TOP-DOWN SPRITE UNAVAILABLE");
        draw_zone_overlay(player, ship, image_lo, ImVec2(side, side));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void sanitize_text(char* text) {
    for (; *text; ++text)
        if (*text == '"' || *text == '\\' || (unsigned char)*text < 32) *text = '_';
}

void draw_editor_panel(const ShipClass* preview_ship) {
    ImGui::TextColored(kAccent, "HARDPOINT ZONE EDITOR");
    ImGui::TextColored(kDim, "Preview and author any registered hull without changing your ship.");
    ImGui::SetNextItemWidth(-1.0f);
    const char* current_name = preview_ship ? preview_ship->display_name.c_str()
                                            : g_layout.ship.c_str();
    if (ImGui::BeginCombo("##editor_ship", current_name)) {
        for (const ShipClass& candidate : ship_class::all()) {
            const bool selected = candidate.name == g_layout.ship;
            if (ImGui::Selectable(candidate.display_name.c_str(), selected)) {
                g_editor_ship = candidate.name;
                ensure_layout(candidate.name, (int)candidate.default_guns.size());
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::TextColored(kDim, "Editing: assets/ships/%s/equipment_hardpoints.json",
                       g_layout.ship.c_str());
    ImGui::TextColored(kDim, "Drag a zone to move; drag its filled corner to resize.");
    ImGui::Separator();
    Zone* zone = selected_zone();
    if (!zone) {
        ImGui::TextDisabled("No zones. Add one to begin.");
    } else {
        char id[96], label[96];
        std::snprintf(id, sizeof id, "%s", zone->id.c_str());
        std::snprintf(label, sizeof label, "%s", zone->label.c_str());
        if (ImGui::InputText("ID", id, sizeof id)) {
            sanitize_text(id); zone->id = id; g_selected_id = zone->id;
        }
        if (ImGui::InputText("Label", label, sizeof label)) {
            sanitize_text(label); zone->label = label;
        }
        int kind = static_cast<int>(zone->kind);
        const char* kinds[] = {"Gun", "Turret", "Launcher"};
        if (ImGui::Combo("Type", &kind, kinds, (int)std::size(kinds))) {
            zone->kind = static_cast<Kind>(kind);
            equipment_hardpoints::normalize_dimensions(g_layout);
        }
        ImGui::InputInt("Slot / side", &zone->slot);
        zone->slot = std::max(0, zone->slot);
        const bool rect_changed = ImGui::InputFloat4("Normalized rect", zone->rect, "%.4f");
        zone->rect[0] = std::clamp(zone->rect[0], 0.0f, 0.965f);
        zone->rect[1] = std::clamp(zone->rect[1], 0.0f, 0.965f);
        zone->rect[2] = std::clamp(zone->rect[2], 0.035f, 1.0f - zone->rect[0]);
        zone->rect[3] = std::clamp(zone->rect[3], 0.035f, 1.0f - zone->rect[1]);
        if (rect_changed)
            equipment_hardpoints::set_category_dimensions(
                g_layout, zone->kind, zone->rect[2], zone->rect[3]);
    }

    if (ImGui::Button("ADD ZONE", ImVec2(130.0f, 34.0f))) {
        Zone added;
        added.id = "zone_" + std::to_string(g_layout.zones.size() + 1);
        added.label = "New Zone";
        g_layout.zones.push_back(added);
        equipment_hardpoints::normalize_dimensions(g_layout);
        g_selected_id = added.id;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!zone);
    if (ImGui::Button("DELETE", ImVec2(100.0f, 34.0f)) && zone) {
        const std::string id = zone->id;
        g_layout.zones.erase(std::remove_if(g_layout.zones.begin(), g_layout.zones.end(),
            [&](const Zone& item) { return item.id == id; }), g_layout.zones.end());
        g_selected_id.clear();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::Button("SAVE LAYOUT", ImVec2(-1.0f, 40.0f)))
        equipment_hardpoints::save(g_layout);
    if (ImGui::Button("RELOAD FROM DISK", ImVec2(-1.0f, 34.0f))) {
        const int mounts = (int)std::count_if(g_layout.zones.begin(), g_layout.zones.end(),
            [](const Zone& item) { return item.kind == Kind::Gun || item.kind == Kind::Turret; });
        equipment_hardpoints::load(g_layout.ship, mounts, g_layout);
        g_selected_id.clear();
    }
    ImGui::Spacing();
    ImGui::TextWrapped("Only physical weapon hardpoints are authored here. Launcher slot 0 is LEFT; slot 1 is RIGHT. Gun and turret slots index PlayerState gun mounts from zero.");
}

const char* category_label(Kind kind) {
    switch (kind) {
        case Kind::Armor:   return "ARMOR";
        case Kind::Shield:  return "SHIELDS";
        case Kind::Engine:  return "ENGINE";
        case Kind::Cargo:   return "CARGO";
        case Kind::Systems: return "SYSTEMS";
        case Kind::Service: return "SERVICE";
        default:            return "EQUIPMENT";
    }
}

void draw_category_menu() {
    constexpr Kind categories[] = {
        Kind::Armor, Kind::Shield, Kind::Engine,
        Kind::Cargo, Kind::Systems, Kind::Service,
    };
    ImGui::TextColored(kDim, "SHIP EQUIPMENT");
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float width = (ImGui::GetContentRegionAvail().x - gap * 2.0f) / 3.0f;
    for (int i = 0; i < (int)std::size(categories); ++i) {
        if (i % 3) ImGui::SameLine();
        const Kind kind = categories[i];
        const bool selected = g_category_selected && g_category == kind;
        if (selected)
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(kind_color(kind).x * 0.38f, kind_color(kind).y * 0.38f,
                       kind_color(kind).z * 0.38f, 1.0f));
        if (ImGui::Button(category_label(kind), ImVec2(width, 32.0f))) {
            g_category = kind;
            g_category_selected = true;
        }
        if (selected) ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

} // namespace

void draw_equipment_screen(BaseContext& ctx) {
    PlayerState& player = *ctx.player;
    const ShipClass* owned_ship = ship_class::find(player.ship_class_name);
    const int owned_mounts = owned_ship ? (int)owned_ship->default_guns.size()
                                        : (int)player.gun_mounts.size();
    if ((int)player.gun_mounts.size() < owned_mounts)
        player.gun_mounts.resize(owned_mounts, MountSlot{});

    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_H)) {
        g_editing = !g_editing;
        g_editor_ship = g_editing ? player.ship_class_name : std::string();
        g_drag = DragMode::None;
    }
    const std::string& schematic_name = g_editing ? g_editor_ship : player.ship_class_name;
    const ShipClass* ship = ship_class::find(schematic_name);
    const int mounts = ship ? (int)ship->default_guns.size() : owned_mounts;
    ensure_layout(schematic_name, mounts);

    const float dpi = sapp_dpi_scale();
    const float sw = (float)sapp_width() / dpi;
    const float sh = (float)sapp_height() / dpi;
    const float left = 28.0f, top = 58.0f, bottom = 76.0f, gap = 12.0f;
    const float total_w = sw - left - 28.0f;
    const float body_h = sh - top - bottom;
    const float schematic_w = total_w * 0.66f;

    ImGui::SetCursorScreenPos(ImVec2(left, top));
    draw_ship_schematic(player, ship, ImVec2(schematic_w, body_h));
    ImGui::SetCursorScreenPos(ImVec2(left + schematic_w + gap, top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025f, 0.030f, 0.043f, 0.96f));
    if (ImGui::BeginChild("##equipment_actions",
                          ImVec2(total_w - schematic_w - gap, body_h), true)) {
        ImGui::TextColored(kAccent, "EQUIPMENT BAY");
        ImGui::SameLine();
        ImGui::TextColored(kGood, "%lld CR", (long long)player.credits);
        ImGui::Separator();
        if (g_editing) {
            draw_editor_panel(ship);
        } else {
            draw_category_menu();
            if (g_category_selected) {
                Zone category;
                category.id = "ship_category";
                category.label = category_label(g_category);
                category.kind = g_category;
                draw_purchase_panel({player, ctx.player_ship, ship, category});
            } else if (Zone* zone = selected_zone()) {
                draw_purchase_panel({player, ctx.player_ship, ship, *zone});
            } else {
                ImGui::TextDisabled("No weapon hardpoints are defined for this ship.");
                ImGui::TextWrapped("Press H to open the editor and add one.");
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace outfitting::equipment_ui
