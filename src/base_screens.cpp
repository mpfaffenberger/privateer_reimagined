#include "sfx.h"
// -----------------------------------------------------------------------------
// base_screens.cpp — Privateer-style 2D base screens implementation.
//
// See base_screens.h for the design rationale (data-driven bases, normalized
// hotspot coords, the screen stack, and the registration hook seam for the
// shop tasks). This file is *pure draw + a tiny stack*: no sim state of its
// own beyond "which base am I in and which screen am I looking at". All the
// per-base content comes from assets/bases/<id>/base.json, parsed once in
// enter() via the house json:: parser.
//
// Rendering follows the established cockpit_hud pattern: a single ImGui
// frame, drawlist primitives in LOGICAL pixels (sapp_width()/dpi), and the
// concourse PNG blitted through simgui_imtextureid — the same texture-id
// bridge the atlas viewer + light editor use for sprite thumbnails. We host
// the hotspots as ImGui::InvisibleButtons so we inherit hover/click/tooltip
// for free and play nicely with the rest of the ImGui frame.
// -----------------------------------------------------------------------------

#include "base_screens.h"

#include "docking.h"
#include "json.h"
#include "material.h"   // TextureSlot, load_texture_png
#include "player.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"   // simgui_imtextureid

#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace base_screens {

namespace {

// ---- per-base data (loaded from base.json) ----------------------------------

// One clickable region on the concourse. Rect is normalized 0..1 (origin
// top-left) so it's resolution-independent — multiplied by the logical
// framebuffer size at draw time.
struct Hotspot {
    BaseScreen  target = BaseScreen::Bar;
    std::string label;
    float       x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    std::string tooltip;
};

struct BaseDef {
    std::string          id;
    std::string          display_name;
    std::string          faction;
    std::string          art_path;     // resolved "assets/..." path to the PNG
    std::vector<Hotspot> hotspots;
    bool                 loaded = false;
};

// ---- module state -----------------------------------------------------------
// Deliberately file-local statics (mirrors sfx.cpp's g_sfx). One base is
// "active" at a time — you can only be landed at one place.
BaseDef                  g_def;
TextureSlot              g_art;            // concourse PNG; valid==false if missing
std::vector<BaseScreen>  g_stack;          // Concourse always sits at index 0
std::array<ScreenHook, 10> g_hooks{};      // index by (int)BaseScreen
bool                     g_launch_pending = false;
// Player's in-flight Ship for this Landed session (np-zte.2). Set by build()
// each frame, handed to the screen hooks via BaseContext so the Repair
// service can read/restore hull armor. Non-owning; null until first build().
Ship*                    g_player_ship = nullptr;

// ---- enum <-> string --------------------------------------------------------

const char* screen_name(BaseScreen s) {
    switch (s) {
        case BaseScreen::Concourse:        return "Concourse";
        case BaseScreen::Bar:              return "Bar";
        case BaseScreen::CommodityExchange:return "Commodity Exchange";
        case BaseScreen::ShipDealer:       return "Ship Dealer";
        case BaseScreen::Equipment:        return "Equipment";
        case BaseScreen::MissionComputer:  return "Mission Computer";
        case BaseScreen::MercenariesGuild: return "Mercenaries' Guild";
        case BaseScreen::MerchantsGuild:   return "Merchants' Guild";
        case BaseScreen::CargoHold:        return "Cargo Hold";
        case BaseScreen::Launch:           return "Launch";
        default:                           return "?";
    }
}

// Parse a base.json "target" enum-name into a BaseScreen. Returns false for
// unknown names (a typo in data shouldn't crash — we skip the hotspot).
bool parse_target(const std::string& s, BaseScreen& out) {
    if (s == "Bar")               { out = BaseScreen::Bar;               return true; }
    if (s == "CommodityExchange") { out = BaseScreen::CommodityExchange; return true; }
    if (s == "ShipDealer")        { out = BaseScreen::ShipDealer;        return true; }
    if (s == "Equipment")         { out = BaseScreen::Equipment;         return true; }
    if (s == "MissionComputer")   { out = BaseScreen::MissionComputer;   return true; }
    if (s == "MercenariesGuild")  { out = BaseScreen::MercenariesGuild;  return true; }
    if (s == "MerchantsGuild")    { out = BaseScreen::MerchantsGuild;    return true; }
    if (s == "CargoHold")         { out = BaseScreen::CargoHold;         return true; }
    if (s == "Launch")            { out = BaseScreen::Launch;            return true; }
    if (s == "Concourse")         { out = BaseScreen::Concourse;         return true; }
    return false;
}

// ---- shared draw helpers (cockpit_hud-style) --------------------------------

struct ScreenSize { float w, h, dpi; };
ScreenSize screen_size() {
    const float dpi = sapp_dpi_scale();
    return { (float)sapp_width() / dpi, (float)sapp_height() / dpi, dpi };
}

// HUD palette — kept in step with cockpit_hud.cpp's amber/cyan vocabulary so
// the base screens feel like the same game.
constexpr ImU32 kAmber    = IM_COL32(255, 217,  77, 255);
constexpr ImU32 kAmberDim = IM_COL32(200, 170,  60, 200);
constexpr ImU32 kWhite    = IM_COL32(225, 232, 238, 255);
constexpr ImU32 kHotFill  = IM_COL32( 40,  80, 120,  70);
constexpr ImU32 kHotHover = IM_COL32( 80, 150, 210, 130);

// Centre `text` inside the pixel rect (x,y,w,h) on `dl`.
void draw_centered(ImDrawList* dl, const char* text, float x, float y,
                   float w, float h, ImU32 col) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f), col, text);
}

