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
#include "savegame.h"
#include "sfx.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"   // simgui_imtextureid

#include <array>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
std::array<ScreenHook, 7> g_hooks{};       // index by (int)BaseScreen
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
constexpr ImU32 kPanelBg  = IM_COL32(  8,  12,  18, 210);
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

// Small player-context strip (credits / ship / base) — proves the data
// wiring the downstream shop screens depend on. Drawn top-left on the
// concourse only.
void draw_context_strip(ImDrawList* dl, const PlayerState& player,
                        const BaseDef& def) {
    char line0[160], line1[96];
    std::snprintf(line0, sizeof(line0), "%s   [%s]",
                  def.display_name.empty() ? "(unknown base)" : def.display_name.c_str(),
                  def.faction.empty() ? "independent" : def.faction.c_str());
    std::snprintf(line1, sizeof(line1), "CR %lld    SHIP %s",
                  (long long)player.credits,
                  player.ship_class_name.empty() ? "(none)" : player.ship_class_name.c_str());

    const ImVec2 s0 = ImGui::CalcTextSize(line0);
    const ImVec2 s1 = ImGui::CalcTextSize(line1);
    const float  pad = 10.0f;
    const float  bw  = (s0.x > s1.x ? s0.x : s1.x) + pad * 2.0f;
    const float  bh  = s0.y + s1.y + pad * 2.0f + 4.0f;

    dl->AddRectFilled(ImVec2(16, 16), ImVec2(16 + bw, 16 + bh), kPanelBg, 6.0f);
    dl->AddRect(ImVec2(16, 16), ImVec2(16 + bw, 16 + bh), kAmberDim, 6.0f);
    dl->AddText(ImVec2(16 + pad, 16 + pad), kAmber, line0);
    dl->AddText(ImVec2(16 + pad, 16 + pad + s0.y + 4.0f), kWhite, line1);
}

// Minimal save affordance (np-ymp.1) drawn under the context strip: a
// "Save game (slot 1)" button plus a one-line readout of the most recent
// autosave (slot 0). The headless harness is the correctness proof; this is
// purely convenience so you can save from the concourse without poking
// dev_remote. Manual saves use slot 1 so they never clobber the autosave.
void draw_save_widget(PlayerState& player) {
    ImGui::SetCursorScreenPos(ImVec2(16, 92));
    if (ImGui::Button("Save game (slot 1)", ImVec2(180, 28))) {
        sfx::ui_click();
        if (savegame::save(player, 1))
            std::printf("[save] manual save -> slot 1 (%lld cr)\n",
                        (long long)player.credits);
    }

    // Last-autosave readout from slot 0's metadata (no full load).
    const savegame::SlotInfo auto0 = savegame::peek(savegame::k_autosave_slot);
    char info[160];
    if (auto0.exists) {
        char when[32] = "?";
        const std::time_t t = (std::time_t)auto0.timestamp;
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
        std::snprintf(info, sizeof(info), "Last autosave: %s  (%s)",
                      auto0.label.c_str(), when);
    } else {
        std::snprintf(info, sizeof(info), "Last autosave: (none yet)");
    }
    ImGui::SetCursorScreenPos(ImVec2(16, 124));
    ImGui::TextColored(ImVec4(0.78f, 0.67f, 0.24f, 1.0f), "%s", info);
}

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

    draw_context_strip(dl, player, g_def);
    draw_save_widget(player);
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

    const std::string path = "assets/bases/" + base_id + "/base.json";
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

void build(PlayerState& player, Ship* player_ship, Docking& d, Camera& cam, GameState& gs) {
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
        docking::launch(d, cam, gs, player);
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
