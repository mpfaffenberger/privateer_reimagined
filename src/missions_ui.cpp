// -----------------------------------------------------------------------------
// missions_ui.cpp — shared Mission Computer / guild board presentation.
// -----------------------------------------------------------------------------

#include "missions.h"

#include "base_screens.h"
#include "player.h"
#include "ship_class.h"
#include "sfx.h"
#include "imgui.h"
#include "sokol_app.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

namespace missions {
namespace {

struct BoardTheme {
    const char* title;
    const char* subtitle;
    ImVec4 accent;
};

BoardTheme board_theme(MissionSource source) {
    switch (source) {
        case MissionSource::MercenariesGuild:
            return {"MERCENARIES GUILD", "High-risk combat contracts and priority defense work",
                    ImVec4(0.95f, 0.38f, 0.28f, 1.0f)};
        case MissionSource::MerchantsGuild:
            return {"MERCHANTS GUILD", "Certified freight, trade-security, and recovery contracts",
                    ImVec4(0.30f, 0.78f, 0.96f, 1.0f)};
        default:
            return {"MISSION COMPUTER", "Public contracts available at this port",
                    ImVec4(1.00f, 0.72f, 0.22f, 1.0f)};
    }
}

ImVec4 type_color(MissionType type) {
    switch (type) {
        case MissionType::CargoDelivery: return ImVec4(0.35f, 0.82f, 1.00f, 1.0f);
        case MissionType::Bounty:        return ImVec4(1.00f, 0.43f, 0.32f, 1.0f);
        case MissionType::Attack:        return ImVec4(1.00f, 0.34f, 0.34f, 1.0f);
        case MissionType::DefendBase:    return ImVec4(0.42f, 0.68f, 1.00f, 1.0f);
        case MissionType::Patrol:        return ImVec4(0.38f, 0.88f, 0.70f, 1.0f);
        case MissionType::Scout:         return ImVec4(0.62f, 0.90f, 0.42f, 1.0f);
    }
    return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
}

int source_index(MissionSource source) {
    return std::clamp(static_cast<int>(source), 0, 2);
}

std::array<std::string, 3> g_selected_offer;
std::array<std::string, 3> g_feedback;
std::array<double, 3> g_feedback_until{};

void flash(MissionSource source, const std::string& text) {
    const int idx = source_index(source);
    g_feedback[idx] = text;
    g_feedback_until[idx] = ImGui::GetTime() + 4.0;
}

bool source_is_guild(MissionSource source) {
    return source == MissionSource::MercenariesGuild ||
           source == MissionSource::MerchantsGuild;
}

bool& guild_flag(PlayerState& player, MissionSource source) {
    return source == MissionSource::MercenariesGuild
         ? player.merc_guild_member : player.merchant_guild_member;
}

int64_t guild_join_fee(MissionSource source) {
    return source == MissionSource::MercenariesGuild
         ? player::k_merc_guild_fee : player::k_merchant_guild_fee;
}

std::string offer_target(const Mission& mission) {
    char out[320]{};
    switch (mission.type) {
        case MissionType::CargoDelivery:
            std::snprintf(out, sizeof out, "%s — %s system",
                          mission.dest_base_name.c_str(), mission.dest_system_name.c_str());
            break;
        case MissionType::Bounty: {
            std::string region;
            for (const std::string& system : mission.bounty_region) {
                if (system.empty()) continue;
                if (!region.empty()) region += " / ";
                region += system;
            }
            std::snprintf(out, sizeof out, "%d %s target%s%s%s",
                          mission.count_required, mission.target_faction.c_str(),
                          mission.count_required == 1 ? "" : "s",
                          region.empty() ? "" : " — ", region.c_str());
            break;
        }
        case MissionType::Patrol:
            std::snprintf(out, sizeof out, "%d navigation points — %s system",
                          mission.nav_count, mission.target_system.c_str());
            break;
        case MissionType::Scout:
        case MissionType::Attack:
            std::snprintf(out, sizeof out, "%s — %s system",
                          mission.nav_targets.empty() ? "Navigation point"
                                                      : mission.nav_targets.front().c_str(),
                          mission.target_system.c_str());
            break;
        case MissionType::DefendBase:
            std::snprintf(out, sizeof out, "%s — %s system",
                          mission.target_base.c_str(), mission.target_system.c_str());
            break;
    }
    return out;
}

struct ProgressCell { std::string text; bool complete = false; };

int navs_done(const ActiveMission& mission) {
    int total = 0;
    for (uint8_t done : mission.nav_done) total += done != 0;
    return total;
}

ProgressCell active_progress(const ActiveMission& mission,
                             const std::string& current_base) {
    char out[160]{};
    switch (static_cast<MissionType>(mission.type)) {
        case MissionType::CargoDelivery:
            if (mission.dest_base == current_base) return {"READY TO DELIVER", true};
            std::snprintf(out, sizeof out, "Deliver to %s", mission.dest_base.c_str());
            break;
        case MissionType::Bounty:
            std::snprintf(out, sizeof out, "%d / %d targets",
                          mission.progress, mission.count_required);
            return {out, mission.count_required > 0 &&
                         mission.progress >= mission.count_required};
        case MissionType::Scout: {
            const bool done = !mission.nav_done.empty() && mission.nav_done.front();
            return {done ? "SURVEY COMPLETE" : "Survey pending", done};
        }
        case MissionType::Patrol: {
            const int required = mission.nav_targets.empty()
                               ? mission.nav_count : static_cast<int>(mission.nav_targets.size());
            const int done = navs_done(mission);
            std::snprintf(out, sizeof out, "%d / %d nav points", done, required);
            return {out, required > 0 && done >= required};
        }
        case MissionType::Attack:
        case MissionType::DefendBase:
            std::snprintf(out, sizeof out, "%d / %d hostiles",
                          mission.progress, mission.hostiles_required);
            return {out, mission.hostiles_required > 0 &&
                         mission.progress >= mission.hostiles_required};
    }
    return {out, false};
}

std::string block_reason(AcceptBlock block, const Mission& mission,
                         int used, int capacity) {
    switch (block) {
        case AcceptBlock::AlreadyActive: return "This contract is already active.";
        case AcceptBlock::ActiveLimit:   return "Contract limit reached — complete or abandon a mission first.";
        case AcceptBlock::CargoSpace: {
            char out[128];
            std::snprintf(out, sizeof out,
                          "Requires %d free cargo units; %d currently available.",
                          mission.units, std::max(0, capacity - used));
            return out;
        }
        case AcceptBlock::JumpDrive: return "A jump drive is required to reach this objective.";
        default: return "Contract ready for acceptance.";
    }
}

void stat_box(const char* label, const std::string& value, const ImVec4& accent,
              float width) {
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.52f, 0.55f, 0.62f, 1.0f), "%s", label);
    ImGui::SameLine();
    ImGui::TextColored(accent, "%s", value.c_str());
    ImGui::Dummy(ImVec2(width, 0.0f));
    ImGui::EndGroup();
}

