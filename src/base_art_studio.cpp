#include "base_art_studio.h"
#include "base_screens.h"
#include "ship_class.h"
#include "imgui.h"
#include "sokol_app.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace base_art_studio {
namespace {

bool g_open = false;
char g_prompt[8192]{};
int g_ref_mode = 1;
std::string g_status = "Idle";
std::string g_result_text;
std::string g_room_identity;
std::string g_job_identity;
std::string g_job_ship;
bool g_job_is_landing = false;
int g_landing_ship_index = 0;
bool g_placement_guide = true;
bool g_background_installed = true;
bool g_background_original = false;
bool g_background_latest = false;
bool g_ship_pose = true;
bool g_ship_design = true;
bool g_composite_installed = false;
bool g_composite_latest = false;
std::thread g_worker;
std::atomic<bool> g_done{false};
std::filesystem::path g_result_path;

constexpr const char* k_ref_labels[] = {"Text only", "Original", "Latest", "Both"};
constexpr const char* k_ref_tokens[] = {"text", "original", "latest", "both"};

std::filesystem::path find_repo_root() {
    auto path = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(path / "tools" / "base_art_studio_job.py") &&
            std::filesystem::exists(path / "CMakeLists.txt")) return path;
        if (!path.has_parent_path()) break;
        path = path.parent_path();
    }
    return {};
}

std::string quote(const std::filesystem::path& path) {
#ifdef _WIN32
    std::string value = path.string();
    size_t pos = 0;
    while ((pos = value.find('"', pos)) != std::string::npos) {
        value.insert(pos, 1, '\\');
        pos += 2;
    }
    return "\"" + value + "\"";
#else
    std::string value = "'";
    for (char c : path.string()) value += c == '\'' ? "'\\''" : std::string(1, c);
    return value + "'";
#endif
}

std::string escape_json(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c != '\r') out += c;
    }
    return out;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string json_string(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return {};
    pos = text.find(':', pos + needle.size());
    pos = text.find('"', pos);
    if (pos == std::string::npos) return {};
    std::string out;
    for (++pos; pos < text.size() && text[pos] != '"'; ++pos) {
        if (text[pos] == '\\' && pos + 1 < text.size()) ++pos;
        out += text[pos];
    }
    return out;
}

std::string identity(const base_screens::CurrentRoomInfo& info) {
    return info.base_id + "|" + info.archetype + "|" + info.room;
}

bool latest_exists(const base_screens::CurrentRoomInfo& info) {
    const auto repo = find_repo_root();
    return !repo.empty() && std::filesystem::is_regular_file(
        repo / "generated" / "base_art_studio" / info.archetype / info.room / "latest.png");
}

void seed_prompt(const base_screens::CurrentRoomInfo& info) {
    const std::string text =
        "Create a fresh cinematic retro-futurist space-opera background for the " +
        info.room + " at " + info.display_name + ", a " + info.archetype +
        " base operated by " + (info.faction.empty() ? "a frontier faction" : info.faction) +
        ". Preserve the room's functional layout and recognizable navigation landmarks. "
        "Grounded industrial materials, layered depth, atmospheric lighting, modern "
        "high-detail readability, wide game-background composition. No text, logos, UI, "
        "watermarks, captions, or recognizable characters.";
    std::snprintf(g_prompt, sizeof(g_prompt), "%s", text.c_str());
}

void launch_request(const char* action, const std::string& request_json,
                    const std::string& job_identity, bool landing,
                    const std::string& ship = {}) {
    if (g_worker.joinable()) g_worker.join();
    const auto repo = find_repo_root();
    if (repo.empty()) { g_status = "Repository root not found"; return; }
    const auto jobs = repo / "generated" / "base_art_studio" / "jobs";
    std::filesystem::create_directories(jobs);
    const auto id = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto request = jobs / (id + ".request.json");
    g_result_path = jobs / (id + ".result.json");
    std::ofstream(request) << request_json << "\n";

    g_job_identity = job_identity;
    g_job_is_landing = landing;
    g_job_ship = ship;
    g_done = false;
    g_status = std::string("Running ") + action + "...";
    g_result_text.clear();
    const auto script = repo / "tools" / "base_art_studio_job.py";
#ifdef _WIN32
    const std::string python = "python";
#else
    const std::string python = "python3";
#endif
    const std::string command = python + " " + quote(script) + " --request " +
        quote(request) + " --result " + quote(g_result_path);
    g_worker = std::thread([command] {
        std::system(command.c_str());
        g_done = true;
    });
}

void launch_room(const char* action, const base_screens::CurrentRoomInfo& info) {
    const std::string json = "{\"action\":\"" + std::string(action) +
        "\",\"archetype\":\"" + escape_json(info.archetype) +
        "\",\"room\":\"" + escape_json(info.room) +
        "\",\"prompt\":\"" + escape_json(g_prompt) +
        "\",\"reference_mode\":\"" + k_ref_tokens[g_ref_mode] + "\"}";
    launch_request(action, json, identity(info), false);
}

