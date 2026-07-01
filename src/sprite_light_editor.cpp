// -----------------------------------------------------------------------------
// sprite_light_editor.cpp — ImGui light-placement tool.
//
// Architecture:
//   * File-scope state: visible flag, selected sprite/light indices,
//     whether the user is mid-drag.
//   * build() does everything: sprite picker, click-capture image widget,
//     per-light side panel, save/reload buttons.
//   * JSON load/save is hand-rolled serialization — the format is small
//     enough (array of flat objects) that bringing in a writer library
//     would be overkill. Read uses the existing json.h parser.
// -----------------------------------------------------------------------------

#include "sprite_light_editor.h"
#include "sprite_light_rec.h"
#include "json.h"

#include "imgui.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace sprite_light_editor {

// ---------------------------------------------------------------------------
// Persistent editor state.
// ---------------------------------------------------------------------------

// Visibility: hidden by default. F2 toggles. Starting visible would clutter
// every screenshot — same rationale as debug_panel's g_visible.
static bool g_visible = false;

// Which entry is being edited. The editor presents a flat list:
// indices [0, sprites.size())               → placed scene SpriteObjects
// indices [sprites.size(), total)            → entries from extra_arts
// `-1` before any item exists. Clamped otherwise.
static int  g_sel_sprite = 0;

// Index into the selected target's lights vector, or -1 if no selection.
// Cleared when switching targets or deleting the currently-selected light.
static int  g_sel_light  = -1;

// ---- Auto-save state -------------------------------------------------
// Per-target snapshot of last-saved lights, keyed by the target's STRING
// NAME (not its address!). The earlier pointer-keyed implementation broke
// catastrophically because main.cpp rebuilds the EditableArt vector every
// frame, so `&extra.name` is a different address each frame even though
// it holds the same string content. The pointer compare always succeeded
// at "target switched" → snapshot reset every frame → user edits never
// triggered a save (except when the heap allocator happened to reuse
// addresses, which was random and lost authoring across hundreds of cells).
//
// String-keyed map fixes that AND naturally tracks all targets at once:
// every frame we walk the full editable list, compare in-memory to
// snapshot, save any that differ. Touching cells you're not currently
// viewing is a non-issue because their `cur_lights` doesn't change unless
// something modified them.
static std::unordered_map<std::string, std::vector<LightSpot>> g_save_snapshots;

static bool lights_equal(const std::vector<LightSpot>& a,
                         const std::vector<LightSpot>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const LightSpot& x = a[i];
        const LightSpot& y = b[i];
        // Field-by-field. NOT memcmp — LightSpot has trailing padding
        // bytes after `kind` that vary based on previous stack contents,
        // and those would defeat change-detection while looking byte-
        // different on consecutive frames.
        if (x.u != y.u || x.v != y.v
         || x.color.X != y.color.X || x.color.Y != y.color.Y
         || x.color.Z != y.color.Z
         || x.size != y.size || x.hz != y.hz
         || x.phase != y.phase || x.kind != y.kind) return false;
    }
    return true;
}

// Pixel-to-world zoom of the sprite preview. 1.0 = the PNG renders at its
// native pixel size; higher zooms help hit small hull features (antenna
// tips, radar dish). Clamped to [1, 4].
static float g_zoom = 1.0f;

// ---- Sticky last-used spot properties ------------------------------------
//
// New light spots inherit colour/kind/hz/phase from the last placed-or-edited
// spot, so authoring runs of identical lights (e.g. five green strobes on
// the same radar dish, or twelve red nav-edges along a wing) doesn't require
// re-picking the same colour + kind + hz + phase from scratch every click.
// Size is intentionally NOT sticky — the user's existing workflow is to
// place at the default 5 px and rescale globally with the python crush
// tool, so changing size-default is a footgun (would silently bloat new
// placements).
//
// Initialised to the historical defaults (red / steady / 0 / 0). Updated:
//   - any time the user edits the selected spot's colour, kind, hz, or phase
//   - any time a new spot is placed (so back-to-back placements stay
//     identical without ever touching the right-column editor)
//
// Modifier-clicks are quick presets that ALSO update the sticky values,
// so back-to-back plain clicks keep repeating whatever you last chose
// ("modifier = switch to this preset, then keep going"):
//   shift-click  → steady blue   (port nav / cool accents)
//   cmd-click    → steady red    (starboard nav / warnings)
//   option-click → green Strobe @ 3 Hz  (blinking beacon)
// (macOS: Cmd surfaces as io.KeyCtrl, Option as io.KeyAlt — see the
// click handler for the why.) Each preset sets colour AND kind AND hz
// together so switching back from the green beacon clears the strobe.
static HMM_Vec3  g_sticky_color = HMM_V3(1.00f, 0.10f, 0.10f);   // hot red
static float     g_sticky_hz    = 0.0f;
static float     g_sticky_phase = 0.0f;
static LightKind g_sticky_kind  = LightKind::Steady;

