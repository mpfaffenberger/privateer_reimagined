#pragma once

#include "equipment_hardpoints.h"

struct BaseContext;
struct PlayerState;
struct Ship;
struct ShipClass;

namespace outfitting::equipment_ui {

struct PanelContext {
    PlayerState& player;
    Ship* live_ship;
    const ShipClass* ship_class;
    const equipment_hardpoints::Zone& zone;
};

void draw_purchase_panel(const PanelContext& ctx);
void draw_dealer_screen(BaseContext& ctx);
void draw_equipment_screen(BaseContext& ctx);

} // namespace outfitting::equipment_ui
