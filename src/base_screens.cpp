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
#include "room_anim.h"     // sky + baked sprite layers for animated rooms (#515)
#include "ship.h"          // Ship (g_player_ship->klass) for the parked-ship pose
#include "ship_class.h"    // ship_class::all() for the landing-pad ship picker
#include "ship_sprite.h"   // atlas load + choose_ship_sprite_frame_by_angles
#include "sprite.h"        // SpriteArt (per-frame hull texture)
#include "world_clock.h"
#include <unordered_map>

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
    std::string          archetype;    // market archetype -> concourse art set
    std::string          art_path;     // resolved "assets/..." path to the PNG
    std::vector<Hotspot> hotspots;
    bool                 loaded = false;
};

// ---- per-archetype room art (assets/concourse/<type>/concourse.json) -------
// A placed transition: a clickable rect (normalized) that moves to `target`.
// Authored per base-TYPE (positioned on the shared art) by the F3 editor;
// at runtime a concourse link is only shown if THIS base actually offers
// that service (intersected with base.json's hotspots).
struct Link {
    BaseScreen target = BaseScreen::Concourse;
    float      rect[4] = {0, 0, 0, 0};
};
// One room of a base's art set: a painted background, optional animation
// around it (room_anim.h: drifting sky + baked sprite layers), and the
// transition links placed on it (doors to other rooms / launch).
struct Room {
    bool                   valid = false;
    TextureSlot            background;
    room_anim::RoomAnim    anim;
    std::vector<Link>      links;
    // Landing pad only: animation for its per-hull full-frame composites
    // ("composite" key, paths templated on {plate}; #553).
    room_anim::RoomAnimDef composite;
};
constexpr int kBaseScreenCount = (int)BaseScreen::OpenMenu + 1;
// The parked ship shown on the landing pad: which hull, at what view-sphere
// angle, drawn into what normalized rect. Authored with the F4 picker and
// saved per base-type to links.json under "_ship".
struct ShipPose {
    std::string cls;                         // ship class name ("" = none)
    float az = 35.0f, el = -20.0f;           // view-sphere azimuth/elevation
    float rect[4] = {0.34f, 0.42f, 0.32f, 0.34f};
    bool  set = false;
};
struct ConcourseSet {
    bool        valid = false;       // concourse room loaded
    std::string type;
    Room        rooms[kBaseScreenCount];   // indexed by (int)BaseScreen
    // Per-ship-class parked poses for THIS base type's landing pad. Each
    // ship you fly gets its own authored angle + size; runtime renders the
    // pose matching the player's current hull.
    std::vector<ShipPose> ship_poses;
};

// ---- module state -----------------------------------------------------------
// Deliberately file-local statics (mirrors sfx.cpp's g_sfx). One base is
// "active" at a time — you can only be landed at one place.
BaseDef                  g_def;
TextureSlot              g_art;            // concourse PNG; valid==false if missing
ConcourseSet             g_concourse;      // animated WCU art set (optional, by archetype)
TextureSlot              g_landing_composite; // installed full-frame ship × archetype art
room_anim::RoomAnim      g_landing_anim;      // its animation (Room::composite)
TextureSlot              g_landing_preview;   // temporary F11 full-frame override
std::string              g_landing_composite_key;
std::string              g_landing_preview_ship;

void release_texture(TextureSlot& texture) {
    if (!texture.valid) return;
    sg_destroy_view(texture.view);
    sg_destroy_image(texture.image);
    texture = TextureSlot{};
}

void release_landing_composite() {
    release_texture(g_landing_composite);
    room_anim::release(g_landing_anim);
}

// "Shop menu open" gate for rooms with an OpenMenu zone (ship dealer, guild
// consoles). The room shows its art + NPC until the player clicks the zone,
// then the shop UI draws over an opaque backdrop. Reset when the active
// screen changes. Declared early — draw_subscreen()'s Back button and the
// build() gating logic both read/write it.
bool       g_menu_open = false;
BaseScreen g_menu_screen = BaseScreen::Concourse;

// ---- landing-pad ship preview (F4 picker) -----------------------------------
// One atlas held at a time for the parked-ship preview, plus the shared art
// cache its frames point into. Persist across bases so re-entering is cheap.
std::unordered_map<std::string, SpriteArt> g_ship_art;
ShipSpriteAtlas g_pose_atlas;
std::string     g_pose_atlas_cls;     // which class g_pose_atlas currently holds
int             g_pose_class_idx = 0; // editor cursor into ship_class::all()
bool            g_pose_edit = false;  // F4 editor active (landing room only)

// Find the authored pose for a ship class in the active set (or null).
ShipPose* find_pose(const std::string& cls) {
    if (cls.empty()) return nullptr;
    for (ShipPose& p : g_concourse.ship_poses) if (p.cls == cls) return &p;
    return nullptr;
}

// Lazily (re)load the atlas for `cls`; returns null on miss/empty.
ShipSpriteAtlas* pose_atlas_for(const std::string& cls) {
    if (cls.empty()) return nullptr;
    if (g_pose_atlas_cls != cls) {
        g_pose_atlas = ShipSpriteAtlas{};
        const std::string stem = resolve_ship_atlas_stem("ships/" + cls + "/atlas_manifest");
        if (!load_ship_sprite_atlas(stem, g_pose_atlas, g_ship_art)) {
            g_pose_atlas_cls = "!" + cls;   // mark failed so we don't retry every frame
            return nullptr;
        }
        g_pose_atlas_cls = cls;
    } else if (!g_pose_atlas_cls.empty() && g_pose_atlas_cls[0] == '!') {
        return nullptr;
    }
    return g_pose_atlas.frames.empty() ? nullptr : &g_pose_atlas;
}
// draw_ship_pose is defined further down (needs ScreenSize); forward-declared
// here so the early statics block stays together.
struct ScreenSize;
void draw_ship_pose(ImDrawList* dl, const ScreenSize& ss, const ShipPose& pose);
std::vector<BaseScreen>  g_stack;          // Concourse always sits at index 0
std::array<ScreenHook, kBaseScreenCount> g_hooks{};   // index by (int)BaseScreen
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
        case BaseScreen::Library:          return "Library";
        case BaseScreen::ResearchComputer: return "Research Computer";
        case BaseScreen::Office:           return "Office";
        case BaseScreen::Launch:           return "Launch";
        case BaseScreen::LandingPad:       return "Landing Pad";
        default:                           return "?";
    }
}