// Activate a hotspot's target: push a sub-screen, or arm the launch. Launch
// is deferred to the end of build() so we don't mutate camera/mode while
// still drawing this frame's screen.
void activate(BaseScreen target) {
    sfx::ui_click();
    if (target == BaseScreen::Launch) {
        g_launch_pending = true;
        std::printf("[base] hotspot -> Launch (queued)\n");
        return;
    }
    g_stack.push_back(target);
    std::printf("[base] push %s (stack depth %zu)\n",
                screen_name(target), g_stack.size());
}

// ---- concourse + sub-screen rendering ---------------------------------------

// Draw the concourse hub: hotspot regions over the art + the context strip.
void draw_concourse(ImDrawList* dl, const ScreenSize& ss, PlayerState& player) {
    for (const Hotspot& hs : g_def.hotspots) {
        const float px = hs.x * ss.w;
        const float py = hs.y * ss.h;
        const float pw = hs.w * ss.w;
        const float ph = hs.h * ss.h;

        ImGui::SetCursorScreenPos(ImVec2(px, py));
        ImGui::InvisibleButton(hs.label.c_str(), ImVec2(pw, ph));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) activate(hs.target);

        dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph),
                          hovered ? kHotHover : kHotFill, 4.0f);
        dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph),
                    hovered ? kAmber : kAmberDim, 4.0f, 0, hovered ? 2.0f : 1.0f);
        draw_centered(dl, hs.label.c_str(), px, py, pw, ph,
                      hovered ? kWhite : kAmber);

        if (hovered && !hs.tooltip.empty()) ImGui::SetTooltip("%s", hs.tooltip.c_str());
    }

    // No overlay — concourse shows the art + hotspots; the player can
    // tell where they are from the art itself. Saving is still handled
    // by the landing-time autosave (see launch/land transitions).
}

// ---- guild screens (#16) ----------------------------------------------------
// The two guild computers (Mercenaries' / Merchants') are first-class base
// screens, but unlike the shops they gate on a PAID membership. Until you
// join, the screen body is a join prompt; once you're a member it shows the
// guild's board. The board ITSELF is #17 — if that task has registered a
// hook for this screen we hand the body to it; otherwise a labelled stub
// makes clear the screen exists but isn't wired yet.

bool is_guild(BaseScreen s) {
    return s == BaseScreen::MercenariesGuild || s == BaseScreen::MerchantsGuild;
}
bool& guild_member_flag(PlayerState& p, BaseScreen s) {
    return (s == BaseScreen::MercenariesGuild) ? p.merc_guild_member
                                               : p.merchant_guild_member;
}
int64_t guild_fee(BaseScreen s) {
    return (s == BaseScreen::MercenariesGuild) ? player::k_merc_guild_fee
                                               : player::k_merchant_guild_fee;
}