void draw_join_prompt(BaseContext& ctx, MissionSource source,
                      const BoardTheme& theme, float sw, float sh) {
    PlayerState& player = *ctx.player;
    const int64_t fee = guild_join_fee(source);
    const float width = std::min(620.0f, sw - 80.0f);
    const float x = (sw - width) * 0.5f;
    const float y = std::max(110.0f, sh * 0.28f);

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025f, 0.03f, 0.045f, 0.94f));
    if (ImGui::BeginChild("##guild_join", ImVec2(width, 270.0f), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        ImGui::TextColored(theme.accent, "%s", theme.title);
        ImGui::TextColored(ImVec4(0.62f, 0.65f, 0.72f, 1.0f), "%s", theme.subtitle);
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextWrapped("Membership unlocks this guild's private contract board and higher-value work.");
        ImGui::Spacing();
        ImGui::Text("ONE-TIME DUES");
        ImGui::SameLine();
        ImGui::TextColored(theme.accent, "%lld credits", (long long)fee);
        ImGui::Text("AVAILABLE");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.48f, 0.90f, 0.55f, 1.0f), "%lld credits",
                           (long long)player.credits);
        ImGui::Spacing();
        const bool affordable = player.credits >= fee;
        ImGui::BeginDisabled(!affordable);
        if (ImGui::Button("JOIN GUILD", ImVec2(190.0f, 42.0f)) &&
            player::spend_credits(player, fee)) {
            guild_flag(player, source) = true;
            sfx::ui_click();
        }
        ImGui::EndDisabled();
        if (!affordable) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.28f, 1.0f),
                               "Need %lld more credits", (long long)(fee - player.credits));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

