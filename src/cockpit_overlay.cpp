// cockpit_overlay.cpp — see header for the frame flow.
//
// Why warp ImGui's output instead of rendering instruments some other way:
// every panel keeps its existing ImGui body (and the STATUS armaments page
// keeps its drag/drop), and ImGui windows are strictly axis-aligned. So the
// panels are laid out flat, and finalize() re-projects their finished vertex
// data. Triangles are copied un-indexed with each vertex pushed through the
// display's homography; per-triangle affine UV interpolation is exact for the
// solid fills and indistinguishable for glyph-sized quads. The source window
// draw list is then emptied — ImGui skips a draw list with no commands
// (AddDrawListToDrawDataEx), and it is reset on the next NewFrame anyway.
#include "cockpit_overlay.h"
#include "camera.h"
#include "pilot_head_motion.h"

#include "material.h"      // TextureSlot + load_texture_png (shared PNG loader)

#include "imgui.h"
#include "imgui_internal.h"   // FindWindowByName, ImGuiWindow::DrawList
#include "sokol_gfx.h"
#include "sokol_app.h"        // must precede sokol_imgui.h
#include "sokol_imgui.h"

#include <algorithm>
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

// Per-display frame state published by draw().
struct DisplayFrame {
    bool       present = false;
    Rect       panel;         // flat layout rect (logical px)
    Quad       quad;          // skewed on-screen glass
    Homography to_screen;     // panel -> quad
    Homography to_panel;      // quad -> panel (mouse)
};

// Frame state published by draw() for the HUD panels and finalize().
const CockpitArt*  g_art = nullptr;
const TextureSlot* g_tex = nullptr;
Fit                g_fit;
PilotHeadMotion    g_head;
Homography         g_head_transform;
int                g_frame = -1;
DisplayFrame       g_displays[kDisplayCount];

// Display glass: near-opaque dark screen so instruments read against a
// bright sun or nebula. Bled under the bezel so no sliver of space shows —
// but only 2 art px: the Centurion's centre MFD has a 3 px bottom bezel with
// open space right below it.
constexpr ImU32 kGlass      = IM_COL32(6, 10, 9, 236);
constexpr float kGlassBleed = 2.0f;    // art px
constexpr unsigned kMaxBatch = 3u * 8192u;   // triangles-per-reserve cap

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

ImVec2 iv(Vec2 p) { return { p.x, p.y }; }

struct Box { float x0, y0, x1, y1; };

Box warped_bounds(const Homography& h, const ImVec4& r) {
    const Vec2 c[4] = { apply(h, { r.x, r.y }), apply(h, { r.z, r.y }),
                        apply(h, { r.z, r.w }), apply(h, { r.x, r.w }) };
    Box b{ c[0].x, c[0].y, c[0].x, c[0].y };
    for (const Vec2& p : c) {
        b.x0 = std::min(b.x0, p.x); b.y0 = std::min(b.y0, p.y);
        b.x1 = std::max(b.x1, p.x); b.y1 = std::max(b.y1, p.y);
    }
    return b;
}

// Re-emit `src`'s triangles into `dst`, every vertex warped panel -> quad.
void warp_draw_list(ImDrawList* dst, ImDrawList* src, const DisplayFrame& d) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 vp_min = vp->Pos, vp_max(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    const Box quad_box = warped_bounds(d.to_screen,
        { d.panel.x, d.panel.y, d.panel.x + d.panel.w, d.panel.y + d.panel.h });
    for (const ImDrawCmd& cmd : src->CmdBuffer) {
        if (cmd.UserCallback || cmd.ElemCount == 0) continue;
        // The command's (flat) scissor, re-projected; the art on top masks
        // the sliver between this bbox and the true quad edge.
        // Clamped to the viewport; sokol_imgui hands the scissor to the GPU
        // unchecked, so an empty/inverted one is skipped outright.
        const Box c = warped_bounds(d.to_screen, cmd.ClipRect);
        const ImVec2 lo(std::max({ c.x0, quad_box.x0, vp_min.x }),
                        std::max({ c.y0, quad_box.y0, vp_min.y }));
        const ImVec2 hi(std::min({ c.x1, quad_box.x1, vp_max.x }),
                        std::min({ c.y1, quad_box.y1, vp_max.y }));
        if (hi.x <= lo.x || hi.y <= lo.y) continue;
        dst->PushClipRect(lo, hi);
        dst->PushTexture(cmd.TexRef);
        const ImDrawIdx*  idx = src->IdxBuffer.Data + cmd.IdxOffset;
        const ImDrawVert* vtx = src->VtxBuffer.Data + cmd.VtxOffset;
        for (unsigned done = 0; done < cmd.ElemCount;) {
            const unsigned n = std::min(cmd.ElemCount - done, kMaxBatch);
            dst->PrimReserve((int)n, (int)n);
            for (unsigned i = 0; i < n; ++i) {
                const ImDrawVert& v = vtx[idx[done + i]];
                dst->PrimVtx(iv(apply(d.to_screen, { v.pos.x, v.pos.y })), v.uv, v.col);
            }
            done += n;
        }
        dst->PopTexture();
        dst->PopClipRect();
    }
    src->CmdBuffer.resize(0);
    src->IdxBuffer.resize(0);
    src->VtxBuffer.resize(0);
}

} // namespace