// The JSON enum token for a screen (matches parse_target). Used to serialize
// placed links from the F3 transition editor.
const char* target_token(BaseScreen s) {
    switch (s) {
        case BaseScreen::Bar:               return "Bar";
        case BaseScreen::CommodityExchange: return "CommodityExchange";
        case BaseScreen::ShipDealer:        return "ShipDealer";
        case BaseScreen::Equipment:         return "Equipment";
        case BaseScreen::MissionComputer:   return "MissionComputer";
        case BaseScreen::MercenariesGuild:  return "MercenariesGuild";
        case BaseScreen::MerchantsGuild:    return "MerchantsGuild";
        case BaseScreen::CargoHold:         return "CargoHold";
        case BaseScreen::Library:           return "Library";
        case BaseScreen::ResearchComputer:  return "ResearchComputer";
        case BaseScreen::Office:            return "Office";
        case BaseScreen::Launch:            return "Launch";
        case BaseScreen::Concourse:         return "Concourse";
        case BaseScreen::LandingPad:         return "LandingPad";
        case BaseScreen::CommodityDisplay:  return "CommodityDisplay";
        case BaseScreen::CommodityBuy:      return "CommodityBuy";
        case BaseScreen::CommoditySell:     return "CommoditySell";
        case BaseScreen::CommodityNext:     return "CommodityNext";
        case BaseScreen::CommodityPrev:     return "CommodityPrev";
        case BaseScreen::OpenMenu:          return "OpenMenu";
        default:                            return "Concourse";
    }
}

// Action zones are editor-placeable but NOT navigable (handled in-room).
bool is_action(BaseScreen s) {
    return s == BaseScreen::CommodityDisplay || s == BaseScreen::CommodityBuy ||
           s == BaseScreen::CommoditySell   || s == BaseScreen::CommodityNext ||
           s == BaseScreen::CommodityPrev   || s == BaseScreen::OpenMenu;
}

// Stable short key for a room in the art manifest / links.json (lowercase,
// distinct from the link target tokens). Only rooms that can carry art.
const char* room_key(BaseScreen s) {
    switch (s) {
        case BaseScreen::Concourse:         return "concourse";
        case BaseScreen::LandingPad:        return "landing";
        case BaseScreen::Bar:               return "bar";
        case BaseScreen::CommodityExchange: return "commodity";
        case BaseScreen::ShipDealer:        return "shipdealer";
        case BaseScreen::Equipment:         return "equipment";
        case BaseScreen::MercenariesGuild:  return "mercguild";
        case BaseScreen::MerchantsGuild:    return "merchguild";
        case BaseScreen::MissionComputer:   return "missions";
        case BaseScreen::CargoHold:         return "cargohold";
        case BaseScreen::Library:           return "library";
        case BaseScreen::ResearchComputer:  return "researchcomputer";
        case BaseScreen::Office:            return "office";
        default:                            return "";
    }
}
// The set of screens that can have an art room (drives load/save/free loops).
constexpr BaseScreen kArtRooms[] = {
    BaseScreen::Concourse, BaseScreen::LandingPad, BaseScreen::Bar,
    BaseScreen::CommodityExchange, BaseScreen::ShipDealer, BaseScreen::Equipment,
    BaseScreen::MercenariesGuild, BaseScreen::MerchantsGuild,
    BaseScreen::MissionComputer, BaseScreen::CargoHold, BaseScreen::Library,
    BaseScreen::ResearchComputer, BaseScreen::Office,
};

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
    if (s == "Library")           { out = BaseScreen::Library;           return true; }
    if (s == "ResearchComputer")  { out = BaseScreen::ResearchComputer;  return true; }
    if (s == "Office")            { out = BaseScreen::Office;            return true; }
    if (s == "Launch")            { out = BaseScreen::Launch;            return true; }
    if (s == "Concourse")         { out = BaseScreen::Concourse;         return true; }
    if (s == "LandingPad")        { out = BaseScreen::LandingPad;        return true; }
    if (s == "CommodityDisplay")  { out = BaseScreen::CommodityDisplay;  return true; }
    if (s == "CommodityBuy")      { out = BaseScreen::CommodityBuy;      return true; }
    if (s == "CommoditySell")     { out = BaseScreen::CommoditySell;     return true; }
    if (s == "CommodityNext")     { out = BaseScreen::CommodityNext;     return true; }
    if (s == "CommodityPrev")     { out = BaseScreen::CommodityPrev;     return true; }
    if (s == "OpenMenu")          { out = BaseScreen::OpenMenu;          return true; }
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
    if (is_action(target)) return;   // action zones are handled in-room
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

// Forward decl: the link renderer lives further down (after the art-set
// types), but the concourse hub below needs it.
void draw_links(ImDrawList* dl, const ScreenSize& ss, const Room& room, bool filter);

// Pop one level off the screen stack (never below the Concourse at index 0).
// Shared by every "<  BACK" affordance so the log line + guard rail live in
// one place.
void pop_screen_stack() {
    if (g_stack.size() <= 1) return;
    g_stack.pop_back();
    std::printf("[base] pop -> %s (stack depth %zu)\n",
                screen_name(g_stack.back()), g_stack.size());
}

// Draws a "<  BACK" button at the bottom-left, wired to pop_screen_stack().
// Shared between draw_subscreen() and the OpenMenu-gated showroom state so
// every screen — including the pre-click guild/ship-dealer rooms — has a
// visible way back to the Concourse, not just Escape.
void draw_back_to_concourse_button(const ScreenSize& ss) {
    ImGui::SetCursorScreenPos(ImVec2(28, ss.h - 56));
    if (ImGui::Button("<  BACK", ImVec2(120, 32))) {
        sfx::ui_click();
        pop_screen_stack();
    }
}

// OpenMenu turns an authored showroom into a modal service overlay. Give that
// state its own readable backdrop and an unmistakable dismissal control;
// relying on a bottom-left generic Back button made the dealer feel like a
// navigation dead end, especially at short window heights.
void draw_open_menu_chrome(ImDrawList* dl, const ScreenSize& ss) {
    dl->AddRectFilled(ImVec2(16, 16), ImVec2(ss.w - 16, ss.h - 16),
                      IM_COL32(8, 12, 22, 224), 7.0f);
    dl->AddRect(ImVec2(16, 16), ImVec2(ss.w - 16, ss.h - 16),
                kAmberDim, 7.0f, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(ss.w - 148, 24));
    if (ImGui::Button("CLOSE  X", ImVec2(116, 32))) {
        g_menu_open = false;
        sfx::ui_click();
        std::printf("[base] close %s menu\n", screen_name(g_menu_screen));
    }
}

