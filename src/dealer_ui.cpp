// Ship Dealer presentation; transactions remain in outfitting.cpp.
#include "equipment_ui_internal.h"

#include "base_screens.h"
#include "outfitting.h"
#include "player.h"
#include "ship_class.h"
#include "sfx.h"
#include "imgui.h"
#include "sokol_app.h"

#include <cstdio>
#include <string>

namespace outfitting::equipment_ui {
namespace {

std::string g_pending_hull;   // palette: equipment_ui_internal.h

} // namespace

void draw_dealer_screen(BaseContext& ctx) {
    PlayerState& player = *ctx.player;
    const float dpi = sapp_dpi_scale();
    const float sw = (float)sapp_width() / dpi;
    const float sh = (float)sapp_height() / dpi;

    ImGui::SetCursorScreenPos(ImVec2(28.0f, 60.0f));
    ImGui::TextColored(kAccent, "SHIP DEALER");
    ImGui::SameLine(250.0f);
    ImGui::TextColored(kGood, "%lld CREDITS", (long long)player.credits);
    ImGui::SetCursorScreenPos(ImVec2(28.0f, 92.0f));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025f, 0.035f, 0.060f, 0.94f));
    if (ImGui::BeginChild("##dealer", ImVec2(sw - 56.0f, sh - 162.0f), true) &&
        ImGui::BeginTable("##hulls", 8,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("HULL", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("SPEED");
        ImGui::TableSetupColumn("ARMOR F/A/P/S");
        ImGui::TableSetupColumn("CARGO");
        ImGui::TableSetupColumn("GUNS");
        ImGui::TableSetupColumn("PRICE");
        ImGui::TableSetupColumn("NET");
        ImGui::TableSetupColumn("ACTION", ImGuiTableColumnFlags_WidthFixed, 245.0f);
        ImGui::TableHeadersRow();

        for (const HullOffer& offer : hull_catalog()) {
            const ShipClass* ship = ship_class::find(offer.id);
            const bool owned = offer.id == player.ship_class_name;
            const int64_t net = hull_net_cost(offer.id, player.ship_class_name);
            ImGui::PushID(offer.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(owned ? kGood : kAccent, "%s",
                               ship ? ship->display_name.c_str() : offer.id.c_str());
            if (owned) { ImGui::SameLine(); ImGui::TextDisabled("CURRENT"); }
            ImGui::TableNextColumn();
            if (ship) ImGui::Text("%.0f / %.0f", ship->cruise_speed, ship->afterburner_speed);
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            if (ship) ImGui::Text("%.0f/%.0f/%.0f/%.0f", ship->armor_fore_cm,
                ship->armor_aft_cm, ship->armor_port_cm, ship->armor_starboard_cm);
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            if (ship) ImGui::Text("%d (%d)", ship->cargo_units, ship->cargo_units_max);
            else ImGui::TextDisabled("--");
            ImGui::TableNextColumn();
            ImGui::Text("%d", ship ? (int)ship->default_guns.size() : 0);
            ImGui::TableNextColumn(); ImGui::Text("%lld", (long long)offer.price);
            ImGui::TableNextColumn();
            if (owned) ImGui::TextColored(kDim, "--"); else ImGui::Text("%lld", (long long)net);
            ImGui::TableNextColumn();
            if (owned) {
                ImGui::TextColored(kGood, "OWNED");
            } else if (g_pending_hull == offer.id) {
                ImGui::TextColored(kBad, "RESETS LOADOUT");
                ImGui::SameLine();
                if (ImGui::SmallButton("CONFIRM")) {
                    if (buy_hull(player, offer.id)) sfx::ui_click();
                    g_pending_hull.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("CANCEL")) g_pending_hull.clear();
            } else {
                ImGui::BeginDisabled(net > 0 && !player::can_afford(player, net));
                if (ImGui::SmallButton("BUY HULL")) g_pending_hull = offer.id;
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace outfitting::equipment_ui
