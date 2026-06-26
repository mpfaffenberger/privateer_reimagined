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
#include <cstdio>
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
};

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
        d.id   = e.contains("item") ? e["item"].as_string() : "scrap_metal";
        d.kind = e.contains("kind") ? e["kind"].as_string() : "salvage";
        d.weight = e.contains("weight") ? e["weight"].as_number() : 1.0;
        if (const json::Value* rc = e.find("rarity_chance");
            rc && rc->is_object()) {
            d.has_rarity_chance     = true;
            d.chance_basic          = rc->contains("basic")     ? (*rc)["basic"].as_number()     : 1.0;
            d.chance_rare           = rc->contains("rare")      ? (*rc)["rare"].as_number()      : 0.0;
            d.chance_legendary      = rc->contains("legendary") ? (*rc)["legendary"].as_number() : 0.0;
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
    item.qty   = 1;   // Phase 4c: drop exactly one item per kill.
    item.rarity = d.has_rarity_chance
        ? roll_rarity(d, is_ace ? T.ace_bonus_legendary : 0.0)
        : inventory::Rarity::Basic;
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
}

const std::vector<LootDrop>& all() {
    return drops();
}

void clear() {
    drops().clear();
}

// ----- render --------------------------------------------------------------

// World->screen projection duplicated locally. Mirrors
// cockpit_hud::project_world_point exactly (same engine quirk: no ndc-Y
// flip; returns false when behind the camera or clip.W <= 0). We can't
// reach cockpit_hud's static helper from another TU, so re-implement
// here. Cheap (~6 mul + a div), called once per drop per frame.
static bool project_world(const Camera& cam, HMM_Vec3 world, float& sx, float& sy) {
    const HMM_Vec3 d = HMM_SubV3(world, cam.position);
    if (HMM_DotV3(d, cam.forward()) <= 0.0f) return false;
    const float dpi     = sapp_dpi_scale();
    const float scr_w   = (float)sapp_width()  / dpi;
    const float scr_h   = (float)sapp_height() / dpi;
    const float aspect  = scr_w / scr_h;
    const HMM_Mat4 vp   = HMM_MulM4(cam.projection(aspect), cam.view());
    const HMM_Vec4 ph   = { world.X, world.Y, world.Z, 1.0f };
    const HMM_Vec4 clip = HMM_MulM4V4(vp, ph);
    if (clip.W <= 0.0f) return false;
    const float ndc_x = clip.X / clip.W;
    const float ndc_y = clip.Y / clip.W;
    sx = (ndc_x * 0.5f + 0.5f) * scr_w;
    sy = (ndc_y * 0.5f + 0.5f) * scr_h;
    return true;
}

void render(const Camera& cam, bool draw_world) {
    if (!draw_world) return;
    const auto& v = drops();
    if (v.empty()) return;
    auto* dl = ImGui::GetForegroundDrawList();

    // Amber for any weapon-kind drop (player reads "this is a gun"), cyan
    // for everything else (junk / salvage / commodity). Matches the rest
    // of the HUD palette without introducing a new colour.
    const ImU32 amber = IM_COL32(255, 217,  77, 240);
    const ImU32 cyan  = IM_COL32(120, 220, 255, 240);
    const ImU32 dot   = IM_COL32(255, 255, 255, 240);

    for (const LootDrop& d : v) {
        float sx, sy;
        if (!project_world(cam, d.pos, sx, sy)) continue;
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
}

} // namespace loot
