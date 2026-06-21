#include "title_screen.h"

#include "imgui.h"

#include <string>

namespace title_screen {

namespace {

// Single-frame click result. Stored module-local so callers get a
// stable Action without us having to thread a pointer back through.
Action s_last_action = Action::None;

// Draw a thin bordered frame to look like an arcade "viewport slot".
// Used as the chrome top + bottom rails of the title screen.
void draw_rail(ImDrawList* dl, ImVec2 p0, ImVec2 p1,
               ImU32 border_col, ImU32 fill_col) {
    dl->AddRectFilled(p0, p1, fill_col, 0.0f);
    // Outer thick + inner thin border for that 'art-deco' chrome look.
    dl->AddRect(p0, p1, border_col, 0.0f, 1.0f);
    ImVec2 inset0(p0.x + 4.0f, p0.y + 4.0f);
    ImVec2 inset1(p1.x - 4.0f, p1.y - 4.0f);
    dl->AddRect(inset0, inset1, border_col, 0.0f, 1.0f);
}

} // namespace

Action draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 sz = vp->WorkSize;

    // Full-screen modal: cover the entire viewport with a translucent
    // background that lets the scene (star + skybox + patrol) show
    // through while keeping the chrome plate readable.
    ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);   // bg is fully transparent so the
                                          // star/skybox/ships render through

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar;

    const ImU32 amber = IM_COL32(255, 200, 60, 255);
    const ImU32 dim   = IM_COL32(170, 170, 180, 255);
    const ImU32 dark  = IM_COL32(0,   0,   0,   160);   // semi-transparent

    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::Begin("##title_screen", nullptr, flags);
    ImGui::PopStyleColor();

    const float W = sz.x;
    const float H = sz.y;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 1) Top chrome rail: holds the PRIVATEER wordmark + subtitle. ~110 px
    //    tall: enough room for a 56pt wordmark and a dim subtitle.
    const float top_h = 110.0f;
    ImVec2 top0(vp->WorkPos.x, vp->WorkPos.y);
    ImVec2 top1(vp->WorkPos.x + W, vp->WorkPos.y + top_h);
    draw_rail(dl, top0, top1, amber, dark);
    // Decorative inner seam along the bottom of the rail.
    dl->AddLine(ImVec2(top0.x + 24.0f, top1.y - 1.0f),
                ImVec2(top1.x - 24.0f, top1.y - 1.0f),
                amber, 1.0f);

    // 2) Bottom chrome rail: holds the buttons + a thin scanline motif.
    //    96 px tall: 32-px button + 32 px margin above + below.
    const float bot_h = 96.0f;
    ImVec2 bot0(vp->WorkPos.x, vp->WorkPos.y + H - bot_h);
    ImVec2 bot1(vp->WorkPos.x + W, vp->WorkPos.y + H);
    draw_rail(dl, bot0, bot1, amber, dark);
    dl->AddLine(ImVec2(bot0.x + 24.0f, bot0.y + 1.0f),
                ImVec2(bot1.x - 24.0f, bot0.y + 1.0f),
                amber, 1.0f);

    // 3) PRIVATEER wordmark + subtitle (centred in the top rail).
    const char* word = "PRIVATEER";
    const float word_sz = 56.0f;
    ImVec2 word_sz_vec = ImGui::CalcTextSize(word);
    word_sz_vec.x *= (word_sz / ImGui::GetTextLineHeight());
    ImVec2 word_pos(vp->WorkPos.x + (W - word_sz_vec.x) * 0.5f,
                    top0.y + 16.0f);
    dl->AddText(ImGui::GetFont(), word_sz, word_pos, amber, word);
    // Subtitle in dim grey below the wordmark.
    const char* sub = "AN ASPECT OF WING COMMANDER";
    ImVec2 sub_sz = ImGui::CalcTextSize(sub);
    dl->AddText(ImVec2(vp->WorkPos.x + (W - sub_sz.x) * 0.5f,
                       word_pos.y + word_sz + 6.0f),
                dim, sub);

    // 4) Action buttons (NEW / LOAD / OPTIONS / QUIT) in the bottom rail.
    const float btn_w   = 168.0f;
    const float btn_h   = 40.0f;
    const float spacing = 22.0f;
    const float row_w   = 4 * btn_w + 3 * spacing;
    const float row_x   = vp->WorkPos.x + (W - row_w) * 0.5f;
    ImGui::SetCursorPos(ImVec2(row_x - vp->WorkPos.x,
                               bot0.y - vp->WorkPos.y + (bot_h - btn_h) * 0.5f));
    auto render_btn = [&](const char* label, Action act) {
        if (ImGui::Button(label, ImVec2(btn_w, btn_h))) {
            s_last_action = act;
        }
    };
    render_btn("NEW",     Action::NewGame);
    ImGui::SameLine(0.0f, spacing);
    render_btn("LOAD",    Action::LoadGame);
    ImGui::SameLine(0.0f, spacing);
    render_btn("OPTIONS", Action::Options);
    ImGui::SameLine(0.0f, spacing);
    render_btn("QUIT",    Action::Quit);

    ImGui::End();
    const Action a = s_last_action;
    s_last_action = Action::None;
    return a;
}

} // namespace title_screen
