// -----------------------------------------------------------------------------
// inventory.cpp — loot pricing + the CargoHold base screen (Phase 4f, #95/96/97).
//
// The sell side of the unified hold. Like outfitting.cpp / economy.cpp this
// file is two halves:
//
//   1. The pricing + transaction MODEL — load loot_prices.json, value an
//      InventoryItem, and sell one through player::add_credits ONLY (one
//      enforcement path; the dev_remote /inventory/sell endpoint calls the
//      SAME sell_item the screen does). Pure data, no ImGui.
//   2. The CargoHold screen body — ImGui, registered with base_screens via
//      the np-9cu.4 hook seam. Mirrors outfitting.cpp's style + palette.
//
// Pricing is data-driven: base value per item id + a rarity multiplier ladder
// live in assets/data/loot_prices.json. Missing entries fall back to sane
// defaults so an incomplete table never crashes a sale.
// -----------------------------------------------------------------------------

#include "inventory.h"

#include "json.h"
#include "player.h"

// The model is pure data; the screen body drags in the UI/audio stack.
// INVENTORY_HEADLESS compiles only the model (mirrors OUTFITTING_HEADLESS).
#ifndef INVENTORY_HEADLESS
#include "base_screens.h"
#include "economy.h"
#include "gun.h"
#include "ship_class.h"
#include "sfx.h"
#include "imgui.h"
#include "sokol_app.h"
#endif

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>

namespace inventory {

namespace {

// ---- pricing model (loaded from loot_prices.json) ---------------------------
// Defaults match the header contract: rare 2.5x, legendary 6x, unknown id 50.
std::unordered_map<std::string, int64_t> g_base_value;
float                                    g_mult_basic     = 1.0f;
float                                    g_mult_rare      = 2.5f;
float                                    g_mult_legendary = 6.0f;

constexpr int64_t k_default_base_value = 50;

float rarity_mult(Rarity r) {
    switch (r) {
        case Rarity::Rare:      return g_mult_rare;
        case Rarity::Legendary: return g_mult_legendary;
        case Rarity::Basic:
        default:                return g_mult_basic;
    }
}

int64_t base_value_of(const std::string& id) {
    const auto it = g_base_value.find(id);
    return (it == g_base_value.end()) ? k_default_base_value : it->second;
}

// ---- upgrade -> PermanentMod registry (#99) ---------------------------------
// Maps an Upgrade-kind item id to the permanent effect it installs into.
// Hand-authored: shipping upgrade ids get a tuned effect here; anything
// not listed falls back to a conservative shield_pct +5% (install_upgrade's
// default) so a new/loot-only upgrade still does SOMETHING without a code
// change. Keep effect strings in sync with apply_player_loadout (main.cpp).
const std::unordered_map<std::string, PermanentMod>& upgrade_mods() {
    static const std::unordered_map<std::string, PermanentMod> m = {
        { "shield_matrix", { "shield_matrix", "shield_pct", 0.05f } },
    };
    return m;
}

// Resolve the PermanentMod an upgrade item installs. Known id -> its tuned
// entry; unknown id -> default shield_pct +0.05 carrying the item's own id.
PermanentMod resolve_mod(const std::string& id) {
    const auto& m = upgrade_mods();
    const auto it = m.find(id);
    if (it != m.end()) return it->second;
    return PermanentMod{ id, "shield_pct", 0.05f };
}

// Human-readable rarity label. Lives in the model (always-compiled) half
// so both the EQUIP log line and the headless build can name a rarity
// without dragging in the UI block; the CargoHold screen reuses it too.
const char* rarity_name(Rarity r) {
    switch (r) {
        case Rarity::Rare:      return "Rare";
        case Rarity::Legendary: return "Legendary";
        case Rarity::Basic:
        default:                return "Basic";
    }
}

} // namespace

int load_prices(const std::string& path) {
    g_base_value.clear();
    g_mult_basic     = 1.0f;
    g_mult_rare      = 2.5f;
    g_mult_legendary = 6.0f;

    const json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr,
            "[inventory] cannot load '%s' — using default loot prices "
            "(rare 2.5x, legendary 6x, base %lld)\n",
            path.c_str(), (long long)k_default_base_value);
        return 0;
    }

    if (const json::Value* m = root.find("rarity_multiplier"); m && m->is_object()) {
        if (const json::Value* v = m->find("basic"))     g_mult_basic     = v->as_float();
        if (const json::Value* v = m->find("rare"))      g_mult_rare      = v->as_float();
        if (const json::Value* v = m->find("legendary")) g_mult_legendary = v->as_float();
    }

    if (const json::Value* b = root.find("base_value"); b && b->is_object()) {
        for (const auto& [id, v] : b->as_object())
            g_base_value[id] = (int64_t)v.as_int();
    }

    std::printf("[inventory] %zu loot prices (rare %.1fx, legendary %.1fx, base %lld)\n",
                g_base_value.size(), g_mult_rare, g_mult_legendary,
                (long long)k_default_base_value);
    return (int)g_base_value.size();
}

