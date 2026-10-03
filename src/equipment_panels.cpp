// Purchase/install panels selected from the visual ship hardpoint schematic.
#include "equipment_ui_internal.h"

#include "armor.h"
#include "credits_format.h"
#include "gun.h"
#include "missile.h"
#include "outfitting.h"
#include "player.h"
#include "repair.h"
#include "ship.h"
#include "ship_class.h"
#include "sfx.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace outfitting::equipment_ui {
namespace {

constexpr ImVec4 kAccent(1.00f, 0.72f, 0.22f, 1.0f);
constexpr ImVec4 kGood(0.48f, 0.92f, 0.56f, 1.0f);
constexpr ImVec4 kBad(1.00f, 0.40f, 0.30f, 1.0f);
constexpr ImVec4 kDim(0.55f, 0.58f, 0.65f, 1.0f);

void heading(const char* title, const char* subtitle = nullptr) {
    ImGui::TextColored(kAccent, "%s", title);
    if (subtitle) ImGui::TextColored(kDim, "%s", subtitle);
    ImGui::Separator();
    ImGui::Spacing();
}

void status(bool installed, const char* installed_text = "INSTALLED") {
    ImGui::SameLine();
    ImGui::TextColored(installed ? kGood : kDim,
                       "%s", installed ? installed_text : "EMPTY");
}

// ---- price + action buttons (#747) -------------------------------------------
// "LABEL   80,000 CR" / "LABEL   +2,500 CR": one price spelling for the bay.
std::string priced(const char* label, int64_t credits, bool refund = false) {
    return std::string(label) + "   " + (refund ? "+" : "") + format_credits(credits) + " CR";
}

// Buy-style button. An unaffordable price reads RED (an inert button with no
// hover, and clicks are ignored). It is NOT ImGui-disabled, because disabling
// fades the red into the same grey as everything else. So "can't afford"
// always shows, even on a mount that's blocked for another reason. `blocked`
// greys the button out (occupied mount, conflicting launcher, ...).
bool buy_button(const PlayerState& p, const std::string& label, int64_t price,
                const ImVec2& size, bool blocked = false) {
    const bool for_sale = price > 0;
    if (for_sale && !player::can_afford(p, price)) {
        const ImVec4 inert(0.22f, 0.07f, 0.06f, 0.85f);
        ImGui::PushStyleColor(ImGuiCol_Button, inert);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, inert);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, inert);
        ImGui::PushStyleColor(ImGuiCol_Text, kBad);
        ImGui::Button(label.c_str(), size);
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not enough credits.");
        return false;
    }
    ImGui::BeginDisabled(blocked || !for_sale);
    const bool clicked = ImGui::Button(label.c_str(), size);
    ImGui::EndDisabled();
    return clicked;
}

// Sell / refund-style button. One choke point so every sell in the bay looks
// alike.
bool sell_button(const std::string& label, const ImVec2& size, bool blocked = false) {
    ImGui::BeginDisabled(blocked);
    const bool clicked = ImGui::Button(label.c_str(), size);
    ImGui::EndDisabled();
    return clicked;
}

// Small coloured delta under a catalog stat, vs the gun fitted in this mount:
// green when the row is better, red when worse. ASCII signs only, because the
// UI font (Inter, default glyph ranges) has no arrow glyphs.
void stat_delta(float delta, bool higher_is_better, int decimals) {
    if (std::fabs(delta) < 0.005f) { ImGui::TextColored(kDim, "same"); return; }
    const bool better = higher_is_better ? delta > 0.0f : delta < 0.0f;
    ImGui::TextColored(better ? kGood : kBad, decimals == 1 ? "%+.1f" : "%+.2f", delta);
}

// "ENERGY  guns 26.7 GJ/s vs regen 30.0 GJ/s": the budget that decides whether
// a loadout can keep firing (#743/#747). Red with a time-to-empty when not.
void draw_energy_budget(const EnergyBudget& b) {
    const bool short_of_power = b.gun_burn_gj_s > b.regen_gj_s + 0.05f;
    ImGui::TextColored(kDim, "ENERGY");
    ImGui::SameLine();
    ImGui::TextColored(short_of_power ? kBad : kGood, "guns %.1f GJ/s  vs  regen %.1f GJ/s",
                       b.gun_burn_gj_s, b.regen_gj_s);
    if (short_of_power) {
        const float drain = b.gun_burn_gj_s - std::max(0.0f, b.regen_gj_s);
        ImGui::TextColored(kBad, "Full fire empties the %.0f GJ bank in %.0f s.",
                           b.bank_gj, b.bank_gj / drain);
    }
}