// Draw the concourse hub. When the art set has placed transition links
// (positioned on the real art by the F3 editor), use those — filtered to the
// services THIS base offers. Otherwise fall back to base.json's grid hotspots.
void draw_concourse(ImDrawList* dl, const ScreenSize& ss, PlayerState& player) {
    if (g_concourse.valid && !g_concourse.rooms[(int)BaseScreen::Concourse].links.empty()) {
        draw_links(dl, ss, g_concourse.rooms[(int)BaseScreen::Concourse], /*filter=*/true);
        return;
    }
    for (size_t i = 0; i < g_def.hotspots.size(); ++i) {
        const Hotspot& hs = g_def.hotspots[i];
        const float px = hs.x * ss.w;
        const float py = hs.y * ss.h;
        const float pw = hs.w * ss.w;
        const float ph = hs.h * ss.h;

        ImGui::PushID((int)i);
        ImGui::SetCursorScreenPos(ImVec2(px, py));
        ImGui::InvisibleButton("##base_hotspot", ImVec2(pw, ph));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) activate(hs.target);
        ImGui::PopID();

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
                         &player, g_player_ship, g_def.archetype };
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
                         &player, g_player_ship, g_def.archetype };
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
    // Title bar — only on screens WITHOUT painted room art. An art room (the
    // commodity console, the bar, the guild offices) carries its own framing
    // in the picture, so the overlaid "<base> - <screen>" title just clutters
    // it; suppress it there.
    const bool has_art = g_concourse.valid && g_concourse.rooms[(int)cur].valid;
    if (!has_art) {
        char title[96];
        std::snprintf(title, sizeof(title), "%s  -  %s",
                      g_def.display_name.empty() ? "Base" : g_def.display_name.c_str(),
                      screen_name(cur));
        dl->AddText(ImVec2(28, 24), kAmber, title);
        dl->AddLine(ImVec2(28, 48), ImVec2(ss.w - 28, 48), kAmberDim, 1.0f);
    }

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
                             &player, g_player_ship, g_def.archetype };
            hook(ctx);
        } else {
            const char* msg = (cur == BaseScreen::Bar)
                ? "BAR - fixers::register_bar_screen() was not called (#137)"
                : "screen stub - wired in by a later task";
            draw_centered(dl, msg, 0, 0, ss.w, ss.h, kWhite);
        }
    }

    // Back affordance. On a gated room (ship dealer / guild consoles) with
    // its menu open, Back closes the menu back to the art+NPC showroom —
    // matching Escape's handle_escape() policy. Otherwise it pops the
    // screen stack to the Concourse as before.
    ImGui::SetCursorScreenPos(ImVec2(28, ss.h - 56));
    if (ImGui::Button("<  BACK", ImVec2(120, 32))) {
        sfx::ui_click();
        if (g_menu_open && g_menu_screen == cur) g_menu_open = false;
        else pop_screen_stack();
    }
}

// Load an animated concourse set (assets/concourse/<type>/concourse.json) if
// one exists for this base's archetype. Best-effort: any miss leaves
// g_concourse.valid==false and the caller falls back to the static PNG.
// Parse one room object. Legacy GIF-derived "overlays" are intentionally not
// loaded (#205): their tiny frames blur badly against the high-resolution
// room art. Animation now comes from the room's "sky"/"layers" keys.
bool load_room(const std::string& dir, const json::Value& r, Room& out) {
    if (!r.is_object()) return false;
    if (!room_anim::load(dir, r, out.background, out.anim)) return false;
    if (const json::Value* c = r.find("composite"))
        room_anim::parse_room_anim(*c, out.composite);
    // Placed transition links: [{ "target": "Bar", "rect": [x,y,w,h] }, ...].
    if (const json::Value* ls = r.find("links"); ls && ls->is_array()) {
        for (const json::Value& l : ls->as_array()) {
            if (!l.is_object()) continue;
            BaseScreen tgt;
            if (!parse_target(l["target"].string_or(""), tgt)) continue;
            Link lk; lk.target = tgt;
            if (const json::Value* rc = l.find("rect"); rc && rc->is_array() && rc->as_array().size() == 4)
                for (int i = 0; i < 4; ++i) lk.rect[i] = (*rc)[(size_t)i].as_float();
            out.links.push_back(lk);
        }
    }
    out.valid = true;
    return true;
}

// Load an animated concourse set (assets/concourse/<type>/concourse.json) with
// its concourse room and optional landing-pad room.
void load_concourse_set(const std::string& archetype) {
    g_concourse = ConcourseSet{};
    if (archetype.empty()) return;
    const std::string dir  = "assets/concourse/" + archetype + "/";
    const json::Value root = json::parse_file(dir + "concourse.json");
    if (!root.is_object()) return;
    const json::Value* rooms = root.find("rooms");
    if (!rooms || !rooms->is_object()) return;
    int loaded = 0;
    for (BaseScreen s : kArtRooms) {
        if (const json::Value* r = rooms->find(room_key(s)))
            if (load_room(dir, *r, g_concourse.rooms[(int)s])) ++loaded;
    }
    if (!g_concourse.rooms[(int)BaseScreen::Concourse].valid) return;

    // Editor overrides: links.json (authored by the F3 transition editor)
    // replaces a room's link list when present, keyed by room_key.
    const json::Value lj = json::parse_file(dir + "links.json");
    if (lj.is_object()) {
        for (BaseScreen s : kArtRooms) {
            const json::Value* arr = lj.find(room_key(s));
            if (!arr || !arr->is_array()) continue;
            Room& rm = g_concourse.rooms[(int)s];
            rm.links.clear();
            for (const json::Value& l : arr->as_array()) {
                if (!l.is_object()) continue;
                BaseScreen tgt;
                if (!parse_target(l["target"].string_or(""), tgt)) continue;
                Link lk; lk.target = tgt;
                if (const json::Value* rc = l.find("rect"); rc && rc->is_array() && rc->as_array().size() == 4)
                    for (int i = 0; i < 4; ++i) lk.rect[i] = (*rc)[(size_t)i].as_float();
                rm.links.push_back(lk);
            }
        }
        // Per-ship landing-pad poses. Accepts the new "_ships" array and the
        // legacy single "_ship" object.
        auto load_pose = [&](const json::Value& sp) {
            if (!sp.is_object()) return;
            ShipPose pose;
            pose.cls = sp["class"].string_or("");
            if (pose.cls.empty()) return;
            pose.az = (float)sp["az"].number_or(pose.az);
            pose.el = (float)sp["el"].number_or(pose.el);
            if (const json::Value* rc = sp.find("rect"); rc && rc->is_array() && rc->as_array().size() == 4)
                for (int i = 0; i < 4; ++i) pose.rect[i] = (*rc)[(size_t)i].as_float();
            pose.set = true;
            g_concourse.ship_poses.push_back(pose);
        };
        if (const json::Value* arr = lj.find("_ships"); arr && arr->is_array())
            for (const json::Value& sp : arr->as_array()) load_pose(sp);
        if (const json::Value* sp = lj.find("_ship")) load_pose(*sp);
    }
    g_concourse.type  = archetype;
    g_concourse.valid = true;
    std::printf("[base] concourse set '%s': %d room(s) loaded\n",
                archetype.c_str(), loaded);
}

