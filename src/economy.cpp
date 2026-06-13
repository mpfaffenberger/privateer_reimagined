// -----------------------------------------------------------------------------
// economy.cpp — per-base pricing math + the Commodity Exchange screen body.
//
// See economy.h for the design (data-driven prices: category base x archetype
// modifier x spread, no per-base tables in C++). This file is two halves:
//
//   1. The pricing model — load the canonical JSON + each base's archetype,
//      and answer economy::price(). Pure data, no ImGui.
//   2. The Commodity Exchange screen body — an ImGui trade table registered
//      with base_screens via the np-9cu.4 hook seam. Every transaction goes
//      through player:: helpers ONLY (so save/load serializes correctly), and
//      enforces credits / cargo capacity / availability before committing.
// -----------------------------------------------------------------------------

#include "economy.h"

#include "commodity.h"
#include "json.h"
#include "player.h"

// The pricing MODEL (load/price/record_purchase) is pure data and unit-
// testable on its own; the VIEW (the ImGui trade table) drags in the whole
// UI/audio stack. ECONOMY_HEADLESS compiles only the model, so the offline
// validation harness (tools/test_economy.cpp) can link the real pricing code
// without ImGui/sokol/sfx. The live game never defines it.
#ifndef ECONOMY_HEADLESS
#include "base_screens.h"
#include "ship_class.h"
#include "sfx.h"
#include "imgui.h"
#include "sokol_app.h"
#endif

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>

namespace economy {

namespace {

// ---- pricing model (loaded from commodity_prices.json) ----------------------

// One archetype's offer on one category: scale the canonical base price by
// price_mult; stock units are offered for sale (0 = not stocked here).
struct CatMod {
    float price_mult = 1.0f;
    int   stock      = 0;
};

struct Archetype {
    CatMod                            def;        // fallback for unlisted cats
    std::unordered_map<std::string, CatMod> by_cat;
};

int                                       g_spread_pct = 18;
std::unordered_map<std::string, int>      g_cat_base_price;   // "RAWMAT" -> 28
std::unordered_map<std::string, Archetype> g_archetypes;      // "mining" -> ...
std::unordered_map<std::string, std::string> g_base_archetype;// "achilles"->"mining"

// Live per-session stock, keyed "base_id|commodity_id". Seeded lazily from
// the archetype on first query, decremented by record_purchase. Absent key =
// "use the data-derived stock"; present = the live remaining count.
std::map<std::string, int> g_live_stock;

std::string stock_key(const std::string& base_id, const std::string& cid) {
    return base_id + "|" + cid;
}

CatMod cat_mod_for(const Archetype& a, const std::string& category) {
    const auto it = a.by_cat.find(category);
    return (it == a.by_cat.end()) ? a.def : it->second;
}

// Parse a {price_mult, stock} object, falling back to the given defaults.
CatMod parse_cat_mod(const json::Value& v, CatMod fallback) {
    CatMod m = fallback;
    if (!v.is_object()) return m;
    if (v.contains("price_mult")) m.price_mult = v["price_mult"].as_float();
    if (v.contains("stock"))      m.stock      = v["stock"].as_int();
    return m;
}

// Read assets/bases/<id>/base.json and return its market archetype name
// ("" if the file or the market.archetype field is missing).
std::string read_base_archetype(const std::string& bases_dir,
                                const std::string& base_id) {
    const json::Value root = json::parse_file(bases_dir + "/" + base_id + "/base.json");
    if (!root.is_object()) return "";
    const json::Value* m = root.find("market");
    if (!m || !m->is_object() || !m->contains("archetype")) return "";
    return (*m)["archetype"].string_or("");
}

} // namespace

int load(const std::string& prices_path, const std::string& bases_dir) {
    g_spread_pct = 18;
    g_cat_base_price.clear();
    g_archetypes.clear();
    g_base_archetype.clear();
    g_live_stock.clear();

    const json::Value root = json::parse_file(prices_path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[economy] cannot load '%s' — pricing disabled\n",
                     prices_path.c_str());
        return 0;
    }

    if (root.contains("spread_pct")) g_spread_pct = root["spread_pct"].as_int();

    if (const json::Value* cb = root.find("category_base_price"); cb && cb->is_object()) {
        for (const auto& [cat, v] : cb->as_object()) g_cat_base_price[cat] = v.as_int();
    }

    if (const json::Value* arch = root.find("archetypes"); arch && arch->is_object()) {
        for (const auto& [name, av] : arch->as_object()) {
            if (!av.is_object()) continue;
            Archetype a;
            if (av.contains("default")) a.def = parse_cat_mod(av["default"], CatMod{});
            if (const json::Value* cats = av.find("categories"); cats && cats->is_object()) {
                for (const auto& [cat, cv] : cats->as_object())
                    a.by_cat[cat] = parse_cat_mod(cv, a.def);
            }
            g_archetypes[name] = std::move(a);
        }
    }

