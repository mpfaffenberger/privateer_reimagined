// -----------------------------------------------------------------------------
// economy.cpp — per-base pricing math + the Commodity Exchange screen body.
//
// See economy.h for the design (data-driven prices: category base x archetype
// modifier x spread, no per-base tables in C++). This file owns the pure
// pricing/stock model and the tiny screen-registration seam. The responsive
// ImGui presentation and transaction orchestration live in commodity_ui.cpp,
// keeping the headless economy harness independent from graphics.
// -----------------------------------------------------------------------------

#include "economy.h"

#include "commodity.h"
#include "json.h"
#include "player.h"

// ECONOMY_HEADLESS omits only screen registration, so the offline validation
// harness can link the real pricing code without the UI/audio stack.
#ifndef ECONOMY_HEADLESS
#include "base_screens.h"
#include "commodity_ui.h"
#endif

#include <cmath>
#include <cstdio>
#include <filesystem>
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

    // Discover every authored base market. The old three-base Troy list made
    // the wider Gemini map silently quote nothing at refinery/military/etc.
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(bases_dir, ec)) {
        if (ec || !entry.is_directory()) continue;
        const std::string id = entry.path().filename().string();
        const std::string arch = read_base_archetype(bases_dir, id);
        if (!arch.empty()) g_base_archetype[id] = arch;
    }
    if (ec)
        std::fprintf(stderr, "[economy] cannot scan bases directory '%s': %s\n",
                     bases_dir.c_str(), ec.message().c_str());

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

// ---- Commodity Exchange screen registration -------------------------------
#ifndef ECONOMY_HEADLESS
void register_exchange_screen() {
    base_screens::register_screen(BaseScreen::CommodityExchange,
                                  commodity_ui::draw_exchange);
}
#endif

} // namespace economy
