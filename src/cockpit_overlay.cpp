// cockpit_overlay.cpp — see header for the layering story.
#include "cockpit_overlay.h"

#include "material.h"      // TextureSlot + load_texture_png (shared PNG loader)

#include "imgui.h"
#include "sokol_gfx.h"
#include "sokol_app.h"     // must precede sokol_imgui.h
#include "sokol_imgui.h"

#include <cstdio>
#include <iterator>

namespace cockpit_overlay {

namespace {

constexpr int kArtCount = (int)std::size(kCockpitArts);

// Lazily-loaded GPU texture per art row. `tried` stops a missing/broken PNG
// from re-hitting the disk (and the log) every frame.
struct LoadedArt { TextureSlot tex; bool tried = false; };
LoadedArt  g_loaded[kArtCount];
sg_sampler g_sampler{};

// Frame state published by draw() for the HUD panels to query.
const CockpitArt* g_art = nullptr;
Fit               g_fit;
int               g_frame = -1;

// MFD glass: near-opaque dark screen so instruments read against a bright
// sun or nebula behind the dash. Bled 2 art px under the bezel so no sliver
// of space shows between glass and frame.
constexpr ImU32 kGlass      = IM_COL32(6, 10, 9, 236);
constexpr float kGlassBleed = 2.0f;

const TextureSlot* texture_for(const CockpitArt& art) {
    LoadedArt& slot = g_loaded[&art - kCockpitArts];
    if (!slot.tried) {
        slot.tried = true;
        if (!load_texture_png(art.path, slot.tex))
            std::fprintf(stderr, "[cockpit] overlay art unavailable: %s\n", art.path);
    }
    if (!slot.tex.valid) return nullptr;
    if (!g_sampler.id) {
        sg_sampler_desc desc{};
        desc.min_filter = SG_FILTER_LINEAR;
        desc.mag_filter = SG_FILTER_LINEAR;
        desc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
        desc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
        desc.label = "cockpit-overlay-linear-sampler";
        g_sampler = sg_make_sampler(&desc);
    }
    return &slot.tex;
}

ImVec2 tl(const Rect& r) { return { r.x, r.y }; }
ImVec2 br(const Rect& r) { return { r.x + r.w, r.y + r.h }; }

} // namespace

void draw(const std::string& ship_class) {
    const CockpitArt* art = find_art(ship_class.c_str());
    if (!art) return;
    const TextureSlot* tex = texture_for(*art);
    if (!tex) return;   // no art -> classic HUD; never park panels in thin air

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    g_art   = art;
    g_fit   = fit_to_viewport(*art, vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
    g_frame = ImGui::GetFrameCount();

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    for (const Rect& hole : art->mfd) {
        const Rect glass = to_screen(g_fit, inset(hole, -kGlassBleed));
        bg->AddRectFilled(tl(glass), br(glass), kGlass);
    }
    const Rect full = to_screen(g_fit, { 0.0f, 0.0f, art->art_w, art->art_h });
    bg->AddImage(simgui_imtextureid_with_sampler(tex->view, g_sampler),
                 tl(full), br(full));
}

bool active() {
    return g_art && g_frame == ImGui::GetFrameCount();
}

bool mfd_rect(Mfd which, Rect& out) {
    if (!active()) return false;
    out = to_screen(g_fit, g_art->mfd[(int)which]);
    return true;
}

} // namespace cockpit_overlay
