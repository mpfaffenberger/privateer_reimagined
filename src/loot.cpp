// -----------------------------------------------------------------------------
// loot.cpp — Phase 4c loot-drop implementation (np-#86).
//
// Schema (assets/data/loot_tables.json):
//   { "tables": { "<faction>": { "drops": [ {"item":"scrap_metal",
//       "kind":"salvage","weight":60}, {"item":"laser_cannon",
//       "kind":"weapon","weight":5,"rarity_chance":{"basic":0.8,
//       "rare":0.18,"legendary":0.02}} ] } },
//     "ace_bonus": { "legendary_chance": 0.03 } }
//
// Roll pipeline:
//   1. faction -> to_name -> look up table; missing -> no-op.
//   2. weighted-pick one entry by `weight`.
//   3. rarity: if "rarity_chance" present, sample basic/rare/legendary by
//      those probabilities; else Basic. Ace bonus adds to the legendary
//      outcome (and only that outcome — ace should still mostly drop
//      common stuff, just occasionally be legendary).
//   4. WeaponMods: Basic = {1,1}, Rare = {1.1, 0.9}, Legendary = {1.2, 0.8}
//      (matches inventory.h "rarer guns shoot faster AND cheaper").
// -----------------------------------------------------------------------------

#include "loot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include "cockpit_hud.h"
#include "player.h"
#include <cstring>
#include <random>

#include "camera.h"
#include "faction.h"
#include "json.h"
#include "imgui.h"
#include "sokol_app.h"

