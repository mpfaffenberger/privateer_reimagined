// -----------------------------------------------------------------------------
// music_dj.cpp — tiny bar-music DJ panel (see music_dj.h; issue #264).
// -----------------------------------------------------------------------------
#include "music_dj.h"

#include "imgui.h"
#include "music.h"
#include "sokol_app.h"

#include <cstdio>

namespace music_dj {
namespace {

bool g_visible = false;

constexpr int k_tracks = 14;   // bar_music_01..14

// Cycle helper: current override -> prev/next track id (1..14). From the
// shuffle state (override 0), Next lands on 1 and Prev lands on 14.
int step(int cur, int dir) {
    if (cur <= 0) return dir > 0 ? 1 : k_tracks;
    int n = cur + dir;
    if (n < 1) n = k_tracks;
    if (n > k_tracks) n = 1;
    return n;
}

} // namespace

bool handle_event(const sapp_event* e) {
    // Ctrl+B, intercepted before ImGui focus can eat it — same reasoning as
    // cinematic_studio's Ctrl+K / debug_panel's Ctrl+M.
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_B &&
        (e->modifiers & SAPP_MODIFIER_CTRL)) {
        g_visible = !g_visible;
        std::printf("[music_dj] Ctrl+B -> %s\n", g_visible ? "show" : "hide");
        return true;
    }
    return false;
}

void set_visible(bool v) {
    g_visible = v;
    std::printf("[music_dj] set_visible -> %s\n", v ? "show" : "hide");
}

void build() {
    if (!g_visible) return;

    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Bar DJ (Ctrl+B)", &g_visible,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        const int pin = music::bar_track_pin();
        const int cur = music::bar_track_override();

        if (pin > 0)
            ImGui::Text("pin: bar_music_%02d (manual)", pin);
        else if (cur > 0)
            ImGui::Text("scene: bar_music_%02d (authored)", cur);
        else
            ImGui::TextDisabled("shuffled pool");

        // ---- cycle row (drives the manual pin; wins over scene music) ----
        if (ImGui::Button("<< prev"))
            music::pin_bar_track(step(cur, -1));
        ImGui::SameLine();
        if (ImGui::Button("next >>"))
            music::pin_bar_track(step(cur, +1));
        ImGui::SameLine();
        if (ImGui::Button("release"))
            music::pin_bar_track(0);

        ImGui::Separator();

        // ---- direct pick: 14 numbered buttons, 7 per row ----
        for (int i = 1; i <= k_tracks; ++i) {
            char label[8];
            std::snprintf(label, sizeof label, "%02d", i);
            const bool active = (cur == i);
            if (active)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      active && pin == i
                                          ? ImVec4(0.20f, 0.55f, 0.30f, 1.0f)
                                          : ImVec4(0.25f, 0.40f, 0.60f, 1.0f));
            if (ImGui::Button(label, ImVec2(34, 0)))
                music::pin_bar_track(pin == i ? 0 : i);   // click again = release
            if (active) ImGui::PopStyleColor();
            if (i % 7 != 0 && i != k_tracks) ImGui::SameLine();
        }

        ImGui::Separator();
        ImGui::TextDisabled("green = manual pin, blue = authored scene");
        ImGui::TextDisabled("track; release returns to scene/shuffle");
    }
    ImGui::End();
}

} // namespace music_dj
