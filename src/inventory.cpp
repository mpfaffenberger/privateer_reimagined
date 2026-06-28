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
#include "ship_class.h"
#include "sfx.h"
#include "imgui.h"
#include "sokol_app.h"
#endif

#include <cmath>
#include <cstdio>
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

const char* rarity_name(Rarity r) {
    switch (r) {
        case Rarity::Rare:      return "Rare";
        case Rarity::Legendary: return "Legendary";
        case Rarity::Basic:
        default:                return "Basic";
    }
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

} // namespace

// ---- CargoHold screen -------------------------------------------------------

void cargohold_screen(BaseContext& ctx) {
    PlayerState& p = *ctx.player;
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    const ScreenWH ss = screen_wh();
    const int used = player::cargo_units_used(p);
    const int cap  = player::cargo_capacity(p, klass);

    // ---- Header: cargo usage + credits -------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(28, 60));
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::Text("CARGO HOLD   %d / %d units    CREDITS %lld",
                used, cap, (long long)p.credits);
    ImGui::PopStyleColor();

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    // ---- Commodities (read-only manifest) ----------------------------------
    ImGui::SetCursorScreenPos(ImVec2(28, 92));
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::TextUnformatted("COMMODITIES (sell at the Commodity Exchange):");
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(28, 112));
    const ImVec2 comm_sz(ss.w - 56, ss.h * 0.30f);
    if (ImGui::BeginChild("##comm", comm_sz, false) &&
        ImGui::BeginTable("commodities", 2, tflags, comm_sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Commodity", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Units");
        ImGui::PushID("comm_header");
        ImGui::TableHeadersRow();
        ImGui::PopID();
        if (p.cargo.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
            ImGui::TextUnformatted("(no bulk commodities)");
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
        } else {
            for (const CargoEntry& e : p.cargo) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(e.commodity_id.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", e.units);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ---- Items (sellable loot) ---------------------------------------------
    const float items_y = 112 + comm_sz.y + 28;
    ImGui::SetCursorScreenPos(ImVec2(28, items_y - 20));
    ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
    ImGui::TextUnformatted("LOOT & SALVAGE (sell for credits):");
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(28, items_y));
    const ImVec2 item_sz(ss.w - 56, ss.h - items_y - 70);
    if (ImGui::BeginChild("##items", item_sz, false) &&
        ImGui::BeginTable("items", 6, tflags, item_sz)) {
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
            for (int i = 0; i < (int)p.items.size(); ++i) {
                const InventoryItem& it = p.items[(size_t)i];
                const int64_t value = item_value(it);
                ImGui::TableNextRow();
                ImGui::PushID(i);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
                ImGui::TextUnformatted(it.id.c_str());
                ImGui::PopStyleColor();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(rarity_name(it.rarity));
                ImGui::TableNextColumn(); ImGui::TextUnformatted(kind_name(it.kind));
                ImGui::TableNextColumn(); ImGui::Text("%d", it.qty);
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
                ImGui::Text("%lld", (long long)value);
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                // Upgrade-kind items install into permanent_mods (#99); every
                // other kind keeps the Sell-for-credits path.
                if (it.kind == ItemKind::Upgrade) {
                    if (ImGui::SmallButton("Install")) install_index = i;
                } else {
                    char b[48];
                    std::snprintf(b, sizeof(b), "Sell (+%lld)", (long long)value);
                    if (ImGui::SmallButton(b)) sell_index = i;
                }
                ImGui::PopID();
            }
            if (install_index >= 0) {
                if (install_upgrade(p, install_index)) sfx::ui_click();
            } else if (sell_index >= 0) {
                if (sell_item(p, sell_index)) sfx::ui_click();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void register_screens() {
    base_screens::register_screen(BaseScreen::CargoHold, cargohold_screen);
}

#else // INVENTORY_HEADLESS

void register_screens() {}

#endif // INVENTORY_HEADLESS

} // namespace inventory