// ---- Recommender state (issue #111) --------------------------------------
//
// "Find more lights like the selected one." The author places one light on a
// repeated hull feature, hits `R` (or the "recommend" button), and the tool
// renders ghost circles over the top-K visually-similar spots on the SAME
// sprite. From there:
//   A      -> accept ALL ghosts as real lights
//   P      -> enter pick mode; then 1..9 spawns that single ranked ghost
//   Esc    -> clear the ghost set
// Accepted ghosts inherit the QUERY light's colour/size/hz/phase/kind, so a
// recommended starboard wingtip matches the port one the author placed.
//
// Ghosts auto-clear when the target sprite changes or the source light is
// edited/deleted (stale suggestions around a moved light are worse than
// none). `patch_px` is the descriptor window the author tunes for the ship's
// feature scale; `k` is how many suggestions to surface.
static std::vector<sprite_light_rec::Candidate> g_ghosts;
static int  g_rec_color_tol = 30;    // segmentation merge threshold (RGB dist)
static int  g_rec_patch_px  = 12;    // feature scale (blur + min size)
static int  g_rec_k         = 8;     // how many suggestions to surface
static int  g_rec_dedupe_px = 5;     // drop suggestions within this many
                                     // SOURCE px of an existing light
static bool g_rec_pick_mode = false; // P: pick individual ghosts by number
static std::string g_ghosts_owner;   // sprite name the ghosts belong to
static int  g_ghosts_src_light = -1; // light index the ghosts were seeded from
static LightSpot g_ghosts_src_snapshot; // source light at seed time (staleness)
static float g_ghosts_canvas_size = -1.0f; // image px size at seed (resize -> clear)

static void clear_ghosts() {
    g_ghosts.clear();
    g_rec_pick_mode = false;
    g_ghosts_owner.clear();
    g_ghosts_src_light = -1;
}

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

void init() {
    // Nothing to set up — ImGui is owned by debug_panel which is init'd
    // before us in main.cpp. This function exists for symmetry with other
    // subsystems and to give future initialisation a home.
    std::printf("[light_editor] ready — F2 to toggle\n");
}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_F2) {
        g_visible = !g_visible;
        return true;   // consumed
    }
    return false;      // let debug_panel / game input see it
}

// ---------------------------------------------------------------------------
// JSON load / save (flat array of lights)
// ---------------------------------------------------------------------------
//
// Format (example):
//   [
//     { "u": 0.12, "v": 0.55, "color": [255, 50, 40], "size": 120, "hz": 1.0,
//       "phase": 0.0, "kind": "strobe" },
//     ...
//   ]
// Colors are stored as 0-255 bytes for human readability; we convert to
// 0-1 floats at load/save boundaries. Kind is a string ("steady"/"pulse"/
// "strobe") for the same reason — authoring-friendly.

static std::string kind_to_string(LightKind k) {
    switch (k) {
    case LightKind::Steady: return "steady";
    case LightKind::Pulse:  return "pulse";
    case LightKind::Strobe: return "strobe";
    }
    return "steady";
}

static LightKind string_to_kind(const std::string& s) {
    if (s == "pulse")  return LightKind::Pulse;
    if (s == "strobe") return LightKind::Strobe;
    return LightKind::Steady;
}

bool load_lights_sidecar(const std::string& sprite_base_path,
                         std::vector<LightSpot>& out) {
    const std::string path = sprite_base_path + ".lights.json";
    // Most sprites don't have a sidecar — pre-flight the existence check
    // ourselves so json::parse_file's "cannot open" warning is reserved
    // for actual misconfigurations (file present but unparseable).
    if (!std::filesystem::exists(path)) return false;
    json::Value v = json::parse_file(path);
    if (!v.is_array()) return false;

    out.clear();
    for (const auto& entry : v.as_array()) {
        if (!entry.is_object()) continue;
        LightSpot ls{};
        if (auto* p = entry.find("u"))     ls.u     = p->as_float();
        if (auto* p = entry.find("v"))     ls.v     = p->as_float();
        if (auto* p = entry.find("size"))  ls.size  = p->as_float();
        if (auto* p = entry.find("hz"))    ls.hz    = p->as_float();
        if (auto* p = entry.find("phase")) ls.phase = p->as_float();
        if (auto* p = entry.find("kind") ; p && p->is_string())
            ls.kind = string_to_kind(p->as_string());
        if (auto* p = entry.find("color"); p && p->is_array() &&
                                            p->as_array().size() >= 3) {
            const auto& a = p->as_array();
            ls.color.X = (float)a[0].as_number() / 255.0f;
            ls.color.Y = (float)a[1].as_number() / 255.0f;
            ls.color.Z = (float)a[2].as_number() / 255.0f;
        }
        out.push_back(ls);
    }
    std::printf("[light_editor] loaded %zu lights from %s\n",
                out.size(), path.c_str());
    return true;
}

