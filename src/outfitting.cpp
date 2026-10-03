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
#include "equipment_ui_internal.h"
#endif

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace outfitting {

namespace {

// ---- pricing model (loaded from ship_prices.json + equipment_prices.json) ---

std::vector<HullOffer>                       g_hulls;        // authored order
float                                        g_trade_in_pct = 0.55f;
std::unordered_map<std::string, int64_t>     g_gun_price;    // short_name -> price
std::unordered_map<std::string, int64_t>     g_armor_price;  // ArmorType::name -> price
std::vector<int64_t>                         g_shield_price; // index = level
std::vector<int64_t>                         g_engine_price; // index = level
float                                        g_engine_regen_mult = 0.08f;   // legacy mult (unused)
std::vector<float>                           g_engine_regen_bonus;   // GJ/s by engine level
std::vector<float>                           g_shield_regen_drain;   // GJ/s by shield level

int64_t                                      g_cargo_expansion_price = 0;
int64_t                                      g_turret_price = 0;          // #145
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
    g_turret_price = 0;
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
        if (ep.contains("turret_price"))
            g_turret_price = (int64_t)ep["turret_price"].as_int();
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

const std::vector<HullOffer>& hull_catalog() { return g_hulls; }

int64_t hull_price(const std::string& hull_id) {
    for (const HullOffer& h : g_hulls) if (h.id == hull_id) return h.price;
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

int64_t turret_price() { return g_turret_price; }

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
    // NOTE: permanent_mods are deliberately NOT cleared — installed upgrades
    // live on the player and persist across hull swaps (#92/#94).
    fit_stock_guns(p, ship_class::find(target), /*with_turrets=*/false);

    std::printf("[outfit] BUY HULL %s -> %s | net %lld | credits %lld | %zu default mounts\n",
                old.c_str(), target.c_str(), (long long)net, (long long)p.credits,
                p.gun_mounts.size());
    return true;
}

void fit_stock_guns(PlayerState& p, const ShipClass* klass, bool with_turrets) {
    p.gun_mounts.clear();
    p.turrets.clear();
    if (!klass) return;
    if (with_turrets)
        for (const TurretSlot& t : klass->turret_slots) p.turrets.push_back(t.id);
    for (const GunMount& m : klass->default_guns) {
        const bool stocked = !m.is_turret || with_turrets;
        p.gun_mounts.push_back(stocked ? MountSlot{gun::to_name(m.type)} : MountSlot{});
    }
}

bool buy_gun(PlayerState& p, const std::string& gun_short_name,
             int mount_index, const ShipClass* klass) {
    const int mounts = klass ? (int)klass->default_guns.size() : (int)p.gun_mounts.size();
    if (!player::mount_fittable(p, klass, mount_index) || mount_index >= mounts) {
        const TurretSlot* t = klass ? klass->turret_slot_for_mount(mount_index) : nullptr;
        if (t) std::printf("[outfit] BUY GUN refused: mount %d needs the %s (not installed)\n",
                           mount_index, t->label.c_str());
        else   std::printf("[outfit] BUY GUN refused: mount %d out of range (hull has %d)\n",
                           mount_index, mounts);
        return false;
    }
    // Never overwrite a fitted gun: its value would vanish unpaid (#741).
    if (player::mount_armed(p, mount_index)) {
        std::printf("[outfit] BUY GUN refused: mount %d already holds %s (sell it first)\n",
                    mount_index, p.gun_mounts[(size_t)mount_index].gun_id.c_str());
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
    // A COPY: the slot is cleared below, and the log line still needs the name.
    const std::string name = p.gun_mounts[(size_t)mount_index].gun_id;
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

bool buy_turret(PlayerState& p, const std::string& slot_id, const ShipClass* klass) {
    const TurretSlot* slot = klass ? klass->find_turret_slot(slot_id) : nullptr;
    if (!slot) {
        std::printf("[outfit] TURRET refused: hull has no '%s' turret position\n", slot_id.c_str());
        return false;
    }
    if (player::has_turret(p, slot_id)) {
        std::printf("[outfit] TURRET refused: %s already installed\n", slot->label.c_str());
        return false;
    }
    const int64_t price = turret_price();
    if (price <= 0) { std::printf("[outfit] TURRET refused: no price\n"); return false; }
    if (!player::can_afford(p, price)) {
        std::printf("[outfit] TURRET refused: %s costs %lld, have %lld\n",
                    slot->label.c_str(), (long long)price, (long long)p.credits);
        return false;
    }
    player::spend_credits(p, price);
    p.turrets.push_back(slot_id);
    std::printf("[outfit] TURRET %s installed @ %lld | credits %lld\n",
                slot->label.c_str(), (long long)price, (long long)p.credits);
    return true;
}

bool sell_turret(PlayerState& p, const std::string& slot_id, const ShipClass* klass) {
    const TurretSlot* slot = klass ? klass->find_turret_slot(slot_id) : nullptr;
    if (!slot || !player::has_turret(p, slot_id)) {
        std::printf("[outfit] TURRET sell refused: '%s' not installed\n", slot_id.c_str());
        return false;
    }
    for (int m : slot->mounts) {
        if (player::mount_armed(p, m)) {
            std::printf("[outfit] TURRET sell refused: %s still carries a gun (mount %d)\n",
                        slot->label.c_str(), m);
            return false;
        }
    }
    const int64_t refund = turret_price();
    if (refund <= 0) { std::printf("[outfit] TURRET sell refused: no price\n"); return false; }
    p.turrets.erase(std::find(p.turrets.begin(), p.turrets.end(), slot_id));
    player::add_credits(p, refund);
    std::printf("[outfit] TURRET %s sold, refund %lld | credits %lld\n",
                slot->label.c_str(), (long long)refund, (long long)p.credits);
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

// ---- Ship Dealer + Equipment screen registration ---------------------------
#ifndef OUTFITTING_HEADLESS

void register_screens() {
    base_screens::register_screen(BaseScreen::ShipDealer, equipment_ui::draw_dealer_screen);
    base_screens::register_screen(BaseScreen::Equipment, equipment_ui::draw_equipment_screen);
}

#endif

} // namespace outfitting
