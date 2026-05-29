#pragma once
// -----------------------------------------------------------------------------
// mesh_orient_editor.h — in-engine ImGui tool for dialling per-PlacedMesh
// orientation (and a few related transform fields) live, while the engine
// is running.
//
// Workflow: press F5. A small ImGui window opens with:
//   * dropdown of every PlacedMesh in the scene (labelled by obj path)
//   * three sliders for euler_deg.{X=pitch, Y=yaw, Z=roll} in degrees
//   * one slider for length_meters (handy when comparing scale)
//   * a "copy override" read-only text field with a ready-to-paste
//     PER_SHIP_OVERRIDES entry for tools/regenerate_mesh_showroom.py
//
// All edits are LIVE — the selected PlacedMesh's fields are mutated in
// place, so Mike sees the ship rotate as he drags the slider. Closing
// the window leaves the values where they are; they don't persist to
// disk (we explicitly want them to evaporate so the showroom generator
// stays the single source of truth, and the human paste-back step
// forces an audit trail).
//
// Sibling of sprite_light_editor (F2) — same one-cpp-one-header shape,
// same handle_event() / build() lifecycle.
// -----------------------------------------------------------------------------

#include "mesh_render.h"
#include "sokol_app.h"

#include <vector>

namespace mesh_orient_editor {

// Set up persistent editor state. Cheap; safe after debug_panel::init().
void init();

// Toggle / dismiss handler. Returns true if the event was consumed so
// main.cpp can skip its own input pipeline for this event. Currently
// only the F5 key is hot-keyed.
bool handle_event(const sapp_event* e);

// Per-frame UI build. Call between simgui_new_frame() and the final
// swapchain pass. `meshes` is the live list the renderer is about to
// draw — we mutate it directly so changes appear next frame. No-op
// when the editor window is hidden.
void build(std::vector<PlacedMesh>& meshes);

} // namespace mesh_orient_editor
