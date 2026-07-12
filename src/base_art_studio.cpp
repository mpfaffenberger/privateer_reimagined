#include "base_art_studio.h"
#include "base_screens.h"
#include "imgui.h"
#include "sokol_app.h"

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

void launch(const char* action, const base_screens::CurrentRoomInfo& info) {
    if (g_worker.joinable()) g_worker.join();
    const auto repo = find_repo_root();
    if (repo.empty()) { g_status = "Repository root not found"; return; }

    const auto jobs = repo / "generated" / "base_art_studio" / "jobs";
    std::filesystem::create_directories(jobs);
    const auto id = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto request = jobs / (id + ".request.json");
    g_result_path = jobs / (id + ".result.json");

    std::ofstream out(request);
    out << "{\"action\":\"" << action
        << "\",\"archetype\":\"" << escape_json(info.archetype)
        << "\",\"room\":\"" << escape_json(info.room)
        << "\",\"prompt\":\"" << escape_json(g_prompt)
        << "\",\"reference_mode\":\"" << k_ref_tokens[g_ref_mode] << "\"}\n";
    out.close();

    g_job_identity = identity(info);
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

void poll() {
    if (!g_done.exchange(false)) return;
    if (g_worker.joinable()) g_worker.join();
    g_result_text = read_all(g_result_path);
    const bool ok = g_result_text.find("\"ok\": true") != std::string::npos;
    const std::string message = json_string(g_result_text, "message");
    g_status = ok ? message : "Job failed: " + message;
    if (!ok) return;

    base_screens::CurrentRoomInfo current;
    if (!base_screens::current_room_info(current) || identity(current) != g_job_identity) {
        g_status += " (saved; switch back to preview)";
        return;
    }
    const std::string preview = json_string(g_result_text, "preview_path");
    if (!preview.empty() && !base_screens::reload_current_room_texture(preview))
        g_status += " (preview reload failed)";
}

} // namespace

void init() { g_status = "Idle"; }
void shutdown() { if (g_worker.joinable()) g_worker.join(); }

bool handle_event(const sapp_event* event) {
    if (event->type == SAPP_EVENTTYPE_KEY_DOWN && !event->key_repeat &&
        event->key_code == SAPP_KEYCODE_F11) {
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
    if (!ImGui::Begin("Base Art Studio (F11)", &g_open)) { ImGui::End(); return; }
    if (!base_screens::current_room_info(info)) {
        ImGui::TextWrapped("Land at a base and open an art-backed room to use the studio.");
        ImGui::End();
        return;
    }

    const std::string room_id = identity(info);
    if (room_id != g_room_identity) {
        g_room_identity = room_id;
        seed_prompt(info);
        g_status = "Ready";
    }
    ImGui::Text("Base: %s (%s)", info.display_name.c_str(), info.base_id.c_str());
    ImGui::Text("Room: %s   Archetype: %s", info.room.c_str(), info.archetype.c_str());
    ImGui::TextWrapped("Installed: %s", info.asset_path.c_str());
    ImGui::Separator();
    ImGui::InputTextMultiline("Prompt", g_prompt, sizeof(g_prompt), ImVec2(-1, 280));
    ImGui::Combo("References", &g_ref_mode, k_ref_labels, 4);

    const bool busy = g_worker.joinable();
    ImGui::BeginDisabled(busy || !g_prompt[0]);
    if (ImGui::Button("Generate + Live Preview")) launch("generate", info);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Install Latest (backup)")) launch("install", info);
    ImGui::SameLine();
    if (ImGui::Button("Revert Original")) launch("revert", info);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextWrapped("Status: %s", g_status.c_str());
    ImGui::BeginChild("##base_art_result", ImVec2(0, 90), true);
    ImGui::TextWrapped("%s", g_result_text.c_str());
    ImGui::EndChild();
    ImGui::End();
}

} // namespace base_art_studio