void launch_landing(const char* action, const base_screens::CurrentLandingInfo& info,
                    const std::string& ship) {
    const auto flag = [](bool value) { return value ? "true" : "false"; };
    const std::string json = "{\"target_kind\":\"landing_ship\",\"action\":\"" +
        std::string(action) + "\",\"archetype\":\"" + escape_json(info.archetype) +
        "\",\"ship\":\"" + escape_json(ship) + "\",\"prompt\":\"" +
        escape_json(g_prompt) + "\",\"references\":{" +
        "\"placement_guide\":" + flag(g_placement_guide) +
        ",\"background_installed\":" + flag(g_background_installed) +
        ",\"background_original\":" + flag(g_background_original) +
        ",\"background_latest\":" + flag(g_background_latest) +
        ",\"ship_pose\":" + flag(g_ship_pose) +
        ",\"ship_design\":" + flag(g_ship_design) +
        ",\"composite_installed\":" + flag(g_composite_installed) +
        ",\"composite_latest\":" + flag(g_composite_latest) + "}}";
    launch_request(action, json, info.base_id + "|" + info.archetype + "|landing",
                   true, ship);
}

void poll() {
    if (!g_done.exchange(false)) return;
    if (g_worker.joinable()) g_worker.join();
    g_result_text = read_all(g_result_path);
    const bool ok = g_result_text.find("\"ok\": true") != std::string::npos;
    const std::string message = json_string(g_result_text, "message");
    g_status = ok ? message : "Job failed: " + message;
    if (!ok) return;

    const std::string preview = json_string(g_result_text, "preview_path");
    if (g_job_is_landing) {
        base_screens::CurrentLandingInfo current;
        if (!base_screens::current_landing_info(current) ||
            current.base_id + "|" + current.archetype + "|landing" != g_job_identity) {
            g_status += " (saved; return to this landing bay to preview)";
            return;
        }
        if (!preview.empty() && !base_screens::preview_current_landing_composite(
                preview, g_job_ship)) g_status += " (composite preview failed)";
        return;
    }
    base_screens::CurrentRoomInfo current;
    if (!base_screens::current_room_info(current) || identity(current) != g_job_identity) {
        g_status += " (saved; switch back to preview)";
        return;
    }
    if (!preview.empty() && !base_screens::reload_current_room_texture(preview))
        g_status += " (preview reload failed)";
}

bool landing_latest_exists(const base_screens::CurrentLandingInfo& info,
                           const std::string& ship) {
    const auto repo = find_repo_root();
    return !repo.empty() && std::filesystem::is_regular_file(repo / "generated" /
        "base_art_studio" / "landing_ships" / info.archetype / ship / "latest.png");
}

void seed_landing_prompt(const base_screens::CurrentLandingInfo& info,
                         const std::string& ship) {
    const std::string text =
        "Create a finished cinematic landing-bay scene for the " + ship + " at " +
        info.display_name + ". Integrate the exact referenced ship design naturally into "
        "the referenced " + info.archetype + " landing bay. Use the placement guide for "
        "approximate position, scale, and camera angle, but correct perspective and contact "
        "with the ground. Match environmental lighting, shadows, reflected color, painterly "
        "detail, atmospheric depth, and resolution so ship and bay look like one authored "
        "image. Preserve the bay layout and ship identity. No text, logos, UI, people, "
        "watermarks, or additional spacecraft.";
    std::snprintf(g_prompt, sizeof(g_prompt), "%s", text.c_str());
}