// Draw an animated plate: the sky (and anything out in it) behind the plate,
// the painted plate stretched to fill, then the sprite layers on top.
void draw_plate(ImDrawList* dl, const ScreenSize& ss, const TextureSlot& plate,
                const room_anim::RoomAnim& anim) {
    const double t = ImGui::GetTime();
    room_anim::draw_under(dl, ss.w, ss.h, anim, t);
    if (plate.valid)
        dl->AddImage(simgui_imtextureid(plate.view), ImVec2(0, 0), ImVec2(ss.w, ss.h));
    room_anim::draw_over(dl, ss.w, ss.h, anim, t);
}

void draw_room(ImDrawList* dl, const ScreenSize& ss, const Room& room) {
    draw_plate(dl, ss, room.background, room.anim);
}

// Short label shown inside a transition box (the door's destination).
const char* link_label(BaseScreen t) {
    switch (t) {
        case BaseScreen::Concourse:         return "ENTER";
        case BaseScreen::LandingPad:        return "LANDING PAD";
        case BaseScreen::Launch:            return "LAUNCH";
        case BaseScreen::Bar:               return "BAR";
        case BaseScreen::CommodityExchange: return "COMMODITIES";
        case BaseScreen::ShipDealer:        return "SHIP DEALER";
        case BaseScreen::Equipment:         return "EQUIPMENT";
        case BaseScreen::MissionComputer:   return "MISSIONS";
        case BaseScreen::MercenariesGuild:  return "MERCENARIES";
        case BaseScreen::MerchantsGuild:    return "MERCHANTS";
        case BaseScreen::CargoHold:         return "CARGO HOLD";
        case BaseScreen::CommodityDisplay:  return "DISPLAY";
        case BaseScreen::CommodityBuy:      return "BUY MODE";
        case BaseScreen::CommoditySell:     return "SELL MODE";
        case BaseScreen::CommodityNext:     return "NEXT";
        case BaseScreen::CommodityPrev:     return "PREV";
        case BaseScreen::OpenMenu:          return "VIEW SHIPS";
        default:                            return "?";
    }
}

// OpenMenu's label depends on WHICH room it's gating — the ship dealer's
// showroom reveals a ship list, a guild's console reveals its mission
// board. Keyed on the current screen rather than hardcoded per-target so
// new OpenMenu rooms just add a case here.
const char* open_menu_label(BaseScreen cur) {
    switch (cur) {
        case BaseScreen::ShipDealer:        return "VIEW SHIPS";
        case BaseScreen::MercenariesGuild:
        case BaseScreen::MerchantsGuild:    return "VIEW MISSIONS";
        default:                            return "OPEN";
    }
}

// Does THIS base offer the service a concourse link points to? Reuses the
// per-base facility list (base.json hotspots). Always-available rooms
// (Launch, LandingPad, Concourse, CargoHold) pass unconditionally.
bool base_offers(BaseScreen t) {
    if (t == BaseScreen::Launch || t == BaseScreen::LandingPad ||
        t == BaseScreen::Concourse || t == BaseScreen::CargoHold ||
        t == BaseScreen::MissionComputer || t == BaseScreen::Bar)
        return true;
    for (const Hotspot& hs : g_def.hotspots) if (hs.target == t) return true;
    return false;
}

// Draw + handle a room's transition links as clickable labelled boxes.
// `filter` gates each link on base_offers() (used by the concourse so a
// guild-less base doesn't show a guild door).
void draw_links(ImDrawList* dl, const ScreenSize& ss, const Room& room, bool filter) {
    for (size_t i = 0; i < room.links.size(); ++i) {
        const Link& lk = room.links[i];
        if (is_action(lk.target)) continue;   // handled inside the room, not nav
        if (filter && !base_offers(lk.target)) continue;
        const float px = lk.rect[0]*ss.w, py = lk.rect[1]*ss.h;
        const float pw = lk.rect[2]*ss.w, ph = lk.rect[3]*ss.h;
        // IDs must identify the ZONE, not its destination. Multiple authored
        // doors may legitimately target the same room; using link_label as
        // the ID made later duplicates impossible to hover or click.
        ImGui::PushID((int)i);
        ImGui::SetCursorScreenPos(ImVec2(px, py));
        ImGui::InvisibleButton("##room_link", ImVec2(pw, ph));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) activate(lk.target);
        ImGui::PopID();
        // Outside edit mode the doorways are INVISIBLE — the painted art is
        // the affordance. On hover we reveal ONLY the label text (no box).
        if (hovered)
            draw_centered(dl, link_label(lk.target), px, py, pw, ph, kWhite);
    }
}

// Draw the parked ship: nearest authored frame to (az,el), aspect-fit +
// centred inside the pose rect. (Defined here — ScreenSize is complete now.)
void draw_ship_pose(ImDrawList* dl, const ScreenSize& ss, const ShipPose& pose) {
    if (pose.cls.empty()) return;
    ShipSpriteAtlas* atlas = pose_atlas_for(pose.cls);
    if (!atlas) return;
    const ShipSpriteFrame* f = choose_ship_sprite_frame_by_angles(*atlas, pose.az, pose.el);
    if (!f || !f->art || !f->art->hull.valid) return;
    const float rx = pose.rect[0]*ss.w, ry = pose.rect[1]*ss.h;
    const float rw = pose.rect[2]*ss.w, rh = pose.rect[3]*ss.h;
    float aw = rw, ah = rh;
    if (f->art->hull_w > 0 && f->art->hull_h > 0) {
        const float ar = (float)f->art->hull_w / (float)f->art->hull_h;
        if (aw / ah > ar) aw = ah * ar; else ah = aw / ar;
    }
    const float cx = rx + rw*0.5f, cy = ry + rh*0.5f;
    const ImVec2 pmin(cx - aw*0.5f, cy - ah*0.5f), pmax(cx + aw*0.5f, cy + ah*0.5f);
    dl->AddImage(simgui_imtextureid(f->art->hull.view), pmin, pmax);
    if (f->art->lights.valid)
        dl->AddImage(simgui_imtextureid(f->art->lights.view), pmin, pmax);
}