namespace loot {

namespace {

// One RNG per process. Deterministic seed: drops are debug-stable across
// runs and reproducibility is more useful than entropy for a feature
// that's mostly cosmetic in Phase 4c.
std::mt19937& rng() {
    static std::mt19937 r{0xC0DEBABEu ^ 0x100D1107u};   // distinct from other modules
    return r;
}

struct DropDef {
    std::string id;
    std::string kind;             // "salvage" | "weapon" | "upgrade" | "commodity" | "cargo" | "contraband"
    double      weight = 1.0;
    bool        has_rarity_chance = false;
    double      chance_basic = 1.0;
    double      chance_rare  = 0.0;
    double      chance_legendary = 0.0;
    // #112 part 3: bulk-commodity-as-loot. A commodity drop hands the player
    // qty_min..qty_max units of `id` (the commodity catalog id) at a fixed
    // rarity. qty defaults to 1/1 so weapon/upgrade/salvage rolls (one per
    // kill) are unchanged.
    int         qty_min = 1;
    int         qty_max = 1;
    bool        has_fixed_rarity = false;
    inventory::Rarity fixed_rarity = inventory::Rarity::Basic;
};

inventory::Rarity rarity_from_string(const std::string& s) {
    using R = inventory::Rarity;
    if (s == "rare")      return R::Rare;
    if (s == "legendary") return R::Legendary;
    return R::Basic;
}

// Tables live keyed by faction::to_name() ("pirate", "militia", "ace", ...).
// t_ace_bonus_legendary is the top-level "ace_bonus.legendary_chance" —
// folded into any drop that already has a rarity_chance.legendary entry.
struct Tables {
    std::vector<DropDef> by_faction[kFactionCount];
    double               ace_bonus_legendary = 0.03;
};

Tables& tables() {
    static Tables t;
    return t;
}

// Loot drops live here. Module-private — only this TU reads/writes it.
std::vector<LootDrop>& drops() {
    static std::vector<LootDrop> v;
    return v;
}

// ----- tractor-beam VFX ----------------------------------------------------
// When try_pull() sucks a drop into the hold we DON'T just vanish it — we
// spawn a short-lived screen-space beam from the cockpit emitter to the
// drop's last world position, with energy pulses travelling INTO the ship
// and the item dot flying along the beam. Purely cosmetic, lives entirely
// in this TU (aged in tick(), drawn in render(), wiped in clear()).
struct TractorBeam {
    HMM_Vec3 item_pos = HMM_V3(0, 0, 0);   // where the drop was when pulled
    ImU32    col      = 0;                  // amber (weapon) / cyan (else)
    float    age_s    = 0.0f;
};
constexpr float k_beam_life_s = 0.6f;   // how long the pull animation plays

std::vector<TractorBeam>& beams() {
    static std::vector<TractorBeam> b;
    return b;
}

// Map the loose "kind" string the loot tables use to the strict
// inventory::ItemKind enum. Unknown strings fall back to Salvage so a
// typo in the table never crashes a kill.
inventory::ItemKind kind_from_string(const std::string& s) {
    using K = inventory::ItemKind;
    if (s == "weapon")     return K::Weapon;
    if (s == "upgrade")    return K::Upgrade;
    if (s == "commodity")  return K::Commodity;
    // Salvage is the catch-all: "salvage", "cargo", "contraband" all
    // just mean "non-equippable junk the player sells as scrap".
    return K::Salvage;
}

const char* kind_cstr(inventory::ItemKind k) {
    switch (k) {
        case inventory::ItemKind::Commodity: return "commodity";
        case inventory::ItemKind::Salvage:  return "salvage";
        case inventory::ItemKind::Weapon:   return "weapon";
        case inventory::ItemKind::Upgrade:  return "upgrade";
    }
    return "salvage";
}

const char* rarity_cstr(inventory::Rarity r) {
    using R = inventory::Rarity;
    switch (r) {
        case R::Basic:     return "basic";
        case R::Rare:      return "rare";
        case R::Legendary: return "legendary";
    }
    return "basic";
}

// Sample one of three buckets (basic / rare / legendary) given the drop's
// authored probs plus the ace bonus. Probs need not sum to 1.0 — we
// normalise the triple to handle authoring slack.
inventory::Rarity roll_rarity(const DropDef& d, double ace_bonus_legendary) {
    double b = d.chance_basic;
    double r = d.chance_rare;
    double l = d.chance_legendary + ace_bonus_legendary;
    if (b + r + l <= 0.0) {
        b = 1.0; r = 0.0; l = 0.0;     // paranoia: always Basic if all zero
    }
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const double roll = U(rng());
    const double pb = b / (b + r + l);
    const double pr = r / (b + r + l);
    if (roll < pb)        return inventory::Rarity::Basic;
    if (roll < pb + pr)   return inventory::Rarity::Rare;
    return inventory::Rarity::Legendary;
}

void mods_for(inventory::Rarity r, inventory::WeaponMods& out) {
    using R = inventory::Rarity;
    if      (r == R::Basic)     { out.fire_rate_mult = 1.0f;  out.energy_mult = 1.0f; }
    else if (r == R::Rare)      { out.fire_rate_mult = 1.1f;  out.energy_mult = 0.9f; }
    else                         { out.fire_rate_mult = 1.2f;  out.energy_mult = 0.8f; } // Legendary
}

// Read the JSON `drops` array for one faction table; missing fields fall
// back to defaults so a partial table still loads.
void ingest_drops(const json::Value& drops_v, std::vector<DropDef>& out) {
    if (!drops_v.is_array()) return;
    for (const auto& e : drops_v.as_array()) {
        if (!e.is_object()) continue;
        DropDef d;
        d.kind = e.contains("kind") ? e["kind"].as_string() : "salvage";
        // Commodity drops carry the catalog id under "commodity_id"; all
        // other kinds use "item". Accept either so the table author isn't
        // forced to remember which key a given kind wants.
        d.id   = e.contains("item")         ? e["item"].as_string()
               : e.contains("commodity_id") ? e["commodity_id"].as_string()
               : "scrap_metal";
        d.weight = e.contains("weight") ? e["weight"].as_number() : 1.0;
        if (const json::Value* rc = e.find("rarity_chance");
            rc && rc->is_object()) {
            d.has_rarity_chance     = true;
            d.chance_basic          = rc->contains("basic")     ? (*rc)["basic"].as_number()     : 1.0;
            d.chance_rare           = rc->contains("rare")      ? (*rc)["rare"].as_number()      : 0.0;
            d.chance_legendary      = rc->contains("legendary") ? (*rc)["legendary"].as_number() : 0.0;
        }
        if (e.contains("qty_min")) d.qty_min = (int)e["qty_min"].as_number();
        if (e.contains("qty_max")) d.qty_max = (int)e["qty_max"].as_number();
        if (d.qty_max < d.qty_min) d.qty_max = d.qty_min;
        if (d.qty_min < 1) d.qty_min = 1;
        if (e.contains("rarity")) {
            d.has_fixed_rarity = true;
            d.fixed_rarity = rarity_from_string(e["rarity"].as_string());
        }
        out.push_back(std::move(d));
    }
}

// Pick an entry from a non-empty drops vector weighted by `weight`.
// Caller guarantees non-empty.
const DropDef& weighted_pick(const std::vector<DropDef>& drops) {
    double total = 0.0;
    for (const auto& d : drops) total += d.weight;
    std::uniform_real_distribution<double> U(0.0, 1.0);
    double roll = U(rng()) * total;
    for (const auto& d : drops) {
        roll -= d.weight;
        if (roll <= 0.0) return d;
    }
    return drops.back();   // numeric safety net
}

} // namespace

// ----- public API ----------------------------------------------------------

void load(const std::string& path) {
    auto& T = tables();
    for (int i = 0; i < kFactionCount; ++i) T.by_faction[i].clear();
    T.ace_bonus_legendary = 0.03;   // sane default even if the file is missing

    json::Value root = json::parse_file(path);
    if (root.is_null() || !root.is_object()) {
        std::printf("[loot] %s: missing or unparseable — drops disabled\n",
                    path.c_str());
        return;
    }

    if (const json::Value* ab = root.find("ace_bonus"); ab && ab->is_object()) {
        if (ab->contains("legendary_chance"))
            T.ace_bonus_legendary = (*ab)["legendary_chance"].as_number();
    }

    int n_factions = 0;
    int n_drops    = 0;
    if (const json::Value* tables_v = root.find("tables");
        tables_v && tables_v->is_object()) {
        for (const auto& [fac_name, fac_v] : tables_v->as_object()) {
            const Faction f = faction::from_name(fac_name);
            if (f == Faction::Count) continue;     // unknown faction id — skip
            if (!fac_v.is_object()) continue;
            std::vector<DropDef> entries;
            if (const json::Value* drops_v = fac_v.find("drops");
                drops_v && drops_v->is_array()) {
                ingest_drops(*drops_v, entries);
            }
            n_drops += (int)entries.size();
            if (!entries.empty()) ++n_factions;
            T.by_faction[(int)f] = std::move(entries);
        }
    }

    std::printf("[loot] %s: %d factions, %d drop entries\n",
                path.c_str(), n_factions, n_drops);
}

void spawn_for(Faction faction, HMM_Vec3 pos, bool is_ace) {
    const auto& T = tables();
    const int   idx = (int)faction;
    if (idx < 0 || idx >= kFactionCount) return;
    const auto& table = T.by_faction[idx];
    if (table.empty()) return;

    const DropDef& d = weighted_pick(table);

    inventory::InventoryItem item;
    item.id    = d.id;
    item.kind  = kind_from_string(d.kind);
    // #112 part 3: commodity (and any qty-ranged) drops hand over a stack;
    // weapon/upgrade/salvage default to 1 (qty_min==qty_max==1).
    item.qty   = (d.qty_max > d.qty_min)
        ? std::uniform_int_distribution<int>(d.qty_min, d.qty_max)(rng())
        : d.qty_min;
    item.rarity = d.has_fixed_rarity
        ? d.fixed_rarity
        : (d.has_rarity_chance
            ? roll_rarity(d, is_ace ? T.ace_bonus_legendary : 0.0)
            : inventory::Rarity::Basic);
    if (item.kind == inventory::ItemKind::Weapon) {
        mods_for(item.rarity, item.mods);
    }

    // Small scatter so overlapping kills don't stack markers pixel-perfect.
    // ~150m in a random unit direction (xz plane — height stays put so
    // the marker still reads as "near the wreck").
    std::uniform_real_distribution<float> ang(0.0f, 6.2831853f);
    std::uniform_real_distribution<float> rad(60.0f, 150.0f);
    const float a = ang(rng());
    const float r = rad(rng());
    const HMM_Vec3 offset = HMM_V3(std::cos(a) * r, 0.0f, std::sin(a) * r);

    LootDrop drop;
    drop.pos  = HMM_AddV3(pos, offset);
    drop.item = item;
    drops().push_back(std::move(drop));

    std::printf("[loot] dropped %s %s (%s) from %s\n",
                rarity_cstr(item.rarity),
                item.id.c_str(),
                kind_cstr(item.kind),
                faction::to_name(faction));
}

void tick(float dt) {
    auto& v = drops();
    const float ttl = k_loot_ttl_s;
    v.erase(std::remove_if(v.begin(), v.end(),
        [dt, ttl](LootDrop& d) {
            d.age_s += dt;
            return d.age_s > ttl;
        }), v.end());
    // Age out finished tractor-beam VFX.
    auto& b = beams();
    b.erase(std::remove_if(b.begin(), b.end(),
        [dt](TractorBeam& tb) {
            tb.age_s += dt;
            return tb.age_s >= k_beam_life_s;
        }), b.end());
}

const std::vector<LootDrop>& all() {
    return drops();
}

void clear() {
    drops().clear();
    beams().clear();   // don't carry stale pull VFX across a system/launch reset
}

// ----- try_pull ------------------------------------------------------------
// Phase 4 (#84 + #83) — Z-key universal tractor. Walks every live drop,
// keeps the ones out of range, and tries to add the in-range ones to the
// unified hold. Commodity-kind items go via `add_cargo` (qty units, free);
// everything else goes via `add_item` (one entry per drop, qty included).
// When the add succeeds the drop is erased; when the add refuses (hold
// full) the drop is LEFT IN PLACE so a later press can retry. Returns
// the number of drops successfully pulled.
int try_pull(HMM_Vec3 player_pos, float range, PlayerState& player,
             int capacity) {
    const float r2 = range * range;     // squared distance avoids sqrt
    auto& v = drops();
    int pulled = 0;
    // Walk by index so we can erase failed-then-retained drops without
    // invalidating the iterator. Iterate in spawn order so the player
    // gets the closest-feeling drop first when two share an overlapping
    // range bucket (unusual, but keeps the tie-break deterministic).
    for (size_t i = 0; i < v.size(); /* manual advance inside branches */) {
        const LootDrop& d = v[i];
        const float dx = d.pos.X - player_pos.X;
        const float dy = d.pos.Y - player_pos.Y;
        const float dz = d.pos.Z - player_pos.Z;
        const float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > r2) { ++i; continue; }   // out of range, leave alone

        // In range. Try the right add for the item's kind. Any refusal
        // (qty<=0, hold full, etc.) leaves the drop in place.
        bool ok = false;
        if (d.item.kind == inventory::ItemKind::Commodity) {
            // add_cargo takes units, not a full InventoryItem. qty IS the
            // unit count for a commodity drop, and the catalog id is the
            // drop's `id` string.
            ok = player::add_cargo(player, d.item.id, d.item.qty,
                                    /*price_per_unit*/ 0, capacity);
        } else {
            ok = player::add_item(player, d.item, capacity);
        }
        if (ok) {
            // Spawn the pull VFX from the drop's last position before it's
            // removed. Amber for weapons, cyan for everything else (matches
            // the drop-marker palette below).
            const ImU32 c = (d.item.kind == inventory::ItemKind::Weapon)
                              ? IM_COL32(255, 217,  77, 255)
                              : IM_COL32(120, 220, 255, 255);
            beams().push_back(TractorBeam{ d.pos, c, 0.0f });
            v.erase(v.begin() + i);    // erase: don't advance i (next entry shifts down)
            ++pulled;
        } else {
            ++i;                       // retain, try next
        }
    }
    if (pulled > 0) {
        std::printf("[tractor] pulled %d item(s)\n", pulled);
    }
    return pulled;
}

