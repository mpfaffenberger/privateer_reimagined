// -----------------------------------------------------------------------------
// navmap_auditor.cpp — F10 ImGui tool for auditing system-map coordinates.
// -----------------------------------------------------------------------------

#include "navmap_auditor.h"

#include "imgui.h"
#include "material.h"
#include "navmap_projection.h"
#include "system_def.h"
#include "sokol_imgui.h"
#include "stb_image.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace navmap_auditor {
namespace {

struct SystemEntry { std::string id, path; };
struct EditNav { HMM_Vec3 wiki = { 0.0f, 0.0f, 0.0f }; };
struct RefImage {
    std::string system_name;
    std::string path;
    TextureSlot tex;
    int w = 0;
    int h = 0;
};

bool s_visible = false;
int  s_selected = 0;
std::vector<SystemEntry> s_systems;
std::optional<StarSystem> s_loaded;
std::string s_loaded_path;
std::string s_edit_path;
std::vector<EditNav> s_edits;
RefImage s_ref;

std::string stem(const std::filesystem::path& p) { return p.stem().string(); }

HMM_Vec3 wiki_from_engine(HMM_Vec3 p) {
    // engine=(wiki.x,wiki.z,wiki.y)*10/3 -> wiki=(engine.X,engine.Z,engine.Y)*0.3
    return HMM_V3(p.X * 0.3f, p.Z * 0.3f, p.Y * 0.3f);
}

HMM_Vec3 engine_from_wiki(HMM_Vec3 w) {
    constexpr float k = 10.0f / 3.0f;
    return HMM_V3(w.X * k, w.Z * k, w.Y * k);
}

HMM_Vec2 map_from_wiki(HMM_Vec3 w) { return navmap_project_world(engine_from_wiki(w)); }

std::string wcnews_url_name(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == ' ') out += '_';
        else if (c == '\'') out += "%27";
        else out += c;
    }
    return out;
}

std::string wcnews_url(const StarSystem& sys) {
    return "https://www.wcnews.com/wcpedia/images/System_Map_-_" +
           wcnews_url_name(sys.name) + ".png";
}

std::string ref_cache_path(const StarSystem& sys) {
    std::filesystem::create_directories("assets/cache/wcnews_maps");
    std::string id = sys.name;
    for (char& c : id) if (c == ' ' || c == '\'') c = '_';
    return "assets/cache/wcnews_maps/System_Map_-_" + id + ".png";
}

void refresh_system_list() {
    namespace fs = std::filesystem;
    s_systems.clear();
    const fs::path dir = "assets/systems";
    if (!fs::exists(dir)) return;
    for (const fs::directory_entry& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".json")
            s_systems.push_back({ stem(e.path()), e.path().string() });
    }
    std::sort(s_systems.begin(), s_systems.end(),
              [](const SystemEntry& a, const SystemEntry& b) { return a.id < b.id; });
    if (s_selected >= (int)s_systems.size()) s_selected = 0;
}

const StarSystem* selected_system() {
    if (s_systems.empty()) return nullptr;
    const std::string& path = s_systems[s_selected].path;
    if (!s_loaded || s_loaded_path != path) {
        s_loaded = load_system(path);
        s_loaded_path = path;
    }
    return s_loaded ? &*s_loaded : nullptr;
}

void reset_edits_from_system(const StarSystem& sys) {
    s_edits.clear();
    s_edits.reserve(sys.nav_points.size());
    for (const NavPointDef& n : sys.nav_points) s_edits.push_back({ wiki_from_engine(n.position) });
    s_edit_path = s_loaded_path;
}

void ensure_edits(const StarSystem& sys) {
    if (s_edit_path != s_loaded_path || s_edits.size() != sys.nav_points.size())
        reset_edits_from_system(sys);
}

bool is_escaped(const std::string& s, size_t pos) {
    int slash_count = 0;
    while (pos > 0 && s[--pos] == '\\') ++slash_count;
    return (slash_count & 1) != 0;
}

size_t find_matching(const std::string& s, size_t open_pos, char open_ch, char close_ch) {
    bool in_str = false;
    int depth = 0;
    for (size_t i = open_pos; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' && !is_escaped(s, i)) in_str = !in_str;
        if (in_str) continue;
        if (c == open_ch) ++depth;
        if (c == close_ch && --depth == 0) return i;
    }
    return std::string::npos;
}

size_t find_key(const std::string& s, const char* key, size_t begin, size_t end) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t p = s.find(needle, begin);
    return (p != std::string::npos && p < end) ? p : std::string::npos;
}

