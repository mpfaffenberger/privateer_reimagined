// Dense HUD text vs. narrow cockpit MFDs (#430).
//
// Real ImGui, the game's font, every hull's real display geometry and the
// real begin_panel() glass font sizing. Only GPU texture allocation is
// stubbed. Run from the repo root (loads assets/fonts/Inter-Regular.ttf).
#include "camera.h"
#include "cockpit_hud_internal.h"
#include "canopy_mask.h"
#include "cockpit_overlay.h"
#include "hud_text_fit.h"
#include "material.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "sokol_app.h"
#include "sokol_imgui.h"

#include <cstdio>

bool load_texture_png(const std::string&, TextureSlot& slot) {
    slot.valid = true;
    slot.view.id = 42;
    return true;
}
bool cockpit_overlay::load_canopy_mask(const std::string&, CanopyMask&) { return false; }
extern "C" sg_sampler sg_make_sampler(const sg_sampler_desc*) { return {1}; }
extern "C" uint64_t simgui_imtextureid_with_sampler(sg_view, sg_sampler) { return 42; }

namespace {

using cockpit_overlay::Display;
using hud_text_fit::Phrasing;

int failures = 0;
void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

constexpr ImU32 kCol = IM_COL32(255, 255, 255, 255);

// Worst cases in shipped data: longest named-NPC display name, gun label,
// faction + stance pair, and a full launcher readout.
constexpr const char* kLongName  = "Vera \"Crusader\" Rostova";
constexpr const char* kFaction   = "merchant";
constexpr const char* kStance    = "NEUTRAL";
constexpr const char* kOldStance = "merchant  NEUTRAL";   // pre-#430 one-liner
const Phrasing kGun[]    = { { "ARMAMENTS", "GUN IONIC PULSE CANNON  4/4" },
                             { "GUN", "IONIC PULSE CANNON  4/4" } };
const Phrasing kLaunch[] = { { "LAUNCH", "TORP x12  [MSL 2 / TORP 1]" },
                             { "LAUNCH", "TORP x12" } };

// Right edge of the current window's content, and the furthest any item has
// reached since reset_extent().
float content_right() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    return w->Pos.x + w->Size.x - w->WindowPadding.x;
}
void reset_extent() {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    w->DC.CursorMaxPos.x = w->DC.CursorStartPos.x;
}
float extent() { return ImGui::GetCurrentWindow()->DC.CursorMaxPos.x; }

void frame_begin() {
    ImGui::NewFrame();
}
void frame_end() {
    ImGui::Render();
    // Pretend to be a renderer: accept every texture request.
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->Status == ImTextureStatus_WantCreate) tex->SetTexID(1);
        if (tex->Status == ImTextureStatus_WantDestroy) tex->SetStatus(ImTextureStatus_Destroyed);
        else if (tex->Status != ImTextureStatus_OK) tex->SetStatus(ImTextureStatus_OK);
    }
}

// TARGET identity column: 3-line thumbnail, then the fitted lines beside it,
// exactly as draw_target_mfd lays them out in a display.
bool target_block_fits(float& old_overflow) {
    const float thumb = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
    ImGui::Dummy(ImVec2(thumb, thumb));
    ImGui::SameLine();
    ImGui::BeginGroup();
    old_overflow = ImGui::CalcTextSize(kOldStance).x - ImGui::GetContentRegionAvail().x;
    reset_extent();
    hud_text_fit::text(kLongName, kCol);
    hud_text_fit::pair_or_wrap(kFaction, kStance, kCol);
    hud_text_fit::text("DIST   99999 m", kCol);
    const bool ok = extent() <= content_right() + 0.01f;
    ImGui::EndGroup();
    return ok;
}