    // Resolve each known base's archetype from its base.json market block.
    // Hardcoding the id list here would re-introduce a per-base table; instead
    // we read the three Troy bases the world ships today. (When a base
    // registry lands this loops over it.)
    for (const char* id : { "achilles", "hector", "helen" }) {
        const std::string arch = read_base_archetype(bases_dir, id);
        if (!arch.empty()) g_base_archetype[id] = arch;
    }

    std::printf("[economy] %zu categories, %zu archetypes, %zu bases (spread %d%%)\n",
                g_cat_base_price.size(), g_archetypes.size(),
                g_base_archetype.size(), g_spread_pct);
    return (int)g_base_archetype.size();
}

Quote price(const std::string& base_id, const std::string& commodity_id) {
    Quote q;
    const Commodity* c = commodity::find(commodity_id);
    if (!c) return q;

    const auto basep = g_cat_base_price.find(c->category);
    if (basep == g_cat_base_price.end()) return q;

    const auto ba = g_base_archetype.find(base_id);
    if (ba == g_base_archetype.end()) return q;
    const auto arch = g_archetypes.find(ba->second);
    if (arch == g_archetypes.end()) return q;

    const CatMod m = cat_mod_for(arch->second, c->category);

    q.valid      = true;
    q.buy_price  = (int)std::lround(basep->second * m.price_mult);
    q.sell_price = (int)std::lround(q.buy_price * (1.0 - g_spread_pct / 100.0));

    // Live remaining stock: the seeded archetype stock unless we've already
    // sold some this session.
    const auto live = g_live_stock.find(stock_key(base_id, commodity_id));
    q.available_units = (live == g_live_stock.end()) ? m.stock : live->second;
    return q;
}

void record_purchase(const std::string& base_id,
                     const std::string& commodity_id, int units) {
    if (units <= 0) return;
    const Quote q = price(base_id, commodity_id);
    if (!q.valid) return;
    const int remaining = q.available_units - units;
    g_live_stock[stock_key(base_id, commodity_id)] = remaining < 0 ? 0 : remaining;
}

// ---- Commodity Exchange screen body -----------------------------------------
#ifndef ECONOMY_HEADLESS

namespace {

// HUD palette echoing base_screens.cpp / cockpit_hud.cpp.
constexpr ImU32 kAmber = IM_COL32(255, 217,  77, 255);
constexpr ImU32 kGreen = IM_COL32(120, 230, 120, 255);
constexpr ImU32 kRed   = IM_COL32(230, 110, 110, 255);
constexpr ImU32 kGrey  = IM_COL32(150, 158, 168, 255);

// Held units + average buy price for a commodity (0/0 if not carried).
void held_of(const PlayerState& p, const std::string& cid, int& units, int& avg) {
    units = 0; avg = 0;
    for (const CargoEntry& e : p.cargo) {
        if (e.commodity_id == cid) { units = e.units; avg = e.bought_at_price; return; }
    }
}

// Attempt a buy of `qty` units, enforcing availability + credits + capacity.
// All mutation flows through player:: helpers. Logs the outcome either way.
void try_buy(BaseContext& ctx, const Commodity& c, const Quote& q,
             int qty, int capacity) {
    PlayerState& p = *ctx.player;
    const int    used = player::cargo_units_used(p);
    const int64_t cost = (int64_t)q.buy_price * qty;

    if (q.buy_price <= 0 || q.available_units < qty) {
        std::printf("[exchange] BUY refused: %s x%d not available here (stock %d)\n",
                    c.label.c_str(), qty, q.available_units);
        return;
    }
    if (used + qty > capacity) {
        std::printf("[exchange] BUY refused: %s x%d would exceed cargo cap (%d/%d)\n",
                    c.label.c_str(), qty, used, capacity);
        return;
    }
    if (!player::can_afford(p, cost)) {
        std::printf("[exchange] BUY refused: %s x%d costs %lld, have %lld\n",
                    c.label.c_str(), qty, (long long)cost, (long long)p.credits);
        return;
    }
    if (player::spend_credits(p, cost) &&
        player::add_cargo(p, c.id, qty, q.buy_price, capacity)) {
        economy::record_purchase(ctx.base_id, c.id, qty);
        sfx::ui_click();
        std::printf("[exchange] BUY %s x%d @ %d = %lld | credits %lld, cargo %d/%d\n",
                    c.label.c_str(), qty, q.buy_price, (long long)cost,
                    (long long)p.credits, player::cargo_units_used(p), capacity);
    }
}

// Attempt a sell of `qty` units. Selling is always allowed for goods carried;
// proceeds added via player::add_credits.
void try_sell(BaseContext& ctx, const Commodity& c, const Quote& q,
              int qty, int capacity) {
    PlayerState& p = *ctx.player;
    if (qty <= 0) return;
    if (player::remove_cargo(p, c.id, qty)) {
        const int64_t gain = (int64_t)q.sell_price * qty;
        player::add_credits(p, gain);
        sfx::ui_click();
        std::printf("[exchange] SELL %s x%d @ %d = %lld | credits %lld, cargo %d/%d\n",
                    c.label.c_str(), qty, q.sell_price, (long long)gain,
                    (long long)p.credits, player::cargo_units_used(p), capacity);
    } else {
        std::printf("[exchange] SELL refused: don't hold %d units of %s\n",
                    qty, c.label.c_str());
    }
}

// The registered screen body. Framework already drew background + title and
// will draw Back; we own the middle.
void draw_exchange(BaseContext& ctx) {
    PlayerState& p = *ctx.player;
    const ShipClass* klass = ship_class::find(p.ship_class_name);
    const int capacity = player::cargo_capacity(p, klass);
    const int used     = player::cargo_units_used(p);

    const float dpi = sapp_dpi_scale();
    const float sw  = (float)sapp_width()  / dpi;
    const float sh  = (float)sapp_height() / dpi;

    // Running totals strip.
    ImGui::SetCursorScreenPos(ImVec2(28, 62));
    ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    ImGui::Text("CREDITS  %lld", (long long)p.credits);
    ImGui::SameLine(260);
    const bool full = used >= capacity;
    ImGui::PushStyleColor(ImGuiCol_Text, full ? kRed : kAmber);
    ImGui::Text("CARGO  %d / %d units", used, capacity);
    ImGui::PopStyleColor(2);

    // Trade table inside a scroll child, leaving room for the Back button.
    ImGui::SetCursorScreenPos(ImVec2(28, 92));
    const ImVec2 child_sz(sw - 56, sh - 92 - 70);

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginChild("##exchange_tbl", child_sz, false) &&
        ImGui::BeginTable("commodities", 7, tflags, child_sz)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Commodity", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("Buy");
        ImGui::TableSetupColumn("Sell");
        ImGui::TableSetupColumn("Avail");
        ImGui::TableSetupColumn("Held");
        ImGui::TableSetupColumn("Trade", ImGuiTableColumnFlags_WidthFixed, 320.0f);
        ImGui::TableHeadersRow();

        for (const Commodity& c : commodity::all()) {
            const Quote q = economy::price(ctx.base_id, c.id);
            if (!q.valid) continue;
            int held = 0, avg = 0;
            held_of(p, c.id, held, avg);
            // Show only what's tradeable here: stocked-to-buy, or goods you
            // already carry (so you can always offload).
            if (q.buy_price <= 0 && held == 0) continue;

            ImGui::TableNextRow();
            ImGui::PushID(c.id.c_str());

            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.label.c_str());
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, kGrey);
            ImGui::TextUnformatted(c.category.c_str());
            ImGui::PopStyleColor();

