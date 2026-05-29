// -----------------------------------------------------------------------------
// mesh_orient_editor.cpp — implementation. See header.
// -----------------------------------------------------------------------------

#include "mesh_orient_editor.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace mesh_orient_editor {
namespace {

// Persistent across frames. -1 = nothing selected yet (auto-pick on
// first build call so the editor is useful immediately after F5).
bool s_visible    = false;
int  s_selected   = -1;

// Strip the directory + suffix off an obj path so the dropdown shows
// "paradigm" instead of "meshes/ships_wcnews/paradigm.obj". Pure
// cosmetic — we still mutate the underlying PlacedMesh by index.
std::string short_label(const std::string& obj_path) {
    namespace fs = std::filesystem;
    return fs::path(obj_path).stem().string();
}

// Wrap a Python-style "[a, b, c]" round-tripping the slider values
// exactly so the user can paste straight into PER_SHIP_OVERRIDES.
void format_override_snippet(char* out, size_t out_sz,
                              const std::string& stem,
                              const PlacedMesh& m) {
    std::snprintf(out, out_sz,
        "\"%s\": {\"euler_deg\": [%g, %g, %g], "
        "\"length_meters\": %g, \"tint\": [%g, %g, %g]},",
        stem.c_str(),
        m.euler_deg.X, m.euler_deg.Y, m.euler_deg.Z,
        m.scale,           // PlacedMesh stores final scale; not length, but close enough for the showroom
        m.body_tint.X, m.body_tint.Y, m.body_tint.Z);
}

} // namespace

void init() {
    s_visible  = false;
    s_selected = -1;
    std::printf("[mesh_orient_editor] ready — F5 to toggle\n");
}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_F5) {
        s_visible = !s_visible;
        return true;
    }
    return false;
}

void build(std::vector<PlacedMesh>& meshes) {
    if (!s_visible) return;
    if (meshes.empty()) {
        // Editor open but nothing to edit — show a small placeholder so
        // the F5 toggle is obviously working.
        ImGui::SetNextWindowSize(ImVec2(360, 70), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Mesh Orientation Editor (F5)", &s_visible)) {
            ImGui::TextWrapped("No PlacedMesh in this scene.");
        }
        ImGui::End();
        return;
    }

    // Auto-select the first mesh on first open; clamp on scene reload.
    if (s_selected < 0 || s_selected >= (int)meshes.size()) s_selected = 0;

    ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mesh Orientation Editor (F5)", &s_visible)) {
        ImGui::End();
        return;
    }

    // ── Mesh picker ───────────────────────────────────────────────────
    const std::string current_label = short_label(meshes[s_selected].name);
    if (ImGui::BeginCombo("mesh", current_label.c_str())) {
        for (int i = 0; i < (int)meshes.size(); ++i) {
            const std::string label = short_label(meshes[i].name);
            const bool sel = (i == s_selected);
            if (ImGui::Selectable(label.c_str(), sel)) s_selected = i;
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    PlacedMesh& m = meshes[s_selected];

    ImGui::Separator();

    // ── Sliders ───────────────────────────────────────────────────────
    // SliderFloat range -180..180; precision %.1f is enough for eyeball
    // tuning and matches the kind of values that end up in
    // PER_SHIP_OVERRIDES (paradigm landed on integers like [100, 0, 135]).
    ImGui::SliderFloat("pitch (X)", &m.euler_deg.X, -180.0f, 180.0f, "%.1f deg");
    ImGui::SliderFloat("yaw   (Y)", &m.euler_deg.Y, -180.0f, 180.0f, "%.1f deg");
    ImGui::SliderFloat("roll  (Z)", &m.euler_deg.Z, -180.0f, 180.0f, "%.1f deg");

    if (ImGui::Button("Reset to 0,0,0")) {
        m.euler_deg = HMM_Vec3{0.0f, 0.0f, 0.0f};
    }
    ImGui::SameLine();
    if (ImGui::Button("Snap to nearest 5deg")) {
        auto snap = [](float v) { return std::round(v / 5.0f) * 5.0f; };
        m.euler_deg.X = snap(m.euler_deg.X);
        m.euler_deg.Y = snap(m.euler_deg.Y);
        m.euler_deg.Z = snap(m.euler_deg.Z);
    }
    ImGui::SameLine();
    if (ImGui::Button("Snap to nearest 1deg")) {
        m.euler_deg.X = std::round(m.euler_deg.X);
        m.euler_deg.Y = std::round(m.euler_deg.Y);
        m.euler_deg.Z = std::round(m.euler_deg.Z);
    }

    ImGui::Separator();

    // ── Auxiliary transform tweaks (handy when you're already here) ───
    ImGui::SliderFloat("scale", &m.scale, 0.01f, 50.0f, "%.3f");
    ImGui::SliderFloat3("tint",  &m.body_tint.X, 0.0f, 3.0f, "%.2f");

    ImGui::Separator();

    // ── Paste-back snippet ────────────────────────────────────────────
    char snippet[256];
    const std::string stem = short_label(m.name);
    format_override_snippet(snippet, sizeof(snippet), stem, m);

    ImGui::TextUnformatted("Paste into PER_SHIP_OVERRIDES "
                           "(tools/regenerate_mesh_showroom.py):");
    // Read-only multi-line so the snippet wraps cleanly + can be selected
    // with mouse for copy. ImGuiInputTextFlags_ReadOnly stops accidental
    // edits; AutoSelectAll makes a single click grab the whole snippet.
    ImGui::InputTextMultiline("##snippet", snippet, sizeof(snippet),
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3),
                              ImGuiInputTextFlags_ReadOnly);
    if (ImGui::Button("Copy to clipboard")) {
        ImGui::SetClipboardText(snippet);
    }

    ImGui::End();
}

} // namespace mesh_orient_editor