bool find_key_array(const std::string& s, const char* key, size_t obj_begin, size_t obj_end,
                    size_t& key_pos, size_t& arr_begin, size_t& arr_end) {
    key_pos = find_key(s, key, obj_begin, obj_end);
    if (key_pos == std::string::npos) return false;
    const size_t colon = s.find(':', key_pos);
    if (colon == std::string::npos || colon >= obj_end) return false;
    arr_begin = s.find('[', colon);
    if (arr_begin == std::string::npos || arr_begin >= obj_end) return false;
    arr_end = find_matching(s, arr_begin, '[', ']');
    return arr_end != std::string::npos && arr_end < obj_end;
}

std::vector<std::pair<size_t, size_t>> nav_object_ranges(const std::string& text) {
    std::vector<std::pair<size_t, size_t>> ranges;
    const size_t nav_key = text.find("\"nav_points\"");
    if (nav_key == std::string::npos) return ranges;
    const size_t arr_begin = text.find('[', nav_key);
    const size_t arr_end = (arr_begin == std::string::npos) ? std::string::npos
                                                            : find_matching(text, arr_begin, '[', ']');
    if (arr_end == std::string::npos) return ranges;

    bool in_str = false;
    int depth = 0;
    size_t obj_begin = std::string::npos;
    for (size_t i = arr_begin + 1; i < arr_end; ++i) {
        const char c = text[i];
        if (c == '"' && !is_escaped(text, i)) in_str = !in_str;
        if (in_str) continue;
        if (c == '{') { if (depth == 0) obj_begin = i; ++depth; }
        else if (c == '}' && depth > 0 && --depth == 0) {
            ranges.push_back({ obj_begin, i });
            obj_begin = std::string::npos;
        }
    }
    return ranges;
}

std::string fmt_position(HMM_Vec3 engine) {
    std::ostringstream os;
    os << "[\n        " << std::lround(engine.X) << ",\n        "
       << std::lround(engine.Y) << ",\n        " << std::lround(engine.Z) << "\n      ]";
    return os.str();
}

void erase_key_value(std::string& text, size_t key_pos, size_t value_end) {
    size_t begin = key_pos;
    while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) --begin;
    size_t end = value_end + 1;
    if (end < text.size() && text[end] == ',') ++end;
    if (end < text.size() && text[end] == '\r') ++end;
    if (end < text.size() && text[end] == '\n') ++end;
    else if (begin > 0 && text[begin - 1] == ',') --begin;
    text.erase(begin, end - begin);
}

bool save_wiki_edits_to_json(const StarSystem& sys) {
    if (s_loaded_path.empty() || s_edits.size() != sys.nav_points.size()) return false;
    std::ifstream in(s_loaded_path);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto ranges = nav_object_ranges(text);
    if (ranges.size() != sys.nav_points.size()) return false;

    for (int i = (int)ranges.size() - 1; i >= 0; --i) {
        size_t obj_begin = ranges[i].first, obj_end = ranges[i].second;
        size_t key_pos = 0, arr_begin = 0, arr_end = 0;
        if (find_key_array(text, "map_position", obj_begin, obj_end, key_pos, arr_begin, arr_end))
            erase_key_value(text, key_pos, arr_end);
        if (find_key_array(text, "position", obj_begin, obj_end, key_pos, arr_begin, arr_end))
            text.replace(arr_begin, arr_end - arr_begin + 1, fmt_position(engine_from_wiki(s_edits[i].wiki)));
    }

    std::ofstream out(s_loaded_path, std::ios::trunc);
    if (!out) return false;
    out << text;

    if (s_loaded) {
        for (int i = 0; i < (int)s_loaded->nav_points.size(); ++i) {
            s_loaded->nav_points[i].position = engine_from_wiki(s_edits[i].wiki);
            s_loaded->nav_points[i].has_map_position = false;
        }
    }
    std::printf("[navmap_auditor] saved wiki coord edits -> %s\n", s_loaded_path.c_str());
    return true;
}

ImU32 color_for(const NavPointDef& n) {
    if (n.dockable) return IM_COL32(110, 230, 130, 255);
    if (n.kind == "jump") return IM_COL32(80, 150, 255, 255);
    return IM_COL32(120, 255, 150, 255);
}

float half_extent_for(const StarSystem& sys) {
    float half = 1.0f;
    for (int i = 0; i < (int)sys.nav_points.size(); ++i) {
        const HMM_Vec2 mp = (i < (int)s_edits.size()) ? map_from_wiki(s_edits[i].wiki)
                                                      : navmap_project_world(sys.nav_points[i].position);
        half = std::max(half, std::fabs(mp.X));
        half = std::max(half, std::fabs(mp.Y));
    }
    return half * 1.05f;
}

