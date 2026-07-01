// -----------------------------------------------------------------------------
// fixers.cpp — fixer registry, gating, actions + the Bar screen body.
// See fixers.h for the model; assets/data/fixers.json for the content.
// -----------------------------------------------------------------------------

#include "fixers.h"

#include "json.h"
#include "player.h"
#include "plot.h"

#include <algorithm>
#include <cstdio>

#ifndef FIXERS_HEADLESS
#include "base_screens.h"
#include "sfx.h"
#include "imgui.h"
#endif

namespace fixers {
namespace {

std::vector<FixerDef> g_fixers;
std::function<void(const std::string&)> g_observer;

void notify(const std::string& what) {
    if (g_observer) g_observer(what);
}

// Read an array-of-strings field into `out` (missing/mistyped = empty).
void read_string_array(const json::Value& obj, const char* key,
                       std::vector<std::string>& out) {
    if (const json::Value* a = obj.find(key); a && a->is_array()) {
        for (const json::Value& v : a->as_array())
            if (v.is_string()) out.push_back(v.as_string());
    }
}

} // namespace

int load(const std::string& path) {
    g_fixers.clear();
    const json::Value root = json::parse_file(path);
    if (!root.is_object() || !root.contains("fixers") ||
        !root["fixers"].is_array()) {
        std::fprintf(stderr, "[fixers] no usable table at %s (registry empty)\n",
                     path.c_str());
        return 0;
    }
    for (const json::Value& e : root["fixers"].as_array()) {
        if (!e.is_object()) continue;
        FixerDef f;
        f.id         = e.contains("id")   ? e["id"].string_or("")   : "";
        f.name       = e.contains("name") ? e["name"].string_or("") : "";
        f.base_id    = e.contains("base") ? e["base"].string_or("") : "";
        read_string_array(e, "archetypes",     f.archetypes);
        read_string_array(e, "exclude_bases",  f.exclude_bases);
        read_string_array(e, "requires_flags", f.requires_flags);
        read_string_array(e, "forbids_flags",  f.forbids_flags);
        read_string_array(e, "dialogue",       f.dialogue);
        f.offer_text = e.contains("offer") ? e["offer"].string_or("") : "";
        read_string_array(e, "accept_actions", f.accept_actions);
        read_string_array(e, "refuse_actions", f.refuse_actions);
        read_string_array(e, "done_actions",   f.done_actions);
        if (f.id.empty()) {
            std::fprintf(stderr, "[fixers] skipping entry with no id\n");
            continue;
        }
        g_fixers.push_back(std::move(f));
    }
    std::printf("[fixers] loaded %zu fixer entries from %s\n",
                g_fixers.size(), path.c_str());
    return (int)g_fixers.size();
}

std::vector<const FixerDef*> present_at(const std::string& base_id,
                                        const std::string& archetype,
                                        const PlayerState& player) {
    std::vector<const FixerDef*> out;
    for (const FixerDef& f : g_fixers) {
        // ---- placement: exact base OR archetype-minus-exclusions ----
        // Base match tolerates the nav-data type suffix: an entry authored
        // "new_detroit" also matches "new_detroit_industrial" (same rule
        // as base_screens' folder resolver, prefix + '_').
        bool placed = !f.base_id.empty() &&
                      (f.base_id == base_id ||
                       (base_id.size() > f.base_id.size() &&
                        base_id.rfind(f.base_id + "_", 0) == 0));
        if (!placed && !f.archetypes.empty()) {
            const bool arch_ok = std::find(f.archetypes.begin(),
                                           f.archetypes.end(),
                                           archetype) != f.archetypes.end();
            const bool excluded = std::find(f.exclude_bases.begin(),
                                            f.exclude_bases.end(),
                                            base_id) != f.exclude_bases.end();
            placed = arch_ok && !excluded;
        }
        if (!placed) continue;

        // ---- plot gate ----
        bool ok = true;
        for (const std::string& fl : f.requires_flags)
            if (!plot::has_flag(player, fl)) { ok = false; break; }
        if (ok)
            for (const std::string& fl : f.forbids_flags)
                if (plot::has_flag(player, fl)) { ok = false; break; }
        if (ok) out.push_back(&f);
    }
    return out;
}

const FixerDef* find(const std::string& id) {
    for (const FixerDef& f : g_fixers)
        if (f.id == id) return &f;
    return nullptr;
}

void accept(const FixerDef& f, PlayerState& player) {
    std::printf("[fixers] ACCEPT '%s'\n", f.id.c_str());
    plot::run_actions(player, f.accept_actions);
    notify("accepted: " + f.id);
}

void refuse(const FixerDef& f, PlayerState& player) {
    std::printf("[fixers] REFUSE '%s'\n", f.id.c_str());
    plot::run_actions(player, f.refuse_actions);
    notify("refused: " + f.id);
}

void dialogue_done(const FixerDef& f, PlayerState& player) {
    std::printf("[fixers] dialogue done '%s'\n", f.id.c_str());
    plot::run_actions(player, f.done_actions);
    notify("dialogue_done: " + f.id);
}

void set_observer(std::function<void(const std::string&)> fn) {
    g_observer = std::move(fn);
}

void note_offered(const FixerDef& f, const std::string& base_id) {
    notify("offered: " + f.id + " @ " + base_id);
}

// ---------------------------------------------------------------------------
// Bar screen body (live build only).
// ---------------------------------------------------------------------------
#ifndef FIXERS_HEADLESS
namespace {

// Conversation state — which fixer the player is talking to and how far
// along. Reset when the bar is entered for a different base (or after the
// conversation closes). `g_noted` de-dupes the "offered" observer line per
// bar visit.
std::string              g_talking_to;     // fixer id, "" = browsing the bar
size_t                   g_paragraph = 0;
std::string              g_last_base;      // bar-visit change detector
std::vector<std::string> g_noted;          // fixers already logged this visit

constexpr ImU32 kAmber    = IM_COL32(255, 176,  48, 255);
constexpr ImU32 kWhite    = IM_COL32(235, 235, 235, 255);
constexpr ImU32 kDim      = IM_COL32(160, 150, 130, 255);
constexpr ImU32 kBackdrop = IM_COL32(8, 10, 14, 222);

void reset_conversation() {
    g_talking_to.clear();
    g_paragraph = 0;
}

void draw_bar_body(BaseContext& ctx) {
    if (ctx.base_id != g_last_base) {           // new bar visit
        g_last_base = ctx.base_id;
        reset_conversation();
        g_noted.clear();
    }
    if (!ctx.player) return;
    PlayerState& player = *ctx.player;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const std::vector<const FixerDef*> present =
        present_at(ctx.base_id, ctx.archetype, player);
    for (const FixerDef* f : present) {
        if (std::find(g_noted.begin(), g_noted.end(), f->id) == g_noted.end()) {
            g_noted.push_back(f->id);
            note_offered(*f, ctx.base_id);
        }
    }

    // ---- browsing: bartender flavor + one talk button per fixer ----------
    if (g_talking_to.empty()) {
        const float bx = 28.0f, bw = 300.0f;
        float by = disp.y - 96.0f - 40.0f * (float)present.size();
        dl->AddText(ImVec2(bx, by - 26.0f), kDim,
                    present.empty() ? "The bartender polishes a glass. Nothing new."
                                    : "Someone here wants a word...");
        for (const FixerDef* f : present) {
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            char label[96];
            std::snprintf(label, sizeof(label), "Talk to %s", f->name.c_str());
            if (ImGui::Button(label, ImVec2(bw, 32))) {
                sfx::ui_click();
                g_talking_to = f->id;
                g_paragraph  = 0;
            }
            by += 40.0f;
        }
        return;
    }

    // ---- conversation panel ----------------------------------------------
    const FixerDef* f = find(g_talking_to);
    if (!f) { reset_conversation(); return; }

    const float pw = std::min(disp.x * 0.62f, 760.0f);
    const float ph = 240.0f;
    const float px = (disp.x - pw) * 0.5f;
    const float py = disp.y - ph - 84.0f;
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), kBackdrop, 8.0f);
    dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), kAmber, 8.0f);
    dl->AddText(ImVec2(px + 18, py + 14), kAmber, f->name.c_str());
    dl->AddLine(ImVec2(px + 18, py + 36), ImVec2(px + pw - 18, py + 36), kDim);

    const bool has_dialogue = !f->dialogue.empty();
    const bool last_para    = !has_dialogue ||
                              g_paragraph + 1 >= f->dialogue.size();
    if (has_dialogue) {
        const std::string& text = f->dialogue[std::min(g_paragraph,
                                                       f->dialogue.size() - 1)];
        // Wrapped body text via ImGui (draw-list text doesn't wrap).
        ImGui::SetCursorScreenPos(ImVec2(px + 18, py + 48));
        ImGui::PushTextWrapPos(px + pw - 18);
        ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    // Offer line, shown with the final paragraph.
    const bool offering = last_para && !f->offer_text.empty();
    if (offering) {
        ImGui::SetCursorScreenPos(ImVec2(px + 18, py + ph - 92));
        ImGui::PushTextWrapPos(px + pw - 18);
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted(f->offer_text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    }

    // ---- controls: NEXT / ACCEPT+REFUSE / LEAVE ---------------------------
    const float byy = py + ph - 44.0f;
    if (!last_para) {
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 130, byy));
        if (ImGui::Button("NEXT >", ImVec2(112, 30))) {
            sfx::ui_click();
            ++g_paragraph;
        }
    } else if (offering) {
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 262, byy));
        if (ImGui::Button("ACCEPT", ImVec2(120, 30))) {
            sfx::ui_click();
            accept(*f, player);
            reset_conversation();
        }
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 130, byy));
        if (ImGui::Button("REFUSE", ImVec2(112, 30))) {
            sfx::ui_click();
            refuse(*f, player);
            reset_conversation();
        }
    } else {
        ImGui::SetCursorScreenPos(ImVec2(px + pw - 130, byy));
        if (ImGui::Button("LEAVE", ImVec2(112, 30))) {
            sfx::ui_click();
            dialogue_done(*f, player);
            reset_conversation();
        }
    }
}

} // namespace

void register_bar_screen() {
    base_screens::register_screen(BaseScreen::Bar, draw_bar_body);
    std::printf("[fixers] Bar screen body registered\n");
}
#endif // FIXERS_HEADLESS

} // namespace fixers
