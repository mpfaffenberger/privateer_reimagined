// -----------------------------------------------------------------------------
// outfitting.cpp — hull/equipment pricing + transactions, and the Ship Dealer
// and Equipment screen bodies.
//
// See outfitting.h for the design. Like economy.cpp this file is two halves:
//
//   1. The pricing + transaction MODEL — load the hand-authored JSON, answer
//      price queries, and apply purchases through player:: helpers ONLY. Pure
//      data, no ImGui; the offline harness (tools/test_outfitting.cpp) links
//      it under OUTFITTING_HEADLESS.
//   2. The Ship Dealer + Equipment screen bodies — ImGui registered with
//      base_screens via the np-9cu.4 hook seam. They call the SAME transaction
//      functions the harness does (DRY: one enforcement path), then play the
//      ui_click sfx on success.
// -----------------------------------------------------------------------------

#include "outfitting.h"

#include "gun.h"
#include "json.h"
#include "player.h"
#include "ship_class.h"

// The model is pure data; the screen bodies drag in the UI/audio stack.
// OUTFITTING_HEADLESS compiles only the model (mirrors ECONOMY_HEADLESS).
#ifndef OUTFITTING_HEADLESS
#include "base_screens.h"
#include "repair.h"
#include "ship.h"
#include "sfx.h"
#include "imgui.h"
#include "sokol_app.h"
#endif

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace outfitting {

namespace {

// ---- pricing model (loaded from ship_prices.json + equipment_prices.json) ---

struct HullPrice { std::string id; int64_t price = 0; };

std::vector<HullPrice>                       g_hulls;        // authored order
float                                        g_trade_in_pct = 0.55f;
std::unordered_map<std::string, int64_t>     g_gun_price;    // short_name -> price
std::vector<int64_t>                         g_shield_price; // index = level
std::vector<int64_t>                         g_engine_price; // index = level
float                                        g_engine_speed_mult = 0.08f;
int64_t                                      g_cargo_expansion_price = 0;

} // namespace

int load(const std::string& ship_prices_path, const std::string& equip_prices_path) {
    g_hulls.clear();
    g_trade_in_pct = 0.55f;
    g_gun_price.clear();
    g_shield_price.clear();
    g_engine_price.clear();
    g_engine_speed_mult = 0.08f;
    g_cargo_expansion_price = 0;

    // ---- hull prices --------------------------------------------------------
    const json::Value sp = json::parse_file(ship_prices_path);
    if (sp.is_object()) {
        if (sp.contains("trade_in_pct")) g_trade_in_pct = sp["trade_in_pct"].as_float();
        if (const json::Value* h = sp.find("hulls"); h && h->is_array()) {
            for (const json::Value& e : h->as_array()) {
                if (!e.is_object() || !e.contains("id") || !e.contains("price")) continue;
                g_hulls.push_back({ e["id"].as_string(), (int64_t)e["price"].as_int() });
            }
        }
    } else {
        std::fprintf(stderr, "[outfit] cannot load '%s' — hull dealer disabled\n",
                     ship_prices_path.c_str());
    }

    // ---- equipment prices ---------------------------------------------------
    const json::Value ep = json::parse_file(equip_prices_path);
    if (ep.is_object()) {
        if (const json::Value* g = ep.find("guns"); g && g->is_object()) {
            for (const auto& [name, v] : g->as_object()) g_gun_price[name] = (int64_t)v.as_int();
        }
        if (const json::Value* s = ep.find("shield_level_price"); s && s->is_array())
            for (const json::Value& v : s->as_array()) g_shield_price.push_back((int64_t)v.as_int());
        if (const json::Value* e = ep.find("engine_level_price"); e && e->is_array())
            for (const json::Value& v : e->as_array()) g_engine_price.push_back((int64_t)v.as_int());
        if (ep.contains("engine_speed_mult_per_level"))
            g_engine_speed_mult = ep["engine_speed_mult_per_level"].as_float();
        if (ep.contains("cargo_expansion_price"))
            g_cargo_expansion_price = (int64_t)ep["cargo_expansion_price"].as_int();
    } else {
        std::fprintf(stderr, "[outfit] cannot load '%s' — equipment shop disabled\n",
                     equip_prices_path.c_str());
    }

    std::printf("[outfit] %zu hulls, %zu guns, shield ladder %zu, engine ladder %zu "
                "(trade-in %.0f%%)\n",
                g_hulls.size(), g_gun_price.size(), g_shield_price.size(),
                g_engine_price.size(), g_trade_in_pct * 100.0f);
    return (int)g_hulls.size();
}

// ---- pricing queries --------------------------------------------------------

int64_t hull_price(const std::string& hull_id) {
    for (const HullPrice& h : g_hulls) if (h.id == hull_id) return h.price;
    return 0;
}

int64_t hull_trade_in(const std::string& hull_id) {
    return (int64_t)std::floor(hull_price(hull_id) * (double)g_trade_in_pct);
}

int64_t hull_net_cost(const std::string& target, const std::string& current) {
    return hull_price(target) - hull_trade_in(current);
}

int64_t gun_price(const std::string& gun_short_name) {
    const auto it = g_gun_price.find(gun_short_name);
    return (it == g_gun_price.end()) ? 0 : it->second;
}

int64_t shield_upgrade_price(int target_level) {
    if (target_level < 0 || target_level >= (int)g_shield_price.size()) return 0;
    return g_shield_price[target_level];
}

int64_t engine_upgrade_price(int target_level) {
    if (target_level < 0 || target_level >= (int)g_engine_price.size()) return 0;
    return g_engine_price[target_level];
}

int64_t cargo_expansion_price() { return g_cargo_expansion_price; }

SpeedCaps effective_speed_caps(const PlayerState& p) {
    SpeedCaps caps;
    const ShipClass* k = ship_class::find(p.ship_class_name);
    if (!k) return caps;  // stock 300/600 fallback
    const float mult = 1.0f + g_engine_speed_mult * (float)p.engine_level;
    caps.cruise0 = k->cruise_speed * mult;
    // afterburner_speed==0 means "none fitted" — fall back to 2x cruise so the
    // camera's cruise ceiling stays above its base.
    const float ab = k->afterburner_speed > k->cruise_speed
                   ? k->afterburner_speed : k->cruise_speed * 2.0f;
    caps.cruise1 = ab * mult;
    return caps;
}

// ---- transactions (headless-safe; shared by UI + harness) -------------------

bool buy_hull(PlayerState& p, const std::string& target) {
    if (target == p.ship_class_name) {
        std::printf("[outfit] BUY HULL refused: already flying %s\n", target.c_str());
        return false;
    }
    if (hull_price(target) <= 0) {
        std::printf("[outfit] BUY HULL refused: '%s' not for sale\n", target.c_str());
        return false;
    }
    const int64_t net = hull_net_cost(target, p.ship_class_name);
    if (net > 0 && !player::can_afford(p, net)) {
        std::printf("[outfit] BUY HULL refused: %s nets %lld, have %lld\n",
                    target.c_str(), (long long)net, (long long)p.credits);
        return false;
    }
    if (net > 0)      player::spend_credits(p, net);
    else if (net < 0) player::add_credits(p, -net);   // trade-in surplus

    // Re-fit the new hull's stock loadout. v1 choice (see header): lossy —
    // old fitted guns/upgrades vanish with no part-out refund; the trade-in
    // already values the WHOLE old ship.
    const std::string old = p.ship_class_name;
    p.ship_class_name = target;
    p.shield_level    = 0;
    p.engine_level    = 0;
    p.cargo_expansion = false;
    p.gun_mounts.clear();
    if (const ShipClass* k = ship_class::find(target))
        for (const GunMount& m : k->default_guns) p.gun_mounts.push_back(gun::to_name(m.type));

    std::printf("[outfit] BUY HULL %s -> %s | net %lld | credits %lld | %zu default mounts\n",
                old.c_str(), target.c_str(), (long long)net, (long long)p.credits,
                p.gun_mounts.size());
    return true;
}

bool buy_gun(PlayerState& p, const std::string& gun_short_name,
             int mount_index, const ShipClass* klass) {
    const int mounts = klass ? (int)klass->default_guns.size() : (int)p.gun_mounts.size();
    if (mount_index < 0 || mount_index >= mounts) {
        std::printf("[outfit] BUY GUN refused: mount %d out of range (hull has %d)\n",
                    mount_index, mounts);
        return false;
    }
    if (gun::from_name(gun_short_name) == GunType::Count) {
        std::printf("[outfit] BUY GUN refused: unknown gun '%s'\n", gun_short_name.c_str());
        return false;
    }
    const int64_t price = gun_price(gun_short_name);
    if (price <= 0) {
        std::printf("[outfit] BUY GUN refused: '%s' not for sale\n", gun_short_name.c_str());
        return false;
    }
    if (!player::can_afford(p, price)) {
        std::printf("[outfit] BUY GUN refused: %s costs %lld, have %lld\n",
                    gun_short_name.c_str(), (long long)price, (long long)p.credits);
        return false;
    }
    player::spend_credits(p, price);
    if ((int)p.gun_mounts.size() < mounts) p.gun_mounts.resize(mounts, "");
    p.gun_mounts[mount_index] = gun_short_name;
    std::printf("[outfit] BUY GUN %s -> mount %d @ %lld | credits %lld\n",
                gun_short_name.c_str(), mount_index, (long long)price, (long long)p.credits);
    return true;
}

bool upgrade_shield(PlayerState& p, const ShipClass* klass) {
    const int cap  = klass ? (int)klass->max_shield_level : 0;
    const int next = p.shield_level + 1;
    if (next > cap) {
        std::printf("[outfit] SHIELD refused: already at hull max (%d)\n", cap);
        return false;
    }
    const int64_t price = shield_upgrade_price(next);
    if (price <= 0) { std::printf("[outfit] SHIELD refused: no price for level %d\n", next); return false; }
    if (!player::can_afford(p, price)) {
        std::printf("[outfit] SHIELD refused: L%d costs %lld, have %lld\n",
                    next, (long long)price, (long long)p.credits);
        return false;
    }
    player::spend_credits(p, price);
    p.shield_level = next;
    std::printf("[outfit] SHIELD -> L%d @ %lld | credits %lld\n",
                next, (long long)price, (long long)p.credits);
    return true;
}

bool upgrade_engine(PlayerState& p, const ShipClass* klass) {
    const int cap  = klass ? (int)klass->max_engine_level : 0;
    const int next = p.engine_level + 1;
    if (next > cap) {
        std::printf("[outfit] ENGINE refused: already at hull max (%d)\n", cap);
        return false;
    }
    const int64_t price = engine_upgrade_price(next);
    if (price <= 0) { std::printf("[outfit] ENGINE refused: no price for level %d\n", next); return false; }
    if (!player::can_afford(p, price)) {
        std::printf("[outfit] ENGINE refused: L%d costs %lld, have %lld\n",
                    next, (long long)price, (long long)p.credits);
        return false;
    }
    player::spend_credits(p, price);
    p.engine_level = next;
    std::printf("[outfit] ENGINE -> L%d @ %lld | credits %lld\n",
                next, (long long)price, (long long)p.credits);
    return true;
}

bool buy_cargo_expansion(PlayerState& p) {
    if (p.cargo_expansion) {
        std::printf("[outfit] CARGO EXPANSION refused: already owned\n");
        return false;
    }
    const int64_t price = cargo_expansion_price();
    if (price <= 0) { std::printf("[outfit] CARGO EXPANSION refused: no price\n"); return false; }
    if (!player::can_afford(p, price)) {
        std::printf("[outfit] CARGO EXPANSION refused: costs %lld, have %lld\n",
                    (long long)price, (long long)p.credits);
        return false;
    }
    player::spend_credits(p, price);
    p.cargo_expansion = true;
    std::printf("[outfit] CARGO EXPANSION bought @ %lld | credits %lld\n",
                (long long)price, (long long)p.credits);
    return true;
}

// ---- Ship Dealer + Equipment screen bodies ----------------------------------
#ifndef OUTFITTING_HEADLESS

namespace {

// HUD palette echoing economy.cpp / base_screens.cpp.
constexpr ImU32 kAmber = IM_COL32(255, 217,  77, 255);
constexpr ImU32 kGreen = IM_COL32(120, 230, 120, 255);
constexpr ImU32 kRed   = IM_COL32(230, 110, 110, 255);
constexpr ImU32 kGrey  = IM_COL32(150, 158, 168, 255);

// Ship Dealer: hull pending confirmation (warn-before-buy, see header). Empty
// = nothing pending. Equipment: which mount slot the gun catalog fits into.
std::string g_pending_hull;
int         g_sel_mount = 0;

struct ScreenWH { float w, h; };
ScreenWH screen_wh() {
    const float dpi = sapp_dpi_scale();
    return { (float)sapp_width() / dpi, (float)sapp_height() / dpi };
}

// ---- Ship Dealer ------------------------------------------------------------

void draw_dealer(BaseContext& ctx) {
    PlayerState& p = *ctx.player;
    const ScreenWH ss = screen_wh();

    ImGui::SetCursorScreenPos(ImVec2(28, 62));
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::Text("CREDITS  %lld", (long long)p.credits);
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(28, 92));
    const ImVec2 child_sz(ss.w - 56, ss.h - 92 - 70);
    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginChild("##dealer", child_sz, false) &&
        ImGui::BeginTable("hulls", 8, tflags, child_sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Hull", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Top Spd");
        ImGui::TableSetupColumn("Armor F/A/S");
        ImGui::TableSetupColumn("Cargo");
        ImGui::TableSetupColumn("Mounts");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Net");
        ImGui::TableSetupColumn("Buy", ImGuiTableColumnFlags_WidthFixed, 280.0f);
        ImGui::TableHeadersRow();

        for (const HullPrice& h : g_hulls) {
            const ShipClass* k = ship_class::find(h.id);
            const bool owned = (h.id == p.ship_class_name);
            ImGui::TableNextRow();
            ImGui::PushID(h.id.c_str());

            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, owned ? kGreen : kAmber);
            ImGui::TextUnformatted(k ? k->display_name.c_str() : h.id.c_str());
            ImGui::PopStyleColor();
            if (owned) { ImGui::SameLine(); ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                         ImGui::TextUnformatted("<-- CURRENT"); ImGui::PopStyleColor(); }

            ImGui::TableNextColumn();
            if (k) ImGui::Text("%.0f/%.0f", k->cruise_speed, k->afterburner_speed);
            else   ImGui::TextUnformatted("--");
            ImGui::TableNextColumn();
            if (k) ImGui::Text("%.0f/%.0f/%.0f", k->armor_fore_cm, k->armor_aft_cm, k->armor_side_cm);
            else   ImGui::TextUnformatted("--");
            ImGui::TableNextColumn();
            if (k) ImGui::Text("%d (%d)", k->cargo_units, k->cargo_units_max);
            else   ImGui::TextUnformatted("--");
            ImGui::TableNextColumn();
            ImGui::Text("%d", k ? (int)k->default_guns.size() : 0);
            ImGui::TableNextColumn();
            ImGui::Text("%lld", (long long)h.price);

            const int64_t net = hull_net_cost(h.id, p.ship_class_name);
            ImGui::TableNextColumn();
            if (owned) { ImGui::PushStyleColor(ImGuiCol_Text, kGrey); ImGui::TextUnformatted("--"); ImGui::PopStyleColor(); }
            else       ImGui::Text("%lld", (long long)net);

            // Buy column: warn-before-purchase. First click arms the confirm
            // (loadout reset is destructive); second click commits.
            ImGui::TableNextColumn();
            if (owned) {
                ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::TextUnformatted("OWNED");
                ImGui::PopStyleColor();
            } else if (g_pending_hull == h.id) {
                ImGui::PushStyleColor(ImGuiCol_Text, kRed);
                ImGui::TextUnformatted("Resets loadout!");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::SmallButton("CONFIRM")) {
                    if (outfitting::buy_hull(p, h.id)) sfx::ui_click();
                    g_pending_hull.clear();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel")) g_pending_hull.clear();
            } else {
                const bool affordable = net <= 0 || player::can_afford(p, net);
                ImGui::BeginDisabled(!affordable);
                if (ImGui::SmallButton("Buy")) g_pending_hull = h.id;
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// ---- Equipment --------------------------------------------------------------

void draw_equipment(BaseContext& ctx) {
    PlayerState& p = *ctx.player;
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    const ScreenWH ss = screen_wh();
    const int mounts = klass ? (int)klass->default_guns.size() : (int)p.gun_mounts.size();
    if ((int)p.gun_mounts.size() < mounts) p.gun_mounts.resize(mounts, "");
    if (g_sel_mount >= mounts) g_sel_mount = 0;

    // Summary strip.
    ImGui::SetCursorScreenPos(ImVec2(28, 60));
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::Text("CREDITS %lld    SHIP %s    CARGO %d/%d%s",
                (long long)p.credits,
                klass ? klass->display_name.c_str() : p.ship_class_name.c_str(),
                player::cargo_units_used(p), player::cargo_capacity(p, klass),
                p.cargo_expansion ? " (expanded)" : "");
    ImGui::PopStyleColor();

    // ---- Mounts: pick the target slot --------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(28, 92));
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::TextUnformatted("WEAPON MOUNTS (click to select fit target):");
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(28, 112));
    for (int i = 0; i < mounts; ++i) {
        if (i) ImGui::SameLine();
        const std::string& g = p.gun_mounts[(size_t)i];
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "[%d] %s", i, g.empty() ? "empty" : g.c_str());
        const bool sel = (i == g_sel_mount);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 110, 60, 255));
        if (ImGui::Button(lbl)) g_sel_mount = i;
        if (sel) ImGui::PopStyleColor();
    }

    // ---- Gun catalog -> fit into selected mount ----------------------------
    ImGui::SetCursorScreenPos(ImVec2(28, 150));
    const ImVec2 gun_sz(ss.w * 0.5f - 40, ss.h - 150 - 70);
    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginChild("##guns", gun_sz, false) &&
        ImGui::BeginTable("guncat", 4, tflags, gun_sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Gun", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Dmg");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Fit");
        ImGui::TableHeadersRow();
        for (int t = 0; t < kGunTypeCount; ++t) {
            const char* name = gun::to_name((GunType)t);
            const int64_t price = gun_price(name);
            if (price <= 0) continue;   // not for sale
            ImGui::TableNextRow();
            ImGui::PushID(t);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", g_gun_stats[t].damage_cm);
            ImGui::TableNextColumn(); ImGui::Text("%lld", (long long)price);
            ImGui::TableNextColumn();
            const bool can = mounts > 0 && player::can_afford(p, price);
            ImGui::BeginDisabled(!can);
            if (ImGui::SmallButton("Fit")) {
                if (outfitting::buy_gun(p, name, g_sel_mount, klass)) sfx::ui_click();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ---- Upgrades (shield / engine / cargo) --------------------------------
    ImGui::SetCursorScreenPos(ImVec2(ss.w * 0.5f + 12, 150));
    ImGui::BeginChild("##upgrades", ImVec2(ss.w * 0.5f - 40, ss.h - 150 - 70), false);
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted("UPGRADES");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Shield ladder.
    const int sh_cap = klass ? (int)klass->max_shield_level : 0;
    ImGui::Text("Shield   L%d / %d", p.shield_level, sh_cap);
    ImGui::SameLine();
    if (p.shield_level >= sh_cap) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey); ImGui::TextUnformatted("MAX"); ImGui::PopStyleColor();
    } else {
        const int64_t price = shield_upgrade_price(p.shield_level + 1);
        char b[48]; std::snprintf(b, sizeof(b), "Upgrade -> L%d (%lld)", p.shield_level + 1, (long long)price);
        ImGui::BeginDisabled(price <= 0 || !player::can_afford(p, price));
        if (ImGui::SmallButton(b)) { if (outfitting::upgrade_shield(p, klass)) sfx::ui_click(); }
        ImGui::EndDisabled();
    }

    // Engine ladder.
    const int en_cap = klass ? (int)klass->max_engine_level : 0;
    ImGui::Text("Engine   L%d / %d", p.engine_level, en_cap);
    ImGui::SameLine();
    if (p.engine_level >= en_cap) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey); ImGui::TextUnformatted("MAX"); ImGui::PopStyleColor();
    } else {
        const int64_t price = engine_upgrade_price(p.engine_level + 1);
        char b[48]; std::snprintf(b, sizeof(b), "Upgrade -> L%d (%lld)", p.engine_level + 1, (long long)price);
        ImGui::BeginDisabled(price <= 0 || !player::can_afford(p, price));
        if (ImGui::SmallButton(b)) { if (outfitting::upgrade_engine(p, klass)) sfx::ui_click(); }
        ImGui::EndDisabled();
    }
    const SpeedCaps caps = effective_speed_caps(p);
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::Text("  effective top speed: %.0f / %.0f (cruise/AB)", caps.cruise0, caps.cruise1);
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Cargo expansion (one-time).
    ImGui::TextUnformatted("Cargo Expansion");
    ImGui::SameLine();
    if (p.cargo_expansion) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen); ImGui::TextUnformatted("OWNED"); ImGui::PopStyleColor();
    } else {
        const int64_t price = cargo_expansion_price();
        char b[40]; std::snprintf(b, sizeof(b), "Buy (%lld)", (long long)price);
        ImGui::BeginDisabled(price <= 0 || !player::can_afford(p, price));
        if (ImGui::SmallButton(b)) { if (outfitting::buy_cargo_expansion(p)) sfx::ui_click(); }
        ImGui::EndDisabled();
    }

    // ---- Repair & Rearm (np-zte.2) -----------------------------------------
    // The base repair service: restore hull armor (Ship state), top off the
    // afterburner tank + restock missiles (PlayerState). Quoted live so the
    // buttons disable when nothing's needed / unaffordable. All transactions
    // route through repair:: -> player::spend_credits (one enforcement path).
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted("REPAIR & REARM");
    ImGui::PopStyleColor();
    {
        Ship* psh = ctx.player_ship;
        const repair::Quote q = repair::quote(psh, p);

        // Hull.
        if (!psh || !q.hull_damaged) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            ImGui::TextUnformatted(psh ? "Hull        FULL" : "Hull        --");
            ImGui::PopStyleColor();
        } else {
            char b[48]; std::snprintf(b, sizeof(b), "Repair hull (%lld)", (long long)q.hull_cost);
            ImGui::BeginDisabled(!player::can_afford(p, q.hull_cost));
            if (ImGui::SmallButton(b)) { if (repair::repair_hull(*psh, p)) sfx::ui_click(); }
            ImGui::EndDisabled();
        }

        // Afterburner fuel row removed (np-zte.2 merged pool). The
        // burner shares the ship's energy bank now; it recharges for
        // free in flight, so there's nothing to sell here.

        // Missiles.
        ImGui::Text("Missiles    DF %d / HS %d / IR %d", p.missiles[0], p.missiles[1], p.missiles[2]);
        ImGui::SameLine();
        if (!q.missiles_low) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGreen); ImGui::TextUnformatted("FULL"); ImGui::PopStyleColor();
        } else {
            char b[48]; std::snprintf(b, sizeof(b), "Rearm (%lld)", (long long)q.missile_cost);
            ImGui::BeginDisabled(!player::can_afford(p, q.missile_cost));
            if (ImGui::SmallButton(b)) { if (repair::rearm(p)) sfx::ui_click(); }
            ImGui::EndDisabled();
        }
    }
    ImGui::EndChild();
}

} // namespace

void register_screens() {
    base_screens::register_screen(BaseScreen::ShipDealer, draw_dealer);
    base_screens::register_screen(BaseScreen::Equipment,  draw_equipment);
}

#endif // OUTFITTING_HEADLESS

} // namespace outfitting
// 1781715641497550000