// Turret HARDWARE block for a turret mount (#145). Returns true when the
// turret is installed, i.e. its mounts can take a gun.
bool draw_turret_hardware(const PanelContext& ctx, const TurretSlot& turret) {
    PlayerState& p = ctx.player;
    const bool owned = player::has_turret(p, turret.id);
    ImGui::TextUnformatted(turret.label.c_str());
    status(owned);
    const int64_t price = turret_price();
    if (!owned) {
        ImGui::TextColored(kDim, "Install the turret to fit guns into its %zu mount%s.",
                           turret.mounts.size(), turret.mounts.size() == 1 ? "" : "s");
        if (buy_button(p, priced("INSTALL TURRET", price), price, ImVec2(-1.0f, 36.0f)) &&
            buy_turret(p, turret.id, ctx.ship_class)) sfx::ui_click();
        return false;
    }
    const bool armed = std::any_of(turret.mounts.begin(), turret.mounts.end(), [&](int m) {
        return player::mount_armed(p, m);
    });
    if (sell_button(priced("REMOVE TURRET", price, true), ImVec2(-1.0f, 32.0f), armed) &&
        sell_turret(p, turret.id, ctx.ship_class)) sfx::ui_click();
    if (armed) ImGui::TextColored(kDim, "Sell the turret's guns before removing it.");
    ImGui::Separator();
    return true;
}

// One compact line: "GUN 2  · Forward ·  Meson Blaster" (#747).
void draw_gun_header(const equipment_hardpoints::Zone& zone, const TurretSlot* turret, const std::string& fitted) {
    std::string label = zone.label;
    for (char& c : label) c = (char)std::toupper((unsigned char)c);
    ImGui::TextColored(kAccent, "%s", label.c_str());
    ImGui::SameLine();
    ImGui::TextColored(kDim, "\xC2\xB7 %s \xC2\xB7", turret ? turret->label.c_str() : "Forward");
    ImGui::SameLine();
    if (fitted.empty()) ImGui::TextColored(kDim, "Empty");
    else                ImGui::TextColored(kGood, "%s", gun::display_name(fitted).c_str());
    ImGui::Separator();
    ImGui::Spacing();
}

