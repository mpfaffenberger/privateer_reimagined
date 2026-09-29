// Headless test of the real overlay compositing code. Only GPU texture
// allocation is stubbed; ImGui draw lists, glass and MFD warping are real.
#include "camera.h"
#include "cockpit_hud_internal.h"
#include "cockpit_overlay.h"
#include "material.h"
#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"
#include <cstdio>
#include <cstring>

bool load_texture_png(const std::string& path, TextureSlot& slot) {
    if (path.find("talon") != std::string::npos) return false;
    slot.valid = true;
    slot.view.id = 42;
    return true;
}
extern "C" sg_sampler sg_make_sampler(const sg_sampler_desc*) { return {1}; }
extern "C" uint64_t simgui_imtextureid_with_sampler(sg_view, sg_sampler) { return 42; }

namespace {
int failures = 0;
void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
int first_color(ImDrawList* dl, ImU32 color) {
    for (int i = 0; i < dl->VtxBuffer.Size; ++i)
        if (dl->VtxBuffer[i].col == color) return i;
    return -1;
}
}

int main() {
    using namespace cockpit_overlay;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1280, 813};
    io.DeltaTime = 1.0f / 60;
    unsigned char* pixels; int w, h;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    io.Fonts->SetTexID(1);
    Camera camera;

    ImGui::NewFrame();
    check(world_draw_list() == ImGui::GetForegroundDrawList(), "classic HUD retains foreground without art");
    draw("centurion", camera);
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    check(world_draw_list() == bg, "active cockpit routes world markers behind the art");
    check(bg->VtxBuffer.empty(), "draw publishes layout but defers glass until finalize");
    constexpr ImU32 marker = IM_COL32(255, 0, 255, 255);
    constexpr ImU32 instrument = IM_COL32(0, 255, 255, 255);
    constexpr ImU32 ui = IM_COL32(250, 100, 15, 255);
    world_draw_list()->AddRectFilled({0, 0}, io.DisplaySize, marker);
    Rect panel;
    check(display_panel(Display::Left, panel), "STATUS glass has a live display panel");
    ImGui::SetNextWindowPos({panel.x, panel.y});
    ImGui::SetNextWindowSize({panel.w, panel.h});
    ImGui::Begin(display_window_id(Display::Left), nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
    ImGui::GetWindowDrawList()->AddRectFilled({panel.x+10, panel.y+10},
                                             {panel.x+30, panel.y+30}, instrument);
    ImGui::End();
    ImGui::GetForegroundDrawList()->AddRectFilled({5, 5}, {20, 20}, ui);
    set_lights({true, false, true, false});
    finalize();
    const int m = first_color(bg, marker);
    const int g = first_color(bg, IM_COL32(6, 10, 9, 255));
    const int i = first_color(bg, instrument);
    check(m == 0 && g > m, "opaque MFD glass covers world markers");
    check(i > g, "live instrument content stays above glass");
    const ImDrawCmd* art_draw = nullptr;
    const ImDrawCmd* last_draw = nullptr;
    for (const ImDrawCmd& cmd : bg->CmdBuffer) {
        if (cmd.ElemCount) last_draw = &cmd;
        if (cmd.ElemCount && cmd.GetTexID() == 42) art_draw = &cmd;
    }
    check(art_draw && bg->IdxBuffer[art_draw->IdxOffset] > i,
          "cockpit PNG is composited over world markers and MFDs");
    check(art_draw && last_draw && last_draw->IdxOffset > art_draw->IdxOffset &&
          first_color(bg, IM_COL32(130, 240, 230, 255)) > i &&
          first_color(bg, IM_COL32(100, 245, 125, 255)) > i,
          "live power and AUTO lamps are painted on top of cockpit metal");
    check(first_color(ImGui::GetForegroundDrawList(), ui) >= 0 && first_color(bg, ui) < 0,
          "menus and cursor foreground stay independent of cockpit mask");
    ImGui::Render();

    io.DeltaTime = 0.9f;
    ImGui::NewFrame();
    draw("centurion", camera);
    set_lights({true, false, true, false});
    finalize();
    check(first_color(ImGui::GetBackgroundDrawList(), IM_COL32(130, 240, 230, 255)) < 0 &&
          first_color(ImGui::GetBackgroundDrawList(), IM_COL32(100, 245, 125, 255)) >= 0,
          "later normal-flight frame visibly changes power lamp, not AUTO");
    const int activity = first_color(ImGui::GetBackgroundDrawList(), IM_COL32(255, 180, 45, 255));
    check(activity >= 0 && ImGui::GetBackgroundDrawList()->VtxBuffer[activity].pos.y < io.DisplaySize.y * 0.5f,
          "normal-flight amber blink is actually emitted on upper cockpit rim");
    ImGui::Render();
    io.DeltaTime = 1.0f / 60;

    ImGui::NewFrame();
    draw("tarsus", camera);
    set_lights({true, false, true, false});
    finalize();
    check(first_color(ImGui::GetBackgroundDrawList(), IM_COL32(100, 245, 125, 255)) >= 0 &&
          first_color(ImGui::GetBackgroundDrawList(), IM_COL32(130, 240, 230, 255)) >= 0,
          "Tarsus auxiliary AUTO and activity lamps render in healthy flight");
    ImGui::Render();
    ImGui::NewFrame();
    draw("tarsus", camera);
    set_lights({true, false, false, false});
    finalize();
    check(first_color(ImGui::GetBackgroundDrawList(), IM_COL32(100, 245, 125, 255)) < 0 &&
          first_color(ImGui::GetBackgroundDrawList(), IM_COL32(255, 70, 35, 255)) < 0,
          "Tarsus blocked AUTO and healthy damage lamps stay dark");
    ImGui::Render();

    // Pilot toggle (#554): OFF is the classic full-screen HUD even on a hull
    // WITH art, through the same draw()/place_panel() calls main.cpp makes.
    using cockpit_hud::PanelPlacement;
    using cockpit_hud::place_panel;
    check(enabled(), "cockpit art defaults on");
    set_enabled(false);
    ImGui::NewFrame();
    draw("centurion", camera);
    set_lights({true, false, true, false});
    const PanelPlacement off = place_panel(Display::Left, "##classic_status",
                                           {16, 16}, {300, 224});
    bool any_display = false;
    for (int d = 0; d < kDisplayCount; ++d)
        any_display |= display_panel((Display)d, panel);
    finalize();
    check(!active() && !any_display && world_draw_list() == ImGui::GetForegroundDrawList(),
          "cockpit OFF: no MFD displays, world markers stay foreground");
    check(!off.in_display && std::strcmp(off.window_id, "##classic_status") == 0 &&
          off.pos.x == 16 && off.pos.y == 16 && off.size.x == 300 && off.size.y == 224,
          "cockpit OFF: HUD panels use the classic floating layout");
    check(ImGui::GetBackgroundDrawList()->VtxBuffer.empty(),
          "cockpit OFF: no art, glass or lamps are composited");
    ImGui::Render();
    set_enabled(true);
    ImGui::NewFrame();
    draw("centurion", camera);
    const PanelPlacement on = place_panel(Display::Left, "##classic_status",
                                          {16, 16}, {300, 224});
    check(active() && on.in_display &&
          std::strcmp(on.window_id, display_window_id(Display::Left)) == 0,
          "cockpit back ON: HUD returns to the cockpit displays");
    ImGui::Render();

    ImGui::NewFrame(); // draw skipped: autopilot/external camera
    check(!active() && world_draw_list() == ImGui::GetForegroundDrawList(),
          "skipped cockpit frame restores classic marker layer");
    draw("unmapped-hull", camera);
    check(!active(), "unknown hull does not activate cockpit occlusion");
    draw("talon", camera);
    check(!active(), "missing art does not hide world markers");
    ImGui::Render();
    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