// Transient feedback line shown under the JOIN button (welcome / refusal).
std::string g_guild_msg;
double      g_guild_msg_until = 0.0;
void guild_flash(const std::string& m) {
    g_guild_msg = m;
    g_guild_msg_until = ImGui::GetTime() + 4.0;
}

void draw_guild(ImDrawList* dl, const ScreenSize& ss, BaseScreen cur,
                PlayerState& player) {
    const char* guild_name = screen_name(cur);   // "Mercenaries' Guild" etc.
    bool&         member = guild_member_flag(player, cur);
    const int64_t fee    = guild_fee(cur);

    // #17 owns the whole guild body — including the membership gate (join
    // prompt vs board). When its hook is registered, hand off entirely so the
    // gate has a single home in missions::draw_board. The join/stub fallback
    // below only runs in a partial build where #17 hasn't wired this screen.
    if (const ScreenHook& board_hook = g_hooks[(int)cur]) {
        BaseContext ctx{ g_def.id, g_def.display_name, g_def.faction,
                         &player, g_player_ship };
        board_hook(ctx);
        return;
    }

    if (!member) {
        const float bx = 40.0f, by = ss.h * 0.38f;
        char prompt[160];
        std::snprintf(prompt, sizeof(prompt),
                      "%s membership costs %lld cr (one-time).",
                      guild_name, (long long)fee);
        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted(prompt);
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(bx, by + 28));
        ImGui::Text("Credits on hand: %lld", (long long)player.credits);

        ImGui::SetCursorScreenPos(ImVec2(bx, by + 64));
        if (ImGui::Button("JOIN GUILD", ImVec2(160, 36))) {
            if (player::spend_credits(player, fee)) {
                member = true;
                sfx::ui_click();
                std::printf("[guild] joined %s (-%lld cr, %lld left)\n",
                            guild_name, (long long)fee, (long long)player.credits);
                guild_flash(std::string("Welcome to the ") + guild_name + ".");
            } else {
                std::printf("[guild] join %s REFUSED — need %lld, have %lld\n",
                            guild_name, (long long)fee, (long long)player.credits);
                guild_flash("COMM: Insufficient credits for membership dues.");
            }
        }
        if (!g_guild_msg.empty() && ImGui::GetTime() < g_guild_msg_until) {
            ImGui::SetCursorScreenPos(ImVec2(bx, by + 112));
            ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
            ImGui::TextUnformatted(g_guild_msg.c_str());
            ImGui::PopStyleColor();
        }
        return;
    }

    // Member: the #17 board hook draws the body if registered; else a stub.
    const ScreenHook& hook = g_hooks[(int)cur];
    if (hook) {
        BaseContext ctx{ g_def.id, g_def.display_name, g_def.faction,
                         &player, g_player_ship };
        hook(ctx);
    } else {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "%s - MEMBER. Board wired in by #17.", guild_name);
        draw_centered(dl, msg, 0, 0, ss.w, ss.h, kWhite);
    }
}

// Draw a sub-screen: title + (hook body OR labelled stub) + Back affordance.
void draw_subscreen(ImDrawList* dl, const ScreenSize& ss, BaseScreen cur,
                    PlayerState& player) {
    // Title bar.
    char title[96];
    std::snprintf(title, sizeof(title), "%s  -  %s",
                  g_def.display_name.empty() ? "Base" : g_def.display_name.c_str(),
                  screen_name(cur));
    dl->AddText(ImVec2(28, 24), kAmber, title);
    dl->AddLine(ImVec2(28, 48), ImVec2(ss.w - 28, 48), kAmberDim, 1.0f);

    // Body: the integration hook owns it if one is registered; otherwise a
    // clearly-labelled stub so it's obvious the screen exists but isn't
    // wired yet. The framework still drew the background + title + (below)
    // the Back button — np-9cu.2/.3/zte.1 only fill in the middle.
    if (is_guild(cur)) {
        // Guilds gate on paid membership (#16) — own body path.
        draw_guild(dl, ss, cur, player);
    } else {
        const ScreenHook& hook = g_hooks[(int)cur];
        if (hook) {
            BaseContext ctx{ g_def.id, g_def.display_name, g_def.faction,
                             &player, g_player_ship };
            hook(ctx);
        } else {
            const char* msg = (cur == BaseScreen::Bar)
                ? "BAR - fixers TBD"
                : "screen stub - wired in by a later task";
            draw_centered(dl, msg, 0, 0, ss.w, ss.h, kWhite);
        }
    }

    // Back affordance — pops to the Concourse.
    ImGui::SetCursorScreenPos(ImVec2(28, ss.h - 56));
    if (ImGui::Button("<  BACK", ImVec2(120, 32))) {
        sfx::ui_click();
        if (g_stack.size() > 1) g_stack.pop_back();
        std::printf("[base] pop -> %s (stack depth %zu)\n",
                    screen_name(g_stack.back()), g_stack.size());
    }
}

} // namespace