// Aligned catalog: WEAPON | DAMAGE | REFIRE | ENERGY | PRICE (#747). Deltas
// compare each row with the gun fitted in this mount; ENERGY turns red when
// fitting that gun here would make the guns out-burn the ship's regen.
void draw_gun_catalog(const PanelContext& ctx, const TurretSlot* turret,
                      const std::string& fitted, const EnergyBudget& budget) {
    PlayerState& player = ctx.player;
    const int slot = ctx.zone.slot;
    const bool armed = !fitted.empty();
    const GunType fitted_type = gun::from_name(fitted);
    const GunStats* fitted_stats =
        fitted_type == GunType::Count ? nullptr : &g_gun_stats[(int)fitted_type];
    // What this mount burns today, so a row can show the ship's total burn
    // with that gun fitted here instead. Turret mounts fire free.
    const inventory::WeaponMods mods = player.gun_mounts[(size_t)slot].mods;
    const float fitted_burn =
        turret ? 0.0f : gun_energy_burn(fitted, mods.fire_rate_mult, mods.energy_mult);

    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                       ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("##gun_catalog", 5, kFlags, ImVec2(0.0f, 0.0f))) return;
    // Units live in the headers and names wrap, so the table still fits a
    // ~400 px panel at 1280x800 without starving the WEAPON column.
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("WEAPON",   ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("DMG cm",   ImGuiTableColumnFlags_WidthFixed, 54.0f);
    ImGui::TableSetupColumn("REFIRE s", ImGuiTableColumnFlags_WidthFixed, 62.0f);
    ImGui::TableSetupColumn("GJ/S",     ImGuiTableColumnFlags_WidthFixed, 48.0f);
    ImGui::TableSetupColumn("PRICE",    ImGuiTableColumnFlags_WidthFixed, 92.0f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < kGunTypeCount; ++i) {
        const GunType type = (GunType)i;
        const char* id = gun::to_name(type);
        const int64_t price = gun_price(id);
        if (price <= 0) continue;
        const GunStats& gs = g_gun_stats[i];
        const bool is_fitted = fitted == id;
        const bool compare = fitted_stats && !is_fitted;
        ImGui::PushID(i);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 46.0f);

        ImGui::TableNextColumn();
        if (is_fitted) ImGui::PushStyleColor(ImGuiCol_Text, kGood);
        ImGui::TextWrapped("%s", gun::display_name(type));
        if (is_fitted) ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        ImGui::Text("%.1f", gs.damage_cm);
        if (compare) stat_delta(gs.damage_cm - fitted_stats->damage_cm, true, 1);

        ImGui::TableNextColumn();
        ImGui::Text("%.2f", gs.refire_delay_s);
        if (compare) stat_delta(gs.refire_delay_s - fitted_stats->refire_delay_s, false, 2);

        ImGui::TableNextColumn();
        if (turret) {
            ImGui::TextColored(kDim, "free");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turrets don't draw on the energy bank.");
        } else {
            const float burn = gun_energy_burn(id);
            const float projected = budget.gun_burn_gj_s - fitted_burn + burn;
            const bool over = !is_fitted && projected > budget.regen_gj_s + 0.05f;
            ImGui::TextColored(over ? kBad : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                               "%.1f", burn);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Fitted here, your guns would burn %.1f GJ/s against "
                                  "%.1f GJ/s of regen.", projected, budget.regen_gj_s);
        }

        ImGui::TableNextColumn();
        if (is_fitted) {
            ImGui::TextColored(kGood, "FITTED");
        } else if (buy_button(player, format_credits(price) + " CR", price,
                              ImVec2(-FLT_MIN, 32.0f), armed) &&
                   buy_gun(player, id, slot, ctx.ship_class)) {
            sfx::ui_click();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_guns(const PanelContext& ctx) {
    PlayerState& player = ctx.player;
    const int slot = ctx.zone.slot;
    const bool valid = slot >= 0 && slot < (int)player.gun_mounts.size();
    // Turret-ness is a property of the HULL's mount (ship.json), not of the
    // schematic zone -- fallback layouts label every mount a forward gun.
    const TurretSlot* turret =
        ctx.ship_class ? ctx.ship_class->turret_slot_for_mount(slot) : nullptr;
    if (!valid) {
        heading(turret ? "TURRET HARDPOINT" : "FORWARD GUN HARDPOINT", ctx.zone.label.c_str());
        ImGui::TextColored(kBad, "Mount %d does not exist on this hull.", slot + 1);
        return;
    }
    // A COPY, not a reference: buy_gun/sell_gun below may resize gun_mounts.
    const std::string fitted = player.gun_mounts[(size_t)slot].gun_id;
    draw_gun_header(ctx.zone, turret, fitted);
    if (turret && !draw_turret_hardware(ctx, *turret)) return;

    if (!fitted.empty()) {
        if (sell_button(priced("SELL FITTED GUN", gun_price(fitted), true), ImVec2(-1.0f, 36.0f)) &&
            sell_gun(player, slot, ctx.ship_class)) sfx::ui_click();
        // buy_gun refuses an occupied mount (#741); say why the prices are greyed.
        ImGui::TextColored(kDim, "Sell the fitted gun to free this mount for another.");
    }
    ImGui::Spacing();
    const EnergyBudget budget = energy_budget(player, ctx.ship_class);
    draw_energy_budget(budget);
    ImGui::Spacing();
    draw_gun_catalog(ctx, turret, fitted, budget);
}

void draw_launcher(const PanelContext& ctx) {
    PlayerState& p = ctx.player;
    const bool left = ctx.zone.slot == 0;
    bool& missile = left ? p.missile_launcher_left : p.missile_launcher_right;
    bool& torpedo = left ? p.torpedo_launcher_left : p.torpedo_launcher_right;
    heading(left ? "LEFT ORDNANCE HARDPOINT" : "RIGHT ORDNANCE HARDPOINT",
            "One launcher type may occupy this physical hardpoint");

    ImGui::TextUnformatted("MISSILE LAUNCHER");
    status(missile);
    if (missile) {
        if (sell_button(priced("SELL", repair::k_missile_launcher_sell_price, true),
                        ImVec2(-1.0f, 34.0f))) {
            const bool ok = left ? repair::sell_missile_launcher_left(p)
                                 : repair::sell_missile_launcher_right(p);
            if (ok) sfx::ui_click();
        }
    } else {
        if (buy_button(p, priced("INSTALL", repair::k_missile_launcher_price),
                       repair::k_missile_launcher_price, ImVec2(-1.0f, 34.0f), torpedo)) {
            const bool ok = left ? repair::buy_missile_launcher_left(p)
                                 : repair::buy_missile_launcher_right(p);
            if (ok) sfx::ui_click();
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("TORPEDO TUBE");
    status(torpedo);
    if (torpedo) {
        if (sell_button(priced("SELL", repair::k_torpedo_launcher_sell_price, true),
                        ImVec2(-1.0f, 34.0f))) {
            const bool ok = left ? repair::sell_torpedo_launcher_left(p)
                                 : repair::sell_torpedo_launcher_right(p);
            if (ok) sfx::ui_click();
        }
    } else {
        if (buy_button(p, priced("INSTALL", repair::k_torpedo_launcher_price),
                       repair::k_torpedo_launcher_price, ImVec2(-1.0f, 34.0f), missile)) {
            const bool ok = left ? repair::buy_torpedo_launcher_left(p)
                                 : repair::buy_torpedo_launcher_right(p);
            if (ok) sfx::ui_click();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(kDim, "ORDNANCE INVENTORY");
    for (int type = 0; type < kMissileRackTypeCount; ++type) {
        ImGui::PushID(type);
        ImGui::Text("%s MISSILES    %d", missile::to_name((MissileType)type),
                    p.missiles[type]);
        ImGui::SameLine(180.0f);
        const bool room = repair::missiles_total(p) < repair::missile_rack_capacity(p);
        ImGui::BeginDisabled(!room || !player::can_afford(p, repair::missile_price(type)));
        if (ImGui::SmallButton("BUY +1") && repair::buy_missiles(p, type, 1)) sfx::ui_click();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(p.missiles[type] <= 0);
        if (ImGui::SmallButton("SELL -1") && repair::sell_missile(p, type)) sfx::ui_click();
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::Text("TORPEDOES      %d / %d", p.torpedoes, repair::torpedo_rack_capacity(p));
    ImGui::SameLine(180.0f);
    ImGui::BeginDisabled(p.torpedoes >= repair::torpedo_rack_capacity(p) ||
                         !player::can_afford(p, repair::torpedo_price()));
    if (ImGui::SmallButton("BUY +1##torp") && repair::buy_torpedo(p, 1)) sfx::ui_click();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(p.torpedoes <= 0);
    if (ImGui::SmallButton("SELL -1##torp") && repair::sell_torpedo(p)) sfx::ui_click();
    ImGui::EndDisabled();
}

void draw_armor(const PanelContext& ctx) {
    PlayerState& p = ctx.player;
    heading("ARMOR PACKAGE", p.armor_name.empty() ? "Stock hull plating" : p.armor_name.c_str());
    for (const ArmorType& armor : armor::all()) {
        const int64_t price = armor_price(armor.name);
        if (price <= 0) continue;
        ImGui::PushID(armor.name.c_str());
        ImGui::TextUnformatted(armor.name.c_str());
        ImGui::TextColored(kDim, "F %.0f  A %.0f  P %.0f  S %.0f cm",
                           armor.front_cm, armor.back_cm, armor.port_cm, armor.starboard_cm);
        ImGui::SameLine(ImGui::GetWindowWidth() - 145.0f);
        if (p.armor_name == armor.name) {
            ImGui::TextColored(kGood, "FITTED");
        } else {
            if (buy_button(p, format_credits(price) + " CR", price, ImVec2(125.0f, 34.0f)) &&
                buy_armor(p, armor.name))
                sfx::ui_click();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
}
void draw_ladder(const PanelContext& ctx, bool shield) {
    PlayerState& p = ctx.player;
    const int level = shield ? p.shield_level : p.engine_level;
    const int cap = ctx.ship_class
                  ? (shield ? ctx.ship_class->max_shield_level
                            : ctx.ship_class->max_engine_level) : 0;
    heading(shield ? "SHIELD GENERATOR" : "ENGINE POWER PLANT");
    ImGui::Text("CURRENT LEVEL    %d / %d", level, cap);
    if (!shield) {
        const SpeedCaps speed = effective_speed_caps(p);
        ImGui::TextColored(kDim, "Hull speed %.0f cruise / %.0f afterburn",
                           speed.cruise0, speed.cruise1);
    }
    const int64_t next_price = shield ? shield_upgrade_price(level + 1)
                                      : engine_upgrade_price(level + 1);
    const std::string upgrade = "UPGRADE TO LEVEL " + std::to_string(level + 1);
    if (buy_button(p, priced(upgrade.c_str(), next_price), next_price, ImVec2(-1.0f, 40.0f),
                   level >= cap)) {
        const bool ok = shield ? upgrade_shield(p, ctx.ship_class)
                               : upgrade_engine(p, ctx.ship_class);
        if (ok) sfx::ui_click();
    }
    if (level > 0) {
        const int64_t refund = shield ? shield_upgrade_price(level)
                                      : engine_upgrade_price(level);
        const std::string sell = "SELL LEVEL " + std::to_string(level);
        if (sell_button(priced(sell.c_str(), refund, true), ImVec2(-1.0f, 36.0f))) {
            const bool ok = shield ? sell_shield(p, ctx.ship_class)
                                   : sell_engine(p, ctx.ship_class);
            if (ok) sfx::ui_click();
        }
    }
}

void draw_cargo(const PanelContext& ctx) {
    PlayerState& p = ctx.player;
    const int used = player::cargo_units_used(p);
    const int capacity = player::cargo_capacity(p, ctx.ship_class);
    heading("CARGO EXPANSION");
    ImGui::Text("HOLD USAGE    %d / %d units", used, capacity);
    if (p.cargo_expansion) {
        ImGui::TextColored(kGood, "EXPANSION INSTALLED");
        if (sell_button(priced("REMOVE", cargo_expansion_price(), true), ImVec2(-1.0f, 38.0f)) &&
            sell_cargo_expansion(p))
            sfx::ui_click();
    } else {
        const int64_t price = cargo_expansion_price();
        if (buy_button(p, priced("INSTALL", price), price, ImVec2(-1.0f, 40.0f)) &&
            buy_cargo_expansion(p))
            sfx::ui_click();
    }
}

bool discrete_owned(const PlayerState& p, const char* id) {
    const std::string item(id);
    if (item == "jump_drive") return p.has_jump_drive;
    if (item == "ecm_l1") return p.ecm_level >= 1;
    if (item == "ecm_l2") return p.ecm_level >= 2;
    if (item == "ecm_l3") return p.ecm_level >= 3;
    if (item == "repair_droid") return p.has_repair_droid && !p.adv_repair_droid;
    if (item == "adv_repair_droid") return p.adv_repair_droid;
    return false;
}

void draw_systems(const PanelContext& ctx) {
    PlayerState& p = ctx.player;
    heading("SHIP SYSTEMS", "Avionics, navigation, ECM, and damage control");
    struct Item { const char* id; const char* label; const char* detail; };
    constexpr std::array<Item, 6> items{{
        {"jump_drive", "Jump Drive", "Enables inter-system jump points"},
        {"ecm_l1", "ECM Level 1", "Basic missile lock disruption"},
        {"ecm_l2", "ECM Level 2", "Improved disruption; requires L1"},
        {"ecm_l3", "ECM Level 3", "Advanced disruption; requires L2"},
        {"repair_droid", "Repair Droid", "Fixes damaged non-weapon systems in flight"},
        {"adv_repair_droid", "Advanced Repair Droid", "Repairs 2x faster; requires standard droid"},
    }};
    // Two columns, so a long detail line wraps instead of running under the
    // price button.
    if (!ImGui::BeginTable("##systems", 2, ImGuiTableFlags_BordersInnerH)) return;
    ImGui::TableSetupColumn("item",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("price", ImGuiTableColumnFlags_WidthFixed, 125.0f);
    for (const Item& item : items) {
        ImGui::PushID(item.id);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, 46.0f);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(item.label);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", item.detail);
        ImGui::PopStyleColor();
        ImGui::TableNextColumn();
        if (discrete_owned(p, item.id)) {
            ImGui::TextColored(kGood, "INSTALLED");
        } else {
            const int64_t price = discrete_price(item.id);
            const bool dependency = std::string(item.id) != "adv_repair_droid" || p.has_repair_droid;
            if (buy_button(p, format_credits(price) + " CR", price, ImVec2(-FLT_MIN, 34.0f),
                           !dependency) &&
                buy_discrete(p, item.id))
                sfx::ui_click();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_service(const PanelContext& ctx) {
    PlayerState& p = ctx.player;
    heading("REPAIR & REARM", "Restore the hull and replenish ordnance");
    const repair::Quote quote = repair::quote(ctx.live_ship, p);
    if (!ctx.live_ship || !quote.hull_damaged) {
        ImGui::TextColored(kGood, "%s", ctx.live_ship ? "HULL AT FULL INTEGRITY" : "HULL STATUS UNAVAILABLE");
    } else {
        if (buy_button(p, priced("REPAIR HULL", quote.hull_cost), quote.hull_cost,
                       ImVec2(-1.0f, 40.0f)) &&
            repair::repair_hull(*ctx.live_ship, p)) sfx::ui_click();
    }
    // Per-component repairs (#141): one button per damaged system, priced
    // by how much of it is missing.
    if (ctx.live_ship && !quote.systems_damaged) {
        ImGui::TextColored(kGood, "ALL SYSTEMS NOMINAL");
    } else if (ctx.live_ship) {
        for (int i = 0; i < kShipSystemCount; ++i) {
            if (quote.system_cost[i] <= 0) continue;
            const ShipSystem sys = ship_systems::at(i);
            const float left = ship_systems::integrity(ctx.live_ship->systems, sys);
            char label[96];
            std::snprintf(label, sizeof label, "REPAIR %-10s %s   %s CR##sys%d",
                          ship_systems::label(sys),
                          left > 0.0f ? "DAMAGED  " : "DESTROYED",
                          format_credits(quote.system_cost[i]).c_str(), i);
            if (buy_button(p, label, quote.system_cost[i], ImVec2(-1.0f, 28.0f)) &&
                repair::repair_system(*ctx.live_ship, p, sys)) sfx::ui_click();
        }
    }
    ImGui::Spacing();
    ImGui::Text("MISSILE RACK    %d / %d",
                repair::missiles_total(p), repair::missile_rack_capacity(p));
    ImGui::Text("TORPEDO RACK    %d / %d",
                repair::torpedoes_total(p), repair::torpedo_rack_capacity(p));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("Select a left or right launcher hardpoint to buy and sell ordnance.");
    ImGui::PopStyleColor();
}

} // namespace

void draw_purchase_panel(const PanelContext& ctx) {
    switch (ctx.zone.kind) {
        case equipment_hardpoints::Kind::Gun:
        case equipment_hardpoints::Kind::Turret:   draw_guns(ctx); break;
        case equipment_hardpoints::Kind::Launcher: draw_launcher(ctx); break;
        case equipment_hardpoints::Kind::Armor:    draw_armor(ctx); break;
        case equipment_hardpoints::Kind::Shield:   draw_ladder(ctx, true); break;
        case equipment_hardpoints::Kind::Engine:   draw_ladder(ctx, false); break;
        case equipment_hardpoints::Kind::Cargo:    draw_cargo(ctx); break;
        case equipment_hardpoints::Kind::Systems:  draw_systems(ctx); break;
        case equipment_hardpoints::Kind::Service:  draw_service(ctx); break;
    }
}

} // namespace outfitting::equipment_ui
