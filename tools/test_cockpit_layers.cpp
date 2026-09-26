// Headless test of the real overlay compositing code. Only GPU texture
// allocation is stubbed; ImGui draw lists, glass and MFD warping are real.
#include "camera.h"
#include "cockpit_overlay.h"
#include "material.h"
#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"
#include <cstdio>

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
    finalize();
    const int m = first_color(bg, marker);
    const int g = first_color(bg, IM_COL32(6, 10, 9, 255));
    const int i = first_color(bg, instrument);
    check(m == 0 && g > m, "opaque MFD glass covers world markers");
    check(i > g, "live instrument content stays above glass");
    const ImDrawCmd* last_draw = nullptr;
    for (const ImDrawCmd& cmd : bg->CmdBuffer)
        if (cmd.ElemCount) last_draw = &cmd; // PopTexture leaves an empty command
    check(bg->VtxBuffer.Size >= i + 4 && last_draw && last_draw->GetTexID() == 42,
          "cockpit PNG is composited last over world markers and MFDs");
    check(first_color(ImGui::GetForegroundDrawList(), ui) >= 0 && first_color(bg, ui) < 0,
          "menus and cursor foreground stay independent of cockpit mask");
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