// ---- public API -------------------------------------------------------------

void register_screen(BaseScreen screen, ScreenHook hook) {
    g_hooks[(int)screen] = std::move(hook);
    std::printf("[base] registered screen hook: %s\n", screen_name(screen));
}

void enter(const std::string& base_id) {
    // Reset any prior base's GPU texture before loading the new one.
    if (g_art.valid) { /* slot reuse handled by load below via destroy */ }
    g_def = BaseDef{};
    g_def.id = base_id;
    g_stack.assign(1, BaseScreen::Concourse);
    g_launch_pending = false;

    // System nav data keys bases with a TYPE suffix (drake_pirate,
    // anapolis_refinery, new_detroit_industrial, ...) but the base FOLDERS
    // are bare names (drake, anapolis, new_detroit). If the exact folder is
    // missing, strip trailing _<suffix> segments until a real base folder
    // turns up, so landing resolves to the right concourse instead of the
    // empty "BASE (no concourse art)" fallback.
    namespace fs = std::filesystem;
    auto has_base = [](const std::string& id) {
        return fs::exists("assets/bases/" + id + "/base.json");
    };
    std::string resolved = base_id;
    if (!has_base(resolved)) {
        std::string t = resolved;
        std::string::size_type pos;
        while (!has_base(t) && (pos = t.rfind('_')) != std::string::npos)
            t = t.substr(0, pos);
        if (has_base(t)) {
            std::printf("[base] base_id '%s' -> folder '%s' (stripped type suffix)\n",
                        base_id.c_str(), t.c_str());
            resolved = t;
        }
    }
    const std::string path = "assets/bases/" + resolved + "/base.json";
    const json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[base] enter('%s'): no/invalid %s — fallback screen\n",
                     base_id.c_str(), path.c_str());
        return;
    }

    g_def.display_name = root.contains("display_name")
        ? root["display_name"].string_or(base_id) : base_id;
    g_def.faction = root.contains("faction") ? root["faction"].string_or("") : "";

    // Concourse art path is stored relative to assets/; resolve it.
    std::string art_rel = root.contains("concourse_art")
        ? root["concourse_art"].string_or("") : "";
    if (!art_rel.empty()) {
        g_def.art_path = "assets/" + art_rel;
    }

    if (const json::Value* hs = root.find("hotspots"); hs && hs->is_array()) {
        for (const json::Value& h : hs->as_array()) {
            if (!h.is_object()) continue;
            Hotspot spot;
            BaseScreen tgt;
            if (!h.contains("target") || !parse_target(h["target"].string_or(""), tgt)) {
                std::fprintf(stderr, "[base] %s: hotspot with bad 'target' — skipped\n",
                             base_id.c_str());
                continue;
            }
            spot.target  = tgt;
            spot.label   = h.contains("label") ? h["label"].string_or(screen_name(tgt))
                                                : screen_name(tgt);
            spot.tooltip = h.contains("tooltip") ? h["tooltip"].string_or("") : "";
            if (const json::Value* r = h.find("rect"); r && r->is_array() && r->as_array().size() == 4) {
                spot.x = (*r)[(size_t)0].as_float();
                spot.y = (*r)[(size_t)1].as_float();
                spot.w = (*r)[(size_t)2].as_float();
                spot.h = (*r)[(size_t)3].as_float();
            } else {
                std::fprintf(stderr, "[base] %s: hotspot '%s' missing rect[4] — skipped\n",
                             base_id.c_str(), spot.label.c_str());
                continue;
            }
            g_def.hotspots.push_back(std::move(spot));
        }
    }

    g_def.loaded = true;

    // Load the concourse PNG (best-effort — a missing file just leaves the
    // gradient fallback). load_texture_png is idempotent + leaves the slot
    // untouched on failure.
    g_art = TextureSlot{};
    if (!g_def.art_path.empty() && !load_texture_png(g_def.art_path, g_art)) {
        std::fprintf(stderr, "[base] enter('%s'): concourse art '%s' missing\n",
                     base_id.c_str(), g_def.art_path.c_str());
    }

    std::printf("[base] enter '%s' (%s) — %zu hotspots, art=%s\n",
                base_id.c_str(), g_def.display_name.c_str(),
                g_def.hotspots.size(), g_art.valid ? "loaded" : "fallback");

    // Dev/capture affordance (sibling to --dev-land): the dev_remote channel
    // can screenshot but not inject the mouse click that opens a sub-screen,
    // so NP_DEV_SCREEN=<TargetName> pre-pushes that screen on entry — lets a
    // capture script grab the Commodity Exchange table directly. No effect
    // when unset; ignored on a bad name. Dev-only, never set in play.
    if (const char* want = std::getenv("NP_DEV_SCREEN"); want && *want) {
        BaseScreen tgt;
        if (parse_target(want, tgt) && tgt != BaseScreen::Concourse) {
            g_stack.push_back(tgt);
            std::printf("[base] NP_DEV_SCREEN -> opened %s\n", screen_name(tgt));
        }
    }
}