// Draw the landing-pad room: art + its transition links (door -> Concourse,
// Launch). No facility filter — every base can launch + enter.
void draw_landing(ImDrawList* dl, const ScreenSize& ss) {
    Room& lp = g_concourse.rooms[(int)BaseScreen::LandingPad];
    const std::string player_class = (g_player_ship && g_player_ship->klass)
                                   ? g_player_ship->klass->name : std::string();

    // An F11 preview may deliberately show a ship other than the player's.
    // It is a complete frame, so never draw the dynamic hull over it.
    bool complete_frame = g_landing_preview.valid;
    if (complete_frame) {
        dl->AddImage(simgui_imtextureid(g_landing_preview.view),
                     ImVec2(0, 0), ImVec2(ss.w, ss.h));
    } else if (!player_class.empty()) {
        const std::string key = g_concourse.type + "|" + player_class;
        if (key != g_landing_composite_key) {
            release_landing_composite();
            g_landing_composite_key = key;
            const std::string dir  = "assets/concourse/" + g_concourse.type + "/";
            const std::string path = dir + "landing_ships/" + player_class + ".png";
            if (std::filesystem::is_regular_file(path))
                room_anim::load(dir, room_anim::for_plate(lp.composite, player_class), path,
                                g_landing_composite, g_landing_anim);
        }
        complete_frame = g_landing_composite.valid;
        if (complete_frame) draw_plate(dl, ss, g_landing_composite, g_landing_anim);
    }

    if (!complete_frame) {
        draw_room(dl, ss, lp);
        if (!player_class.empty()) {
            if (const ShipPose* p = find_pose(player_class)) draw_ship_pose(dl, ss, *p);
            else { ShipPose def; def.cls = player_class; draw_ship_pose(dl, ss, def); }
        }
    }
    // Navigation remains data-driven and clickable above every visual path.
    draw_links(dl, ss, lp, /*filter=*/false);
}

// ---- F3 transition editor ---------------------------------------------------
// Author the placement of room transition links directly on the art. Edits
// the CURRENT room's links in place; Cmd/Ctrl+S writes them to
// assets/concourse/<type>/links.json (preferred by the loader on next entry).
struct EditState { bool on = false; int sel = -1; };
EditState g_edit;

void save_links() {
    const std::string path = "assets/concourse/" + g_concourse.type + "/links.json";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { std::printf("[base] editor: cannot write %s\n", path.c_str()); return; }
    std::fprintf(f, "{\n");
    // One array per art room, keyed by room_key. Rooms with no links are
    // still emitted (empty) so the file documents the full room set.
    for (size_t ri = 0; ri < (sizeof(kArtRooms)/sizeof(kArtRooms[0])); ++ri) {
        const Room& rm = g_concourse.rooms[(int)kArtRooms[ri]];
        const bool last = (ri + 1 == sizeof(kArtRooms)/sizeof(kArtRooms[0]));
        std::fprintf(f, "  \"%s\": [", room_key(kArtRooms[ri]));
        for (size_t i = 0; i < rm.links.size(); ++i) {
            const Link& lk = rm.links[i];
            std::fprintf(f, "%s\n    { \"target\": \"%s\", \"rect\": [%.5f, %.5f, %.5f, %.5f] }",
                         i ? "," : "", target_token(lk.target),
                         lk.rect[0], lk.rect[1], lk.rect[2], lk.rect[3]);
        }
        (void)last;
        std::fprintf(f, "%s],\n", rm.links.empty() ? "" : "\n  ");
    }
    // Per-ship landing-pad poses (P picker).
    std::fprintf(f, "  \"_ships\": [");
    for (size_t i = 0; i < g_concourse.ship_poses.size(); ++i) {
        const ShipPose& sp = g_concourse.ship_poses[i];
        std::fprintf(f,
            "%s\n    { \"class\": \"%s\", \"az\": %.2f, \"el\": %.2f, "
            "\"rect\": [%.5f, %.5f, %.5f, %.5f] }",
            i ? "," : "", sp.cls.c_str(), sp.az, sp.el,
            sp.rect[0], sp.rect[1], sp.rect[2], sp.rect[3]);
    }
    std::fprintf(f, "%s]\n", g_concourse.ship_poses.empty() ? "" : "\n  ");
    std::fprintf(f, "}\n");
    std::fclose(f);
    std::printf("[base] editor: saved %s\n", path.c_str());
}

