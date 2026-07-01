#pragma once
// -----------------------------------------------------------------------------
// base_screens.h — Privateer-style 2D base screens (concourse + sub-screens).
//
// What replaces the np-eag.2 "LANDED @ x — base screens TBD" stub. While
// the game is in GameMode::Landed (game_state.h) we render a full-screen
// concourse image for the docked base, draw clickable hotspot regions over
// it, and let the player walk a tiny screen stack:
//
//   Concourse (the hub)
//     ├─ Bar               — fixers (fixers.cpp registers the body, #137)
//     ├─ Commodity Exchange— buy/sell goods            (np-9cu.2 fills in)
//     ├─ Ship Dealer       — hulls                     (np-9cu.3 fills in)
//     ├─ Equipment         — guns/shields/engines      (np-9cu.3 fills in)
//     ├─ Mission Computer  — generated missions        (np-zte.1 fills in)
//     └─ Launch            — back to Flight (docking::launch)
//
// Data-driven, no hardcoded layouts: every base is described by
// assets/bases/<base_id>/base.json — its display name, faction, concourse
// art path, and a list of hotspots. Each hotspot's rect is in NORMALIZED
// 0..1 screen coords so the same data lays out correctly at any resolution.
// The C++ here knows the *grammar* (a screen enum, a hotspot list, a stack);
// the *content* (which base has which doors, where) lives entirely in JSON.
//
// Lifecycle (driven by main.cpp at the mode boundaries):
//   enter(base_id)  — on transition INTO Landed. Loads base.json + the
//                     concourse PNG, resets the stack to {Concourse}.
//   build(...)      — once per Landed frame, between simgui_new_frame() and
//                     simgui_render(). Draws the current screen + handles
//                     hotspot clicks / Back / Launch.
//   exit()          — on transition OUT of Landed. Frees the GPU texture.
//
// Integration seam for the shop tasks: the CommodityExchange / ShipDealer /
// Equipment / MissionComputer screens are placeholders today. Rather than
// have np-9cu.2/.3/zte.1 reach into this file, each fillable screen exposes
// a registration hook (register_screen). Those tasks call register_screen()
// from their own module's init and draw their UI into the body the framework
// hands them — the framework still owns the background, the title, and the
// Back affordance. No hook registered => the clearly-labelled stub draws.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <functional>
#include <string>

struct PlayerState;
struct Docking;
struct Camera;
struct GameState;
struct Ship;

// The screens reachable from a base. Concourse is the hub; the rest are
// pushed onto the stack by hotspot clicks. Launch is special — it isn't a
// screen you sit on, it triggers docking::launch and returns to Flight.
// Names match the enum-name strings used in base.json's hotspot "target".
enum class BaseScreen {
    Concourse = 0,
    Bar,
    CommodityExchange,
    ShipDealer,
    Equipment,
    MissionComputer,
    MercenariesGuild,   // #16 — paid combat-mission board
    MerchantsGuild,     // #16 — paid trade-mission board
    CargoHold,          // Phase 4f — sell loot/salvage from the unified hold
    Launch,
    LandingPad,         // hangar room shown on dock; door leads to Concourse
    // --- action zones (not navigable screens) -------------------------------
    // Editor-placeable hotspots that trigger an in-room ACTION rather than a
    // screen change. Used by the Commodity Exchange (display panel + buy/sell
    // /next/prev zones placed via the F3 editor).
    CommodityDisplay,
    CommodityBuy,
    CommoditySell,
    CommodityNext,
    CommodityPrev,
    // Generic "click to open this room's shop menu" zone (placed over e.g.
    // the ship dealer's character, or a guild's mission-computer console).
    // Rooms WITH this zone start on the art/character and reveal the shop
    // UI (or mission board) only after a click.
    OpenMenu,
};

// Everything a sub-screen renderer needs without reaching into AppState.
// Bundled so the integration hooks (below) have one stable parameter even
// as the shops grow. `player` is mutable — the commodity/ship/equipment
// screens spend credits and swap cargo/equipment through it.
struct BaseContext {
    std::string  base_id;       // "achilles" — the assets/bases/<id> key
    std::string  display_name;  // "Achilles Mining Base"
    std::string  faction;       // "Confederation" (display string)
    PlayerState* player = nullptr;
    // The player's in-flight Ship (np-zte.2) — its hull armor is the
    // transient state the Repair service restores. nullptr-safe: a screen
    // that needs it (Equipment's repair section) skips when absent.
    Ship*        player_ship = nullptr;
    // Concourse/market archetype ("mining", "pirate", ...) from base.json.
    // The fixer framework's predicate placement keys on this (#137: Goodin
    // appears at ANY mining base — M22).
    std::string  archetype;
};

namespace base_screens {

// Integration seam (np-9cu.2/.3/zte.1). A screen renderer draws ONLY the
// body content — the framework has already drawn the background, the screen
// title, and will draw the Back affordance afterwards. Register from the
// owning module's init; unregistered fillable screens fall back to a stub.
using ScreenHook = std::function<void(BaseContext&)>;
void register_screen(BaseScreen screen, ScreenHook hook);

// Look up an editor-placed ACTION zone in the currently-active room by its
// target. Writes the normalized [x,y,w,h] rect into out_xywh and returns
// true if a zone with that target was placed; false otherwise. Used by the
// Commodity Exchange to find its display/buy/sell/next/prev hotspots.
bool current_room_zone(BaseScreen target, float out_xywh[4]);

// Called on the transition INTO Landed. Loads assets/bases/<base_id>/base.json
// and its concourse PNG, and resets the screen stack to {Concourse}. Safe to
// call with an unknown/empty base_id — degrades to a labelled fallback.
void enter(const std::string& base_id);

// Called once per Landed frame, between simgui_new_frame() and
// simgui_render(). Draws the active screen, fields hotspot clicks (pushing
// sub-screens / popping Back / launching), and shows the player-context
// strip on the concourse. Needs the docking trio so the Launch hotspot can
// call docking::launch(d, cam, gs, player).
void build(PlayerState& player, Ship* player_ship, Docking& d, Camera& cam, GameState& gs,
           HMM_Vec3 sun_pos);

// Called on the transition OUT of Landed. Releases the concourse texture.
void exit();

// Escape policy helper for main.cpp's event_cb: if a sub-screen is open,
// pop it back to the Concourse and return true (Escape consumed). On the
// Concourse it returns false — the caller decides what Escape does there
// (today: launch, the intended exit).
bool handle_escape();

// ---- agentic-testing seams (dev_remote /base + POST /base/screen) ----------
// Navigate the Landed screen stack by enum name ("Bar", "MissionComputer").
// "Concourse" resets the stack to the hub; any other art screen pushes
// (deduped — already-current is a no-op). Returns false when not landed,
// the name is unknown, or the target isn't navigable (Launch/zones).
bool dev_open(const std::string& screen_name);

// Snapshot of where the player is in the base UI: the base id + the screen
// stack as names, hub-first. Empty base_id == not currently entered.
struct DevState {
    std::string              base_id;
    std::string              archetype;   // concourse archetype ("mining", ...)
    std::vector<std::string> stack;
};
DevState dev_state();

} // namespace base_screens
