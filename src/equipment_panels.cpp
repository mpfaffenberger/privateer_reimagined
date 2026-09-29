// Purchase/install panels selected from the visual ship hardpoint schematic.
#include "equipment_ui_internal.h"

#include "armor.h"
#include "gun.h"
#include "missile.h"
#include "outfitting.h"
#include "player.h"
#include "repair.h"
#include "scanner.h"
#include "ship.h"
#include "ship_class.h"
#include "sfx.h"
#include "imgui.h"

#include <array>
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

void draw_guns(const PanelContext& ctx) {
    PlayerState& player = ctx.player;
    const int slot = ctx.zone.slot;
    const bool valid = slot >= 0 && slot < (int)player.gun_mounts.size();
    heading(ctx.zone.kind == equipment_hardpoints::Kind::Turret
                ? "TURRET HARDPOINT" : "FORWARD GUN HARDPOINT",
            ctx.zone.label.c_str());
    if (!valid) {
        ImGui::TextColored(kBad, "Mount %d does not exist on this hull.", slot + 1);
        return;
    }

    const std::string& fitted = player.gun_mounts[(size_t)slot].gun_id;
    ImGui::Text("MOUNT %d", slot + 1);
    status(!fitted.empty(), fitted.empty() ? "EMPTY" : fitted.c_str());
    if (!fitted.empty()) {
        const int64_t refund = gun_price(fitted);
        char sell[80];
        std::snprintf(sell, sizeof sell, "SELL FITTED GUN  +%lld CR", (long long)refund);
        if (ImGui::Button(sell, ImVec2(-1.0f, 36.0f)) &&
            sell_gun(player, slot, ctx.ship_class)) sfx::ui_click();
    }

    ImGui::Spacing();
    ImGui::TextColored(kDim, "COMPATIBLE WEAPONS");
    if (ImGui::BeginChild("##gun_catalog", ImVec2(0, 0), false)) {
        for (int i = 0; i < kGunTypeCount; ++i) {
            const char* name = gun::to_name((GunType)i);
            const int64_t price = gun_price(name);
            if (price <= 0) continue;
            ImGui::PushID(i);
            ImGui::BeginGroup();
            ImGui::TextUnformatted(name);
            ImGui::TextColored(kDim, "Damage %.1f cm    Refire %.2fs",
                               g_gun_stats[i].damage_cm, g_gun_stats[i].refire_delay_s);
            ImGui::EndGroup();
            ImGui::SameLine(ImGui::GetWindowWidth() - 145.0f);
            char buy[48]; std::snprintf(buy, sizeof buy, "FIT  %lld", (long long)price);
            ImGui::BeginDisabled(!player::can_afford(player, price));
            if (ImGui::Button(buy, ImVec2(125.0f, 34.0f)) &&
                buy_gun(player, name, slot, ctx.ship_class)) sfx::ui_click();
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
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
        char sell[64]; std::snprintf(sell, sizeof sell, "SELL  +%lld CR",
                                     (long long)repair::k_missile_launcher_sell_price);
        if (ImGui::Button(sell, ImVec2(-1.0f, 34.0f))) {
            const bool ok = left ? repair::sell_missile_launcher_left(p)
                                 : repair::sell_missile_launcher_right(p);
            if (ok) sfx::ui_click();
        }
    } else {
        ImGui::BeginDisabled(torpedo || !player::can_afford(p, repair::k_missile_launcher_price));
        char buy[64]; std::snprintf(buy, sizeof buy, "INSTALL  %lld CR",
                                    (long long)repair::k_missile_launcher_price);
        if (ImGui::Button(buy, ImVec2(-1.0f, 34.0f))) {
            const bool ok = left ? repair::buy_missile_launcher_left(p)
                                 : repair::buy_missile_launcher_right(p);
            if (ok) sfx::ui_click();
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("TORPEDO TUBE");
    status(torpedo);
    if (torpedo) {
        char sell[64]; std::snprintf(sell, sizeof sell, "SELL  +%lld CR",
                                     (long long)repair::k_torpedo_launcher_sell_price);
        if (ImGui::Button(sell, ImVec2(-1.0f, 34.0f))) {
            const bool ok = left ? repair::sell_torpedo_launcher_left(p)
                                 : repair::sell_torpedo_launcher_right(p);
            if (ok) sfx::ui_click();
        }
    } else {
        ImGui::BeginDisabled(missile || !player::can_afford(p, repair::k_torpedo_launcher_price));
        char buy[64]; std::snprintf(buy, sizeof buy, "INSTALL  %lld CR",
                                    (long long)repair::k_torpedo_launcher_price);
        if (ImGui::Button(buy, ImVec2(-1.0f, 34.0f))) {
            const bool ok = left ? repair::buy_torpedo_launcher_left(p)
                                 : repair::buy_torpedo_launcher_right(p);
            if (ok) sfx::ui_click();
        }
        ImGui::EndDisabled();
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
            char buy[48]; std::snprintf(buy, sizeof buy, "FIT  %lld", (long long)price);
            ImGui::BeginDisabled(!player::can_afford(p, price));
            if (ImGui::Button(buy, ImVec2(125.0f, 34.0f)) && buy_armor(p, armor.name))
                sfx::ui_click();
            ImGui::EndDisabled();
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
    ImGui::BeginDisabled(level >= cap || next_price <= 0 || !player::can_afford(p, next_price));
    char upgrade[80]; std::snprintf(upgrade, sizeof upgrade, "UPGRADE TO LEVEL %d   %lld CR",
                                    level + 1, (long long)next_price);
    if (ImGui::Button(upgrade, ImVec2(-1.0f, 40.0f))) {
        const bool ok = shield ? upgrade_shield(p, ctx.ship_class)
                               : upgrade_engine(p, ctx.ship_class);
        if (ok) sfx::ui_click();
    }
    ImGui::EndDisabled();
    if (level > 0) {
        const int64_t refund = shield ? shield_upgrade_price(level)
                                      : engine_upgrade_price(level);
        char sell[80]; std::snprintf(sell, sizeof sell, "SELL LEVEL %d   +%lld CR",
                                     level, (long long)refund);
        if (ImGui::Button(sell, ImVec2(-1.0f, 36.0f))) {
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
        char sell[64]; std::snprintf(sell, sizeof sell, "REMOVE   +%lld CR",
                                     (long long)cargo_expansion_price());
        if (ImGui::Button(sell, ImVec2(-1.0f, 38.0f)) && sell_cargo_expansion(p))
            sfx::ui_click();
    } else {
        const int64_t price = cargo_expansion_price();
        ImGui::BeginDisabled(price <= 0 || !player::can_afford(p, price));
        char buy[64]; std::snprintf(buy, sizeof buy, "INSTALL   %lld CR", (long long)price);
        if (ImGui::Button(buy, ImVec2(-1.0f, 40.0f)) && buy_cargo_expansion(p))
            sfx::ui_click();
        ImGui::EndDisabled();
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

// Scanner bay (#143): one scanner fitted at a time. Fitting over the
// current unit trades it in at full price, so each row shows the NET charge.
void draw_scanners(PlayerState& p) {
    const ScannerType* fitted = scanner::find(p.scanner_id);
    ImGui::Spacing();
    heading("SCANNER", fitted ? fitted->name.c_str() : "No scanner fitted");
    if (fitted) {
        char sell[80]; std::snprintf(sell, sizeof sell, "SELL %s   +%lld CR",
                                     fitted->name.c_str(), (long long)fitted->price);
        if (ImGui::Button(sell, ImVec2(-1.0f, 36.0f)) && scanner::sell(p)) sfx::ui_click();
    }
    const int64_t trade_in = fitted ? fitted->price : 0;
    for (const ScannerType& s : scanner::catalog()) {
        ImGui::PushID(s.id.c_str());
        ImGui::TextUnformatted(s.name.c_str());
        ImGui::TextColored(kDim, "%.1f km  %s%s%s", s.range_m * 0.001f,
                           s.color_iff ? "COLOUR IFF" : "MONOCHROME",
                           s.target_lock ? "  LOCK" : "", s.itts ? "  ITTS" : "");
        ImGui::SameLine(ImGui::GetWindowWidth() - 145.0f);
        if (&s == fitted) {
            ImGui::TextColored(kGood, "FITTED");
        } else {
            const int64_t net = s.price - trade_in;
            char buy[48];
            if (net >= 0) std::snprintf(buy, sizeof buy, "FIT  %lld", (long long)net);
            else          std::snprintf(buy, sizeof buy, "FIT  +%lld", (long long)-net);
            ImGui::BeginDisabled(net > 0 && !player::can_afford(p, net));
            if (ImGui::Button(buy, ImVec2(125.0f, 34.0f)) && scanner::buy(p, s.id))
                sfx::ui_click();
            ImGui::EndDisabled();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
}

void draw_systems(const PanelContext& ctx) {
    PlayerState& p = ctx.player;
    heading("SHIP SYSTEMS", "Avionics, navigation, ECM, damage control, and scanners");
    struct Item { const char* id; const char* label; const char* detail; };
    constexpr std::array<Item, 6> items{{
        {"jump_drive", "Jump Drive", "Enables inter-system jump points"},
        {"ecm_l1", "ECM Level 1", "Basic missile lock disruption"},
        {"ecm_l2", "ECM Level 2", "Improved disruption; requires L1"},
        {"ecm_l3", "ECM Level 3", "Advanced disruption; requires L2"},
        {"repair_droid", "Repair Droid", "Repairs hull damage in flight"},
        {"adv_repair_droid", "Advanced Repair Droid", "Faster repair; requires standard droid"},
    }};
    for (const Item& item : items) {
        ImGui::PushID(item.id);
        ImGui::TextUnformatted(item.label);
        ImGui::TextColored(kDim, "%s", item.detail);
        ImGui::SameLine(ImGui::GetWindowWidth() - 145.0f);
        if (discrete_owned(p, item.id)) {
            ImGui::TextColored(kGood, "INSTALLED");
        } else {
            const int64_t price = discrete_price(item.id);
            const bool dependency = std::string(item.id) != "adv_repair_droid" || p.has_repair_droid;
            char buy[48]; std::snprintf(buy, sizeof buy, "BUY  %lld", (long long)price);
            ImGui::BeginDisabled(price <= 0 || !dependency || !player::can_afford(p, price));
            if (ImGui::Button(buy, ImVec2(125.0f, 34.0f)) && buy_discrete(p, item.id))
                sfx::ui_click();
            ImGui::EndDisabled();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    draw_scanners(p);
}

void draw_service(const PanelContext& ctx) {
    PlayerState& p = ctx.player;
    heading("REPAIR & REARM", "Restore the hull and replenish ordnance");
    const repair::Quote quote = repair::quote(ctx.live_ship, p);
    if (!ctx.live_ship || !quote.hull_damaged) {
        ImGui::TextColored(kGood, "%s", ctx.live_ship ? "HULL AT FULL INTEGRITY" : "HULL STATUS UNAVAILABLE");
    } else {
        char repair_label[80];
        std::snprintf(repair_label, sizeof repair_label, "REPAIR HULL   %lld CR",
                      (long long)quote.hull_cost);
        ImGui::BeginDisabled(!player::can_afford(p, quote.hull_cost));
        if (ImGui::Button(repair_label, ImVec2(-1.0f, 40.0f)) &&
            repair::repair_hull(*ctx.live_ship, p)) sfx::ui_click();
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    ImGui::Text("MISSILE RACK    %d / %d",
                repair::missiles_total(p), repair::missile_rack_capacity(p));
    ImGui::Text("TORPEDO RACK    %d / %d",
                repair::torpedoes_total(p), repair::torpedo_rack_capacity(p));
    ImGui::TextColored(kDim, "Select a left or right launcher hardpoint to buy and sell ordnance.");
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