void draw_link_editor(ImDrawList* dl, const ScreenSize& ss, Room& room) {
    ImGuiIO& io = ImGui::GetIO();
    // --- hotkeys ---
    if (ImGui::IsKeyPressed(ImGuiKey_A)) {
        Link lk; lk.target = BaseScreen::Bar;
        lk.rect[0] = 0.42f; lk.rect[1] = 0.42f; lk.rect[2] = 0.16f; lk.rect[3] = 0.14f;
        room.links.push_back(lk); g_edit.sel = (int)room.links.size() - 1;
    }
    if (g_edit.sel >= (int)room.links.size()) g_edit.sel = -1;
    if (g_edit.sel >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_X))) {
        room.links.erase(room.links.begin() + g_edit.sel); g_edit.sel = -1;
    }
    if (g_edit.sel >= 0) {
        int d = ImGui::IsKeyPressed(ImGuiKey_RightBracket) ? 1
              : ImGui::IsKeyPressed(ImGuiKey_LeftBracket)  ? -1 : 0;
        if (d) {
            int t = ((int)room.links[g_edit.sel].target + d + kBaseScreenCount) % kBaseScreenCount;
            room.links[g_edit.sel].target = (BaseScreen)t;
        }
    }
    if ((io.KeySuper || io.KeyCtrl) && ImGui::IsKeyPressed(ImGuiKey_S)) save_links();

    // --- draggable boxes ---
    for (int i = 0; i < (int)room.links.size(); ++i) {
        Link& lk = room.links[i];
        const float px = lk.rect[0]*ss.w, py = lk.rect[1]*ss.h;
        const float pw = lk.rect[2]*ss.w, ph = lk.rect[3]*ss.h;
        const float hsz = 22.0f;
        ImGui::PushID(i);
        // Submit the corner RESIZE handle FIRST so it wins hit-priority where
        // it overlaps the body button (ImGui gives the earlier-submitted item
        // precedence at overlapping pixels).
        ImGui::SetCursorScreenPos(ImVec2(px + pw - hsz, py + ph - hsz));
        ImGui::InvisibleButton("rs", ImVec2(hsz, hsz));
        const bool resizing = ImGui::IsItemActive();
        if (ImGui::IsItemActivated()) g_edit.sel = i;
        if (resizing && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            float nw = lk.rect[2] + io.MouseDelta.x / ss.w;
            float nh = lk.rect[3] + io.MouseDelta.y / ss.h;
            lk.rect[2] = nw > 0.03f ? nw : 0.03f;
            lk.rect[3] = nh > 0.03f ? nh : 0.03f;
        }
        // Body button (move) — submitted after, so the corner above takes
        // precedence; the body claims the rest of the rect.
        ImGui::SetCursorScreenPos(ImVec2(px, py));
        ImGui::InvisibleButton("body", ImVec2(pw > 8 ? pw : 8, ph > 8 ? ph : 8));
        if (ImGui::IsItemActivated()) g_edit.sel = i;
        if (!resizing && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            lk.rect[0] += io.MouseDelta.x / ss.w;
            lk.rect[1] += io.MouseDelta.y / ss.h;
        }
        ImGui::PopID();
        const bool seld = (i == g_edit.sel);
        const ImU32 col = seld ? IM_COL32(120, 255, 160, 255) : IM_COL32(255, 210, 80, 220);
        dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), col, 3.0f, 0, seld ? 3.0f : 1.5f);
        dl->AddRectFilled(ImVec2(px + pw - hsz, py + ph - hsz), ImVec2(px + pw, py + ph), col, 2.0f);
        draw_centered(dl, target_token(lk.target), px, py, pw, ph, col);
    }
    // --- target palette: click a label to assign it to the selected box ---
    if (g_edit.sel >= 0 && g_edit.sel < (int)room.links.size()) {
        static const BaseScreen kOpts[] = {
            BaseScreen::Bar, BaseScreen::CommodityExchange, BaseScreen::ShipDealer,
            BaseScreen::Equipment, BaseScreen::MissionComputer,
            BaseScreen::MercenariesGuild, BaseScreen::MerchantsGuild,
            BaseScreen::CargoHold, BaseScreen::Concourse, BaseScreen::LandingPad,
            BaseScreen::Launch,
            // Commodity Exchange action zones:
            BaseScreen::CommodityDisplay, BaseScreen::CommodityBuy,
            BaseScreen::CommoditySell, BaseScreen::CommodityNext,
            BaseScreen::CommodityPrev, BaseScreen::OpenMenu,
        };
        const float bx = 12.0f, bw = 210.0f, bh = 26.0f;
        float by = 40.0f;
        dl->AddText(ImVec2(bx, by - 18), IM_COL32(120, 255, 160, 255), "ASSIGN TARGET:");
        for (BaseScreen opt : kOpts) {
            ImGui::PushID((int)opt + 5000);
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            ImGui::InvisibleButton("opt", ImVec2(bw, bh));
            const bool hov = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) room.links[g_edit.sel].target = opt;
            ImGui::PopID();
            const bool curr = (room.links[g_edit.sel].target == opt);
            const ImU32 fill = curr ? IM_COL32(40, 110, 70, 230)
                                    : hov ? IM_COL32(50, 60, 80, 230)
                                          : IM_COL32(20, 26, 36, 220);
            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), fill, 4.0f);
            dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh),
                        curr ? IM_COL32(120, 255, 160, 255) : IM_COL32(90, 100, 120, 200), 4.0f);
            dl->AddText(ImVec2(bx + 10, by + 5),
                        curr ? IM_COL32(180, 255, 200, 255) : IM_COL32(210, 215, 225, 255),
                        target_token(opt));
            by += bh + 4.0f;
        }
    }

    // --- help bar ---
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(ss.w, 30), IM_COL32(0, 0, 0, 190));
    char help[220];
    std::snprintf(help, sizeof(help),
        "TRANSITION EDITOR  type=%s  room=%s   A:add  X:del  click box:select  click palette:set target  "
        "[ ]:cycle  drag:move  corner:resize  Cmd/Ctrl+S:save  F3:exit",
        g_concourse.type.c_str(),
        (&room == &g_concourse.rooms[(int)BaseScreen::LandingPad]) ? "landing" : "concourse");
    dl->AddText(ImVec2(12, 8), IM_COL32(120, 255, 160, 255), help);
}

// ---- F4 landing-pad ship picker --------------------------------------------
// Cycle ships ( , / . ), scrub azimuth/elevation (arrows), drag/resize the
// parked-ship rect, Cmd/Ctrl+S to save into links.json "_ship".
void draw_ship_pose_editor(ImDrawList* dl, const ScreenSize& ss) {
    const std::vector<ShipClass>& classes = ship_class::all();
    ImGuiIO& io = ImGui::GetIO();
    if (classes.empty()) return;

    g_pose_class_idx = (g_pose_class_idx % (int)classes.size() + (int)classes.size()) % (int)classes.size();
    if (ImGui::IsKeyPressed(ImGuiKey_Comma))
        g_pose_class_idx = (g_pose_class_idx - 1 + (int)classes.size()) % (int)classes.size();
    if (ImGui::IsKeyPressed(ImGuiKey_Period))
        g_pose_class_idx = (g_pose_class_idx + 1) % (int)classes.size();
    const std::string cls = classes[g_pose_class_idx].name;

    // Edit the saved pose for THIS class if one exists; otherwise a transient
    // default that we only COMMIT to the set once the user actually changes
    // something (so merely browsing ships doesn't litter the file).
    ShipPose* saved = find_pose(cls);
    ShipPose  scratch;
    if (!saved) scratch.cls = cls;
    ShipPose& pose = saved ? *saved : scratch;
    bool edited = false;

    const float az_step = io.KeyShift ? 5.0f : 15.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  { pose.az -= az_step; edited = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { pose.az += az_step; edited = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    { pose.el += az_step; edited = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  { pose.el -= az_step; edited = true; }
    if (pose.az > 180.0f)  pose.az -= 360.0f;
    if (pose.az < -180.0f) pose.az += 360.0f;
    if (pose.el >  89.0f)  pose.el =  89.0f;
    if (pose.el < -89.0f)  pose.el = -89.0f;

    // Render the parked ship at the current pose.
    draw_ship_pose(dl, ss, pose);

    // Move/resize handles on the pose rect (corner-first, like the link editor).
    const float px = pose.rect[0]*ss.w, py = pose.rect[1]*ss.h;
    const float pw = pose.rect[2]*ss.w, ph = pose.rect[3]*ss.h;
    const float hsz = 22.0f;
    ImGui::PushID("shippose");
    ImGui::SetCursorScreenPos(ImVec2(px + pw - hsz, py + ph - hsz));
    ImGui::InvisibleButton("rs", ImVec2(hsz, hsz));
    const bool resizing = ImGui::IsItemActive();
    if (resizing && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float nw = pose.rect[2] + io.MouseDelta.x / ss.w;
        float nh = pose.rect[3] + io.MouseDelta.y / ss.h;
        pose.rect[2] = nw > 0.05f ? nw : 0.05f;
        pose.rect[3] = nh > 0.05f ? nh : 0.05f;
        edited = true;
    }
    ImGui::SetCursorScreenPos(ImVec2(px, py));
    ImGui::InvisibleButton("body", ImVec2(pw > 8 ? pw : 8, ph > 8 ? ph : 8));
    if (!resizing && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        pose.rect[0] += io.MouseDelta.x / ss.w;
        pose.rect[1] += io.MouseDelta.y / ss.h;
        edited = true;
    }
    ImGui::PopID();
    const ImU32 cyan = IM_COL32(120, 220, 255, 230);
    dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), cyan, 3.0f, 0, 2.0f);
    dl->AddRectFilled(ImVec2(px + pw - hsz, py + ph - hsz), ImVec2(px + pw, py + ph), cyan, 2.0f);

    // Commit a brand-new pose to the set the first time the user edits it.
    if (edited) { pose.set = true; if (!saved) g_concourse.ship_poses.push_back(pose); }
    if ((io.KeySuper || io.KeyCtrl) && ImGui::IsKeyPressed(ImGuiKey_S)) save_links();

    // Help / status bar.
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(ss.w, 30), IM_COL32(0, 0, 0, 190));
    char help[220];
    std::snprintf(help, sizeof(help),
        "SHIP PICKER  ship=%s (%d/%d)  az=%.0f  el=%.0f  saved=%zu   ,/.:ship  arrows:az/el "
        "(Shift=fine)  drag:move  corner:resize  Cmd/Ctrl+S:save  P:exit",
        cls.c_str(), g_pose_class_idx + 1, (int)classes.size(), pose.az, pose.el,
        g_concourse.ship_poses.size());
    dl->AddText(ImVec2(12, 8), IM_COL32(120, 220, 255, 255), help);
}

} // namespace

