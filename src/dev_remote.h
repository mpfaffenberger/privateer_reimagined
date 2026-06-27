#pragma once
// -----------------------------------------------------------------------------
// dev_remote.h — tiny in-game HTTP control server for dev tooling.
//
// Lets an external process (Code Puppy, a shell script, curl) drive the
// camera, grab screenshots, and inspect runtime state while the game is
// running. Only bound to 127.0.0.1 so we don't expose anything to the
// network. Intended for development — in a ship build we'd compile it
// out entirely.
//
// Threading model
// ---------------
// A single background thread runs the HTTP accept loop. Incoming
// requests mutate nothing directly — they enqueue Commands to a
// mutex-guarded deque which the main thread drains once per frame via
// `drain_commands()`. Commands that need a reply (e.g. screenshot,
// state query) carry a condition variable the HTTP thread blocks on
// until the main thread fulfills them. This keeps every game-state
// mutation on the main thread, so nothing else in the renderer needs
// to know about thread safety.
//
// Endpoints (all respond with JSON unless noted)
//   GET  /state          → { pos, euler, fps, system }
//   POST /camera/set     → { x, y, z, yaw, pitch, roll }   (all optional)
//   POST /screenshot     → saves a PNG to /tmp/np_shot.png,
//                          returns { path, ok }
//   POST /project        → projects mesh-local 3D points into screen
//                          UV using the live render matrices. Body is a
//                          flat float array [x,y,z,nx,ny,nz, ...] (6 per
//                          point: position + outward normal). Returns
//                          { results: [ {u,v,front,facing}, ... ] } where
//                          u,v are 0..1 image-space (0,0 = top-left),
//                          `front` = in front of camera, `facing` =
//                          normal points toward camera (rough visibility).
//   POST /voice/say      → enqueue a voiced comm line. Body:
//                          { faction, category, to_player? } where
//                          to_player defaults true. Returns { ok } or
//                          { ok:false, error } on a bad faction/category.
//   POST /comm/bark      → enqueue a hostile comm bark. Body { faction }.
//                          Returns { ok } or { ok:false, error }.
//   GET  /ships          → { ships: [ { id, faction, dist, alive,
//                          aggro_player, provoked }, ... ] } — a snapshot
//                          of the NPC ships near the player, published
//                          once per Flight frame by the host.
//   POST /cargo/give     → add cargo to the player's hold. Body
//                          { commodity, units }. Validated + enqueued;
//                          the host's registered hook runs it on the main
//                          thread. Returns { ok } or { ok:false, error }.
//   POST /spawn          → spawn an NPC near the player. Body
//                          { faction, class, dist? } (dist defaults to a
//                          host-chosen distance). Faction validated via
//                          faction::from_name; enqueued and run on the
//                          main thread by the registered spawn hook.
//                          Returns { ok } or { ok:false, error }.
//   GET  /loot           → { loot: [ { dist, id, kind, rarity }, ... ] }
//   GET  /inventory      → { items: [...], cargo_used, cargo_cap }
//   POST /kill           → kill a ship. Body { id? } (0/missing = nearest).
//   POST /tractor/pull   → pull in-range loot into the hold.
//   POST /inventory/sell → sell the unified-hold item at { index:N }.
//                          Returns { ok:true }.
//
// Everything else 404s.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct Camera;