bool armaments_header_fits(float& old_overflow) {
    old_overflow = ImGui::CalcTextSize(kGun[0].head).x + ImGui::GetStyle().ItemSpacing.x +
                   ImGui::CalcTextSize(kGun[0].tail).x - ImGui::GetContentRegionAvail().x;
    reset_extent();
    hud_text_fit::line(kGun, IM_ARRAYSIZE(kGun), kCol, kCol);
    hud_text_fit::line(kLaunch, IM_ARRAYSIZE(kLaunch), kCol, kCol);
    return extent() <= content_right() + 0.01f;
}

// Did the pre-#430 layout overflow on any hull? [0] TARGET, [1] ARMAMENTS.
bool g_old_overflow[2] = {};

void check_hull(const char* hull, const Camera& cam) {
    char label[160];
    frame_begin();
    cockpit_overlay::draw(hull, cam);
    const struct { Display d; const char* page; } pages[] = {
        { Display::Right, "TARGET" }, { Display::Left, "STATUS/Weapons" } };
    for (const auto& page : pages) {
        cockpit_hud::PanelPlacement p;
        std::snprintf(label, sizeof label, "%s has a %s display", hull, page.page);
        check(cockpit_hud::display_placement(page.d, p), label);
        if (!p.in_display) continue;
        cockpit_hud::begin_panel(p);
        float old = 0.0f;
        const bool target = page.d == Display::Right;
        const bool fits = target ? target_block_fits(old) : armaments_header_fits(old);
        g_old_overflow[target ? 0 : 1] |= old > 0.0f;
        std::snprintf(label, sizeof label, "%s %s (%.0fx%.0f, %.1fpx font) stays inside glass",
                      hull, page.page, p.size.x, p.size.y, ImGui::GetFontSize());
        check(fits, label);
        cockpit_hud::end_panel(p);
    }
    frame_end();
}

// Classic 280 px free-floating panels keep their original wording whenever
// it fits; long gun names (which clipped even there) now fall back instead.
void check_classic() {
    frame_begin();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(280, 248));
    cockpit_hud::push_hud_style();
    ImGui::Begin("##classic", nullptr, cockpit_hud::kHudWindowFlags);
    const Phrasing gun[] = { { "ARMAMENTS", "GUN LASER  2/2" },
                             { "GUN", "LASER  2/2" } };
    const Phrasing launch[] = { { "LAUNCH", "HS x4  [MSL 1 / TORP 0]" },
                                { "LAUNCH", "HS x4" } };
    const hud_text_fit::Fit g = hud_text_fit::line(gun, 2, kCol, kCol);
    const hud_text_fit::Fit l = hud_text_fit::line(launch, 2, kCol, kCol);
    check(g.option == 0 && g.scale == 1.0f && l.option == 0 && l.scale == 1.0f,
          "classic ARMAMENTS keeps full title + launcher counts, unscaled");
    float unused;
    check(armaments_header_fits(unused), "classic ARMAMENTS with the longest gun stays inside");
    ImGui::Dummy(ImVec2(80, 80));   // classic TARGET thumbnail
    ImGui::SameLine();
    ImGui::BeginGroup();
    check(hud_text_fit::pair_or_wrap(kFaction, kStance, kCol) == 1,
          "classic TARGET keeps faction + stance on one line");
    ImGui::EndGroup();
    ImGui::End();
    cockpit_hud::pop_hud_style();
    frame_end();
}

} // namespace

int main() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = { 1280, 813 };
    io.DeltaTime = 1.0f / 60;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImFont* font = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Regular.ttf", 16.0f);
    check(font != nullptr, "game HUD font loads (run from repo root)");
    if (!font) return 1;
    io.FontDefault = font;

    Camera cam;
    for (const char* hull : { "centurion", "talon", "tarsus", "galaxy" })
        check_hull(hull, cam);
    check(g_old_overflow[0], "harness reproduces the pre-#430 TARGET faction-line overflow");
    check(g_old_overflow[1], "harness reproduces the pre-#430 ARMAMENTS header overflow");
    check_classic();

    ImGui::DestroyContext();
    return failures ? 1 : 0;
}