void draw_map(const StarSystem& sys, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float edge = std::min(size.x, size.y);
    const ImVec2 ctr { p0.x + edge * 0.5f, p0.y + edge * 0.5f };
    const float half = half_extent_for(sys);
    const float scale = 0.92f * edge / (half * 2.0f);
    auto to_screen = [&](float x, float y) { return ImVec2(ctr.x + x * scale, ctr.y - y * scale); };

    dl->AddRect(p0, ImVec2(p0.x + edge, p0.y + edge), IM_COL32(90, 110, 130, 220));
    constexpr int kGrid = 7;
    for (int i = 0; i <= kGrid; ++i) {
        const float t = -half + (2.0f * half * i / kGrid);
        dl->AddLine(to_screen(t, -half), to_screen(t, half), IM_COL32(55, 80, 110, 170));
        dl->AddLine(to_screen(-half, t), to_screen(half, t), IM_COL32(55, 80, 110, 170));
    }
    dl->AddLine(to_screen(-half, 0), to_screen(half, 0), IM_COL32(120, 120, 80, 120));
    dl->AddLine(to_screen(0, -half), to_screen(0, half), IM_COL32(120, 120, 80, 120));

    for (int i = 0; i < (int)sys.nav_points.size(); ++i) {
        const NavPointDef& n = sys.nav_points[i];
        const HMM_Vec2 mp = (i < (int)s_edits.size()) ? map_from_wiki(s_edits[i].wiki)
                                                      : navmap_project_world(n.position);
        const ImVec2 sp = to_screen(mp.X, mp.Y);
        if (n.dockable) {
            dl->AddRectFilled(ImVec2(sp.x - 5, sp.y - 5), ImVec2(sp.x + 5, sp.y + 5), color_for(n));
            dl->AddRect(ImVec2(sp.x - 5, sp.y - 5), ImVec2(sp.x + 5, sp.y + 5), IM_COL32_WHITE);
        } else {
            dl->AddCircleFilled(sp, 5.0f, color_for(n), 16);
        }
        char label[16];
        std::snprintf(label, sizeof(label), "%d", i + 1);
        dl->AddText(ImVec2(sp.x + 7, sp.y - 7), IM_COL32(230, 230, 230, 255), label);
    }
    ImGui::Dummy(ImVec2(edge, edge));
}

std::string edited_json_snippet(const StarSystem& sys) {
    std::ostringstream os;
    for (int i = 0; i < (int)sys.nav_points.size(); ++i) {
        os << "// " << sys.nav_points[i].name << "\n"
           << "\"position\": " << fmt_position(engine_from_wiki(s_edits[i].wiki)) << ",\n";
    }
    return os.str();
}

void draw_nav_table(const StarSystem& sys) {
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("nav_audit_table", 10, flags, ImVec2(0, 0))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 260);
    ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("wiki X", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("wiki Y", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("wiki Z", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("map X,Y", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableSetupColumn("flip X", ImGuiTableColumnFlags_WidthFixed, 62);
    ImGui::TableSetupColumn("flip Z", ImGuiTableColumnFlags_WidthFixed, 62);
    ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, 62);
    ImGui::TableHeadersRow();

    for (int i = 0; i < (int)sys.nav_points.size(); ++i) {
        const NavPointDef& n = sys.nav_points[i];
        EditNav& e = s_edits[i];
        const HMM_Vec2 mp = map_from_wiki(e.wiki);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", i + 1);
        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(n.name.c_str());
        if (n.has_map_position) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1, .8f, .2f, 1), "*"); }
        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(n.kind.c_str());
        ImGui::PushID(i);
        ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputFloat("##wikix", &e.wiki.X, 0, 0, "%.0f") && false) {}
        if (ImGui::IsItemDeactivatedAfterEdit()) save_wiki_edits_to_json(sys);
        ImGui::TableSetColumnIndex(4); ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputFloat("##wikiy", &e.wiki.Y, 0, 0, "%.0f") && false) {}
        if (ImGui::IsItemDeactivatedAfterEdit()) save_wiki_edits_to_json(sys);
        ImGui::TableSetColumnIndex(5); ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputFloat("##wikiz", &e.wiki.Z, 0, 0, "%.0f") && false) {}
        if (ImGui::IsItemDeactivatedAfterEdit()) save_wiki_edits_to_json(sys);
        ImGui::TableSetColumnIndex(6); ImGui::Text("%.0f, %.0f", mp.X, mp.Y);
        ImGui::TableSetColumnIndex(7); if (ImGui::SmallButton("X")) { e.wiki.X = -e.wiki.X; save_wiki_edits_to_json(sys); }
        ImGui::TableSetColumnIndex(8); if (ImGui::SmallButton("Z")) { e.wiki.Z = -e.wiki.Z; save_wiki_edits_to_json(sys); }
        ImGui::TableSetColumnIndex(9); if (ImGui::SmallButton("R")) { e.wiki = wiki_from_engine(n.position); save_wiki_edits_to_json(sys); }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void destroy_ref_texture() {
    if (s_ref.tex.valid) {
        sg_destroy_view(s_ref.tex.view);
        sg_destroy_image(s_ref.tex.image);
    }
    s_ref.tex = {};
    s_ref.w = 0;
    s_ref.h = 0;
}