// ---- public API -------------------------------------------------------------

void register_screen(BaseScreen screen, ScreenHook hook) {
    g_hooks[(int)screen] = std::move(hook);
    std::printf("[base] registered screen hook: %s\n", screen_name(screen));
}

bool current_room_zone(BaseScreen target, float out_xywh[4]) {
    if (!g_concourse.valid || g_stack.empty()) return false;
    const Room& rm = g_concourse.rooms[(int)g_stack.back()];
    for (const Link& lk : rm.links) {
        if (lk.target != target) continue;
        for (int i = 0; i < 4; ++i) out_xywh[i] = lk.rect[i];
        return true;
    }
    return false;
}

bool current_room_info(CurrentRoomInfo& out) {
    if (g_stack.empty() || g_def.id.empty() || !g_concourse.valid) return false;
    const BaseScreen screen = g_stack.back();
    const char* key = room_key(screen);
    if (!key[0]) return false;
    out.base_id = g_def.id; out.display_name = g_def.display_name;
    out.faction = g_def.faction; out.archetype = g_concourse.type; out.room = key;
    out.asset_path = "assets/concourse/" + g_concourse.type + "/" + key + "_bg.png";
    return true;
}

bool reload_current_room_texture(const std::string& png_path) {
    if (g_stack.empty() || !g_concourse.valid) return false;
    Room& room = g_concourse.rooms[(int)g_stack.back()];
    TextureSlot replacement{};
    if (!load_texture_png(png_path, replacement)) return false;
    release_texture(room.background);
    room.background = replacement;
    room.valid = true;
    // Baked layers and the sky mask are encoded against the old plate's
    // pixels; over new art they would print patches of the old painting.
    if (!room.anim.layers.empty() || room.anim.sky_fill.valid)
        std::printf("[base] %s animation dropped: baked against the previous art\n",
                    room_key(g_stack.back()));
    room_anim::release(room.anim);
    std::printf("[base] hot reloaded %s from %s\n", room_key(g_stack.back()), png_path.c_str());
    return true;
}

bool current_landing_info(CurrentLandingInfo& out) {
    CurrentRoomInfo room;
    if (!current_room_info(room) || room.room != "landing") return false;
    out.base_id = room.base_id; out.display_name = room.display_name;
    out.faction = room.faction; out.archetype = room.archetype;
    out.background_path = room.asset_path;
    out.player_ship_class = (g_player_ship && g_player_ship->klass)
                          ? g_player_ship->klass->name : std::string();
    return true;
}

bool preview_current_landing_composite(const std::string& png_path,
                                       const std::string& ship_class) {
    CurrentLandingInfo info;
    if (!current_landing_info(info) || ship_class.empty()) return false;
    TextureSlot replacement{};
    if (!load_texture_png(png_path, replacement)) return false;
    release_texture(g_landing_preview);
    g_landing_preview = replacement;
    g_landing_preview_ship = ship_class;
    std::printf("[base] previewing %s landing composite from %s\n",
                ship_class.c_str(), png_path.c_str());
    return true;
}

void clear_current_landing_composite_preview() {
    release_texture(g_landing_preview);
    g_landing_preview_ship.clear();
    // An install may have replaced the file behind the installed texture.
    // Force lazy discovery/reload on the next landing frame.
    release_landing_composite();
    g_landing_composite_key.clear();
}