bool save_lights_sidecar(const std::string& sprite_base_path,
                         const std::vector<LightSpot>& lights) {
    const std::string path = sprite_base_path + ".lights.json";
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "[light_editor] cannot open %s for write\n",
                     path.c_str());
        return false;
    }
    // Hand-serialise. Two-space indent, one light per line for git diffs.
    f << "[\n";
    for (size_t i = 0; i < lights.size(); ++i) {
        const LightSpot& ls = lights[i];
        const int r = (int)std::round(ls.color.X * 255.0f);
        const int g = (int)std::round(ls.color.Y * 255.0f);
        const int b = (int)std::round(ls.color.Z * 255.0f);
        f << "  { "
          << "\"u\": "     << ls.u     << ", "
          << "\"v\": "     << ls.v     << ", "
          << "\"color\": [" << r << ", " << g << ", " << b << "], "
          << "\"size\": "  << ls.size  << ", "
          << "\"hz\": "    << ls.hz    << ", "
          << "\"phase\": " << ls.phase << ", "
          << "\"kind\": \"" << kind_to_string(ls.kind) << "\" }";
        if (i + 1 < lights.size()) f << ",";
        f << "\n";
    }
    f << "]\n";
    std::printf("[light_editor] saved %zu lights to %s\n",
                lights.size(), path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

// Colour picker swatch for a LightSpot. Writes back into `color` in-place.
// Uses ImGui::ColorEdit3 with the NoInputs flag to keep the widget compact.
static void color_picker(HMM_Vec3& color, const char* id) {
    float rgb[3] = { color.X, color.Y, color.Z };
    if (ImGui::ColorEdit3(id, rgb,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_NoLabel)) {
        color.X = rgb[0];
        color.Y = rgb[1];
        color.Z = rgb[2];
    }
}

static const char* kKindLabels[] = { "steady", "pulse", "strobe" };

// ---------------------------------------------------------------------------
// Per-frame UI
// ---------------------------------------------------------------------------

void build(std::vector<SpriteObject>& sprites,
           const std::vector<EditableArt>& extra_arts) {
    if (!g_visible) return;

    // Unified target resolution. Each selectable item maps to (art, lights
    // vector, sidecar stem). Placed sprites edit the instance's lights;
    // extra arts edit the asset's `light_spots` directly so all in-world
    // billboards using that art see the change live (essential for ship-
    // sprite atlases where the SpriteObject is rebuilt each frame).
    const int total = (int)sprites.size() + (int)extra_arts.size();
    if (total == 0) return;
    g_sel_sprite = std::max(0, std::min(g_sel_sprite, total - 1));

    auto resolve_target = [&](int idx,
                              const SpriteArt*& out_art,
                              std::vector<LightSpot>*& out_lights,
                              const std::string*& out_name) {
        if (idx < (int)sprites.size()) {
            SpriteObject& s = sprites[idx];
            out_art    = s.art;
            out_lights = &s.lights;
            out_name   = s.art ? &s.art->name : nullptr;
        } else {
            const EditableArt& e = extra_arts[idx - (int)sprites.size()];
            out_art    = e.art;
            out_lights = e.art ? &e.art->light_spots : nullptr;
            out_name   = &e.name;
        }
    };

    const SpriteArt*        sel_art    = nullptr;
    std::vector<LightSpot>* sel_lights = nullptr;
    const std::string*      sel_name   = nullptr;
    resolve_target(g_sel_sprite, sel_art, sel_lights, sel_name);
    if (!sel_art || !sel_lights || !sel_name) return;

    ImGui::SetNextWindowSize(ImVec2(1200, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sprite Light Editor (F2)", &g_visible)) {
        ImGui::End();
        return;
    }

    // ---- Top toolbar row ------------------------------------------------
    // Layout: [<] [>] [\u2192\u2205] [stem dropdown] [auto-save \u25cf] [reload] [zoom]
    // Prev/next step linearly. The "\u2192\u2205" jump button skips ahead to
    // the next item with NO authored lights — invaluable when you've cycled
    // through 80 cells and need to find which 11 you missed. The dropdown
    // marks each item with \u2713 (authored, has lights) or \u00b7 (empty)
    // so you can scan the whole atlas for gaps without leaving the popup.
    auto step_selection = [&](int delta) {
        g_sel_sprite = ((g_sel_sprite + delta) % total + total) % total;
        g_sel_light  = -1;
    };
    auto target_lights_count = [&](int idx) -> int {
        const SpriteArt*        art    = nullptr;
        std::vector<LightSpot>* lights = nullptr;
        const std::string*      name   = nullptr;
        resolve_target(idx, art, lights, name);
        return (lights ? (int)lights->size() : -1);
    };
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) step_selection(-1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous sprite");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) step_selection(+1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next sprite");
    ImGui::SameLine();
    if (ImGui::Button("->0")) {
        // Find next item (wrapping) with zero lights. Stops at current if
        // nothing else is empty. Cap at `total` iterations so an all-full
        // atlas doesn't infinite-loop.
        for (int step = 1; step <= total; ++step) {
            const int idx = ((g_sel_sprite + step) % total + total) % total;
            if (target_lights_count(idx) == 0) {
                g_sel_sprite = idx;
                g_sel_light  = -1;
                break;
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to next sprite with no lights");
    ImGui::SameLine();

    // Tally for the toolbar status line — "57/80 authored" makes "how
    // many cells am I really done with" answerable at a glance, instead
    // of relying on `ls | wc -l` from a terminal.
    int authored_count = 0;
    for (int i = 0; i < total; ++i) {
        if (target_lights_count(i) > 0) ++authored_count;
    }

    ImGui::SetNextItemWidth(-360.0f);   // leave room for buttons + zoom + tally
    if (ImGui::BeginCombo("##sprite_combo", sel_name->c_str())) {
        for (int i = 0; i < total; ++i) {
            const SpriteArt*        art    = nullptr;
            std::vector<LightSpot>* lights = nullptr;
            const std::string*      name   = nullptr;
            resolve_target(i, art, lights, name);
            if (!art || !name) continue;
            const bool is_sel = (i == g_sel_sprite);
            const bool authored = (lights && !lights->empty());
            // Prefix marker: "v" for authored, "-" for empty. Plain ASCII
            // because the imgui default font may not have ✓/✗ glyphs.
            char label[256];
            std::snprintf(label, sizeof(label), "%s  %s",
                          authored ? "[v]" : "[ ]", name->c_str());
            if (ImGui::Selectable(label, is_sel)) {
                g_sel_sprite = i;
                g_sel_light  = -1;
            }
            if (is_sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextColored(
        authored_count == total ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                : ImVec4(1.0f, 0.85f, 0.30f, 1.0f),
        "%d/%d", authored_count, total);
    // No "save" button — every change auto-writes the sidecar at end of
    // frame (see auto-save block at the bottom of build()). Status text
    // confirms the target file path so the user knows where edits land.
    ImGui::SameLine();
    ImGui::TextDisabled("auto-save \xee\x9c\xa0");   // visual cue (degree mark — font-safe)
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Edits write to %s.lights.json automatically",
                          sel_name->c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("reload")) {
        std::vector<LightSpot> fresh;
        if (load_lights_sidecar(*sel_name, fresh)) {
            *sel_lights = std::move(fresh);
            g_sel_light = -1;
            // Sync snapshot so auto-save doesn't immediately re-write
            // the file we just loaded from.
            g_save_snapshots[*sel_name] = *sel_lights;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reload from disk, dropping in-memory edits");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("zoom", &g_zoom, 1.0f, 6.0f, "%.1fx");

    ImGui::Separator();

    // ---- Two-column layout: image on left, light list/editor on right ---
    // The image pane auto-fits the available content region (so a 1022-px
    // ship cell doesn't push the right panel off-screen). Zoom slider
    // operates ON TOP of the fit-size and triggers scrollbars when >1x.
    std::vector<LightSpot>& cur_lights = *sel_lights;

    // ---- Recommender (issue #111): "find more lights like this one" ------
    // Stale-ghost hygiene: drop suggestions when the target sprite changed,
    // or when the source light was edited/moved/deleted (a suggestion set
    // anchored to a light that no longer looks like its seed is misleading).
    if (!g_ghosts.empty() && g_ghosts_owner != *sel_name) clear_ghosts();
    if (!g_ghosts.empty()) {
        const int s = g_ghosts_src_light;
        if (s < 0 || s >= (int)cur_lights.size()) {
            clear_ghosts();
        } else {
            const LightSpot& c = cur_lights[s];
            const LightSpot& p = g_ghosts_src_snapshot;
            const bool same = c.u == p.u && c.v == p.v && c.size == p.size &&
                              c.hz == p.hz && c.phase == p.phase &&
                              c.kind == p.kind && c.color.X == p.color.X &&
                              c.color.Y == p.color.Y && c.color.Z == p.color.Z;
            if (!same) clear_ghosts();
        }
    }

    const std::string hull_png = *sel_name + ".png";
    auto seed_recommend = [&]() {
        clear_ghosts();
        if (g_sel_light < 0 || g_sel_light >= (int)cur_lights.size()) return;
        const LightSpot& q = cur_lights[g_sel_light];
        g_ghosts = sprite_light_rec::find_candidates(
            hull_png, q.u, q.v, g_rec_k, g_rec_color_tol, g_rec_patch_px);
        // Dedupe (radius = g_rec_dedupe_px SOURCE pixels; 0 disables). UV
        // distances are scaled back to pixels by the sprite's native dims so
        // the threshold is resolution-independent. Two steps:
        if (g_rec_dedupe_px > 0 && !g_ghosts.empty()) {
            const float ex = (float)g_rec_dedupe_px;
            const float ex2 = ex * ex;
            const float W = (sel_art && sel_art->hull_w > 0) ? (float)sel_art->hull_w : 512.0f;
            const float H = (sel_art && sel_art->hull_h > 0) ? (float)sel_art->hull_h : 512.0f;

            // 1. Drop suggestions sitting on top of a light that already
            //    exists (incl. the query + previously-accepted ghosts), so
            //    re-running recommend never re-marks an already-lit feature.
            g_ghosts.erase(std::remove_if(g_ghosts.begin(), g_ghosts.end(),
                [&](const sprite_light_rec::Candidate& c) {
                    for (const LightSpot& ls : cur_lights) {
                        const float dx = (c.u - ls.u) * W, dy = (c.v - ls.v) * H;
                        if (dx * dx + dy * dy < ex2) return true;
                    }
                    return false;
                }), g_ghosts.end());

            // 2. Collapse clusters of nearby suggestions into ONE marker at
            //    the cluster CENTROID (best-first greedy: each unused ghost
            //    seeds a cluster and absorbs any remaining ghost within ex px
            //    of the running centroid; the merged score is the best in the
            //    cluster).
            std::vector<sprite_light_rec::Candidate> merged;
            std::vector<bool> used(g_ghosts.size(), false);
            for (size_t i = 0; i < g_ghosts.size(); ++i) {
                if (used[i]) continue;
                used[i] = true;
                double su = g_ghosts[i].u, sv = g_ghosts[i].v;
                float  best = g_ghosts[i].score;
                int    n = 1;
                for (size_t j = i + 1; j < g_ghosts.size(); ++j) {
                    if (used[j]) continue;
                    const float dx = (g_ghosts[j].u - (float)(su / n)) * W;
                    const float dy = (g_ghosts[j].v - (float)(sv / n)) * H;
                    if (dx * dx + dy * dy < ex2) {
                        su += g_ghosts[j].u; sv += g_ghosts[j].v;
                        best = std::max(best, g_ghosts[j].score);
                        ++n; used[j] = true;
                    }
                }
                merged.push_back({ (float)(su / n), (float)(sv / n), best });
            }
            g_ghosts.swap(merged);
        }
        g_ghosts_owner        = *sel_name;
        g_ghosts_src_light    = g_sel_light;
        g_ghosts_src_snapshot = q;
        g_ghosts_canvas_size  = -1.0f;   // re-armed by the image pane below
    };
    auto accept_ghost = [&](int gi) {
        if (gi < 0 || gi >= (int)g_ghosts.size()) return;
        if (g_ghosts_src_light < 0 ||
            g_ghosts_src_light >= (int)cur_lights.size()) return;
        LightSpot ls = cur_lights[g_ghosts_src_light];   // inherit query props
        ls.u = g_ghosts[gi].u;
        ls.v = g_ghosts[gi].v;
        cur_lights.push_back(ls);
        g_ghosts.erase(g_ghosts.begin() + gi);           // consumed
        if (g_ghosts.empty()) clear_ghosts();
    };
    auto accept_all = [&]() {
        if (g_ghosts_src_light >= 0 &&
            g_ghosts_src_light < (int)cur_lights.size()) {
            const LightSpot proto = cur_lights[g_ghosts_src_light];
            for (const sprite_light_rec::Candidate& g : g_ghosts) {
                LightSpot ls = proto;
                ls.u = g.u; ls.v = g.v;
                cur_lights.push_back(ls);
            }
        }
        clear_ghosts();
    };

    // Toolbar row: recommend button + author-tunable descriptor controls.
    if (ImGui::Button("recommend (R)")) seed_recommend();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Find up to K regions matching the SELECTED light's\n"
                          "colour + size; marks each at its centre.\n"
                          "A = accept all, P then 1-9 = pick one, Esc = clear.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt("color tol", &g_rec_color_tol, 2, 120);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Segmentation merge threshold (RGB distance).\n"
                          "Lower = split a pod off its hull; higher = merge\n"
                          "similar shades into bigger regions.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderInt("patch px", &g_rec_patch_px, 1, 96);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Feature scale \xe2\x80\x94 blurs away specks smaller than this\n"
                          "and drops sub-patch regions (small=intakes, large=pods).");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderInt("dedupe px", &g_rec_dedupe_px, 0, 64);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Merge suggestions within this many source px into\n"
                          "one marker at the cluster centroid, and drop any\n"
                          "within this range of an existing light. 0 = off.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::SliderInt("K", &g_rec_k, 1, 24);
    if (!g_ghosts.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.30f, 0.85f, 1.0f, 1.0f),
                           "%d suggestion%s%s", (int)g_ghosts.size(),
                           g_ghosts.size() == 1 ? "" : "s",
                           g_rec_pick_mode ? "  [PICK 1-9]" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton("accept all (A)")) accept_all();
        ImGui::SameLine();
        if (ImGui::SmallButton("clear (Esc)"))    clear_ghosts();
    }

    // Hotkeys \xe2\x80\x94 only when this editor window owns the keyboard, so the
    // game never sees R/A/P (no accidental ship roll) and we don't fight a
    // focused text field.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_R))      seed_recommend();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) clear_ghosts();
        if (!g_ghosts.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_A)) accept_all();
            if (ImGui::IsKeyPressed(ImGuiKey_P)) g_rec_pick_mode = !g_rec_pick_mode;
            if (g_rec_pick_mode)
                for (int n = 1; n <= 9; ++n)
                    if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + n - 1)))
                        accept_ghost(n - 1);   // "1" -> top-ranked ghost
        }
    }

    ImGui::Separator();

    constexpr float kRightPanelMin = 340.0f;
    constexpr float kImageColPadding = 16.0f;
    const ImVec2  region        = ImGui::GetContentRegionAvail();
    const float   image_col_w   = std::max(160.0f,
                                  region.x - kRightPanelMin - kImageColPadding);
    const float   fit_size      = std::max(64.0f,
                                  std::min(image_col_w, region.y - 8.0f));
    const float   img_size      = fit_size * g_zoom;

    // Canvas resize (zoom or window) moves every ghost's screen position;
    // rather than chase it, clear the suggestion set (acceptance #111).
    if (!g_ghosts.empty()) {
        if (g_ghosts_canvas_size < 0.0f)        g_ghosts_canvas_size = img_size;
        else if (std::fabs(img_size - g_ghosts_canvas_size) > 0.5f) clear_ghosts();
    }

    ImGui::BeginChild("image_col",
                      ImVec2(image_col_w, 0),
                      ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        const ImVec2 img_pos = ImGui::GetCursorScreenPos();
        ImGui::Image(simgui_imtextureid(sel_art->hull.view),
                     ImVec2(img_size, img_size));

        // Click handling. Must be checked AFTER Image so the InvisibleButton
        // trick is unnecessary — Image acts as an item and IsItemClicked()
        // works directly on it.
        const bool image_hovered = ImGui::IsItemHovered();
        const bool image_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        // Image-local mouse position in UV (0..1), valid only when hovered.
        ImVec2 mouse_uv{0, 0};
        if (image_hovered) {
            const ImVec2 m = ImGui::GetMousePos();
            mouse_uv.x = (m.x - img_pos.x) / img_size;
            mouse_uv.y = (m.y - img_pos.y) / img_size;
        }

        // Draw gizmo circles for every existing light. Draw order: the
        // Image is already drawn; overlay with ImDrawList so circles sit
        // on top of the texture.
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Gizmo hit test: pixel radius on screen. Larger than the visual
        // radius so small lights are still clickable.
        const float hit_radius = 10.0f;
        int hit_light = -1;
        float hit_dist2 = hit_radius * hit_radius;

        for (int i = 0; i < (int)cur_lights.size(); ++i) {
            const LightSpot& ls = cur_lights[i];
            const ImVec2 c{ img_pos.x + ls.u * img_size,
                            img_pos.y + ls.v * img_size };
            const ImU32 col = IM_COL32(
                (int)(ls.color.X * 255),
                (int)(ls.color.Y * 255),
                (int)(ls.color.Z * 255), 220);
            const ImU32 outline = (i == g_sel_light) ?
                IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);

            dl->AddCircleFilled(c, 5.0f, col);
            dl->AddCircle(c, 6.0f, outline, 0, 2.0f);

            if (image_hovered) {
                const float dx = ImGui::GetMousePos().x - c.x;
                const float dy = ImGui::GetMousePos().y - c.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < hit_dist2) { hit_dist2 = d2; hit_light = i; }
            }
        }

        // ---- Recommender ghosts (issue #111) ------------------------
        // Hollow cyan circles + rank number over each suggested spot.
        // Clicking one accepts just that ghost (same as P then its number).
        int   ghost_hit    = -1;
        float ghost_hit_d2 = hit_radius * hit_radius;
        for (int i = 0; i < (int)g_ghosts.size(); ++i) {
            const ImVec2 c{ img_pos.x + g_ghosts[i].u * img_size,
                            img_pos.y + g_ghosts[i].v * img_size };
            const ImU32 ring = IM_COL32(70, 210, 255, 210);
            dl->AddCircle(c, 8.0f, ring, 0, 2.0f);
            dl->AddCircleFilled(c, 1.5f, ring);
            char num[8];
            std::snprintf(num, sizeof(num), "%d", i + 1);
            dl->AddText(ImVec2(c.x + 9.0f, c.y - 7.0f), ring, num);
            if (image_hovered) {
                const float dx = ImGui::GetMousePos().x - c.x;
                const float dy = ImGui::GetMousePos().y - c.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < ghost_hit_d2) { ghost_hit_d2 = d2; ghost_hit = i; }
            }
        }

        // Click dispatch:
        //   - hit on ghost  → accept that suggestion
        //   - hit on gizmo  → select it
        //   - hit on empty area → spawn new light at that UV, select it
        if (image_clicked) {
            if (ghost_hit >= 0) {
                accept_ghost(ghost_hit);
            } else if (hit_light >= 0) {
                g_sel_light = hit_light;
            } else if (mouse_uv.x >= 0 && mouse_uv.x <= 1 &&
                       mouse_uv.y >= 0 && mouse_uv.y <= 1) {
                // Modifier-click presets. Each sets colour + kind + hz and
                // makes them the new sticky values, so subsequent plain
                // clicks repeat the same light.
                //
                // macOS modifier reality check: with ImGui's mac behaviour
                // the Cmd key surfaces as io.KeyCtrl (NOT io.KeySuper), and
                // the physical Ctrl key / KeySuper aren't reliably reachable.
                // So we bind to the three modifiers that DO come through
                // cleanly: Shift, Cmd(=KeyCtrl), and Option(=KeyAlt).
                //   shift  → blue
                //   cmd    → red
                //   option → green strobe @ 3 Hz
                // Priority option > cmd > shift if chorded.
                const ImGuiIO& io = ImGui::GetIO();
                if (io.KeyAlt) {
                    // Option/Alt-click → blinking green beacon at 3 Hz.
                    g_sticky_color = HMM_V3(0.10f, 0.85f, 0.25f);
                    g_sticky_kind  = LightKind::Strobe;
                    g_sticky_hz    = 3.0f;
                } else if (io.KeyCtrl) {
                    // Cmd-click (mac) / Ctrl-click (other) → steady red.
                    g_sticky_color = HMM_V3(1.00f, 0.10f, 0.10f);
                    g_sticky_kind  = LightKind::Steady;
                    g_sticky_hz    = 0.0f;
                } else if (io.KeyShift) {
                    // Shift-click → steady blue.
                    g_sticky_color = HMM_V3(0.20f, 0.55f, 1.00f);
                    g_sticky_kind  = LightKind::Steady;
                    g_sticky_hz    = 0.0f;
                }
                LightSpot ls{};
                ls.u = mouse_uv.x;
                ls.v = mouse_uv.y;
                ls.color = g_sticky_color;
                ls.size  = 5.0f;     // see g_sticky_color comment re: size
                ls.hz    = g_sticky_hz;
                ls.phase = g_sticky_phase;
                ls.kind  = g_sticky_kind;
                cur_lights.push_back(ls);
                g_sel_light = (int)cur_lights.size() - 1;
            }
        }

        // Drag-to-move the selected light. Only active while LMB is held
        // AND the drag started on the selected gizmo (to avoid grabbing a
        // light every time the user clicks nearby).
        if (g_sel_light >= 0 && image_hovered &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            LightSpot& ls = cur_lights[g_sel_light];
            ls.u = std::min(1.0f, std::max(0.0f, mouse_uv.x));
            ls.v = std::min(1.0f, std::max(0.0f, mouse_uv.y));
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Right column: light list + selected-light editor ---------------
    ImGui::BeginChild("edit_col", ImVec2(0, 0), ImGuiChildFlags_Border);
    {
        ImGui::Text("%zu light%s", cur_lights.size(),
                    cur_lights.size() == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::Button("+ add")) {
            // Same sticky values as click-spawn (kept in sync so users get a
            // consistent first-time experience whichever entry path they
            // use). Shift-modifier intentionally not honoured here — the
            // button is for "give me a light I'll position later", not
            // "give me one in this colour".
            LightSpot ls{};
            ls.u = 0.5f; ls.v = 0.5f;
            ls.color = g_sticky_color;
            ls.size  = 5.0f;
            ls.hz    = g_sticky_hz;
            ls.phase = g_sticky_phase;
            ls.kind  = g_sticky_kind;
            cur_lights.push_back(ls);
            g_sel_light = (int)cur_lights.size() - 1;
        }

        // ---- Extrapolation helpers ----------------------------------
        // Authoring 80 ship-atlas cells from scratch is brutal; the
        // common workflow is to author one cell well, then copy/mirror
        // its lights onto the neighbouring az/el cells and tweak. These
        // buttons grab the prev/next item's lights wholesale (replacing
        // the current list) — "mirror X" flips u→1-u for the opposite
        // side of the ship (e.g. authoring az=90 then mirror-copying to
        // az=270 with the port/starboard swap baked in).
        ImGui::Separator();
        ImGui::TextUnformatted("copy lights from neighbour:");
        auto copy_from = [&](int delta, bool mirror_x) {
            if (total <= 1) return;
            const int src_idx = ((g_sel_sprite + delta) % total + total) % total;
            const SpriteArt*        src_art    = nullptr;
            std::vector<LightSpot>* src_lights = nullptr;
            const std::string*      src_name   = nullptr;
            resolve_target(src_idx, src_art, src_lights, src_name);
            if (!src_lights) return;
            cur_lights = *src_lights;     // value-copy, preserves color/hz/etc
            if (mirror_x) {
                for (LightSpot& ls : cur_lights) ls.u = 1.0f - ls.u;
            }
            g_sel_light = -1;
        };
        if (ImGui::Button("< prev"))           copy_from(-1, false);
        ImGui::SameLine();
        if (ImGui::Button("next >"))           copy_from(+1, false);
        ImGui::SameLine();
        if (ImGui::Button("< prev (mirror X)")) copy_from(-1, true);
        ImGui::SameLine();
        if (ImGui::Button("next > (mirror X)")) copy_from(+1, true);

        ImGui::Separator();

        // Scrollable list — each row: color swatch + kind label + u/v.
        ImGui::BeginChild("light_list", ImVec2(0, 160),
                          ImGuiChildFlags_Border);
        for (int i = 0; i < (int)cur_lights.size(); ++i) {
            LightSpot& ls = cur_lights[i];
            ImGui::PushID(i);
            char label[128];
            std::snprintf(label, sizeof(label),
                          "#%d  %s  @(%.2f,%.2f)", i,
                          kKindLabels[(int)ls.kind], ls.u, ls.v);
            if (ImGui::Selectable(label, i == g_sel_light)) {
                g_sel_light = i;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Separator();

        if (g_sel_light >= 0 && g_sel_light < (int)cur_lights.size()) {
            LightSpot& ls = cur_lights[g_sel_light];
            ImGui::Text("Selected light #%d", g_sel_light);

            ImGui::DragFloat("u", &ls.u, 0.002f, 0.0f, 1.0f, "%.3f");
            ImGui::DragFloat("v", &ls.v, 0.002f, 0.0f, 1.0f, "%.3f");

            // Edits to colour / hz / phase / kind also update the
            // module-static stickies so the next placed spot inherits the
            // value the user just dialled in. Size is intentionally NOT
            // sticky (see g_sticky_color comment).
            float rgb[3] = { ls.color.X, ls.color.Y, ls.color.Z };
            if (ImGui::ColorEdit3("color", rgb)) {
                ls.color.X = rgb[0];
                ls.color.Y = rgb[1];
                ls.color.Z = rgb[2];
                g_sticky_color = ls.color;
            }

            ImGui::SliderFloat("size",  &ls.size,  5.0f, 300.0f, "%.0f");
            if (ImGui::SliderFloat("hz",    &ls.hz,    0.0f, 5.0f,   "%.2f")) {
                g_sticky_hz = ls.hz;
            }
            if (ImGui::SliderFloat("phase", &ls.phase, 0.0f, 1.0f,   "%.2f")) {
                g_sticky_phase = ls.phase;
            }

            int kind_i = (int)ls.kind;
            if (ImGui::Combo("kind", &kind_i, kKindLabels,
                             IM_ARRAYSIZE(kKindLabels))) {
                ls.kind = (LightKind)kind_i;
                g_sticky_kind = ls.kind;
            }

            ImGui::Separator();
            if (ImGui::Button("delete")) {
                cur_lights.erase(cur_lights.begin() + g_sel_light);
                g_sel_light = -1;
            }
        } else {
            ImGui::TextDisabled("Click a light to edit, or click the sprite to add one.");
            ImGui::TextDisabled("  plain=last  shift=blue  cmd=red  option=green strobe 3Hz");
        }
    }
    ImGui::EndChild();

    // -----------------------------------------------------------------
    // Auto-save: walk EVERY editable target, save any whose lights have
    // changed since their last snapshot. First sighting of a target seeds
    // its snapshot from current state without saving (matches what was
    // just loaded from disk). Walking all targets per-frame catches edits
    // even if the user has navigated away from the cell — important
    // because copy-from-prev mutates a cell's lights in place and the
    // user might switch off it before this code runs.
    //
    // O(N_targets) per frame; N is ~85 (mining base + 80 ship cells).
    // lights_equal is field-by-field on small vectors, microseconds total.
    // -----------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        const SpriteArt*        art    = nullptr;
        std::vector<LightSpot>* lights = nullptr;
        const std::string*      name   = nullptr;
        resolve_target(i, art, lights, name);
        if (!lights || !name) continue;
        auto it = g_save_snapshots.find(*name);
        if (it == g_save_snapshots.end()) {
            // First sighting — adopt current state as the baseline. The
            // contents already match disk because load_sprite_art seeded
            // light_spots from the sidecar at startup.
            g_save_snapshots.emplace(*name, *lights);
        } else if (!lights_equal(*lights, it->second)) {
            save_lights_sidecar(*name, *lights);
            it->second = *lights;
        }
    }

    ImGui::End();
}

} // namespace sprite_light_editor
