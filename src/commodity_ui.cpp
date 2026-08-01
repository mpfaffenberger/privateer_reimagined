#include "commodity_ui.h"

#include "base_screens.h"
#include "commodity.h"
#include "economy.h"
#include "player.h"
#include "sfx.h"
#include "ship_class.h"

#include "imgui.h"
#include "sokol_app.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace commodity_ui {
namespace {

constexpr ImVec4 kAmber(1.00f, 0.74f, 0.24f, 1.0f);
constexpr ImVec4 kGreen(0.42f, 0.90f, 0.55f, 1.0f);
constexpr ImVec4 kRed(1.00f, 0.38f, 0.30f, 1.0f);
constexpr ImVec4 kDim(0.55f, 0.58f, 0.65f, 1.0f);
constexpr ImVec4 kWhite(0.92f, 0.94f, 0.97f, 1.0f);

enum class Mode { Buy, Sell };
Mode g_mode = Mode::Buy;
std::string g_selected;
std::string g_category = "ALL";
char g_search[96]{};
int g_quantity = 1;

struct MarketRow {
    const Commodity* commodity = nullptr;
    economy::Quote quote;
    int held = 0;
    int average_price = 0;
};

void held_of(const PlayerState& player, const std::string& id, int& units, int& average) {
    units = 0;
    average = 0;
    for (const CargoEntry& entry : player.cargo) {
        if (entry.commodity_id != id) continue;
        units = entry.units;
        average = entry.bought_at_price;
        return;
    }
}

std::string lowercase(std::string value) {
    for (char& c : value) c = (char)std::tolower((unsigned char)c);
    return value;
}

bool matches_filter(const Commodity& commodity) {
    if (g_category != "ALL" && commodity.category != g_category) return false;
    const std::string needle = lowercase(g_search);
    if (needle.empty()) return true;
    return lowercase(commodity.label).find(needle) != std::string::npos ||
           lowercase(commodity.category).find(needle) != std::string::npos;
}

std::vector<MarketRow> build_rows(const BaseContext& ctx) {
    const PlayerState& player = *ctx.player;
    std::vector<MarketRow> rows;
    if (g_mode == Mode::Buy) {
        for (const Commodity& commodity : commodity::all()) {
            if (!matches_filter(commodity)) continue;
            const economy::Quote quote = economy::price(ctx.base_id, commodity.id);
            if (!quote.valid || quote.buy_price <= 0 || quote.available_units <= 0) continue;
            int held = 0, average = 0;
            held_of(player, commodity.id, held, average);
            rows.push_back({&commodity, quote, held, average});
        }
    } else {
        for (const CargoEntry& entry : player.cargo) {
            if (entry.units <= 0) continue;
            const Commodity* commodity = commodity::find(entry.commodity_id);
            if (!commodity || !matches_filter(*commodity)) continue;
            rows.push_back({commodity, economy::price(ctx.base_id, entry.commodity_id),
                            entry.units, entry.bought_at_price});
        }
    }
    return rows;
}

MarketRow* selected_row(std::vector<MarketRow>& rows) {
    auto selected = std::find_if(rows.begin(), rows.end(), [](const MarketRow& row) {
        return row.commodity && row.commodity->id == g_selected;
    });
    if (selected != rows.end()) return &*selected;
    if (rows.empty()) {
        g_selected.clear();
        return nullptr;
    }
    g_selected = rows.front().commodity->id;
    g_quantity = 1;
    return &rows.front();
}

int maximum_quantity(const MarketRow& row, const PlayerState& player,
                     int used, int capacity) {
    if (g_mode == Mode::Sell) return std::max(0, row.held);
    if (row.quote.buy_price <= 0) return 0;
    const int by_credits = (int)std::min<int64_t>(
        player.credits / row.quote.buy_price, 1000000);
    return std::max(0, std::min({row.quote.available_units,
                                capacity - used, by_credits}));
}

bool buy(BaseContext& ctx, const MarketRow& row, int quantity, int capacity) {
    PlayerState& player = *ctx.player;
    const int64_t cost = (int64_t)row.quote.buy_price * quantity;
    if (quantity <= 0 || quantity > row.quote.available_units ||
        player::cargo_units_used(player) + quantity > capacity ||
        !player::can_afford(player, cost)) return false;
    if (!player::spend_credits(player, cost)) return false;
    if (!player::add_cargo(player, row.commodity->id, quantity,
                           row.quote.buy_price, capacity)) {
        player::add_credits(player, cost); // defensive rollback
        return false;
    }
    economy::record_purchase(ctx.base_id, row.commodity->id, quantity);
    sfx::ui_click();
    std::printf("[exchange] BUY %s x%d @ %d = %lld\n",
                row.commodity->label.c_str(), quantity, row.quote.buy_price,
                (long long)cost);
    return true;
}

bool sell(BaseContext& ctx, const MarketRow& row, int quantity) {
    if (quantity <= 0 || quantity > row.held || row.quote.sell_price <= 0) return false;
    PlayerState& player = *ctx.player;
    if (!player::remove_cargo(player, row.commodity->id, quantity)) return false;
    const int64_t proceeds = (int64_t)row.quote.sell_price * quantity;
    player::add_credits(player, proceeds);
    sfx::ui_click();
    std::printf("[exchange] SELL %s x%d @ %d = %lld\n",
                row.commodity->label.c_str(), quantity, row.quote.sell_price,
                (long long)proceeds);
    return true;
}

void mode_button(const char* label, Mode mode, float width) {
    const bool selected = g_mode == mode;
    if (selected) ImGui::PushStyleColor(ImGuiCol_Button,
        mode == Mode::Buy ? ImVec4(0.12f, 0.38f, 0.22f, 1.0f)
                          : ImVec4(0.42f, 0.25f, 0.08f, 1.0f));
    if (ImGui::Button(label, ImVec2(width, 38.0f)) && !selected) {
        g_mode = mode;
        g_selected.clear();
        g_quantity = 1;
    }
    if (selected) ImGui::PopStyleColor();
}

void draw_filters() {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62f);
    ImGui::InputTextWithHint("##commodity_search", "Search commodities or categories...",
                             g_search, sizeof g_search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##commodity_category", g_category.c_str())) {
        if (ImGui::Selectable("ALL CATEGORIES", g_category == "ALL"))
            g_category = "ALL";
        std::vector<std::string> categories;
        for (const Commodity& commodity : commodity::all())
            if (std::find(categories.begin(), categories.end(), commodity.category) == categories.end())
                categories.push_back(commodity.category);
        std::sort(categories.begin(), categories.end());
        for (const std::string& category : categories)
            if (ImGui::Selectable(category.c_str(), g_category == category))
                g_category = category;
        ImGui::EndCombo();
    }
}