namespace dev_remote {

// Bring the server up on the given port. No-op if already running or if
// binding fails (e.g. another instance of the game is still listening).
// The failure path is non-fatal on purpose — the game should keep running
// even if the dev channel can't open.
void start(int port = 8765);

// Tear down the server cleanly. Safe to call unconditionally at shutdown.
void stop();

// Apply queued commands to game state. Call once per frame from the
// main thread, before you query the camera to render.
void drain_commands(Camera& cam);

// Publish the latest FPS reading for `/state` queries. Called whenever
// the game recomputes it (once per second is enough).
void publish_fps(int fps);

// End-of-frame hook. If a screenshot command is in flight, this is
// where it gets taken (after the current frame's swap has completed)
// and the requesting HTTP thread is woken up. Must be called AFTER
// `sg_commit()` so the final composited frame is on the window.
void maybe_capture_screenshot();

// For `/state` — main thread calls this each frame to publish the
// current system name (we don't hold a reference to the StarSystem
// to keep coupling low).
void publish_system_name(const char* name);

// Publish the render matrices the `/project` endpoint needs: the current
// view-projection, the model matrix of the mesh being projected against
// (the capture scene has exactly one placed mesh at the origin), and the
// world-space camera position (for the normal-facing visibility test).
// Call once per frame from the main thread, after the camera + placed
// mesh transforms are finalised for the frame.
void publish_render_matrices(const HMM_Mat4& view_proj,
                             const HMM_Mat4& model,
                             HMM_Vec3 cam_pos);

// ---------------------------------------------------------------------------
// /ships snapshot (issue #103)
// ---------------------------------------------------------------------------
// A flat, decoupled view of one NPC ship near the player. The host builds
// a vector of these from its ShipRegistry each Flight frame and hands it to
// publish_ships(); the HTTP thread serves the latest copy to GET /ships.
// dev_remote never sees the real Ship/AppState types — same decoupling
// trick `encounters` uses with its SpawnFn.
struct ShipInfo {
    uint32_t    id;
    std::string faction;
    float       dist;
    bool        alive;
    bool        aggro_player;
    bool        provoked;
};

// Publish the latest near-player ship snapshot for GET /ships. Called once
// per Flight frame from the main thread. Stored mutex-guarded; the HTTP
// thread serialises whatever the most recent call left behind.
void publish_ships(const std::vector<ShipInfo>& ships);

// ---------------------------------------------------------------------------
// Registered host hooks (issue #103) — the decoupling seam.
// ---------------------------------------------------------------------------
// POST /cargo/give validates + enqueues a command; drain_commands invokes
// this hook on the main thread with (commodity_id, units). The host wires
// it to player::add_cargo.
void set_cargo_give_hook(std::function<void(std::string commodity, int units)> hook);

// POST /spawn validates the faction, enqueues a command; drain_commands
// invokes this hook on the main thread with (faction, class, dist). The
// host wires it to its existing debug-spawn recipe.
void set_spawn_hook(std::function<void(std::string faction, std::string klass, float dist)> hook);

// ---------------------------------------------------------------------------
// /loot snapshot — in-world loot drops near the player (the loot loop).
// ---------------------------------------------------------------------------
// A flat view of one live loot drop. The host builds a vector of these from
// loot::all() each Flight frame and hands it to publish_loot(); the HTTP
// thread serves the latest copy to GET /loot. Same decoupling trick as
// ShipInfo — dev_remote never sees the real LootDrop/InventoryItem types.
struct LootInfo {
    float       dist;
    std::string id;
    int         kind;
    int         rarity;
};

// Publish the latest near-player loot snapshot for GET /loot. Called once
// per Flight frame from the main thread; stored mutex-guarded.
void publish_loot(const std::vector<LootInfo>& loot);

// ---------------------------------------------------------------------------
// /inventory snapshot — the player's unified-hold items + cargo usage.
// ---------------------------------------------------------------------------
// A flat view of one unified-hold item. The host builds these from
// g.player.items each Flight frame and hands them to publish_inventory()
// along with the current cargo usage + capacity.
struct ItemInfo {
    std::string id;
    int         kind;
    int         rarity;
    int         qty;
};

// Publish the latest inventory snapshot for GET /inventory. Called once
// per Flight frame from the main thread; stored mutex-guarded.
void publish_inventory(const std::vector<ItemInfo>& items, int used, int cap);

// POST /kill enqueues a command; drain_commands invokes this hook on the
// main thread with the requested ship id (0 = "nearest alive non-player").
// The host wires it to its kill-processing path.
void set_kill_hook(std::function<void(uint32_t id)> hook);

// POST /tractor/pull enqueues a command; drain_commands invokes this hook
// on the main thread. The host wires it to loot::try_pull.
void set_tractor_pull_hook(std::function<void()> hook);

// POST /inventory/sell enqueues a command; drain_commands invokes this hook
// on the main thread with the requested item index. The host wires it to
// inventory::sell_item(g.player, index).
void set_inventory_sell_hook(std::function<void(int index)> hook);

} // namespace dev_remote
