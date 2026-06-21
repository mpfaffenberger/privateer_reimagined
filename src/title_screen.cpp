#include "title_screen.h"

#include "imgui.h"

#include <string>

namespace title_screen {

namespace {

// Single-frame click result. Stored module-local so callers get a
// stable Action without us having to thread a pointer back through.
Action s_last_action = Action::None;

// Helper: draw a small bordered frame with an optional label. We use
// ImGui::GetWindowDrawList() to get a rectangle into the chrome without
// needing to manage sub-window state.
void draw_viewport_frame(ImDrawList* dl, ImVec2 p0, ImVec2 p1,
                         ImU32 border_col, ImU32 fill_col) {
    // Filled interior (dark inset feel).
    dl->AddRectFilled(p0, p1, fill_col, 4.0f);
    // Ornate double border: outer thick + inner thin.
    dl->AddRect(p0, p1, border_col, 4.0f, 0, 3.0f);
    ImVec2 inset0(p0.x + 6.0f, p0.y + 6.0f);
    ImVec2 inset1(p1.x - 6.0f, p1.y - 6.0f);
    dl->AddRect(inset0, inset1, border_col, 2.0f, 0, 1.0f);
}

} // namespace

Action draw() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                        vp->WorkPos.y + vp->WorkSize.y * 0.5f);

    // The chrome plate is 720 wide and ~440 tall: similar aspect to the
    // classic WCPrivateer title screen so the proportions read right.
    constexpr float kW = 720.0f;
    constexpr float kH = 440.0f;
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(kW, kH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoTitleBar;

    // Colour palette — matches the cockpit amber HUD + a cooler cyan for
    // body text accents, so the chrome reads as part of the same UI.
    const ImU32 amber = IM_COL32(255, 200, 60, 255);
    const ImU32 cyan  = IM_COL32(120, 220, 255, 255);
    const ImU32 dim   = IM_COL32(170, 170, 180, 255);
    const ImU32 dark  = IM_COL32(20, 20, 30, 255);

    ImGui::PushStyleColor(ImGuiCol_Text, amber);
    ImGui::Begin("##title_screen", nullptr, flags);
    ImGui::PopStyleColor();

    // 1) Wordmark at the top. We don't have a custom font, so use the
    // largest ImGui size for a bold-ish read. The underline rect ties it
    // to the chrome plate.
    const char* word = "PRIVATEER";
    const float word_sz = 56.0f;
    ImVec2 word_sz_vec = ImGui::CalcTextSize(word);
    word_sz_vec.x *= (word_sz / ImGui::GetTextLineHeight());
    ImVec2 word_pos(ImGui::GetWindowPos().x + (kW - word_sz_vec.x) * 0.5f,
                    ImGui::GetWindowPos().y + 18.0f);
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(), word_sz, word_pos, amber, word);
    // Decorative underline beneath the wordmark.
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(word_pos.x - 12.0f, word_pos.y + word_sz + 4.0f),
        ImVec2(word_pos.x + word_sz_vec.x + 12.0f,
               word_pos.y + word_sz + 4.0f),
        amber, 2.0f);

    // 2) Subtitle ("AN ASPECT OF WING COMMANDER" - classic style)
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::SetCursorPos(ImVec2(0.0f, 88.0f));
    // Centre the subtitle using a dummy spacer: render text centred
    // inside a full-width invisible group.
    const char* sub = "AN ASPECT OF WING COMMANDER";
    const float sub_w = ImGui::CalcTextSize(sub).x;
    ImGui::Dummy(ImVec2(kW, 0.0f));
    ImGui::SameLine((kW - sub_w) * 0.5f);
    ImGui::TextUnformatted(sub);
    ImGui::PopStyleColor();

    // 3) Viewport frame: the classic 'loading image' slot. Right now
    // we just draw a placeholder with a starfield-ish gradient so the
    // chrome has a focal point. Future work: render a tiny 3D scene
    // (planet + ship) inside this rectangle.
    const float vp_top    = 130.0f;
    const float vp_h      = 200.0f;
    const float vp_margin = 36.0f;
    ImVec2 win_pos = ImGui::GetWindowPos();
    ImVec2 vp0(win_pos.x + vp_margin,
               win_pos.y + vp_top);
    ImVec2 vp1(win_pos.x + kW - vp_margin,
               win_pos.y + vp_top + vp_h);
    draw_viewport_frame(ImGui::GetWindowDrawList(), vp0, vp1, amber, dark);
    // Tag inside the frame so the player knows what this is going to be.
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImVec2 tag = ImGui::CalcTextSize("[ starfield will go here ]");
    ImGui::GetWindowDrawList()->AddText(
        ImVec2((vp0.x + vp1.x - tag.x) * 0.5f,
               (vp0.y + vp1.y - tag.y) * 0.5f),
        dim, "[ starfield will go here ]");
    ImGui::PopStyleColor();

    // 4) Action buttons: NEW, LOAD, OPTIONS, QUIT. Each is a wide
    // fixed-width button laid out horizontally near the bottom.
    ImGui::SetCursorPos(ImVec2(0.0f, kH - 56.0f));
    ImGui::Dummy(ImVec2(0, 0));   // anchor
    const float btn_w   = 132.0f;
    const float btn_h   = 32.0f;
    const float spacing = 18.0f;
    const float row_w   = 4 * btn_w + 3 * spacing;
    const float row_x   = (kW - row_w) * 0.5f;
    // ImGui doesn't position buttons relative to a custom anchor so we
    // use SetCursorPos + a horizontal group via SameLine.
    ImGui::SetCursorPos(ImVec2(row_x, kH - 48.0f));
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
    s_last_action = Action::None;   // consume
    return a;
}

} // namespace title_screen
