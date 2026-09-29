// hud_text_fit.cpp — see header (#430).
#include "hud_text_fit.h"

#include "imgui_internal.h"   // GImGui->FontSizeBase: the size PushFont takes

#include <algorithm>
#include <cstdio>

namespace hud_text_fit {
namespace {

bool has(const char* s) { return s && *s; }

float text_w(const char* s) {
    return has(s) ? ImGui::CalcTextSize(s, nullptr, false).x : 0.0f;
}

// Fixed (font-independent) gap SameLine() leaves between head and tail.
float gap(const Phrasing& p) {
    return has(p.head) && has(p.tail) ? ImGui::GetStyle().ItemSpacing.x : 0.0f;
}

float width(const Phrasing& p) { return text_w(p.head) + gap(p) + text_w(p.tail); }

void colored(const char* s, ImU32 col) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

void draw(const Phrasing& p, ImU32 head_col, ImU32 tail_col) {
    if (has(p.head)) colored(p.head, head_col);
    if (has(p.head) && has(p.tail)) ImGui::SameLine();
    if (has(p.tail)) colored(p.tail, tail_col);
}

// Draw `p` at the current font if it fits the rest of the line; otherwise
// push a font shrunk until it does. Glyph advances don't scale perfectly
// linearly with size, so re-measure after each push instead of trusting the
// ratio. Returns the scale used.
float draw_fitted(const Phrasing& p, ImU32 head_col, ImU32 tail_col) {
    const float room = ImGui::GetContentRegionAvail().x;
    const float base = GImGui->FontSizeBase;
    float scale = 1.0f;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (scale != 1.0f) ImGui::PushFont(nullptr, base * scale);
        const float text = width(p) - gap(p);
        if (text + gap(p) <= room || text <= 0.0f) {
            draw(p, head_col, tail_col);
            if (scale != 1.0f) ImGui::PopFont();
            return scale;
        }
        if (scale != 1.0f) ImGui::PopFont();
        scale *= std::max(room - gap(p), 1.0f) / text * 0.995f;
    }
    // Pathologically narrow window: draw at the smallest scale reached.
    ImGui::PushFont(nullptr, base * scale);
    draw(p, head_col, tail_col);
    ImGui::PopFont();
    return scale;
}

} // namespace

void text(const char* s, ImU32 col) {
    draw_fitted({ s, nullptr }, col, col);
}

Fit line(const Phrasing* options, int count, ImU32 head_col, ImU32 tail_col) {
    IM_ASSERT(options && count > 0);
    const float room = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i + 1 < count; ++i) {
        if (width(options[i]) <= room) {
            draw(options[i], head_col, tail_col);
            return { i, 1.0f };
        }
    }
    return { count - 1, draw_fitted(options[count - 1], head_col, tail_col) };
}

int pair_or_wrap(const char* head, const char* tail, ImU32 col) {
    char joined[128];
    std::snprintf(joined, sizeof(joined), "%s  %s", head, tail);
    if (text_w(joined) <= ImGui::GetContentRegionAvail().x) {
        colored(joined, col);
        return 1;
    }
    text(head, col);
    text(tail, col);
    return 2;
}

} // namespace hud_text_fit
