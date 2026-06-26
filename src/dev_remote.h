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
//
// Everything else 404s.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

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

} // namespace dev_remote