            // Buy price (grey if not stocked).
            ImGui::TableNextColumn();
            if (q.available_units > 0 && q.buy_price > 0) ImGui::Text("%d", q.buy_price);
            else { ImGui::PushStyleColor(ImGuiCol_Text, kGrey); ImGui::TextUnformatted("--"); ImGui::PopStyleColor(); }

            // Sell price — green when above your average paid (a profit).
            ImGui::TableNextColumn();
            const bool profit = held > 0 && q.sell_price > avg;
            ImGui::PushStyleColor(ImGuiCol_Text, profit ? kGreen : kGrey);
            ImGui::Text("%d", q.sell_price);
            ImGui::PopStyleColor();

            ImGui::TableNextColumn(); ImGui::Text("%d", q.available_units);
            ImGui::TableNextColumn();
            if (held > 0) ImGui::Text("%d (@%d)", held, avg);
            else          { ImGui::PushStyleColor(ImGuiCol_Text, kGrey); ImGui::TextUnformatted("0"); ImGui::PopStyleColor(); }

            // Trade buttons. Disabled states make the limits visible rather
            // than mysterious (the click path also logs the refusal).
            ImGui::TableNextColumn();
            const int  space   = capacity - used;
            const bool canBuy1 = q.buy_price > 0 && q.available_units >= 1 &&
                                 space >= 1 && player::can_afford(p, q.buy_price);
            const bool canBuy10= q.buy_price > 0 && q.available_units >= 10 &&
                                 space >= 10 && player::can_afford(p, (int64_t)q.buy_price * 10);

            ImGui::BeginDisabled(!canBuy1);
            if (ImGui::SmallButton("Buy 1"))  try_buy(ctx, c, q, 1, capacity);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!canBuy10);
            if (ImGui::SmallButton("Buy 10")) try_buy(ctx, c, q, 10, capacity);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(held < 1);
            if (ImGui::SmallButton("Sell 1")) try_sell(ctx, c, q, 1, capacity);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(held < 1);
            if (ImGui::SmallButton("Sell All")) try_sell(ctx, c, q, held, capacity);
            ImGui::EndDisabled();

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

} // namespace

void register_exchange_screen() {
    base_screens::register_screen(BaseScreen::CommodityExchange, draw_exchange);
}

#endif // ECONOMY_HEADLESS

} // namespace economy