const char* display_window_id(Display d) {
    switch (d) {
    case Display::Left:   return "##cockpit_display_left";
    case Display::Center: return "##cockpit_display_center";
    case Display::Right:  return "##cockpit_display_right";
    case Display::Banner:   return "##cockpit_display_banner";
    case Display::SetSpeed: return "##cockpit_display_set_speed";
    case Display::Velocity: default: return "##cockpit_display_velocity";
    }
}

void draw(const std::string& ship_class, const Camera& camera) {
    const CockpitArt* art = find_art(ship_class.c_str());
    if (!art) return;
    const TextureSlot* tex = texture_for(*art);
    if (!tex) return;   // no art -> classic HUD; never park panels in thin air

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Skipped frames mean an external/cinematic camera or a different hull:
    // discard old head lag instead of replaying it on cockpit re-entry.
    if (g_frame != ImGui::GetFrameCount() - 1 || g_art != art)
        g_head.reset();
    const float tau = camera.turn_response_seconds;
    g_head.update(camera.yaw_response.acceleration(tau) / std::max(camera.max_yaw_rate, 0.01f),
                  camera.pitch_response.acceleration(tau) / std::max(camera.max_pitch_rate, 0.01f),
                  ImGui::GetIO().DeltaTime);
    g_head_transform = g_head.transform(
        {vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y}, camera.cockpit_head_motion_strength);
    g_art   = art;
    g_tex   = tex;
    g_fit   = fit_to_viewport(*art, vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);
    g_frame = ImGui::GetFrameCount();

    ImDrawList* bg = ImGui::GetBackgroundDrawList();

    for (int i = 0; i < kDisplayCount; ++i) {
        DisplayFrame& d = g_displays[i];
        d.present = present(art->display[i]);
        if (!d.present) continue;
        d.quad      = to_screen(g_fit, art->display[i]);
        d.panel     = panel_rect(d.quad);
        // Keep the flat layout stable (no font reflow as the head moves).
        // The same affine head transform moves glass, content and PNG.
        d.to_screen = multiply(g_head_transform, rect_to_quad(d.panel, d.quad));
        d.to_panel  = inverse(d.to_screen);
        // Glass: the flat panel grown by the bleed, warped onto the bezel.
        const Rect g{ d.panel.x - kGlassBleed * g_fit.scale_x,
                      d.panel.y - kGlassBleed * g_fit.scale_y,
                      d.panel.w + 2.0f * kGlassBleed * g_fit.scale_x,
                      d.panel.h + 2.0f * kGlassBleed * g_fit.scale_y };
        bg->AddQuadFilled(iv(apply(d.to_screen, { g.x,       g.y       })),
                          iv(apply(d.to_screen, { g.x + g.w, g.y       })),
                          iv(apply(d.to_screen, { g.x + g.w, g.y + g.h })),
                          iv(apply(d.to_screen, { g.x,       g.y + g.h })), kGlass);
    }
}

void finalize() {
    if (!active()) return;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < kDisplayCount; ++i) {
        if (!g_displays[i].present) continue;
        ImGuiWindow* w = ImGui::FindWindowByName(display_window_id((Display)i));
        if (w && w->LastFrameActive == ImGui::GetFrameCount())
            warp_draw_list(bg, w->DrawList, g_displays[i]);
    }
    const Rect full = to_screen(g_fit, Rect{ 0.0f, 0.0f, g_art->art_w, g_art->art_h });
    bg->AddImageQuad(simgui_imtextureid_with_sampler(g_tex->view, g_sampler),
        iv(apply(g_head_transform, {full.x, full.y})),
        iv(apply(g_head_transform, {full.x + full.w, full.y})),
        iv(apply(g_head_transform, {full.x + full.w, full.y + full.h})),
        iv(apply(g_head_transform, {full.x, full.y + full.h})));
}

bool active() {
    return g_art && g_frame == ImGui::GetFrameCount();
}

bool display_panel(Display d, Rect& out) {
    if (!active() || !g_displays[(int)d].present) return false;
    out = g_displays[(int)d].panel;
    return true;
}

bool screen_to_panel(Display d, Vec2 screen, Vec2& out) {
    if (!active() || !g_displays[(int)d].present) return false;
    out = apply(g_displays[(int)d].to_panel, screen);
    return true;
}

} // namespace cockpit_overlay
