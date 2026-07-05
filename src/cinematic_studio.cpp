// -----------------------------------------------------------------------------
// cinematic_studio.cpp — the Cinematic Studio ImGui panel (Phase B).
//
// Widget layer only: all file IO / JSON shaping lives in cinematic_studio_io
// (engine-free, headlessly tested), and all playback verbs go through host
// hooks wired in main.cpp. See cinematic_studio.h for the design contract.
//
// Widget style follows debug_panel.cpp: fixed char[] InputText buffers (no
// imgui_stdlib in the tree), file-scope statics for the one-and-only panel,
// PushID per repeated row. Directory scans are throttled to ~2s.
// -----------------------------------------------------------------------------

#include "cinematic_studio.h"

#include "cinematic.h"          // play/stop status introspection
#include "cinematic_parse.h"    // engine-free parse_document for the Lines tab
#include "cinematic_studio_io.h"
#include "cinematic_triggers.h" // triggers::count / triggers::load

#include "HandmadeMath.h"       // HMM_Vec3 for offset override
#include "imgui.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cinematic::studio {

namespace io = cinematic::studio_io;

static constexpr const char* kCinematicsDir  = "assets/cinematics";
static constexpr const char* kTriggersPath   = "assets/cinematics/triggers.json";
static constexpr const char* kVoiceProfiles  = "assets/data/voice_profiles.json";
static constexpr double      kScanPeriodSec  = 2.0;

static bool  g_visible = false;
static Hooks g_hooks;

// One-line feedback strip at the bottom of the window ("wrote req_...",
// "play refused: not in Flight", ...). Overwritten by each action.
static std::string g_status;

void set_hooks(Hooks h) { g_hooks = std::move(h); }

bool handle_event(const sapp_event* e) {
    // Ctrl+K, intercepted before ImGui focus can eat it — same reasoning
    // (and same Ctrl-not-Cmd choice) as debug_panel's Ctrl+M toggle.
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_K &&
        (e->modifiers & SAPP_MODIFIER_CTRL)) {
        g_visible = !g_visible;
        return true;
    }
    return false;
}

// ---- small helpers ----------------------------------------------------------

