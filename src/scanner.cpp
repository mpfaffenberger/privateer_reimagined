// -----------------------------------------------------------------------------
// scanner.cpp — scanner catalog load + fit/sell transactions (#143).
// See scanner.h for the design and who consumes the capabilities.
// -----------------------------------------------------------------------------

#include "scanner.h"

#include "json.h"
#include "player.h"

#include <cstdio>

namespace scanner {

namespace {
std::vector<ScannerType> g_catalog;   // authored order (the shop lists it so)
} // namespace

int load(const std::string& equip_prices_path) {
    g_catalog.clear();
    const json::Value root = json::parse_file(equip_prices_path);
    const json::Value* arr = root.is_object() ? root.find("scanners") : nullptr;
    if (!arr || !arr->is_array()) {
        std::fprintf(stderr, "[scanner] no `scanners` in '%s' — scanner shop empty\n",
                     equip_prices_path.c_str());
        return 0;
    }
    for (const json::Value& e : arr->as_array()) {
        if (!e.is_object() || !e.contains("id")) continue;
        ScannerType s;
        s.id          = e["id"].string_or("");
        s.name        = e.contains("name") ? e["name"].string_or(s.id) : s.id;
        s.price       = e.contains("price")       ? (int64_t)e["price"].number_or(0)     : 0;
        s.range_m     = e.contains("range_m")     ? (float)e["range_m"].number_or(0)     : 0.0f;
        s.color_iff   = e.contains("color_iff")   && e["color_iff"].bool_or(false);
        s.target_lock = e.contains("target_lock") && e["target_lock"].bool_or(false);
        s.itts        = e.contains("itts")        && e["itts"].bool_or(false);
        if (s.id.empty() || find(s.id)) {
            std::fprintf(stderr, "[scanner] skipping blank/duplicate id '%s'\n", s.id.c_str());
            continue;
        }
        g_catalog.push_back(std::move(s));
    }
    std::printf("[scanner] %zu scanners in catalog\n", g_catalog.size());
    return (int)g_catalog.size();
}

const std::vector<ScannerType>& catalog() { return g_catalog; }

const ScannerType* find(const std::string& id) {
    if (id.empty()) return nullptr;
    for (const ScannerType& s : g_catalog) if (s.id == id) return &s;
    return nullptr;
}

bool buy(PlayerState& p, const std::string& id) {
    const ScannerType* next = find(id);
    if (!next || next->price <= 0) {
        std::printf("[scanner] BUY refused: '%s' not for sale\n", id.c_str());
        return false;
    }
    if (p.scanner_id == id) {
        std::printf("[scanner] BUY refused: '%s' already fitted\n", id.c_str());
        return false;
    }
    // The fitted unit goes back to the dealer at full price, exactly what
    // sell() would pay — buying over it is just sell + buy in one click.
    const ScannerType* old = find(p.scanner_id);
    const int64_t trade_in = old ? old->price : 0;
    const int64_t net      = next->price - trade_in;
    if (net > 0 && !player::can_afford(p, net)) {
        std::printf("[scanner] BUY refused: %s nets %lld, have %lld\n",
                    next->name.c_str(), (long long)net, (long long)p.credits);
        return false;
    }
    if (net > 0)      player::spend_credits(p, net);
    else if (net < 0) player::add_credits(p, -net);
    p.scanner_id = next->id;
    std::printf("[scanner] FIT %s (trade-in %lld) | net %lld | credits %lld\n",
                next->name.c_str(), (long long)trade_in, (long long)net,
                (long long)p.credits);
    return true;
}

bool sell(PlayerState& p) {
    const ScannerType* fitted = find(p.scanner_id);
    if (!fitted) {
        std::printf("[scanner] SELL refused: no scanner fitted\n");
        return false;
    }
    player::add_credits(p, fitted->price);
    p.scanner_id.clear();
    std::printf("[scanner] SOLD %s +%lld | credits %lld\n",
                fitted->name.c_str(), (long long)fitted->price, (long long)p.credits);
    return true;
}

} // namespace scanner