void build(PlayerState& player, Ship* player_ship, Docking& d, Camera& cam, GameState& gs,
           HMM_Vec3 sun_pos) {
    g_player_ship = player_ship;   // forwarded to screen hooks via BaseContext
    const ScreenSize ss = screen_size();

    // One borderless, input-transparent-background fullscreen window hosts
    // the whole base screen — its drawlist blits the art and we place the
    // hotspot InvisibleButtons inside it.
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(ss.w, ss.h));
    ImGui::Begin("##base_screen", nullptr, flags);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const BaseScreen cur = g_stack.empty() ? BaseScreen::Concourse : g_stack.back();

    // Background. The concourse shows the full-screen base art; sub-screens
    // get a plain dark gradient so the shop hooks (np-9cu.2/.3/zte.1) draw
    // onto a clean canvas instead of fighting the concourse doorways.
    if (cur == BaseScreen::Concourse && g_art.valid) {
        dl->AddImage(simgui_imtextureid(g_art.view), ImVec2(0, 0), ImVec2(ss.w, ss.h));
    } else {
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(ss.w, ss.h),
            IM_COL32(18, 22, 32, 255), IM_COL32(18, 22, 32, 255),
            IM_COL32(6, 8, 12, 255),   IM_COL32(6, 8, 12, 255));
        if (cur == BaseScreen::Concourse) {
            char label[96];
            std::snprintf(label, sizeof(label), "%s (no concourse art)",
                          g_def.display_name.empty() ? "BASE" : g_def.display_name.c_str());
            draw_centered(dl, label, 0, 0, ss.w, ss.h * 0.4f, kAmberDim);
        }
    }

    if (cur == BaseScreen::Concourse) draw_concourse(dl, ss, player);
    else                              draw_subscreen(dl, ss, cur, player);

    ImGui::End();

    // Deferred Launch — run after the window closes so the screen we drew
    // this frame stays consistent. docking::launch requests Flight (applied
    // at the top of the next frame, where main calls base_screens::exit()).
    if (g_launch_pending) {
        g_launch_pending = false;
        std::printf("[base] LAUNCH from %s — handing off to docking::launch\n",
                    g_def.id.c_str());
        docking::launch(d, cam, gs, player, sun_pos);
    }
}

void exit() {
    if (g_art.valid) {
        sg_destroy_view(g_art.view);
        sg_destroy_image(g_art.image);
        g_art = TextureSlot{};
    }
    g_stack.clear();
    std::printf("[base] exit '%s'\n", g_def.id.c_str());
}

bool handle_escape() {
    if (g_stack.size() > 1) {
        g_stack.pop_back();
        sfx::ui_click();
        std::printf("[base] ESC pop -> %s (stack depth %zu)\n",
                    screen_name(g_stack.back()), g_stack.size());
        return true;
    }
    return false;   // on the Concourse — caller decides (main launches)
}

} // namespace base_screens