void draw_market_table(std::vector<MarketRow>& rows) {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##commodity_market", 6, flags, ImVec2(0, -1))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("COMMODITY", ImGuiTableColumnFlags_WidthStretch, 2.2f);
    ImGui::TableSetupColumn("CLASS", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn(g_mode == Mode::Buy ? "BUY" : "SELL",
                            ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("STOCK", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("ABOARD", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("P/L EACH", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableHeadersRow();
    for (MarketRow& row : rows) {
        const Commodity& commodity = *row.commodity;
        ImGui::PushID(commodity.id.c_str());
        ImGui::TableNextRow(0, 34.0f);
        ImGui::TableSetColumnIndex(0);
        if (ImGui::Selectable(commodity.label.c_str(), g_selected == commodity.id,
                              ImGuiSelectableFlags_SpanAllColumns,
                              ImVec2(0, 30.0f))) {
            g_selected = commodity.id;
            g_quantity = 1;
            sfx::ui_click();
        }
        if (commodity::is_contraband(commodity.id)) {
            ImGui::SameLine();
            ImGui::TextColored(kRed, "!");
        }
        ImGui::TableSetColumnIndex(1); ImGui::TextColored(kDim, "%s", commodity.category.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(g_mode == Mode::Buy ? kAmber : kGreen, "%d CR",
                           g_mode == Mode::Buy ? row.quote.buy_price : row.quote.sell_price);
        ImGui::TableSetColumnIndex(3); ImGui::Text("%d", row.quote.available_units);
        ImGui::TableSetColumnIndex(4); ImGui::Text("%d", row.held);
        ImGui::TableSetColumnIndex(5);
        if (row.held > 0 && row.quote.sell_price > 0) {
            const int delta = row.quote.sell_price - row.average_price;
            ImGui::TextColored(delta >= 0 ? kGreen : kRed, "%+d", delta);
        } else ImGui::TextDisabled("--");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_manifest(const PlayerState& player) {
    ImGui::TextColored(kAmber, "CARGO MANIFEST");
    ImGui::Separator();
    if (player.cargo.empty()) {
        ImGui::TextDisabled("No commercial cargo aboard.");
        return;
    }
    if (ImGui::BeginTable("##manifest", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("GOOD", 0, 2.0f);
        ImGui::TableSetupColumn("UNITS", 0, 0.6f);
        ImGui::TableSetupColumn("AVG", 0, 0.8f);
        ImGui::TableHeadersRow();
        for (const CargoEntry& entry : player.cargo) {
            const Commodity* commodity = commodity::find(entry.commodity_id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(
                commodity ? commodity->label.c_str() : entry.commodity_id.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", entry.units);
            ImGui::TableNextColumn(); ImGui::Text("%d CR", entry.bought_at_price);
        }
        ImGui::EndTable();
    }
}

void draw_detail(BaseContext& ctx, MarketRow* row, int used, int capacity) {
    PlayerState& player = *ctx.player;
    if (!row) {
        ImGui::TextColored(kAmber, g_mode == Mode::Buy ? "NO MARKET LISTINGS" : "CARGO HOLD EMPTY");
        ImGui::Spacing();
        ImGui::TextWrapped(g_mode == Mode::Buy
            ? "This market has no goods matching the current filters."
            : "No carried commodities match the current filters.");
        ImGui::Separator();
        draw_manifest(player);
        return;
    }

    const Commodity& commodity = *row->commodity;
    ImGui::TextColored(kWhite, "%s", commodity.label.c_str());
    ImGui::SameLine(); ImGui::TextColored(kDim, " / %s", commodity.category.c_str());
    if (commodity::is_contraband(commodity.id))
        ImGui::TextColored(kRed, "CONTRABAND - SCAN RISK");
    ImGui::Separator();
    ImGui::TextColored(kDim, "UNIT PRICE"); ImGui::SameLine();
    const int unit_price = g_mode == Mode::Buy ? row->quote.buy_price : row->quote.sell_price;
    ImGui::TextColored(g_mode == Mode::Buy ? kAmber : kGreen, "%d CR", unit_price);
    ImGui::Text("Market stock: %d", row->quote.available_units);
    ImGui::Text("Aboard: %d units at %d CR average", row->held, row->average_price);
    ImGui::Text("Free hold space: %d units", std::max(0, capacity - used));
    ImGui::Spacing();

    const int maximum = maximum_quantity(*row, player, used, capacity);
    g_quantity = std::clamp(g_quantity, maximum > 0 ? 1 : 0, maximum);
    if (ImGui::Button("-", ImVec2(42, 36))) g_quantity = std::max(1, g_quantity - 1);
    ImGui::SameLine(); ImGui::SetNextItemWidth(100.0f);
    if (ImGui::InputInt("##trade_quantity", &g_quantity, 0, 0))
        g_quantity = std::clamp(g_quantity, maximum > 0 ? 1 : 0, maximum);
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(42, 36))) g_quantity = std::min(maximum, g_quantity + 1);
    ImGui::SameLine();
    if (ImGui::Button("MAX", ImVec2(70, 36))) g_quantity = maximum;

    const int64_t total = (int64_t)unit_price * g_quantity;
    ImGui::Spacing();
    ImGui::TextColored(kDim, g_mode == Mode::Buy ? "TOTAL COST" : "TOTAL PROCEEDS");
    ImGui::SameLine();
    ImGui::TextColored(g_mode == Mode::Buy ? kAmber : kGreen,
                       "%lld CR", (long long)total);
    const bool enabled = maximum > 0 && g_quantity > 0 && row->quote.valid;
    ImGui::BeginDisabled(!enabled);
    char action[96];
    std::snprintf(action, sizeof action, "%s %d UNIT%s",
                  g_mode == Mode::Buy ? "BUY" : "SELL", g_quantity,
                  g_quantity == 1 ? "" : "S");
    if (ImGui::Button(action, ImVec2(-1.0f, 46.0f))) {
        const bool completed = g_mode == Mode::Buy
            ? buy(ctx, *row, g_quantity, capacity)
            : sell(ctx, *row, g_quantity);
        if (completed) g_quantity = 1;
    }
    ImGui::EndDisabled();
    if (!enabled) {
        if (!row->quote.valid || unit_price <= 0)
            ImGui::TextColored(kRed, "This port does not quote this commodity.");
        else if (g_mode == Mode::Buy && capacity <= used)
            ImGui::TextColored(kRed, "Cargo hold is full.");
        else if (g_mode == Mode::Buy && player.credits < unit_price)
            ImGui::TextColored(kRed, "Insufficient credits for one unit.");
        else if (g_mode == Mode::Sell)
            ImGui::TextColored(kRed, "No units available to sell.");
        else ImGui::TextColored(kRed, "No stock available.");
    }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    draw_manifest(player);
}

} // namespace

void draw_exchange(BaseContext& ctx) {
    PlayerState& player = *ctx.player;
    const ShipClass* ship = ship_class::find(player.ship_class_name);
    const int capacity = player::cargo_capacity(player, ship);
    const int used = player::cargo_units_used(player);
    const float dpi = sapp_dpi_scale();
    const float sw = (float)sapp_width() / dpi;
    const float sh = (float)sapp_height() / dpi;
    const float left = 24.0f, top = 56.0f, bottom = 76.0f;

    ImGui::SetCursorScreenPos(ImVec2(left, top));
    // Semi-transparent scrim instead of a near-opaque slab: the base room art
    // stays visible behind the exchange so the screen feels anchored in the
    // scene. Inner panels add their own wash, so keep this one light — the
    // composited alpha behind the tables lands around ~0.75. Tuned lighter
    // so the room art clearly reads through the buy/sell menu.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.012f, 0.018f, 0.028f, 0.55f));
    if (ImGui::BeginChild("##commodity_exchange",
                          ImVec2(sw - left - 24.0f, sh - top - bottom), true)) {
        ImGui::TextColored(kAmber, "%s COMMODITY EXCHANGE", ctx.display_name.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 440.0f);
        ImGui::TextColored(kDim, "CREDITS"); ImGui::SameLine();
        ImGui::TextColored(kGreen, "%lld", (long long)player.credits);
        ImGui::SameLine(); ImGui::TextColored(kDim, "  CARGO"); ImGui::SameLine();
        ImGui::TextColored(used >= capacity ? kRed : kAmber, "%d / %d", used, capacity);
        ImGui::Separator();

        mode_button("BUY MARKET", Mode::Buy, 160.0f);
        ImGui::SameLine(); mode_button("SELL CARGO", Mode::Sell, 160.0f);
        ImGui::SameLine();
        ImGui::TextColored(kDim, g_mode == Mode::Buy
            ? "Acquire local stock for transport or use."
            : "Liquidate cargo at this port's current bid.");
        ImGui::Spacing();

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float gap = 10.0f;
        const float market_width = available.x * 0.62f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.018f, 0.023f, 0.034f, 0.30f));
        if (ImGui::BeginChild("##market_catalog", ImVec2(market_width, available.y), true)) {
            draw_filters();
            ImGui::Separator();
            std::vector<MarketRow> rows = build_rows(ctx);
            selected_row(rows);
            draw_market_table(rows);
        }
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
        if (ImGui::BeginChild("##trade_detail",
                              ImVec2(available.x - market_width - gap, available.y), true)) {
            std::vector<MarketRow> rows = build_rows(ctx);
            draw_detail(ctx, selected_row(rows), used, capacity);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace commodity_ui
