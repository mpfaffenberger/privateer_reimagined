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

#include <algorithm>  // std::clamp
#include <array>      // std::array

#include "armor.h"
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
std::unordered_map<std::string, int64_t>     g_armor_price;  // ArmorType::name -> price
std::vector<int64_t>                         g_shield_price; // index = level
std::vector<int64_t>                         g_engine_price; // index = level
float                                        g_engine_regen_mult = 0.08f;   // legacy mult (unused)
std::vector<float>                           g_engine_regen_bonus;   // GJ/s by engine level
std::vector<float>                           g_shield_regen_drain;   // GJ/s by shield level

int64_t                                      g_cargo_expansion_price = 0;
std::unordered_map<std::string, int64_t>     g_discrete_price;   // np-3dp.27

} // namespace

int load(const std::string& ship_prices_path, const std::string& equip_prices_path) {
    g_hulls.clear();
    g_trade_in_pct = 0.55f;
    g_gun_price.clear();
    g_armor_price.clear();
    g_shield_price.clear();
    g_engine_price.clear();
    g_engine_regen_mult = 0.08f;
    g_engine_regen_bonus.clear();
    g_shield_regen_drain.clear();
    g_cargo_expansion_price = 0;
    g_discrete_price.clear();

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
        if (const json::Value* a = ep.find("armor"); a && a->is_object()) {
            for (const auto& [name, v] : a->as_object()) g_armor_price[name] = (int64_t)v.as_int();
        }
        if (const json::Value* s = ep.find("shield_level_price"); s && s->is_array())
            for (const json::Value& v : s->as_array()) g_shield_price.push_back((int64_t)v.as_int());
        if (const json::Value* e = ep.find("engine_level_price"); e && e->is_array())
            for (const json::Value& v : e->as_array()) g_engine_price.push_back((int64_t)v.as_int());
        if (ep.contains("engine_regen_mult_per_level"))
            g_engine_regen_mult = ep["engine_regen_mult_per_level"].as_float();
        if (const json::Value* e = ep.find("engine_regen_bonus_per_level"); e && e->is_array())
            for (const json::Value& v : e->as_array())
                g_engine_regen_bonus.push_back((float)v.as_float());
        if (const json::Value* e = ep.find("shield_recharge_drain_per_level"); e && e->is_array())
            for (const json::Value& v : e->as_array())
                g_shield_regen_drain.push_back((float)v.as_float());
        if (ep.contains("cargo_expansion_price"))
            g_cargo_expansion_price = (int64_t)ep["cargo_expansion_price"].as_int();
        if (const json::Value* d = ep.find("discrete_equipment"); d && d->is_object())
            for (const auto& [name, v] : d->as_object())
                g_discrete_price[name] = (int64_t)v.as_int();
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

int64_t armor_price(const std::string& armor_name) {
    const auto it = g_armor_price.find(armor_name);
    return (it == g_armor_price.end()) ? 0 : it->second;
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

int64_t discrete_price(const std::string& item) {
    const auto it = g_discrete_price.find(item);
    return (it == g_discrete_price.end()) ? 0 : it->second;
}

bool buy_discrete(PlayerState& p, const std::string& item) {
    const int64_t price = discrete_price(item);
    if (price <= 0) {
        std::printf("[outfit] DISCRETE refused: '%s' not for sale\n", item.c_str());
        return false;
    }
    // ECM is a ladder: each level is a distinct item (ecm_l1..l3), but
    // require the previous level first so you can't jump straight to L3.
    if (item == "ecm_l1") {
        if (p.ecm_level >= 1) { std::printf("[outfit] ECM L1 refused: already owned\n"); return false; }
    } else if (item == "ecm_l2") {
        if (p.ecm_level >= 2) { std::printf("[outfit] ECM L2 refused: already owned\n"); return false; }
        if (p.ecm_level < 1)  { std::printf("[outfit] ECM L2 refused: own ECM L1 first\n"); return false; }
    } else if (item == "ecm_l3") {
        if (p.ecm_level >= 3) { std::printf("[outfit] ECM L3 refused: already owned\n"); return false; }
        if (p.ecm_level < 2)  { std::printf("[outfit] ECM L3 refused: own ECM L2 first\n"); return false; }
    } else if (item == "jump_drive") {
        if (p.has_jump_drive)   { std::printf("[outfit] JUMP DRIVE refused: already owned\n"); return false; }
    } else if (item == "tractor_beam") {
        // (#82) Universal tractor — no longer purchasable. Every new
        // ship gets the flag for free (player.cpp new_game). If some
        // legacy code path tries to buy one, refuse with a noisy log so
        // it's obvious the item is intentionally not for sale.
        std::printf("[outfit] DISCRETE refused: '%s' not for sale (universal)\n", item.c_str());
        return false;
    } else if (item == "repair_droid") {
        if (p.has_repair_droid) { std::printf("[outfit] REPAIR DROID refused: already owned\n"); return false; }
    } else if (item == "adv_repair_droid") {
        if (!p.has_repair_droid) { std::printf("[outfit] ADV REPAIR DROID refused: own Repair Droid first\n"); return false; }
        // adv_repair_droid is an UPGRADE that replaces the stock repair droid;
        // a player can't own BOTH. Flag via has_repair_droid=true + ecm_level==0
        // won't work, so we tag by also blocking the buy of the base if adv is
        // owned. The runtime uses has_repair_droid as the "owns a droid" gate
        // and ecm_level==0 isn't relevant here — we use a sentinel: if the
        // player already has adv and tries to buy the stock, refuse (handled
        // in the buy_discrete path for 'repair_droid' above is already short-
        // circuited by has_repair_droid). For now we don't separately track
        // adv — both flags just enable the in-flight hull repair tick.
    } else {
        std::printf("[outfit] DISCRETE refused: unknown item '%s'\n", item.c_str());
        return false;
    }
    if (!player::can_afford(p, price)) {
        std::printf("[outfit] DISCRETE '%s' refused: costs %lld, have %lld\n",
                    item.c_str(), (long long)price, (long long)p.credits);
        return false;
    }
    player::spend_credits(p, price);
    if      (item == "jump_drive")        p.has_jump_drive = true;
    else if (item == "ecm_l1")            p.ecm_level = 1;
    else if (item == "ecm_l2")            p.ecm_level = 2;
    else if (item == "ecm_l3")            p.ecm_level = 3;
    else if (item == "repair_droid")      p.has_repair_droid = true;
    else if (item == "adv_repair_droid") { p.has_repair_droid = true; p.adv_repair_droid = true; }
    std::printf("[outfit] DISCRETE '%s' bought @ %lld | credits %lld\n",
                item.c_str(), (long long)price, (long long)p.credits);
    return true;
}

SpeedCaps effective_speed_caps(const PlayerState& p) {
    SpeedCaps caps;
    const ShipClass* k = ship_class::find(p.ship_class_name);
    if (!k) return caps;  // stock 300/600 fallback
    // Top speed (and afterburner speed) are HULL-only. Engine upgrades no
    // longer scale them — they boost the energy regen rate via
    // engine_recharge_mult_for() instead. (gamefaq 4.6.2.)
    caps.cruise0 = k->cruise_speed;
    // afterburner_speed==0 means "none fitted" — fall back to 2x cruise so the
    // camera's cruise ceiling stays above its base.
    const float ab = k->afterburner_speed > k->cruise_speed
                   ? k->afterburner_speed : k->cruise_speed * 2.0f;
    caps.cruise1 = ab;
    return caps;
}

// Absolute GJ/s the engine upgrade ADDS to the player's recharge rate,
// per upgrade level. L0 = 0. NPCs always get 0 (engine_level is a player
// construct). Out-of-range levels clamp to the last populated entry.
float engine_recharge_bonus_for(int engine_level) {
    if (g_engine_regen_bonus.empty()) return 0.0f;
    const int idx = std::clamp(engine_level, 0, (int)g_engine_regen_bonus.size() - 1);
    return g_engine_regen_bonus[idx];
}

// Absolute GJ/s the shield generator CONSUMES from the recharge budget,
// per upgrade level. L0 = 0 (no shield gen installed). Out-of-range
// levels clamp to the last populated entry.
float shield_recharge_drain_for(int shield_level) {
    if (g_shield_regen_drain.empty()) return 0.0f;
    const int idx = std::clamp(shield_level, 0, (int)g_shield_regen_drain.size() - 1);
    return g_shield_regen_drain[idx];
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
    p.armor_name      = "";
    p.cargo_expansion = false;
    p.gun_mounts.clear();
    // NOTE: permanent_mods are deliberately NOT cleared — installed upgrades
    // live on the player and persist across hull swaps (#92/#94).
    if (const ShipClass* k = ship_class::find(target))
        for (const GunMount& m : k->default_guns) p.gun_mounts.push_back(MountSlot{gun::to_name(m.type)});

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
    if ((int)p.gun_mounts.size() < mounts) p.gun_mounts.resize(mounts, MountSlot{});
    p.gun_mounts[mount_index] = MountSlot{gun_short_name};
    std::printf("[outfit] BUY GUN %s -> mount %d @ %lld | credits %lld\n",
                gun_short_name.c_str(), mount_index, (long long)price, (long long)p.credits);
    return true;
}

bool sell_gun(PlayerState& p, int mount_index, const ShipClass* klass) {
    const int mounts = klass ? (int)klass->default_guns.size() : (int)p.gun_mounts.size();
    if (mount_index < 0 || mount_index >= mounts) {
        std::printf("[outfit] SELL GUN refused: mount %d out of range (hull has %d)\n",
                    mount_index, mounts);
        return false;
    }
    if ((int)p.gun_mounts.size() < mounts) p.gun_mounts.resize(mounts, MountSlot{});
    const std::string& name = p.gun_mounts[(size_t)mount_index].gun_id;
    if (name.empty()) {
        std::printf("[outfit] SELL GUN refused: mount %d is empty\n", mount_index);
        return false;
    }
    const int64_t refund = gun_price(name);
    if (refund <= 0) {
        std::printf("[outfit] SELL GUN refused: '%s' has no price\n", name.c_str());
        return false;
    }
    p.gun_mounts[(size_t)mount_index] = MountSlot{};
    player::add_credits(p, refund);
    std::printf("[outfit] SELL GUN %s <- mount %d refund %lld | credits %lld\n",
                name.c_str(), mount_index, (long long)refund, (long long)p.credits);
    return true;
}

bool buy_armor(PlayerState& p, const std::string& armor_name) {
    if (armor_name.empty() || !armor::find(armor_name)) {
        std::printf("[outfit] ARMOR refused: unknown armor '%s'\n", armor_name.c_str());
        return false;
    }
    if (p.armor_name == armor_name) {
        std::printf("[outfit] ARMOR refused: '%s' already fitted\n", armor_name.c_str());
        return false;
    }
    const int64_t price = armor_price(armor_name);
    if (price <= 0) {
        std::printf("[outfit] ARMOR refused: '%s' not for sale\n", armor_name.c_str());
        return false;
    }
    if (!player::can_afford(p, price)) {
        std::printf("[outfit] ARMOR refused: %s costs %lld, have %lld\n",
                    armor_name.c_str(), (long long)price, (long long)p.credits);
        return false;
    }
    player::spend_credits(p, price);
    p.armor_name = armor_name;
    // Armor changes max hull cm; clear stale damage snapshot so launch/reload
    // does not reapply old lower-armor HP onto the new fitted package.
    p.hp_valid = false;
    std::printf("[outfit] ARMOR fitted '%s' @ %lld | credits %lld\n",
                armor_name.c_str(), (long long)price, (long long)p.credits);
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

bool sell_shield(PlayerState& p, const ShipClass* klass) {
    if (p.shield_level <= 0) {
        std::printf("[outfit] SHIELD sell refused: nothing to sell (L0)\n");
        return false;
    }
    // Refund the price the player originally paid for THIS level. Going
    // down one rung so we can offer a partial refund rather than a hard
    // wipe, matching how upgrade works one step at a time.
    const int from = p.shield_level;
    const int to   = from - 1;
    const int64_t refund = shield_upgrade_price(from);
    if (refund <= 0) { std::printf("[outfit] SHIELD sell refused: no price for L%d\n", from); return false; }
    p.shield_level = to;
    player::add_credits(p, refund);
    std::printf("[outfit] SHIELD sold L%d -> L%d, refund %lld | credits %lld\n",
                from, to, (long long)refund, (long long)p.credits);
    return true;
}

bool sell_engine(PlayerState& p, const ShipClass* klass) {
    if (p.engine_level <= 0) {
        std::printf("[outfit] ENGINE sell refused: nothing to sell (L0)\n");
        return false;
    }
    const int from = p.engine_level;
    const int to   = from - 1;
    const int64_t refund = engine_upgrade_price(from);
    if (refund <= 0) { std::printf("[outfit] ENGINE sell refused: no price for L%d\n", from); return false; }
    p.engine_level = to;
    player::add_credits(p, refund);
    std::printf("[outfit] ENGINE sold L%d -> L%d, refund %lld | credits %lld\n",
                from, to, (long long)refund, (long long)p.credits);
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

bool sell_cargo_expansion(PlayerState& p) {
    if (!p.cargo_expansion) {
        std::printf("[outfit] CARGO EXPANSION sell refused: not owned\n");
        return false;
    }
    const int64_t refund = cargo_expansion_price();
    if (refund <= 0) { std::printf("[outfit] CARGO EXPANSION sell refused: no price\n"); return false; }
    p.cargo_expansion = false;
    player::add_credits(p, refund);
    std::printf("[outfit] CARGO EXPANSION sold, refund %lld | credits %lld\n",
                (long long)refund, (long long)p.credits);
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
    // The dealer is modal over bright authored room art. Keep the table's own
    // surfaces opaque enough to remain legible even before considering the
    // base-screen smoked-glass backdrop; headers and alternating rows retain
    // clear hierarchy rather than borrowing accidental colors from the art.
    ImGui::PushStyleColor(ImGuiCol_ChildBg,   ImVec4(0.025f, 0.035f, 0.060f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Header,    ImVec4(0.12f, 0.14f, 0.19f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.17f, 0.19f, 0.25f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBg,    ImVec4(0.035f, 0.045f, 0.070f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, ImVec4(0.065f, 0.075f, 0.105f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(0.55f, 0.46f, 0.22f, 0.55f));
    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginChild("##dealer", child_sz, false) &&
        ImGui::BeginTable("hulls", 8, tflags, child_sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Hull", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Top Spd");
        ImGui::TableSetupColumn("Armor F/A/P/St");
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
            if (k) ImGui::Text("%.0f/%.0f/%.0f/%.0f",
                                k->armor_fore_cm, k->armor_aft_cm,
                                k->armor_port_cm, k->armor_starboard_cm);
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
    ImGui::PopStyleColor(6);
}

// ---- Equipment --------------------------------------------------------------

void draw_equipment(BaseContext& ctx) {
    PlayerState& p = *ctx.player;
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    const ScreenWH ss = screen_wh();
    const int mounts = klass ? (int)klass->default_guns.size() : (int)p.gun_mounts.size();
    if ((int)p.gun_mounts.size() < mounts) p.gun_mounts.resize(mounts, MountSlot{});
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
        const std::string& g = p.gun_mounts[(size_t)i].gun_id;
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "[%d] %s", i, g.empty() ? "empty" : g.c_str());
        const bool sel = (i == g_sel_mount);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(60, 110, 60, 255));
        if (ImGui::Button(lbl)) g_sel_mount = i;
        if (sel) ImGui::PopStyleColor();
    }
    // Sell-back button for the gun currently selected. Refused if the
    // mount is empty (the function logs and returns false; UI just shows
    // a disabled button so the player sees the affordance).
    ImGui::SameLine();
    ImGui::PushID("sell_selected_weapon");
    const bool can_sell_gun = g_sel_mount < mounts &&
                              (int)p.gun_mounts.size() > g_sel_mount &&
                              !p.gun_mounts[(size_t)g_sel_mount].gun_id.empty();
    ImGui::BeginDisabled(!can_sell_gun);
    if (ImGui::SmallButton("Sell weapon")) {
        if (outfitting::sell_gun(p, g_sel_mount, klass)) sfx::ui_click();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && can_sell_gun) {
        const int64_t refund = gun_price(p.gun_mounts[(size_t)g_sel_mount].gun_id.c_str());
        ImGui::SetTooltip("Sell the weapon at mount %d (+%lld)",
                          g_sel_mount, (long long)refund);
    }
    ImGui::PopID();

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
        // Wrap the header row in its own scope so it can't collide with the
        // PushID(t) wrapping each data row below (np-3dp.28). Without this,
        // mousing over the header cells trips 'two invisible items with
        // conflicting ID!'.
        ImGui::PushID("weapons_header");
        ImGui::TableHeadersRow();
        ImGui::PopID();
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

    // ---- Upgrades (armor / shield / engine / cargo) ------------------------
    ImGui::SetCursorScreenPos(ImVec2(ss.w * 0.5f + 12, 150));
    ImGui::BeginChild("##upgrades", ImVec2(ss.w * 0.5f - 40, ss.h - 150 - 70), false);
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::TextUnformatted("UPGRADES");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Armor packages. Armor is a purchasable upgrade only — no ship spawns
    // with one (np armor), so an empty armor_name means NO package fitted
    // (base hull cm only). Buying fits the package per-instance; nothing
    // touches the shared ShipClass, so NPCs never inherit your shopping.
    {
        ImGui::PushID("armor_packages");
        ImGui::Text("Armor   %s",
                    p.armor_name.empty() ? "None (base hull only)"
                                         : p.armor_name.c_str());
        for (const ArmorType& a : armor::all()) {
            const int64_t price = armor_price(a.name);
            if (price <= 0) continue;
            ImGui::PushID(a.name.c_str());
            const bool fitted = (p.armor_name == a.name);
            ImGui::Text("  %s  %.0f/%.0f/%.0f/%.0f cm",
                        a.name.c_str(), a.front_cm, a.back_cm,
                        a.port_cm, a.starboard_cm);
            ImGui::SameLine();
            if (fitted) {
                ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::TextUnformatted("FITTED");
                ImGui::PopStyleColor();
            } else {
                char b[48]; std::snprintf(b, sizeof(b), "Fit (%lld)", (long long)price);
                ImGui::BeginDisabled(!player::can_afford(p, price));
                if (ImGui::SmallButton(b)) {
                    if (outfitting::buy_armor(p, a.name)) sfx::ui_click();
                }
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::Spacing();

    // Shield ladder.
    const int sh_cap = klass ? (int)klass->max_shield_level : 0;
    ImGui::PushID("shield_ladder");
    ImGui::Text("Shield   L%d / %d", p.shield_level, sh_cap);
    ImGui::SameLine();
    if (p.shield_level >= sh_cap) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey); ImGui::TextUnformatted("MAX"); ImGui::PopStyleColor();
    } else {
        const int64_t price = shield_upgrade_price(p.shield_level + 1);
        char b[48]; std::snprintf(b, sizeof(b), "Upgrade -> L%d (%lld)", p.shield_level + 1, (long long)price);
        ImGui::PushID("buy");
        ImGui::BeginDisabled(price <= 0 || !player::can_afford(p, price));
        if (ImGui::SmallButton(b)) { if (outfitting::upgrade_shield(p, klass)) sfx::ui_click(); }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (p.shield_level > 0) {
        ImGui::PushID("sell");
        const int64_t refund = shield_upgrade_price(p.shield_level);
        char s[48]; std::snprintf(s, sizeof(s), "Sell L%d (-%lld)", p.shield_level, (long long)refund);
        if (ImGui::SmallButton(s)) { if (outfitting::sell_shield(p, klass)) sfx::ui_click(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sell the current shield generator and drop to L%d", p.shield_level - 1);
        ImGui::PopID();
    }
    ImGui::PopID();

    // Engine ladder.
    const int en_cap = klass ? (int)klass->max_engine_level : 0;
    ImGui::PushID("engine_ladder");
    ImGui::Text("Engine   L%d / %d", p.engine_level, en_cap);
    ImGui::SameLine();
    if (p.engine_level >= en_cap) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey); ImGui::TextUnformatted("MAX"); ImGui::PopStyleColor();
    } else {
        const int64_t price = engine_upgrade_price(p.engine_level + 1);
        char b[48]; std::snprintf(b, sizeof(b), "Upgrade -> L%d (%lld)", p.engine_level + 1, (long long)price);
        ImGui::PushID("buy");
        ImGui::BeginDisabled(price <= 0 || !player::can_afford(p, price));
        if (ImGui::SmallButton(b)) { if (outfitting::upgrade_engine(p, klass)) sfx::ui_click(); }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (p.engine_level > 0) {
        ImGui::PushID("sell");
        const int64_t refund = engine_upgrade_price(p.engine_level);
        char s[48]; std::snprintf(s, sizeof(s), "Sell L%d (-%lld)", p.engine_level, (long long)refund);
        if (ImGui::SmallButton(s)) { if (outfitting::sell_engine(p, klass)) sfx::ui_click(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sell the current engine upgrade and drop to L%d", p.engine_level - 1);
        ImGui::PopID();
    }
    ImGui::PopID();
    const SpeedCaps caps = effective_speed_caps(p);
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::Text("  effective top speed: %.0f / %.0f (cruise/AB)", caps.cruise0, caps.cruise1);
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Cargo expansion (one-time).
    ImGui::PushID("cargo_expansion");
    ImGui::TextUnformatted("Cargo Expansion");
    ImGui::SameLine();
    if (p.cargo_expansion) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen); ImGui::TextUnformatted("OWNED"); ImGui::PopStyleColor();
        ImGui::SameLine();
        const int64_t price = cargo_expansion_price();
        char s[40]; std::snprintf(s, sizeof(s), "Sell (-%lld)", (long long)price);
        if (ImGui::SmallButton(s)) { if (outfitting::sell_cargo_expansion(p)) sfx::ui_click(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sell the cargo expansion back to the dealer (full refund)");
    } else {
        const int64_t price = cargo_expansion_price();
        char b[40]; std::snprintf(b, sizeof(b), "Buy (%lld)", (long long)price);
        ImGui::BeginDisabled(price <= 0 || !player::can_afford(p, price));
        if (ImGui::SmallButton(b)) { if (outfitting::buy_cargo_expansion(p)) sfx::ui_click(); }
        ImGui::EndDisabled();
    }
    ImGui::PopID();

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

        // Missiles -- buy by type up to the rack capacity (np-zte.2).
        // Capacity scales with owned missile launchers: 1 launcher = 10
        // slots, 2 launchers = 20 slots. The new-game Tarsus starts with
        // 1 launcher so the cap is 10. Players can buy a second launcher
        // (10k) below to double this row.
        const int mcap = repair::missile_rack_capacity(p);
        const int mtot = repair::missiles_total(p);
        if (mcap <= 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            ImGui::Text("Missiles    -- (no missile launcher fitted)");
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("Missiles    DF %d / HS %d / IR %d   (%d/%d)",
                        p.missiles[0], p.missiles[1], p.missiles[2],
                        mtot, mcap);
        }
        const char* mlbl[3] = { "DF", "HS", "IR" };
        for (int t = 0; t < 3; ++t) {
            if (mcap <= 0) break;          // hide the buy buttons if no rack
            ImGui::SameLine();
            ImGui::PushID(100 + t);        // distinct ID space from torpedo row
            char b[40];
            std::snprintf(b, sizeof(b), "+%s (%lld)", mlbl[t],
                          (long long)repair::missile_price(t));
            const bool can = mtot < mcap &&
                             player::can_afford(p, repair::missile_price(t));
            ImGui::BeginDisabled(!can);
            if (ImGui::SmallButton(b)) { if (repair::buy_missiles(p, t, 1)) sfx::ui_click(); }
            ImGui::EndDisabled();
            ImGui::PopID();
            // Sell-back: one round at a time, full price refund. Refused
            // when none of this type loaded.
            ImGui::SameLine();
            ImGui::PushID(300 + t);
            char s[40];
            std::snprintf(s, sizeof(s), "-%s", mlbl[t]);
            const bool can_sell = p.missiles[t] > 0;
            ImGui::BeginDisabled(!can_sell);
            if (ImGui::SmallButton(s)) { if (repair::sell_missile(p, t)) sfx::ui_click(); }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sell 1 %s missile (+%lld)", mlbl[t],
                                  (long long)repair::missile_price(t));
            ImGui::PopID();
        }

        // Torpedoes -- single price (Proton Torpedo at 35 cr), no DF/HS/IR
        // split. Rack capacity scales with owned torpedo tubes (one per
        // tube, max 2 tubes). Capacity of 0 means the player has no tube
        // fitted, so the row reads "no torpedo tube fitted" in red.
        const int tcap = repair::torpedo_rack_capacity(p);
        const int ttot = repair::torpedoes_total(p);
        const int64_t tprice = repair::torpedo_price();
        if (tcap <= 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            ImGui::Text("Torpedoes   -- (no torpedo tube fitted)");
            ImGui::PopStyleColor();
        } else {
            ImGui::Text("Torpedoes   %d   (%d/%d)", ttot, ttot, tcap);
        }
        if (tcap > 0) {
            // Buy one (+/- pair). Same pattern as the DF/HS/IR missiles
            // above but only one button pair instead of three.
            ImGui::SameLine();
            ImGui::PushID(200);
            char b[40];
            std::snprintf(b, sizeof(b), "+1 (%lld)", (long long)tprice);
            const bool can = ttot < tcap && player::can_afford(p, tprice);
            ImGui::BeginDisabled(!can);
            if (ImGui::SmallButton(b)) { if (repair::buy_torpedo(p, 1)) sfx::ui_click(); }
            ImGui::EndDisabled();
            ImGui::PopID();

            ImGui::SameLine();
            ImGui::PushID(201);
            const bool can_sell = p.torpedoes > 0;
            ImGui::BeginDisabled(!can_sell);
            if (ImGui::SmallButton("-1")) { if (repair::sell_torpedo(p)) sfx::ui_click(); }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sell 1 torpedo (+%lld)", (long long)tprice);
            ImGui::PopID();
        }

        // ---- Discrete (one-per-ship) equipment (np-3dp.27) ------------------
        // Each row is a buy button (or OWNED marker) for a piece of equipment
        // you can install: Jump Drive, ECM L1..L3 (ladder), Repair Droid /
        // Advanced Repair Droid, Tractor Beam. Bought flags live on
        // PlayerState; the runtime effects are wired at jump.cpp, the missile
        // ECM check, the in-flight hull-repair tick, and the cargo tractor.
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("LAUNCHER HARDWARE");
        ImGui::PopStyleColor();
        {
            // Per-side launcher hardpoints (np-launchers-bump). The
            // hull has two physical hardpoints per ammo type (LEFT and
            // RIGHT), each of which can be bought independently for 10k
            // (missile) or 2.5k (torpedo). Sell returns 75% of buy.
            // Rack capacity is the sum of fitted hardpoints times the
            // per-launcher slot count.
        }
        {
            // Two-row, per-side, callable UI for the four hardpoints.
            ImGui::TextUnformatted("Missile Launcher");
            ImGui::SameLine();
            // LEFT hardpoint
            {
                ImGui::PushID("ml_L_buy");
                const bool can = repair::left_hardpoint_free(p) &&
                                 player::can_afford(p, repair::k_missile_launcher_price);
                char b[40]; std::snprintf(b, sizeof(b), "Buy L (%lld)",
                                          (long long)repair::k_missile_launcher_price);
                ImGui::BeginDisabled(!can);
                if (ImGui::SmallButton(b)) {
                    if (repair::buy_missile_launcher_left(p)) sfx::ui_click();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::PushID("ml_L_state");
                if (p.missile_launcher_left) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                    ImGui::TextUnformatted("L:ON"); ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
                    ImGui::TextUnformatted("L:--"); ImGui::PopStyleColor();
                }
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::PushID("ml_L_sell");
                char s[40]; std::snprintf(s, sizeof(s), "Sell L (+%lld)",
                                          (long long)repair::k_missile_launcher_sell_price);
                ImGui::BeginDisabled(!p.missile_launcher_left);
                if (ImGui::SmallButton(s)) {
                    if (repair::sell_missile_launcher_left(p)) sfx::ui_click();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            // RIGHT hardpoint
            {
                ImGui::SameLine();
                ImGui::PushID("ml_R_buy");
                const bool can = repair::right_hardpoint_free(p) &&
                                 player::can_afford(p, repair::k_missile_launcher_price);
                char b[40]; std::snprintf(b, sizeof(b), "Buy R (%lld)",
                                          (long long)repair::k_missile_launcher_price);
                ImGui::BeginDisabled(!can);
                if (ImGui::SmallButton(b)) {
                    if (repair::buy_missile_launcher_right(p)) sfx::ui_click();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::PushID("ml_R_state");
                if (p.missile_launcher_right) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                    ImGui::TextUnformatted("R:ON"); ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
                    ImGui::TextUnformatted("R:--"); ImGui::PopStyleColor();
                }
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::PushID("ml_R_sell");
                char s[40]; std::snprintf(s, sizeof(s), "Sell R (+%lld)",
                                          (long long)repair::k_missile_launcher_sell_price);
                ImGui::BeginDisabled(!p.missile_launcher_right);
                if (ImGui::SmallButton(s)) {
                    if (repair::sell_missile_launcher_right(p)) sfx::ui_click();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }

            // Torpedo launchers (2.5k buy, 1.875k sell). Both sides
            // start empty on the Tarsus.
            ImGui::TextUnformatted("Torpedo Launcher");
            ImGui::SameLine();
            // LEFT hardpoint
            {
                ImGui::PushID("tl_L_buy");
                const bool can = repair::left_hardpoint_free(p) &&
                                 player::can_afford(p, repair::k_torpedo_launcher_price);
                char b[40]; std::snprintf(b, sizeof(b), "Buy L (%lld)",
                                          (long long)repair::k_torpedo_launcher_price);
                ImGui::BeginDisabled(!can);
                if (ImGui::SmallButton(b)) {
                    if (repair::buy_torpedo_launcher_left(p)) sfx::ui_click();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::PushID("tl_L_state");
                if (p.torpedo_launcher_left) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                    ImGui::TextUnformatted("L:ON"); ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
                    ImGui::TextUnformatted("L:--"); ImGui::PopStyleColor();
                }
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::PushID("tl_L_sell");
                char s[40]; std::snprintf(s, sizeof(s), "Sell L (+%lld)",
                                          (long long)repair::k_torpedo_launcher_sell_price);
                ImGui::BeginDisabled(!p.torpedo_launcher_left);
                if (ImGui::SmallButton(s)) {
                    if (repair::sell_torpedo_launcher_left(p)) sfx::ui_click();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            // RIGHT hardpoint
            {
                ImGui::SameLine();
                ImGui::PushID("tl_R_buy");
                const bool can = repair::right_hardpoint_free(p) &&
                                 player::can_afford(p, repair::k_torpedo_launcher_price);
                char b[40]; std::snprintf(b, sizeof(b), "Buy R (%lld)",
                                          (long long)repair::k_torpedo_launcher_price);
                ImGui::BeginDisabled(!can);
                if (ImGui::SmallButton(b)) {
                    if (repair::buy_torpedo_launcher_right(p)) sfx::ui_click();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::PushID("tl_R_state");
                if (p.torpedo_launcher_right) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                    ImGui::TextUnformatted("R:ON"); ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, kRed);
                    ImGui::TextUnformatted("R:--"); ImGui::PopStyleColor();
                }
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::PushID("tl_R_sell");
                char s[40]; std::snprintf(s, sizeof(s), "Sell R (+%lld)",
                                          (long long)repair::k_torpedo_launcher_sell_price);
                ImGui::BeginDisabled(!p.torpedo_launcher_right);
                if (ImGui::SmallButton(s)) {
                    if (repair::sell_torpedo_launcher_right(p)) sfx::ui_click();
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
            ImGui::TextUnformatted("SPECIAL EQUIPMENT");
            ImGui::PopStyleColor();
        }
        {
            struct Row { const char* item; const char* label; const char* hint; };
            // (#82) tractor_beam dropped from the purchasable list — it's
            // universal on every ship now (player.cpp new_game + #83). The
            // PlayerState flag is still alive (savegame schema) for any
            // future design that wants to gate pulling behind a heavier
            // cargo-tractor upgrade.
            const std::array<Row, 6> rows = {{
                { "jump_drive",       "Jump Drive",        "Allows using jump points" },
                { "ecm_l1",           "ECM Level 1",       "25% chance/s to break missile lock" },
                { "ecm_l2",           "ECM Level 2",       "50% chance/s (needs L1)" },
                { "ecm_l3",           "ECM Level 3",       "75% chance/s (needs L2)" },
                { "repair_droid",     "Repair Droid",      "Repairs hull while flying" },
                { "adv_repair_droid", "Adv Repair Droid",  "2x faster (needs Repair Droid)" },
            }};
            for (const Row& r : rows) {
                ImGui::PushID(r.item);
                const int64_t price = outfitting::discrete_price(r.item);
                bool owned = false;
                if      (std::string(r.item) == "jump_drive")    owned = p.has_jump_drive;
                else if (std::string(r.item) == "ecm_l1")        owned = p.ecm_level >= 1;
                else if (std::string(r.item) == "ecm_l2")        owned = p.ecm_level >= 2;
                else if (std::string(r.item) == "ecm_l3")        owned = p.ecm_level >= 3;
                // repair_droid is OWNED only if the stock was bought and we
                // haven't upgraded to adv (which uses the same flag).
                else if (std::string(r.item) == "repair_droid")  owned = p.has_repair_droid && !p.adv_repair_droid;
                else if (std::string(r.item) == "adv_repair_droid") owned = p.adv_repair_droid;
                ImGui::TextUnformatted(r.label);
                ImGui::SameLine();
                if (owned) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                    ImGui::TextUnformatted("OWNED");
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.hint);
                } else {
                    char b[48]; std::snprintf(b, sizeof(b), "Buy (%lld)", (long long)price);
                    const bool can = price > 0 && player::can_afford(p, price);
                    const bool adv_ok = (std::string(r.item) != "adv_repair_droid") || p.has_repair_droid;
                    ImGui::BeginDisabled(!can || !adv_ok);
                    if (ImGui::SmallButton(b)) { if (outfitting::buy_discrete(p, r.item)) sfx::ui_click(); }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.hint);
                }
                ImGui::PopID();
            }
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