const Mission* selected_offer(MissionSource source) {
    const auto& offers = board(source);
    if (offers.empty()) return nullptr;
    std::string& selected = g_selected_offer[source_index(source)];
    auto it = std::find_if(offers.begin(), offers.end(),
                           [&](const Mission& mission) { return mission.id == selected; });
    if (it == offers.end()) {
        selected = offers.front().id;
        return &offers.front();
    }
    return &*it;
}

void draw_offer_list(MissionSource source, const BoardTheme& theme,
                     const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.018f, 0.022f, 0.032f, 0.92f));
    if (ImGui::BeginChild("##offer_list", size, true)) {
        ImGui::TextColored(theme.accent, "AVAILABLE CONTRACTS  %zu", board(source).size());
        ImGui::Separator();
        for (const Mission& mission : board(source)) {
            ImGui::PushID(mission.id.c_str());
            const bool selected = g_selected_offer[source_index(source)] == mission.id;
            ImGui::PushStyleColor(ImGuiCol_Header,
                                  ImVec4(theme.accent.x * 0.24f, theme.accent.y * 0.24f,
                                         theme.accent.z * 0.24f, 0.88f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.16f, 0.19f, 0.25f, 0.96f));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0.20f, 0.23f, 0.30f, 1.0f));
            const float card_width = ImGui::GetContentRegionAvail().x;
            const float text_width = std::max(40.0f, card_width - 20.0f);
            const float title_height = ImGui::CalcTextSize(
                mission.title.c_str(), nullptr, false, text_width).y;
            const float card_height = std::max(86.0f, 41.0f + title_height);
            if (ImGui::Selectable("##offer", selected, 0,
                                  ImVec2(0.0f, card_height))) {
                g_selected_offer[source_index(source)] = mission.id;
                sfx::ui_click();
            }
            ImGui::PopStyleColor(3);

            const ImVec2 lo = ImGui::GetItemRectMin();
            const ImVec2 hi = ImGui::GetItemRectMax();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->PushClipRect(ImVec2(lo.x + 1.0f, lo.y + 1.0f),
                             ImVec2(hi.x - 1.0f, hi.y - 1.0f), true);
            dl->AddText(ImVec2(lo.x + 10.0f, lo.y + 8.0f),
                        ImGui::ColorConvertFloat4ToU32(type_color(mission.type)),
                        type_label(mission.type));
            char reward[48];
            std::snprintf(reward, sizeof reward, "%lld CR", (long long)mission.reward);
            const float reward_w = ImGui::CalcTextSize(reward).x;
            dl->AddText(ImVec2(hi.x - reward_w - 10.0f, lo.y + 8.0f),
                        IM_COL32(120, 230, 140, 255), reward);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        ImVec2(lo.x + 10.0f, lo.y + 31.0f),
                        IM_COL32(232, 235, 240, 255), mission.title.c_str(),
                        nullptr, text_width);
            dl->PopClipRect();
            // Submit real layout space between cards. Never teleport the cursor
            // to extend a child window: modern ImGui correctly asserts on that.
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_briefing(BaseContext& ctx, MissionSource source,
                   const BoardTheme& theme, const Mission* mission,
                   int used, int capacity, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.025f, 0.030f, 0.043f, 0.94f));
    if (ImGui::BeginChild("##briefing", size, true)) {
        ImGui::TextColored(theme.accent, "CONTRACT BRIEFING");
        ImGui::Separator();
        if (!mission) {
            ImGui::TextDisabled("No contracts are currently posted at this source.");
        } else {
            ImGui::TextColored(type_color(mission->type), "%s", type_label(mission->type));
            ImGui::SameLine();
            ImGui::TextDisabled("  |  %s", source_label(source));
            ImGui::Spacing();
            ImGui::PushTextWrapPos();
            ImGui::TextWrapped("%s", mission->title.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "OBJECTIVE");
            ImGui::TextWrapped("%s", offer_target(*mission).c_str());
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.65f, 1.0f), "DETAILS");
            ImGui::TextWrapped("%s", mission->description.c_str());
            ImGui::Spacing();
            if (mission->type == MissionType::CargoDelivery) {
                ImGui::Text("CONSIGNMENT   %d units of %s",
                            mission->units, mission->commodity_id.c_str());
                ImGui::Text("HOLD AFTER ACCEPTANCE   %d / %d",
                            used + mission->units, capacity);
            }
            ImGui::Text("REWARD");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.48f, 0.92f, 0.56f, 1.0f),
                               "%lld credits", (long long)mission->reward);

            const AcceptBlock blocked = accept_block(*ctx.player, *mission, capacity);
            const bool ready = blocked == AcceptBlock::None;
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextColored(ready ? ImVec4(0.48f, 0.92f, 0.56f, 1.0f)
                                     : ImVec4(1.0f, 0.42f, 0.30f, 1.0f),
                               "%s", block_reason(blocked, *mission, used, capacity).c_str());
            ImGui::Spacing();
            ImGui::BeginDisabled(!ready);
            if (ImGui::Button("ACCEPT CONTRACT", ImVec2(220.0f, 42.0f))) {
                if (accept(*ctx.player, *mission, capacity)) {
                    sfx::ui_click();
                    flash(source, "Contract accepted and added to your active roster.");
                }
            }
            ImGui::EndDisabled();
        }
        const int idx = source_index(source);
        if (ImGui::GetTime() < g_feedback_until[idx]) {
            ImGui::Spacing();
            ImGui::TextColored(theme.accent, "%s", g_feedback[idx].c_str());
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_active_missions(BaseContext& ctx, const BoardTheme& theme,
                          float width, float height) {
    PlayerState& player = *ctx.player;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.018f, 0.022f, 0.032f, 0.94f));
    if (ImGui::BeginChild("##active_contracts", ImVec2(width, height), true)) {
        ImGui::TextColored(theme.accent, "ACTIVE CONTRACTS  %zu / 3", player.missions.size());
        ImGui::Separator();
        if (player.missions.empty()) {
            ImGui::TextDisabled("No active contracts. Select an offer above to review its briefing.");
        } else if (ImGui::BeginTable("##active_table", 4,
                    ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("CONTRACT", ImGuiTableColumnFlags_WidthStretch, 0.46f);
            ImGui::TableSetupColumn("PROGRESS", ImGuiTableColumnFlags_WidthStretch, 0.24f);
            ImGui::TableSetupColumn("REWARD", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("ACTION", ImGuiTableColumnFlags_WidthFixed, 170.0f);
            ImGui::TableHeadersRow();
            std::string deliver_id, abandon_id;
            for (const ActiveMission& mission : player.missions) {
                ImGui::PushID(mission.id.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", mission.title.c_str());
                ImGui::TableNextColumn();
                const ProgressCell progress = active_progress(mission, ctx.base_id);
                ImGui::TextColored(progress.complete
                                     ? ImVec4(0.48f, 0.92f, 0.56f, 1.0f)
                                     : ImVec4(0.82f, 0.84f, 0.88f, 1.0f),
                                   "%s", progress.text.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%lld", (long long)mission.reward);
                ImGui::TableNextColumn();
                if (mission.type == (int)MissionType::CargoDelivery &&
                    mission.dest_base == ctx.base_id) {
                    if (ImGui::SmallButton("DELIVER")) deliver_id = mission.id;
                    ImGui::SameLine();
                }
                if (mission.source == (int)MissionSource::Fixer) {
                    ImGui::TextDisabled("STORY");
                } else if (ImGui::SmallButton("ABANDON")) {
                    abandon_id = mission.id;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!deliver_id.empty() && complete_delivery(player, deliver_id, ctx.base_id))
                sfx::ui_click();
            if (!abandon_id.empty() && abandon(player, abandon_id))
                sfx::ui_click();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_board(BaseContext& ctx, MissionSource source) {
    PlayerState& player = *ctx.player;
    const BoardTheme theme = board_theme(source);
    const float dpi = sapp_dpi_scale();
    const float sw = (float)sapp_width() / dpi;
    const float sh = (float)sapp_height() / dpi;

    if (source_is_guild(source) && !guild_flag(player, source)) {
        draw_join_prompt(ctx, source, theme, sw, sh);
        return;
    }

    const ShipClass* ship = ship_class::find(player.ship_class_name);
    const int capacity = player::cargo_capacity(player, ship);
    const int used = player::cargo_units_used(player);

    const float left = 28.0f;
    const float right = 28.0f;
    const float top = 58.0f;
    const float bottom = 76.0f;
    const float gap = 12.0f;
    const float width = sw - left - right;
    const float active_h = player.missions.empty()
                         ? 62.0f : std::clamp(sh * 0.22f, 132.0f, 220.0f);
    const float body_y = top + 62.0f;
    const float active_y = sh - bottom - active_h;
    const float body_h = std::max(180.0f, active_y - body_y - gap);
    const float list_w = std::clamp(width * 0.40f, 360.0f, 680.0f);

    ImGui::SetCursorScreenPos(ImVec2(left, top));
    ImGui::BeginGroup();
    ImGui::TextColored(theme.accent, "%s", theme.title);
    ImGui::TextColored(ImVec4(0.58f, 0.61f, 0.68f, 1.0f), "%s", theme.subtitle);
    ImGui::EndGroup();
    ImGui::SameLine(left + std::max(480.0f, width * 0.48f));
    stat_box("CREDITS", std::to_string(player.credits), theme.accent, 130.0f);
    ImGui::SameLine();
    stat_box("CARGO", std::to_string(used) + " / " + std::to_string(capacity),
             used >= capacity ? ImVec4(1.0f, 0.38f, 0.28f, 1.0f) : theme.accent, 130.0f);
    ImGui::SameLine();
    stat_box("ACTIVE", std::to_string(player.missions.size()) + " / 3", theme.accent, 100.0f);

    const Mission* selected = selected_offer(source);
    ImGui::SetCursorScreenPos(ImVec2(left, body_y));
    draw_offer_list(source, theme, ImVec2(list_w, body_h));
    ImGui::SetCursorScreenPos(ImVec2(left + list_w + gap, body_y));
    draw_briefing(ctx, source, theme, selected, used, capacity,
                  ImVec2(width - list_w - gap, body_h));
    ImGui::SetCursorScreenPos(ImVec2(left, active_y));
    draw_active_missions(ctx, theme, width, active_h);
}

} // namespace

void register_screen() {
    base_screens::register_screen(BaseScreen::MissionComputer,
        [](BaseContext& ctx) { draw_board(ctx, MissionSource::Computer); });
    base_screens::register_screen(BaseScreen::MercenariesGuild,
        [](BaseContext& ctx) { draw_board(ctx, MissionSource::MercenariesGuild); });
    base_screens::register_screen(BaseScreen::MerchantsGuild,
        [](BaseContext& ctx) { draw_board(ctx, MissionSource::MerchantsGuild); });
}

} // namespace missions