void build_landing(const base_screens::CurrentLandingInfo& info) {
    const auto& ships = ship_class::all();
    if (ships.empty()) { ImGui::TextUnformatted("No ship classes are loaded."); return; }
    const std::string landing_id = info.base_id + "|" + info.archetype + "|landing";
    if (landing_id != g_room_identity) {
        g_room_identity = landing_id;
        g_landing_ship_index = 0;
        for (int i = 0; i < (int)ships.size(); ++i)
            if (ships[(size_t)i].name == info.player_ship_class) g_landing_ship_index = i;
        seed_landing_prompt(info, ships[(size_t)g_landing_ship_index].name);
        g_status = "Ready";
    }
    g_landing_ship_index = std::clamp(g_landing_ship_index, 0, (int)ships.size() - 1);
    const std::string ship = ships[(size_t)g_landing_ship_index].name;

    ImGui::Text("Landing Composite: %s (%s)", info.display_name.c_str(),
                info.archetype.c_str());
    if (ImGui::BeginCombo("Ship", ship.c_str())) {
        for (int i = 0; i < (int)ships.size(); ++i) {
            if (ImGui::Selectable(ships[(size_t)i].name.c_str(), i == g_landing_ship_index)) {
                g_landing_ship_index = i;
                base_screens::clear_current_landing_composite_preview();
                seed_landing_prompt(info, ships[(size_t)i].name);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::TextWrapped("Background: %s", info.background_path.c_str());
    ImGui::InputTextMultiline("Prompt", g_prompt, sizeof(g_prompt), ImVec2(-1, 230));
    if (ImGui::CollapsingHeader("Reference images", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Placement guide (background + posed ship)", &g_placement_guide);
        ImGui::Checkbox("Installed landing background", &g_background_installed);
        ImGui::Checkbox("Protected original landing background", &g_background_original);
        ImGui::Checkbox("Latest generated landing background", &g_background_latest);
        ImGui::Checkbox("Nearest posed ship sprite", &g_ship_pose);
        ImGui::Checkbox("Canonical ship design references", &g_ship_design);
        ImGui::Checkbox("Installed ship composite", &g_composite_installed);
        ImGui::Checkbox("Latest generated ship composite", &g_composite_latest);
    }

    const bool busy = g_worker.joinable();
    const bool has_latest = landing_latest_exists(info, ship);
    ImGui::BeginDisabled(busy || !g_prompt[0]);
    if (ImGui::Button("Generate Ship Scene + Live Preview"))
        launch_landing("generate", info, ship);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !has_latest);
    if (ImGui::Button("Install for This Ship (backup)"))
        launch_landing("install", info, ship);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Clear Preview")) {
        base_screens::clear_current_landing_composite_preview();
        g_status = "Showing installed composite or dynamic ship fallback";
    }
    ImGui::EndDisabled();
    if (!has_latest) ImGui::TextDisabled("Generate this ship once to unlock installation.");
}

} // namespace

void init() { g_status = "Idle"; }
void shutdown() { if (g_worker.joinable()) g_worker.join(); }

bool handle_event(const sapp_event* event) {
    if (event->type == SAPP_EVENTTYPE_KEY_DOWN && !event->key_repeat &&
        event->key_code == SAPP_KEYCODE_F1) {
        g_open = !g_open;
        return true;
    }
    return false;
}

void build() {
    poll();
    if (!g_open) return;

    base_screens::CurrentRoomInfo info;
    ImGui::SetNextWindowSize(ImVec2(720, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Base Art Studio (F1)", &g_open)) { ImGui::End(); return; }
    if (!base_screens::current_room_info(info)) {
        ImGui::TextWrapped("Land at a base and open an art-backed room to use the studio.");
        ImGui::End();
        return;
    }

    base_screens::CurrentLandingInfo landing;
    if (base_screens::current_landing_info(landing)) {
        build_landing(landing);
        ImGui::Separator();
        ImGui::TextWrapped("Status: %s", g_status.c_str());
        ImGui::BeginChild("##landing_art_result", ImVec2(0, 70), true);
        ImGui::TextWrapped("%s", g_result_text.c_str());
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    const std::string room_id = identity(info);
    if (room_id != g_room_identity) {
        g_room_identity = room_id;
        g_ref_mode = 1; // Original is always valid; Latest/Both are room-specific.
        seed_prompt(info);
        g_status = "Ready";
    }
    ImGui::Text("Base: %s (%s)", info.display_name.c_str(), info.base_id.c_str());
    ImGui::Text("Room: %s   Archetype: %s", info.room.c_str(), info.archetype.c_str());
    ImGui::TextWrapped("Installed: %s", info.asset_path.c_str());
    ImGui::Separator();
    ImGui::InputTextMultiline("Prompt", g_prompt, sizeof(g_prompt), ImVec2(-1, 280));
    const bool has_latest = latest_exists(info);
    if (!has_latest && g_ref_mode >= 2) g_ref_mode = 1;
    if (ImGui::BeginCombo("References", k_ref_labels[g_ref_mode])) {
        for (int i = 0; i < 4; ++i) {
            const bool unavailable = i >= 2 && !has_latest;
            ImGui::BeginDisabled(unavailable);
            if (ImGui::Selectable(k_ref_labels[i], g_ref_mode == i)) g_ref_mode = i;
            ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    if (!has_latest)
        ImGui::TextDisabled("Latest and Both unlock after this room's first generation.");

    const bool busy = g_worker.joinable();
    ImGui::BeginDisabled(busy || !g_prompt[0]);
    if (ImGui::Button("Generate + Live Preview")) launch_room("generate", info);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !has_latest);
    if (ImGui::Button("Install Latest (backup)")) launch_room("install", info);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Revert Original")) launch_room("revert", info);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextWrapped("Status: %s", g_status.c_str());
    ImGui::BeginChild("##base_art_result", ImVec2(0, 90), true);
    ImGui::TextWrapped("%s", g_result_text.c_str());
    ImGui::EndChild();
    ImGui::End();
}

} // namespace base_art_studio
