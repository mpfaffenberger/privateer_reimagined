#pragma once

#include "equipment_hardpoints.h"
#include "imgui.h"

struct BaseContext;
struct PlayerState;
struct Ship;
struct ShipClass;

namespace outfitting::equipment_ui {

// One equipment-bay palette for the schematic and the panels.
inline constexpr ImVec4 kAccent(1.00f, 0.72f, 0.22f, 1.0f);
inline constexpr ImVec4 kGood(0.48f, 0.92f, 0.56f, 1.0f);
inline constexpr ImVec4 kBad(1.00f, 0.40f, 0.30f, 1.0f);
inline constexpr ImVec4 kDim(0.55f, 0.58f, 0.65f, 1.0f);

struct PanelContext {
    PlayerState& player;
    Ship* live_ship;
    const ShipClass* ship_class;
    const equipment_hardpoints::Zone& zone;
};

void draw_purchase_panel(const PanelContext& ctx);
// Pinned ship status under the purchase panel (#748): energy budget,
// shield/engine/armor, cargo and ordnance at a glance.
void draw_ship_summary(const PlayerState& player, const ShipClass* klass);
void draw_dealer_screen(BaseContext& ctx);
void draw_equipment_screen(BaseContext& ctx);

} // namespace outfitting::equipment_ui