// ---- pricing queries + transaction (headless-safe; shared by UI + dev) ------

int64_t item_value(const InventoryItem& it) {
    const int qty = it.qty > 0 ? it.qty : 0;
    const double v = (double)base_value_of(it.id) * (double)rarity_mult(it.rarity)
                   * (double)qty;
    return (int64_t)std::floor(v);
}

bool sell_item(PlayerState& p, int index) {
    if (index < 0 || index >= (int)p.items.size()) {
        std::printf("[inventory] SELL refused: index %d out of range (have %zu)\n",
                    index, p.items.size());
        return false;
    }
    const InventoryItem& it = p.items[(size_t)index];
    const int64_t value = item_value(it);
    player::add_credits(p, value);
    std::printf("[inventory] SELL '%s' x%d @ %lld | credits %lld\n",
                it.id.c_str(), it.qty, (long long)value, (long long)p.credits);
    p.items.erase(p.items.begin() + index);
    return true;
}

int64_t default_cargo_unit_value() { return k_default_base_value; }

bool sell_cargo_unit(PlayerState& p, int cargo_index, int64_t unit_price) {
    if (cargo_index < 0 || cargo_index >= (int)p.cargo.size()) {
        std::printf("[inventory] SELL-UNIT refused: index %d out of range (have %zu)\n",
                    cargo_index, p.cargo.size());
        return false;
    }
    CargoEntry& e = p.cargo[(size_t)cargo_index];
    if (e.units <= 0) return false;
    if (unit_price < 0) unit_price = 0;
    e.units -= 1;
    player::add_credits(p, unit_price);
    std::printf("[inventory] SELL-UNIT '%s' 1u @ %lld | %d left, credits %lld\n",
                e.commodity_id.c_str(), (long long)unit_price, e.units,
                (long long)p.credits);
    if (e.units <= 0) p.cargo.erase(p.cargo.begin() + cargo_index);
    return true;
}

bool install_upgrade(PlayerState& p, int index) {
    if (index < 0 || index >= (int)p.items.size()) {
        std::printf("[inventory] INSTALL refused: index %d out of range (have %zu)\n",
                    index, p.items.size());
        return false;
    }
    const InventoryItem& it = p.items[(size_t)index];
    if (it.kind != ItemKind::Upgrade) {
        std::printf("[inventory] INSTALL refused: '%s' is not an Upgrade\n",
                    it.id.c_str());
        return false;
    }
    const PermanentMod mod = resolve_mod(it.id);
    // No stacking: a second mod with an id already present is refused so the
    // same buff can't be doubled up (mirrors PermanentMod's contract).
    for (const PermanentMod& existing : p.permanent_mods) {
        if (existing.id == mod.id) {
            std::printf("[inventory] INSTALL refused: '%s' already installed\n",
                        mod.id.c_str());
            return false;
        }
    }
    p.permanent_mods.push_back(mod);
    p.items.erase(p.items.begin() + index);
    std::printf("[inventory] INSTALL '%s'\n", mod.id.c_str());
    return true;
}

bool equip_weapon(PlayerState& p, int item_index, int mount_index) {
    if (item_index < 0 || item_index >= (int)p.items.size()) {
        std::printf("[inventory] EQUIP refused: item index %d out of range (have %zu)\n",
                    item_index, p.items.size());
        return false;
    }
    if (mount_index < 0) {
        std::printf("[inventory] EQUIP refused: bad mount index %d\n", mount_index);
        return false;
    }
    // Copy by value first: the erase below invalidates the reference, and we
    // still want the id/rarity for the new MountSlot + the log line.
    const InventoryItem it = p.items[(size_t)item_index];
    if (it.kind != ItemKind::Weapon) {
        std::printf("[inventory] EQUIP refused: '%s' is not a Weapon\n", it.id.c_str());
        return false;
    }
    // Grow gun_mounts with empty slots so the requested mount exists; the
    // MountSlot default ctor leaves "" = empty for any gap we just opened.
    if ((int)p.gun_mounts.size() <= mount_index)
        p.gun_mounts.resize((size_t)mount_index + 1);
    // The MountSlot(id, rarity) ctor derives the WeaponMods from rarity, so
    // the rarity tuning rides onto the hull at the next apply_player_loadout.
    p.gun_mounts[(size_t)mount_index] = MountSlot{ it.id, it.rarity };
    p.items.erase(p.items.begin() + item_index);
    std::printf("[inventory] EQUIP '%s' (%s) -> mount %d\n",
                it.id.c_str(), rarity_name(it.rarity), mount_index);
    return true;
}