// ----- render --------------------------------------------------------------

// World->screen projection now uses the shared cockpit_hud::project_world_point
// (declared in cockpit_hud.h, defined in cockpit_hud.cpp). Previously this
// block had its own private project_world implementation that re-derived
// the projection matrix + screen-size math; we now share the helper and
// the DRY fix removes the duplicate code (#84).

void render(const Camera& cam, bool draw_world) {
    if (!draw_world) return;
    const auto& v = drops();
    if (v.empty() && beams().empty()) return;   // beams outlive their drop
    auto* dl = ImGui::GetForegroundDrawList();

    // Amber for any weapon-kind drop (player reads "this is a gun"), cyan
    // for everything else (junk / salvage / commodity). Matches the rest
    // of the HUD palette without introducing a new colour.
    const ImU32 amber = IM_COL32(255, 217,  77, 240);
    const ImU32 cyan  = IM_COL32(120, 220, 255, 240);
    const ImU32 dot   = IM_COL32(255, 255, 255, 240);

    for (const LootDrop& d : v) {
        float sx, sy;
        if (!cockpit_hud::project_world_point(cam, d.pos, sx, sy)) continue;
        constexpr float r = 9.0f;   // a touch smaller than the objective marker

        // Weapon = amber; everything else = cyan. Same diamond shape as
        // draw_objective_marker so the loot drops don't visually clash
        // with mission nav diamonds.
        const ImU32 col = (d.item.kind == inventory::ItemKind::Weapon) ? amber : cyan;
        dl->AddQuad(ImVec2(sx, sy - r), ImVec2(sx + r, sy),
                    ImVec2(sx, sy + r), ImVec2(sx - r, sy), col, 2.0f);
        dl->AddCircleFilled(ImVec2(sx, sy), 2.0f, dot);

        // Label = the item id, or "SALVAGE" / "WEAPON" fallback so the
        // marker reads even when the player doesn't know the id. Uppercase
        // is intentional: it's a HUD badge, not a tooltip.
        char label[40];
        std::snprintf(label, sizeof(label), "%s",
                      d.item.kind == inventory::ItemKind::Weapon ? "WEAPON" :
                      d.item.kind == inventory::ItemKind::Upgrade ? "UPGRADE" :
                      d.item.kind == inventory::ItemKind::Commodity ? "COMMODITY" :
                      "SALVAGE");
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(sx - ts.x * 0.5f, sy - r - ts.y - 3.0f), col, label);
    }

    // ---- tractor-beam pull VFX --------------------------------------------
    // For each active beam: draw a ray from the cockpit emitter (bottom
    // centre) to the captured item, with the item flying inbound along the
    // ray, energy pulses streaming toward the ship, and the whole thing
    // fading as the item is absorbed.
    const auto& bm = beams();
    if (!bm.empty()) {
        auto with_a = [](ImU32 c, float a01) -> ImU32 {
            if (a01 < 0.0f) a01 = 0.0f; else if (a01 > 1.0f) a01 = 1.0f;
            return (c & 0x00FFFFFFu) | ((ImU32)(255.0f * a01) << 24);
        };
        const ImU32 white = IM_COL32(255, 255, 255, 255);
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const ImVec2 emitter(disp.x * 0.5f, disp.y * 0.96f);
        for (const TractorBeam& tb : bm) {
            float sx, sy;
            if (!cockpit_hud::project_world_point(cam, tb.item_pos, sx, sy)) continue;
            const float t    = tb.age_s / k_beam_life_s;   // 0..1
            const float fade = 1.0f - t;
            const ImVec2 item(sx, sy);
            const ImVec2 cur(item.x + (emitter.x - item.x) * t,
                             item.y + (emitter.y - item.y) * t);
            // Beam: soft coloured glow + bright white core.
            dl->AddLine(emitter, cur, with_a(tb.col, 0.25f * fade), 5.0f);
            dl->AddLine(emitter, cur, with_a(white,  0.55f * fade), 1.5f);
            // Energy pulses travelling INTO the ship.
            for (int k = 0; k < 3; ++k) {
                float pp = std::fmod(t * 1.7f + (float)k * 0.34f, 1.0f);
                ImVec2 pulse(cur.x + (emitter.x - cur.x) * pp,
                             cur.y + (emitter.y - cur.y) * pp);
                dl->AddCircleFilled(pulse, 3.0f * fade, with_a(tb.col, 0.9f * fade));
            }
            // The captured item, shrinking as it reaches the ship.
            dl->AddCircleFilled(cur, 6.0f * fade, with_a(tb.col, fade));
            dl->AddCircleFilled(cur, 2.5f * fade, with_a(white,  fade));
        }
    }
}

} // namespace loot