void load_ref_texture_from_cache() {
    destroy_ref_texture();
    if (!std::filesystem::exists(s_ref.path)) return;
    int channels = 0;
    stbi_info(s_ref.path.c_str(), &s_ref.w, &s_ref.h, &channels);
    load_texture_png(s_ref.path, s_ref.tex);
}

void ensure_reference_loaded(const StarSystem& sys) {
    if (s_ref.system_name == sys.name) return;
    destroy_ref_texture();
    s_ref = RefImage{};
    s_ref.system_name = sys.name;
    s_ref.path = ref_cache_path(sys);
    load_ref_texture_from_cache();
}

void draw_reference_panel(const StarSystem& sys) {
    ensure_reference_loaded(sys);
    const std::string url = wcnews_url(sys);
    ImGui::TextWrapped("WCNews reference: %s", url.c_str());
    if (ImGui::Button("Copy URL")) ImGui::SetClipboardText(url.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Fetch/cache image")) {
        const std::string cmd = "curl -L --fail --silent --show-error -o \"" + s_ref.path + "\" \"" + url + "\"";
        const int rc = std::system(cmd.c_str());
        std::printf("[navmap_auditor] fetch rc=%d url=%s\n", rc, url.c_str());
        load_ref_texture_from_cache();
    }
    ImGui::Separator();
    if (s_ref.tex.valid) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float img_w = (s_ref.w > 0) ? float(s_ref.w) : 1.0f;
        const float img_h = (s_ref.h > 0) ? float(s_ref.h) : 1.0f;
        const float scale = std::min(avail.x / img_w, avail.y / img_h);
        const ImVec2 draw_sz(img_w * scale, img_h * scale);
        const float indent = std::max(0.0f, (avail.x - draw_sz.x) * 0.5f);
        if (indent > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
        ImGui::Image(simgui_imtextureid(s_ref.tex.view), draw_sz);
    } else {
        ImGui::TextWrapped("No cached image yet. Hit Fetch/cache image. If WCNews 404s, copy URL and yell at the filename goblin.");
    }
}

} // namespace

void init() {
    refresh_system_list();
    std::printf("[navmap_auditor] ready — F10 to toggle (%zu systems)\n", s_systems.size());
}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN && e->key_code == SAPP_KEYCODE_F10) {
        s_visible = !s_visible;
        if (s_visible && s_systems.empty()) refresh_system_list();
        return true;
    }
    return false;
}

void build() {
    if (!s_visible) return;
    ImGui::SetNextWindowSize(ImVec2(1320, 780), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Navmap Auditor (F10)", &s_visible)) { ImGui::End(); return; }

    if (ImGui::Button("Refresh systems")) refresh_system_list();
    ImGui::SameLine();
    ImGui::TextDisabled("Edit wiki X/Y/Z; blur cell to save. * means stale authored map_position existed and will be removed on save.");

    ImGui::BeginChild("systems", ImVec2(210, 0), true);
    for (int i = 0; i < (int)s_systems.size(); ++i) {
        if (ImGui::Selectable(s_systems[i].id.c_str(), i == s_selected)) {
            s_selected = i;
            s_loaded.reset();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("audit", ImVec2(0, 0), false);
    const StarSystem* sys = selected_system();
    if (!sys) { ImGui::TextWrapped("No system loaded."); ImGui::EndChild(); ImGui::End(); return; }
    ensure_edits(*sys);

    ImGui::Text("%s — %zu nav points", sys->name.c_str(), sys->nav_points.size());
    ImGui::SameLine();
    if (ImGui::Button("Reset edits from JSON")) reset_edits_from_system(*sys);
    ImGui::SameLine();
    if (ImGui::Button("Copy edited position snippets")) {
        const std::string text = edited_json_snippet(*sys);
        ImGui::SetClipboardText(text.c_str());
    }

    ImGui::BeginChild("map", ImVec2(430, 430), true);
    draw_map(*sys, ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("reference", ImVec2(0, 430), true);
    draw_reference_panel(*sys);
    ImGui::EndChild();

    ImGui::Separator();
    draw_nav_table(*sys);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace navmap_auditor