#ifndef INVENTORY_HEADLESS

namespace {

// HUD palette echoing outfitting.cpp / base_screens.cpp.
constexpr ImU32 kAmber = IM_COL32(255, 217,  77, 255);
constexpr ImU32 kGreen = IM_COL32(120, 230, 120, 255);
constexpr ImU32 kGrey  = IM_COL32(150, 158, 168, 255);

struct ScreenWH { float w, h; };
ScreenWH screen_wh() {
    const float dpi = sapp_dpi_scale();
    return { (float)sapp_width() / dpi, (float)sapp_height() / dpi };
}

const char* kind_name(ItemKind k) {
    switch (k) {
        case ItemKind::Commodity: return "Commodity";
        case ItemKind::Salvage:   return "Salvage";
        case ItemKind::Weapon:    return "Weapon";
        case ItemKind::Upgrade:   return "Upgrade";
        default:                  return "?";
    }
}

ImU32 rarity_color(Rarity r) {
    switch (r) {
        case Rarity::Rare:      return IM_COL32(120, 200, 255, 255);  // cyan
        case Rarity::Legendary: return IM_COL32(255, 200,  80, 255);  // gold
        case Rarity::Basic:
        default:                return kGrey;
    }
}

// Resolve a loot weapon id to a GunType so we can show its base specs.
// Loot ids carry flavour suffixes the gun table doesn't ("laser_cannon"
// vs the catalog's "laser"), so we try the id verbatim, then strip a
// trailing "_cannon" / "_gun". Returns GunType::Count when nothing matches
// (the item is a flavour weapon with no canonical stat row).
GunType resolve_gun(const std::string& id) {
    GunType t = gun::from_name(id);
    if (t != GunType::Count) return t;
    for (const char* suf : { "_cannon", "_gun" }) {
        const std::string s(suf);
        if (id.size() > s.size() &&
            id.compare(id.size() - s.size(), s.size(), s) == 0) {
            t = gun::from_name(id.substr(0, id.size() - s.size()));
            if (t != GunType::Count) return t;
        }
    }
    return GunType::Count;
}

// Hover tooltip for a Weapon-kind item: base specs (when the id maps to a
// catalog gun) + the rarity bonus deltas vs Basic (#112 follow-up). Makes
// "is this loot worth fitting?" answerable without docking.
void weapon_tooltip(const InventoryItem& it) {
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushStyleColor(ImGuiCol_Text, rarity_color(it.rarity));
    ImGui::Text("%s  \xe2\x80\x94  %s", it.id.c_str(), rarity_name(it.rarity));
    ImGui::PopStyleColor();
    ImGui::Separator();

    const GunType gt = resolve_gun(it.id);
    if (gt != GunType::Count) {
        const GunStats& g = g_gun_stats[(int)gt];
        ImGui::Text("Damage      %.0f cm", g.damage_cm);
        ImGui::Text("Proj. speed %.0f m/s", g.speed_mps);
        ImGui::Text("Range       %.0f m", g.range_m);
        // Refire + energy reflect the rarity mods the item actually carries.
        const float refire = g.refire_delay_s * (it.mods.fire_rate_mult > 0.0f
                                                 ? 1.0f / it.mods.fire_rate_mult : 1.0f);
        ImGui::Text("Refire      %.2f s", refire);
        ImGui::Text("Energy/shot %.0f GJ", g.energy_cost_gj * it.mods.energy_mult);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
        ImGui::TextUnformatted("(no catalog stats for this weapon)");
        ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (it.rarity == Rarity::Basic) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
        ImGui::TextUnformatted("Basic quality \xe2\x80\x94 no bonuses.");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::TextUnformatted("Bonuses vs Basic:");
        const float rof = (it.mods.fire_rate_mult - 1.0f) * 100.0f;
        const float en  = (it.mods.energy_mult   - 1.0f) * 100.0f;
        if (rof != 0.0f) ImGui::Text("  Fire rate   %+.0f%%  (faster)", rof);
        if (en  != 0.0f) ImGui::Text("  Energy/shot %+.0f%%  (cheaper)", en);
        ImGui::PopStyleColor();
    }
    ImGui::EndTooltip();
}

constexpr ImGuiTableFlags kHoldTableFlags =
    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

// Shared bulk-commodity table (#112): used by BOTH the LANDED CargoHold and
// the in-flight panel. When `allow_sell`, each row gets a "Sell 1" button
// priced by `unit_price(e)` (market price at a base; flat default in flight).
// Mutations route through sell_cargo_unit() — one enforcement path.
void draw_commodities_table(PlayerState& p, ImVec2 sz, bool allow_sell,
                            const std::function<int64_t(const CargoEntry&)>& unit_price) {
    const int cols = allow_sell ? 3 : 2;
    if (ImGui::BeginChild("##comm", sz, false) &&
        ImGui::BeginTable("commodities", cols, kHoldTableFlags, sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Commodity", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Units");
        if (allow_sell) ImGui::TableSetupColumn("Sell");
        ImGui::PushID("comm_header");
        ImGui::TableHeadersRow();
        ImGui::PopID();
        if (p.cargo.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
            ImGui::TextUnformatted("(no bulk commodities)");
            ImGui::PopStyleColor();
            for (int c = 1; c < cols; ++c) ImGui::TableNextColumn();
        } else {
            int sell_index = -1;
            for (int i = 0; i < (int)p.cargo.size(); ++i) {
                const CargoEntry& e = p.cargo[(size_t)i];
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(e.commodity_id.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", e.units);
                if (allow_sell) {
                    ImGui::TableNextColumn();
                    char b[48];
                    std::snprintf(b, sizeof(b), "Sell 1 (+%lld)",
                                  (long long)unit_price(e));
                    if (ImGui::SmallButton(b)) sell_index = i;
                }
                ImGui::PopID();
            }
            if (sell_index >= 0) {
                const int64_t up = unit_price(p.cargo[(size_t)sell_index]);
                if (sell_cargo_unit(p, sell_index, up)) sfx::ui_click();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

// Shared loot/items table (#112): Sell (Salvage/Commodity), Fit (Weapon),
// Install (Upgrade). Used by both the CargoHold and the in-flight panel.
void draw_items_table(PlayerState& p, ImVec2 sz, bool allow_sell) {
    if (ImGui::BeginChild("##items", sz, false) &&
        ImGui::BeginTable("items", 6, kHoldTableFlags, sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Rarity");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Qty");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Sell");
        ImGui::PushID("items_header");
        ImGui::TableHeadersRow();
        ImGui::PopID();

        if (p.items.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
            ImGui::TextUnformatted("(hold is empty — go shoot something)");
            ImGui::PopStyleColor();
            for (int c = 0; c < 5; ++c) ImGui::TableNextColumn();
        } else {
            // Iterate by index. A successful sell/install erases p.items[i], so
            // capture the index to act on and apply it AFTER the loop that frame;
            // ImGui redraws next frame against the shrunken vector.
            int sell_index    = -1;
            int install_index = -1;
            int equip_index   = -1;
            for (int i = 0; i < (int)p.items.size(); ++i) {
                const InventoryItem& it = p.items[(size_t)i];
                const int64_t value = item_value(it);
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                ImGui::TextUnformatted(it.id.c_str());
                ImGui::PopStyleColor();
                // Weapon loot: hover the name for base specs + rarity deltas.
                if (it.kind == ItemKind::Weapon) weapon_tooltip(it);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, rarity_color(it.rarity));
                ImGui::TextUnformatted(rarity_name(it.rarity));
                ImGui::PopStyleColor();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(kind_name(it.kind));
                ImGui::TableNextColumn(); ImGui::Text("%d", it.qty);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::Text("%lld", (long long)value);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                // Upgrade-kind items install into permanent_mods (#99);
                // Weapon-kind items get a Fit button that equips them into
                // a gun mount (#98); every other kind keeps Sell-for-credits.
                // Weapon -> Fit, Upgrade -> Install (both also work in flight).
                // Selling is a BASE-ONLY transaction (no buyer mid-flight), so
                // a Sell button shows for EVERY kind when allow_sell, and the
                // dim "dock to sell" hint shows in flight for non-equippables.
                if (it.kind == ItemKind::Upgrade) {
                    if (ImGui::SmallButton("Install")) install_index = i;
                } else if (it.kind == ItemKind::Weapon) {
                    if (ImGui::SmallButton("Fit")) equip_index = i;
                }
                if (allow_sell) {
                    if (it.kind == ItemKind::Weapon || it.kind == ItemKind::Upgrade)
                        ImGui::SameLine();
                    char b[48];
                    std::snprintf(b, sizeof(b), "Sell (+%lld)", (long long)value);
                    if (ImGui::SmallButton(b)) sell_index = i;
                } else if (it.kind != ItemKind::Weapon && it.kind != ItemKind::Upgrade) {
                    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
                    ImGui::TextUnformatted("dock to sell");
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }
            if (install_index >= 0) {
                if (install_upgrade(p, install_index)) sfx::ui_click();
            } else if (equip_index >= 0) {
                // Fit into the FIRST empty mount (gun_id == ""); if every
                // mount is occupied, overwrite mount 0.
                int mount = -1;
                for (int mi = 0; mi < (int)p.gun_mounts.size(); ++mi) {
                    if (p.gun_mounts[(size_t)mi].gun_id.empty()) { mount = mi; break; }
                }
                if (mount < 0) mount = 0;
                if (equip_weapon(p, equip_index, mount)) sfx::ui_click();
            } else if (sell_index >= 0) {
                if (sell_item(p, sell_index)) sfx::ui_click();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace

// ---- CargoHold screen (LANDED) ----------------------------------------------

void cargohold_screen(BaseContext& ctx) {
    PlayerState& p = *ctx.player;
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    const ScreenWH ss = screen_wh();
    const int used = player::cargo_units_used(p);
    const int cap  = player::cargo_capacity(p, klass);
    const std::string base_id = ctx.base_id;

    // ---- Header: cargo usage + credits -------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(28, 60));
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::Text("CARGO HOLD   %d / %d units    CREDITS %lld",
                used, cap, (long long)p.credits);
    ImGui::PopStyleColor();

    // ---- Commodities (sellable at this base's market price, #112) -----------
    ImGui::SetCursorScreenPos(ImVec2(28, 92));
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::TextUnformatted("COMMODITIES (sell here at market price):");
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(28, 112));
    const ImVec2 comm_sz(ss.w - 56, ss.h * 0.30f);
    draw_commodities_table(p, comm_sz, /*allow_sell=*/true,
        [&](const CargoEntry& e) -> int64_t {
            // Landed: real exchange sell price; fall back to the flat default
            // if this base doesn't trade that commodity.
            const economy::Quote q = economy::price(base_id, e.commodity_id);
            return (q.valid && q.sell_price > 0) ? (int64_t)q.sell_price
                                                 : default_cargo_unit_value();
        });

    // ---- Items (sellable loot) ---------------------------------------------
    const float items_y = 112 + comm_sz.y + 28;
    ImGui::SetCursorScreenPos(ImVec2(28, items_y - 20));
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::TextUnformatted("LOOT & SALVAGE (sell for credits):");
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(28, items_y));
    const ImVec2 item_sz(ss.w - 56, ss.h - items_y - 70);
    draw_items_table(p, item_sz, /*allow_sell=*/true);   // landed: sell here
}

// ---- In-flight inventory panel (FLIGHT, #112 part 1) ------------------------

void in_flight_panel(PlayerState& p, bool* p_open) {
    if (!p_open || !*p_open) return;
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    const int used = player::cargo_units_used(p);
    const int cap  = player::cargo_capacity(p, klass);

    ImGui::SetNextWindowSize(ImVec2(460, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("INVENTORY  (I)", p_open)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::Text("CARGO  %d / %d units     CREDITS %lld",
                    used, cap, (long long)p.credits);
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
        ImGui::TextUnformatted("BULK COMMODITIES (read-only \xe2\x80\x94 sell at a base):");
        ImGui::PopStyleColor();
        const float rest = ImGui::GetContentRegionAvail().y;
        // In flight the hold is VIEW-only for cargo: no buyer out here. Selling
        // happens at the LANDED CargoHold / Commodity Exchange.
        draw_commodities_table(p, ImVec2(0, rest * 0.40f), /*allow_sell=*/false,
            [](const CargoEntry&) -> int64_t { return 0; });

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
        ImGui::TextUnformatted("LOOT & SALVAGE  (hover a weapon for specs):");
        ImGui::PopStyleColor();
        // allow_sell=false: Fit/Install still work in flight, but Sell is
        // base-only (shows "dock to sell").
        draw_items_table(p, ImVec2(0, 0), /*allow_sell=*/false);
    }
    ImGui::End();
}

void register_screens() {
    base_screens::register_screen(BaseScreen::CargoHold, cargohold_screen);
}

#else // INVENTORY_HEADLESS

void register_screens() {}

#endif // INVENTORY_HEADLESS

} // namespace inventory