static std::string trimmed(const char* buf) {
    std::string s(buf);
    const size_t nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    // strip leading/trailing whitespace so " id " doesn't make weird files
    const size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

// Unique request id: unixtime, suffixed when two sends land in one second.
static std::string fresh_request_id() {
    static long long last_t = 0;
    static int       seq    = 0;
    const long long t = (long long)std::time(nullptr);
    seq = (t == last_t) ? seq + 1 : 0;
    last_t = t;
    char buf[48];
    if (seq == 0) std::snprintf(buf, sizeof(buf), "req_%lld", t);
    else          std::snprintf(buf, sizeof(buf), "req_%lld_%d", t, seq);
    return buf;
}

// ---- REQUESTS cache (shared by Compose's post-send refresh) ------------------

static std::vector<io::RequestEntry> g_requests;
static double g_next_scan = 0.0;   // ImGui::GetTime() deadline; 0 = scan now

static void refresh_requests(bool force) {
    const double now = ImGui::GetTime();
    if (!force && now < g_next_scan) return;
    g_requests  = io::scan_requests(io::kStudioDir);
    g_next_scan = now + kScanPeriodSec;
}

// ---- COMPOSE ------------------------------------------------------------------

static const char* kQualities[] = { "low", "medium", "high" };

static void build_compose() {
    static char brief[4096]    = "";
    static char triggers[2048] = "";
    static char outcome[2048]  = "";
    static char suggest[128]   = "";
    static char style[256]     = "";
    static int  quality        = 1;   // medium

    ImGui::TextWrapped("Describe the cutscene in English. The out-of-game "
                       "director agent compiles it into a cinematic JSON + "
                       "triggers + outcome.");
    // Full-width (-1) boxes clip ImGui's right-side labels off the panel, so
    // caption each box ABOVE it and hide the widget label with "##".
    ImGui::TextDisabled("BRIEF - the cutscene, in English");
    ImGui::InputTextMultiline("##brief",    brief,    sizeof(brief),    ImVec2(-1, 90));
    ImGui::TextDisabled("TRIGGERS - when it fires (ship/cargo/missiles/nav proximity...)");
    ImGui::InputTextMultiline("##triggers", triggers, sizeof(triggers), ImVec2(-1, 55));
    ImGui::TextDisabled("OUTCOME - world state after it ends (player position, spawned ships...)");
    ImGui::InputTextMultiline("##outcome",  outcome,  sizeof(outcome),  ImVec2(-1, 55));
    ImGui::InputText("suggested id", suggest, sizeof(suggest));
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("image quality", &quality, kQualities, 3);
    ImGui::InputText("style extra", style, sizeof(style));

    const bool can_send = !trimmed(brief).empty();
    if (!can_send) ImGui::BeginDisabled();
    if (ImGui::Button("Send to Director")) {
        io::Request r;
        r.id                = fresh_request_id();
        r.kind              = "author";
        r.cinematic_id      = trimmed(suggest);
        r.brief             = trimmed(brief);
        r.triggers_text     = trimmed(triggers);
        r.outcome_text      = trimmed(outcome);
        r.image_quality     = kQualities[quality];
        r.image_style_extra = trimmed(style);
        std::string err;
        g_status = io::write_request(r, io::kStudioDir, err)
                       ? "queued " + r.id + " (watch the Requests tab)"
                       : "write FAILED: " + err;
        refresh_requests(/*force=*/true);   // show it immediately
    }
    if (!can_send) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("non-blocking: writes %s/requests/", io::kStudioDir);
}

// ---- REQUESTS ------------------------------------------------------------------

static void build_requests() {
    refresh_requests(/*force=*/false);
    if (ImGui::Button("Refresh")) refresh_requests(/*force=*/true);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu request(s), rescans every %.0fs",
                        g_requests.size(), kScanPeriodSec);

    // Bridge liveness: the daemon touches studio/bridge.alive every poll.
    // A dead/absent bridge is otherwise invisible (requests sit "pending"
    // forever) — that silence once cost 11 minutes of waiting.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto hb = fs::path(io::kStudioDir) / "bridge.alive";
        auto mt = fs::last_write_time(hb, ec);
        bool alive = false;
        if (!ec) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                fs::file_time_type::clock::now() - mt).count();
            alive = age < 10;
        }
        ImGui::SameLine();
        if (alive) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "bridge: online");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "BRIDGE OFFLINE — run: python -m tools.cinematics.studio_bridge");
        }
    }
    ImGui::Separator();

    if (g_requests.empty()) {
        ImGui::TextDisabled("(no requests yet — Compose one, or the dir "
                            "doesn't exist until the first send)");
        return;
    }

    const long long now = (long long)std::time(nullptr);
    std::string to_delete;   // deferred: don't mutate the list mid-iteration

    for (const io::RequestEntry& e : g_requests) {
        ImGui::PushID(e.id.c_str());
        const long long age = (e.mtime > 0) ? (now - e.mtime) : 0;
        ImGui::Text("%s  [%s]", e.id.c_str(), e.kind.c_str());
        ImGui::SameLine();
        if (age < 120)       ImGui::TextDisabled("%llds ago", age);
        else if (age < 7200) ImGui::TextDisabled("%lldm ago", age / 60);
        else                 ImGui::TextDisabled("%lldh ago", age / 3600);

        const bool done  = (e.status == "done");
        const bool error = (e.status == "error");
        ImGui::SameLine();
        ImGui::TextColored(error ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                           : done ? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
                                  : ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                           "%s", e.status.c_str());
        if (!e.cinematic_id.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("-> %s", e.cinematic_id.c_str());
        }
        if (!e.message.empty()) ImGui::TextWrapped("  %s", e.message.c_str());

        if (done && !e.cinematic_id.empty()) {
            if (ImGui::Button("Reload")) {
                std::string err;
                if (!g_hooks.reload)               g_status = "reload hook not wired";
                else if (g_hooks.reload(e.cinematic_id, err))
                    g_status = "reloaded '" + e.cinematic_id + "'";
                else                               g_status = "reload failed: " + err;
            }
            ImGui::SameLine();
            if (ImGui::Button("Play")) {
                std::string err;
                if (!g_hooks.play)                 g_status = "play hook not wired";
                else if (g_hooks.play(e.cinematic_id, err))
                    g_status = "playing '" + e.cinematic_id + "'";
                else                               g_status = "play refused: " + err;
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Delete")) to_delete = e.id;
        ImGui::Separator();
        ImGui::PopID();
    }

    if (!to_delete.empty()) {
        io::delete_request(io::kStudioDir, to_delete);
        g_status = "deleted " + to_delete;
        refresh_requests(/*force=*/true);
    }
}

// ---- LINES (fine-tune) ----------------------------------------------------------

// Editable state for one `line` cue. char buffers per debug_panel style.
struct LineEdit {
    std::string speaker, portrait, orig_text;
    float t = 0.0f;
    char  text[512]           = "";
    char  emotion[256]        = "";   // side map only — NOT in the cinematic JSON
    char  portrait_extra[256] = "";
    char  seed_image[512]     = "";
    int   voice_idx = -1;             // index into g_profiles; -1 = keep current
    float speed = 1.0f;
    bool  regen_portrait = false, regen_voice = false;
};

static std::vector<std::string>     g_cine_ids;      // *.json stems
static int                          g_cine_sel = -1;
static std::string                  g_loaded_id;     // whose lines are loaded
static std::vector<LineEdit>        g_lines;
static std::vector<io::VoiceProfile> g_profiles;
static bool                         g_lists_scanned = false;
// Emotion/direction survives switching cinematics within a session (it has
// no home in the cinematic JSON — docs §3 keeps it request-only).
static std::unordered_map<std::string, std::string> g_emotion_notes;

static std::string emotion_key(const std::string& cine, int idx) {
    return cine + "#" + std::to_string(idx);
}

static void rescan_lists() {
    g_cine_ids = io::list_cinematic_ids(kCinematicsDir);
    g_profiles = io::load_voice_profiles(kVoiceProfiles);
    g_lists_scanned = true;
    // keep the selection pointing at the same id if it survived the rescan
    g_cine_sel = -1;
    for (int i = 0; i < (int)g_cine_ids.size(); ++i)
        if (g_cine_ids[i] == g_loaded_id) g_cine_sel = i;
}

static void load_lines(const std::string& id) {
    g_lines.clear();
    g_loaded_id = id;
    const json::Value root =
        json::parse_file(std::string(kCinematicsDir) + "/" + id + ".json");
    cinematic::Cinematic doc;
    std::string err;
    if (!cinematic::parse_document(root, id, doc, err)) {
        g_status = "parse failed for '" + id + "': " + err;
        return;
    }
    int line_idx = 0;
    for (const Cue& cue : doc.cues) {
        if (cue.cmd != Cmd::Line) continue;
        LineEdit le;
        le.speaker   = cue.speaker;
        le.portrait  = cue.portrait;
        le.orig_text = cue.text;
        le.t         = cue.t;
        std::snprintf(le.text, sizeof(le.text), "%s", cue.text.c_str());
        const auto em = g_emotion_notes.find(emotion_key(id, line_idx));
        if (em != g_emotion_notes.end())
            std::snprintf(le.emotion, sizeof(le.emotion), "%s", em->second.c_str());
        g_lines.push_back(std::move(le));
        ++line_idx;
    }
    g_status = "loaded " + std::to_string(g_lines.size()) + " line(s) from '"
             + id + "'";
}

static void build_one_line(int i, LineEdit& le) {
    ImGui::PushID(i);
    char header[160];
    std::snprintf(header, sizeof(header), "line %d — %s (t=%.1fs)###line%d",
                  i, le.speaker.empty() ? "?" : le.speaker.c_str(), le.t, i);
    if (ImGui::CollapsingHeader(header)) {
        ImGui::InputText("text", le.text, sizeof(le.text));
        if (ImGui::InputText("emotion / direction", le.emotion, sizeof(le.emotion)))
            g_emotion_notes[emotion_key(g_loaded_id, i)] = le.emotion;
        ImGui::InputText("portrait prompt extra", le.portrait_extra,
                         sizeof(le.portrait_extra));

        // voice profile combo: entry 0 = "(keep current)"
        const char* preview = (le.voice_idx >= 0 &&
                               le.voice_idx < (int)g_profiles.size())
                                  ? g_profiles[(size_t)le.voice_idx].label.c_str()
                                  : "(keep current)";
        if (ImGui::BeginCombo("voice profile", preview)) {
            if (ImGui::Selectable("(keep current)", le.voice_idx < 0))
                le.voice_idx = -1;
            for (int p = 0; p < (int)g_profiles.size(); ++p) {
                if (ImGui::Selectable(g_profiles[(size_t)p].label.c_str(),
                                      le.voice_idx == p))
                    le.voice_idx = p;
                if (ImGui::IsItemHovered() &&
                    !g_profiles[(size_t)p].flavor.empty())
                    ImGui::SetTooltip("%s", g_profiles[(size_t)p].flavor.c_str());
            }
            ImGui::EndCombo();
        }

        ImGui::SliderFloat("speed", &le.speed, 0.7f, 1.3f, "%.2f");
        ImGui::InputText("seed image", le.seed_image, sizeof(le.seed_image));
        ImGui::SameLine();
        const std::string guess = io::character_from_portrait(le.portrait);
        if (guess.empty()) ImGui::BeginDisabled();
        if (ImGui::Button("Use current _ref")) {
            std::snprintf(le.seed_image, sizeof(le.seed_image),
                          "assets/cinematics/portraits/%s/_ref.png",
                          guess.c_str());
        }
        if (guess.empty()) ImGui::EndDisabled();
        ImGui::TextDisabled("portrait: %s", le.portrait.empty()
                                                ? "(none)" : le.portrait.c_str());
        ImGui::Checkbox("regen portrait", &le.regen_portrait);
        ImGui::SameLine();
        ImGui::Checkbox("regen voice", &le.regen_voice);
    }
    ImGui::PopID();
}

static void send_refine() {
    io::Request r;
    r.id           = fresh_request_id();
    r.kind         = "refine";
    r.cinematic_id = g_loaded_id;
    for (int i = 0; i < (int)g_lines.size(); ++i) {
        const LineEdit& le = g_lines[(size_t)i];
        io::LineOverride lo;
        lo.index = i;
        const std::string txt = trimmed(le.text);
        if (txt != le.orig_text)      { lo.has_text = true;    lo.text = txt; }
        const std::string emo = trimmed(le.emotion);
        if (!emo.empty())             { lo.has_emotion = true; lo.emotion = emo; }
        const std::string pex = trimmed(le.portrait_extra);
        if (!pex.empty()) { lo.has_portrait_extra = true; lo.portrait_prompt_extra = pex; }
        if (le.voice_idx >= 0 && le.voice_idx < (int)g_profiles.size()) {
            lo.has_voice_id = true;
            lo.voice_id = g_profiles[(size_t)le.voice_idx].id;
        }
        if (le.speed < 0.995f || le.speed > 1.005f) {
            lo.has_speed = true; lo.speed = le.speed;
        }
        const std::string seed = trimmed(le.seed_image);
        if (!seed.empty())            { lo.has_seed_image = true; lo.seed_image = seed; }
        if (lo.any()) r.line_overrides.push_back(std::move(lo));
        if (le.regen_portrait) r.regen_portraits.push_back(i);
        if (le.regen_voice)    r.regen_voices.push_back(i);
    }
    if (r.line_overrides.empty() && r.regen_portraits.empty() &&
        r.regen_voices.empty()) {
        g_status = "nothing to refine — tweak a field or tick a regen box first";
        return;
    }
    std::string err;
    g_status = io::write_request(r, io::kStudioDir, err)
                   ? "queued " + r.id + " (refine '" + g_loaded_id + "')"
                   : "write FAILED: " + err;
    refresh_requests(/*force=*/true);
}

static void build_lines() {
    if (!g_lists_scanned) rescan_lists();
    if (ImGui::Button("Rescan")) rescan_lists();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    const char* preview = (g_cine_sel >= 0 && g_cine_sel < (int)g_cine_ids.size())
                              ? g_cine_ids[(size_t)g_cine_sel].c_str()
                              : "(pick a cinematic)";
    if (ImGui::BeginCombo("##cine", preview)) {
        for (int i = 0; i < (int)g_cine_ids.size(); ++i) {
            if (ImGui::Selectable(g_cine_ids[(size_t)i].c_str(), g_cine_sel == i)) {
                g_cine_sel = i;
                load_lines(g_cine_ids[(size_t)i]);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu voice profile(s)", g_profiles.size());
    ImGui::Separator();

    if (g_loaded_id.empty()) {
        ImGui::TextDisabled("(select a cinematic to edit its lines)");
        return;
    }
    if (g_lines.empty()) {
        ImGui::TextDisabled("'%s' has no line cues", g_loaded_id.c_str());
        return;
    }
    for (int i = 0; i < (int)g_lines.size(); ++i)
        build_one_line(i, g_lines[(size_t)i]);

    ImGui::Separator();
    if (ImGui::Button("Send refine request")) send_refine();
    ImGui::SameLine();
    ImGui::TextDisabled("only changed fields are sent; emotion lives only in "
                        "the request");
}

// ---- TIMELINE (drag-to-retime cues) -------------------------------------------

struct TimelineEdit {
    float       t      = 0.0f;
    float       orig_t = 0.0f;
    float       dur    = 0.0f;
    std::string label;              // "line: Vance" / "spawn: talon1" / etc.
    int         idx    = 0;          // position in the timeline array
};

static std::vector<TimelineEdit> g_timeline;
static std::string               g_timeline_loaded_id;

static std::string cmd_label(const json::Value& cue) {
    const std::string cmd = cue.contains("cmd")
        ? cue["cmd"].string_or("") : "?";
    if (cmd == "line") {
        const std::string sp = cue.contains("speaker")
            ? cue["speaker"].string_or("?") : "?";
        return "line: " + sp;
    }
    if (cmd == "spawn") {
        const std::string ac = cue.contains("actor")
            ? cue["actor"].string_or("?") : "?";
        return "spawn: " + ac;
    }
    if (cmd == "camera_path") return "camera";
    if (cmd == "actor_path") {
        const std::string ac = cue.contains("actor")
            ? cue["actor"].string_or("?") : "?";
        return "path: " + ac;
    }
    if (cmd == "music")      return "music";
    if (cmd == "sfx")        return "sfx";
    if (cmd == "subtitle")   return "subtitle";
    if (cmd == "fade_in")    return "fade_in";
    if (cmd == "fade_out")   return "fade_out";
    if (cmd == "end")        return "END";
    return cmd;
}

static void load_timeline(const std::string& id) {
    g_timeline.clear();
    g_timeline_loaded_id = id;
    const json::Value root =
        json::parse_file(std::string(kCinematicsDir) + "/" + id + ".json");
    if (!root.is_object() || !root.contains("timeline")) {
        g_status = "timeline: no 'timeline' array in '" + id + "'";
        return;
    }
    const auto& tl = root["timeline"].as_array();
    for (size_t i = 0; i < tl.size(); ++i) {
        const json::Value& cue = tl[i];
        if (!cue.is_object() || !cue.contains("t")) continue;
        TimelineEdit te;
        te.t      = cue["t"].as_float();
        te.orig_t = te.t;
        te.dur    = cue.contains("dur") ? cue["dur"].as_float() : 0.0f;
        te.label  = cmd_label(cue);
        te.idx    = (int)i;
        g_timeline.push_back(std::move(te));
    }
    g_status = "timeline: loaded " + std::to_string(g_timeline.size())
             + " cue(s) from '" + id + "'";
}

// Raw-text replacement: read the file, find each `"t": <num>` in the
// timeline array in order, replace with the slider value.  No JSON
// emitter in the engine, so we do surgical text edits.
static bool save_timeline(const std::string& id) {
    namespace fs = std::filesystem;
    const fs::path path =
        fs::path(kCinematicsDir) / (id + ".json");
    std::ifstream in(path);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    in.close();

    // Find the "timeline" array and work only within it.
    const size_t tl_pos = text.find("\"timeline\"");
    if (tl_pos == std::string::npos) return false;
    size_t cursor = text.find('[', tl_pos);
    if (cursor == std::string::npos) return false;

    int replaced = 0;
    for (const TimelineEdit& te : g_timeline) {
        size_t t_pos = text.find("\"t\"", cursor);
        if (t_pos == std::string::npos) break;
        size_t colon = text.find(':', t_pos);
        if (colon == std::string::npos) break;
        size_t num_start = colon + 1;
        while (num_start < text.size() &&
               (text[num_start] == ' ' || text[num_start] == '\t' ||
                text[num_start] == '\n' || text[num_start] == '\r'))
            ++num_start;
        size_t num_end = num_start;
        while (num_end < text.size() &&
               (std::isdigit((unsigned char)text[num_end]) ||
                text[num_end] == '.' || text[num_end] == '-' ||
                text[num_end] == '+' || text[num_end] == 'e' ||
                text[num_end] == 'E'))
            ++num_end;
        if (num_end == num_start) { cursor = num_end; continue; }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", te.t);
        text.replace(num_start, num_end - num_start, buf);
        cursor = num_start + std::strlen(buf);
        ++replaced;
    }

    if (replaced == 0) return false;
    std::ofstream out(path);
    if (!out) return false;
    out << text;
    out.close();
    g_status = "timeline: saved " + std::to_string(replaced)
             + " cue(s) to '" + id + "'";
    return true;
}

static void build_timeline() {
    if (!g_lists_scanned) rescan_lists();
    if (ImGui::Button("Rescan")) rescan_lists();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    const char* preview = (g_cine_sel >= 0 && g_cine_sel < (int)g_cine_ids.size())
                              ? g_cine_ids[(size_t)g_cine_sel].c_str()
                              : "(pick a cinematic)";
    if (ImGui::BeginCombo("##tl_cine", preview)) {
        for (int i = 0; i < (int)g_cine_ids.size(); ++i) {
            if (ImGui::Selectable(g_cine_ids[(size_t)i].c_str(), g_cine_sel == i)) {
                g_cine_sel = i;
                load_timeline(g_cine_ids[(size_t)i]);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Separator();

    if (g_timeline_loaded_id.empty()) {
        ImGui::TextDisabled("(select a cinematic to retime its cues)");
        return;
    }
    if (g_timeline.empty()) {
        ImGui::TextDisabled("'%s' has no timed cues", g_timeline_loaded_id.c_str());
        return;
    }

    float max_t = 1.0f;
    for (const auto& te : g_timeline)
        max_t = std::max(max_t, te.t);
    max_t = std::ceil(max_t / 5.0f) * 5.0f;

    ImGui::TextDisabled("drag sliders to retime cues, then Save & Reload");
    ImGui::Separator();

    for (int i = 0; i < (int)g_timeline.size(); ++i) {
        TimelineEdit& te = g_timeline[(size_t)i];
        ImGui::PushID(i);
        const bool changed = std::abs(te.t - te.orig_t) > 0.05f;
        if (changed) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
        ImGui::SetNextItemWidth(-140.0f);
        ImGui::SliderFloat(te.label.c_str(), &te.t, 0.0f, max_t, "%.1fs");
        if (changed) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (te.dur > 0.0f)
            ImGui::TextDisabled("dur %.1f", te.dur);
        else
            ImGui::TextDisabled("-");
        ImGui::PopID();
    }

    ImGui::Separator();

    // ---- Live camera offset editor ---------------------------------------
    // While a cinematic is playing, drag these sliders to adjust the
    // follow-cam offset in real time. Changes apply instantly via
    // cinematic::set_follow_offset_override — no save needed.
    if (cinematic::active()) {
        static float off_x = 0.0f, off_y = 0.0f, off_z = 0.0f;
        static bool  off_active = false;
        ImGui::TextDisabled("live camera offset (follow-cam, while playing):");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##off_x", &off_x, -3000.0f, 3000.0f, "X %.0f");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##off_y", &off_y, -3000.0f, 3000.0f, "Y %.0f");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##off_z", &off_z, -3000.0f, 3000.0f, "Z %.0f");
        if (ImGui::Button("Apply")) {
            cinematic::set_follow_offset_override(
                HMM_V3(off_x, off_y, off_z));
            off_active = true;
            g_status = "live offset applied";
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            cinematic::clear_follow_offset_override();
            off_active = false;
            g_status = "live offset cleared";
        }
        ImGui::SameLine();
        if (ImGui::Button("Save to JSON")) {
            // Write the current offset into the active camera_path cue's
            // keys[0].pos in the JSON file, then reload.
            if (!g_timeline_loaded_id.empty()) {
                namespace fs = std::filesystem;
                const fs::path path =
                    fs::path(kCinematicsDir) / (g_timeline_loaded_id + ".json");
                std::ifstream in(path);
                if (in) {
                    std::string text((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
                    in.close();
                    // Find the timeline array, then find the camera_path
                    // whose t is closest to cinematic::time().
                    // Simple approach: find "pos" in the first camera_path
                    // after the current time. This is a rough heuristic but
                    // good enough for live tweaking.
                    // Find all camera_path cue positions and pick the one
                    // nearest the current playback time.
                    size_t tl_pos = text.find("\"timeline\"");
                    if (tl_pos != std::string::npos) {
                        size_t cursor = text.find('[', tl_pos);
                        float best_dist = 1e9f;
                        size_t best_pos_loc = std::string::npos;
                        while (cursor < text.size()) {
                            size_t cmd_pos = text.find("\"camera_path\"", cursor);
                            if (cmd_pos == std::string::npos) break;
                            // find the "t" value
                            size_t t_pos = text.find("\"t\"", cmd_pos);
                            if (t_pos == std::string::npos) break;
                            size_t colon = text.find(':', t_pos);
                            float cue_t = std::strtof(text.c_str() + colon + 1, nullptr);
                            float dist = std::abs(cue_t - cinematic::time());
                            // find the "pos" array after this camera_path
                            size_t pos_loc = text.find("\"pos\"", cmd_pos);
                            if (pos_loc != std::string::npos && dist < best_dist) {
                                best_dist = dist;
                                best_pos_loc = pos_loc;
                            }
                            cursor = cmd_pos + 1;
                        }
                        if (best_pos_loc != std::string::npos) {
                            // Replace the array values after "pos":
                            size_t bracket = text.find('[', best_pos_loc);
                            size_t end = text.find(']', bracket);
                            if (bracket != std::string::npos && end != std::string::npos) {
                                char buf[64];
                                std::snprintf(buf, sizeof(buf),
                                    "[%.0f, %.0f, %.0f]", off_x, off_y, off_z);
                                text.replace(bracket, end - bracket + 1, buf);
                                std::ofstream out(path);
                                if (out) {
                                    out << text;
                                    out.close();
                                    std::string err2;
                                    if (g_hooks.reload) g_hooks.reload(g_timeline_loaded_id, err2);
                                    g_status = "offset saved to JSON + reloaded";
                                }
                            }
                        }
                    }
                }
            }
        }
        ImGui::Separator();
    }

    if (ImGui::Button("Save & Reload")) {
        if (save_timeline(g_timeline_loaded_id)) {
            std::string err;
            if (g_hooks.reload) g_hooks.reload(g_timeline_loaded_id, err);
            for (auto& te : g_timeline) te.orig_t = te.t;
        } else {
            g_status = "timeline: save FAILED for '" + g_timeline_loaded_id + "'";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        for (auto& te : g_timeline) te.t = te.orig_t;
        g_status = "timeline: reset to saved values";
    }
    ImGui::SameLine();
    if (ImGui::Button("Play")) {
        std::string err;
        if (g_hooks.play) g_hooks.play(g_timeline_loaded_id, err);
    }
}

// ---- PLAYBACK ------------------------------------------------------------------

static std::deque<std::string> g_events;   // last 10 "cinematic" beats

void note_event(const std::string& text) {
    g_events.push_back(text);
    while (g_events.size() > 10) g_events.pop_front();
}

static void build_playback() {
    const bool  act = cinematic::active();
    const float dur = cinematic::duration();
    if (act)
        ImGui::Text("playing '%s'  %.1f / %.1fs",
                    cinematic::current_id(), cinematic::time(), dur);
    else if (dur > 0.0f)
        ImGui::Text("idle  (loaded: '%s', %.1fs)", cinematic::current_id(), dur);
    else
        ImGui::Text("idle  (nothing loaded)");

    const std::string sel = (g_cine_sel >= 0 &&
                             g_cine_sel < (int)g_cine_ids.size())
                                ? g_cine_ids[(size_t)g_cine_sel] : "";
    if (ImGui::Button(sel.empty() ? "Play" : ("Play '" + sel + "'").c_str())) {
        std::string err;
        if (sel.empty())          g_status = "pick a cinematic on the Lines tab first";
        else if (!g_hooks.play)   g_status = "play hook not wired";
        else if (g_hooks.play(sel, err)) g_status = "playing '" + sel + "'";
        else                      g_status = "play refused: " + err;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        if (g_hooks.stop) { g_hooks.stop(); g_status = "stopped"; }
        else              g_status = "stop hook not wired";
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) {
        // empty id = "the current/last one" per cinematic::reload's contract;
        // fall back to the Lines selection when nothing was ever loaded.
        const std::string id = act ? "" : sel;
        std::string err;
        if (!g_hooks.reload)              g_status = "reload hook not wired";
        else if (g_hooks.reload(id, err)) g_status = "reloaded (+triggers)";
        else                              g_status = "reload failed: " + err;
    }

    // Seek: track the live time while idle-handed; commit on slider release
    // so we don't spam per-frame seeks (each one re-establishes actors).
    static float seek_pos    = 0.0f;
    static bool  seek_active = false;   // was the slider grabbed last frame?
    if (act && dur > 0.0f) {
        if (!seek_active) seek_pos = cinematic::time();   // follow playback
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::SliderFloat("seek", &seek_pos, 0.0f, dur, "%.1fs");
        seek_active = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            std::string err;
            if (!g_hooks.seek)                 g_status = "seek hook not wired";
            else if (g_hooks.seek(seek_pos, err)) g_status = "seeked";
            else                               g_status = "seek failed: " + err;
        }
    } else {
        seek_active = false;
        ImGui::TextDisabled("(seek available while playing)");
    }

    ImGui::Separator();
    ImGui::Text("triggers loaded: %d", cinematic::triggers::count());
    ImGui::SameLine();
    if (ImGui::Button("Reload Triggers")) {
        cinematic::triggers::load(kTriggersPath);   // engine-free, main thread
        g_status = "trigger table reloaded ("
                 + std::to_string(cinematic::triggers::count()) + ")";
    }

    ImGui::Separator();
    ImGui::TextDisabled("recent cinematic events:");
    if (g_events.empty()) ImGui::TextDisabled("  (none yet)");
    for (const std::string& e : g_events) ImGui::BulletText("%s", e.c_str());
}

// ---- window shell ----------------------------------------------------------------

void build() {
    if (!g_visible) return;

    ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Cinematic Studio (Ctrl+K)", &g_visible)) {
        if (ImGui::BeginTabBar("##studio_tabs")) {
            if (ImGui::BeginTabItem("Compose"))  { build_compose();  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Requests")) { build_requests(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Lines"))    { build_lines();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Timeline")) { build_timeline(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Playback")) { build_playback(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        if (!g_status.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", g_status.c_str());
        }
    }
    ImGui::End();
}

} // namespace cinematic::studio