void enter(const std::string& base_id) {
    // Reset any prior base's GPU texture before loading the new one.
    if (g_art.valid) { /* slot reuse handled by load below via destroy */ }
    clear_current_landing_composite_preview();
    g_def = BaseDef{};
    g_def.id = base_id;
    g_stack.assign(1, BaseScreen::Concourse);
    g_launch_pending = false;
    g_menu_open = false;

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

    // Visual identity normally follows the economy archetype, but canonical
    // one-off locations (New Detroit, Oxford, Perry) can override artwork
    // without corrupting their commodity-pricing behavior.
    g_def.archetype.clear();
    if (const json::Value* mk = root.find("market"); mk && mk->is_object()) {
        if (mk->contains("archetype")) g_def.archetype = (*mk)["archetype"].string_or("");
    }
    if (root.contains("visual_archetype"))
        g_def.archetype = root["visual_archetype"].string_or(g_def.archetype);

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
    // Prefer an animated WCU concourse set for this archetype if one exists;
    // otherwise the static PNG above remains the background.
    load_concourse_set(g_def.archetype);
    // If the set has a hangar room, you arrive there first and click the
    // door to enter the concourse (matches the original base flow).
    if (g_concourse.valid && g_concourse.rooms[(int)BaseScreen::LandingPad].valid)
        g_stack.assign(1, BaseScreen::LandingPad);

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

    // Background. Every room with art (concourse, landing, AND the service
    // rooms) blits its art; the service shop hooks then draw their UI on top.
    // Rooms without art fall back to the static PNG / dark gradient.
    const bool has_room = g_concourse.valid && g_concourse.rooms[(int)cur].valid;
    if (has_room) {
        draw_room(dl, ss, g_concourse.rooms[(int)cur]);
    } else if (cur == BaseScreen::Concourse && g_art.valid) {
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

    // F3 toggles the transition editor on any art room; F4 toggles the
    // landing-pad ship picker (landing room only). They're mutually exclusive.
    const bool landing = (cur == BaseScreen::LandingPad) && g_concourse.valid;
    if (ImGui::IsKeyPressed(ImGuiKey_F3) && has_room) { g_edit.on = !g_edit.on; g_pose_edit = false; }
    // 'P' (not F4 — macOS eats F4 for Launchpad) toggles the ship picker.
    if (ImGui::IsKeyPressed(ImGuiKey_P) && landing && !g_edit.on) {
        g_pose_edit = !g_pose_edit;
        if (g_pose_edit) {   // open the picker on the player's current hull
            const std::string pcls = (g_player_ship && g_player_ship->klass)
                                   ? g_player_ship->klass->name : std::string();
            const std::vector<ShipClass>& cl = ship_class::all();
            for (int i = 0; i < (int)cl.size(); ++i)
                if (cl[i].name == pcls) { g_pose_class_idx = i; break; }
        }
    }

    if (g_pose_edit && landing) {
        draw_ship_pose_editor(dl, ss);
    } else if (g_edit.on && has_room) {
        draw_link_editor(dl, ss, g_concourse.rooms[(int)cur]);
    } else if (cur == BaseScreen::LandingPad) {
        draw_landing(dl, ss);
    } else if (cur == BaseScreen::Concourse) {
        draw_concourse(dl, ss, player);
    } else {
        // Service screen. If the room has an OpenMenu zone (e.g. the ship
        // dealer's character, or a guild's mission computer), show art + NPC
        // first and reveal the shop UI only after the player clicks the
        // zone — then over an opaque panel.
        float zr[4];
        const bool gated = current_room_zone(BaseScreen::OpenMenu, zr);
        if (gated && g_menu_screen != cur) g_menu_open = false;
        if (gated && !g_menu_open) {
            const float px = zr[0]*ss.w, py = zr[1]*ss.h, pw = zr[2]*ss.w, ph = zr[3]*ss.h;
            ImGui::SetCursorScreenPos(ImVec2(px, py));
            ImGui::InvisibleButton("##openmenu", ImVec2(pw, ph));
            if (ImGui::IsItemClicked()) { g_menu_open = true; g_menu_screen = cur; sfx::ui_click(); }
            if (ImGui::IsItemHovered())
                draw_centered(dl, open_menu_label(cur), px, py, pw, ph, kWhite);
            draw_links(dl, ss, g_concourse.rooms[(int)cur], /*filter=*/false);  // back door
            draw_back_to_concourse_button(ss);  // visible affordance, not just Escape
        } else {
            // A service room without OpenMenu still owns authored navigation
            // links (for example Ship Dealer -> Equipment). Previously this
            // branch jumped straight to the shop renderer, so every F3 box
            // in a non-gated service room existed in memory but was never
            // submitted to ImGui. Submit links first so deliberate overlaps
            // with shop UI remain usable as authored navigation targets.
            if (!gated && has_room) {
                draw_links(dl, ss, g_concourse.rooms[(int)cur], /*filter=*/false);
            }
            // No opaque backdrop here anymore — the room's real pre-rendered
            // background (already blitted above via draw_room/has_room) stays
            // visible behind the shop UI, same as Commodity Exchange/Bar
            // already did. Matches the original game's look: the trading/
            // dealer/guild computer is an overlay ON the scene, not a screen
            // that replaces it.
            if (gated) {
                g_menu_screen = cur;
                draw_open_menu_chrome(dl, ss);
            }
            draw_subscreen(dl, ss, cur, player);
        }
    }

    // Gemini Lives clock: base screens are where a pilot notices time pass.
    // Keep it on the two arrival/hub rooms; service UIs already have dense
    // chrome and can read the same day through their BaseContext later.
    if (cur == BaseScreen::LandingPad || cur == BaseScreen::Concourse) {
        const std::string stamp = "STARDATE " + world_clock::stardate_string(player.day);
        const ImVec2 text_size = ImGui::CalcTextSize(stamp.c_str());
        const ImVec2 lo(ss.w - text_size.x - 34.0f, 18.0f);
        const ImVec2 hi(ss.w - 18.0f, 18.0f + text_size.y + 14.0f);
        dl->AddRectFilled(lo, hi, IM_COL32(5, 8, 14, 190), 3.0f);
        dl->AddRect(lo, hi, kAmberDim, 3.0f);
        dl->AddText(ImVec2(lo.x + 8.0f, lo.y + 7.0f), kAmber, stamp.c_str());
    }

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
    release_texture(g_art);
    clear_current_landing_composite_preview();
    // Release every room's GPU textures (background + animation).
    auto free_room = [](Room& rm) {
        release_texture(rm.background);
        room_anim::release(rm.anim);
    };
    for (Room& rm : g_concourse.rooms) free_room(rm);
    g_concourse = ConcourseSet{};
    g_stack.clear();
    std::printf("[base] exit '%s'\n", g_def.id.c_str());
}

bool dev_open(const std::string& screen_name) {
    if (g_stack.empty()) return false;   // not landed / not entered
    BaseScreen target;
    if (!parse_target(screen_name, target)) return false;
    // "Launch" is an ACTION, not a screen: arm the same deferred-launch
    // path the concourse hotspot uses (consumed by the next build()).
    if (target == BaseScreen::Launch) {
        g_launch_pending = true;
        std::printf("[base] dev_open -> LAUNCH\n");
        return true;
    }
    // Only navigable art screens — the click-zone pseudo-targets aren't
    // screens you can stand on.
    if (target == BaseScreen::OpenMenu ||
        (int)target > (int)BaseScreen::LandingPad) return false;
    if (target == BaseScreen::Concourse) {
        g_stack.assign(1, BaseScreen::Concourse);
    } else if (g_stack.back() != target) {
        g_stack.push_back(target);
    }
    std::printf("[base] dev_open -> %s (stack depth %zu)\n",
                screen_name.c_str(), g_stack.size());
    return true;
}

DevState dev_state() {
    DevState s;
    if (g_stack.empty()) return s;
    s.base_id   = g_def.id;
    s.archetype = g_def.archetype;
    for (BaseScreen sc : g_stack) s.stack.emplace_back(screen_name(sc));
    return s;
}

bool handle_escape() {
    if (g_menu_open) {   // close an open shop menu back to the showroom first
        g_menu_open = false;
        sfx::ui_click();
        return true;
    }
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
