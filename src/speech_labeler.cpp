// -----------------------------------------------------------------------------
// speech_labeler.cpp — F9 audition + hand-label tool for Privateer speech.
// See header for the why. Mirrors src/sound_labeler.cpp (same ImGui layout,
// persistence, toolbar, auto-advance and reverse-lookup pattern) — see that
// file for the deeper design notes; we keep this one's comments terser.
//
// Differences from the F7 SFX tool:
//   * 544 entries not ~43 — bigger list, no scrollbar hacks needed.
//   * No engine bindings yet, so the "current use" column always reads
//     "(unused)". The tag struct is in place so wiring a binding later is a
//     one-line edit (no header reshuffling).
//   * No auto-mute ducking: speech is ~2s one-shots that wouldn't fight the
//     music bed, unlike music tracks that run 50-150s.
// -----------------------------------------------------------------------------

#include "speech_labeler.h"

#include "audio.h"
#include "json.h"

#include "imgui.h"
#include "sokol_gfx.h"      // must precede sokol_imgui.h (pipeline types)
#include "sokol_imgui.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace speech_labeler {

namespace {

// Where the converted speech WAVs live (tracked, alongside SPEECH.PAK).
// Run `python3 tools/extract_speech_pak.py assets/speech/SPEECH.PAK
//                                  --convert-wav assets/speech/original`
// to populate after a clean clone.
constexpr const char* k_speech_dir = "assets/speech/original";
// Bar/fixer conversations from CONV/*.VPK live in assets/speech/bar/
// alongside original/. They share the same label namespace — index
// numbers are disjoint so the two sets don't collide. Re-extract via
// `python3 tools/extract_bar_speech.py <conv_dir> assets/speech/bar`.
constexpr const char* k_speech_dir_bar = "assets/speech/bar";
// Where the labels are written — TEXT, committable, sibling of the F7/F8
// labels files. NOT under any gitignored tree.
constexpr const char* k_labels_path = "docs/speech_labels.json";

// Label buffers are fixed-size so ImGui::InputText can write into them
// directly each frame without per-keystroke allocation. 128 is plenty for
// "player_accept / pirate_threaten" style descriptions.
constexpr int k_label_cap = 128;

// ---- one auditionable clip --------------------------------------------------
struct Clip {
    int         index = -1;            // unique id (see k_speech_*_base below)
    std::string filename;              // display filename
    std::string path;                  // full relative path
    SampleId    sample      = 0;       // 0 = failed to load
    float       duration_s  = 0.0f;    // from the WAV header (0 if unknown)
    char        label[k_label_cap] = {0};
    const char* current_use = "(unused)";  // reverse lookup, never null
    bool        is_bar      = false;   // bar/fixer conv (true) vs in-flight (false)
};

// Index base for bar/fixer entries — keeps in-flight and bar audio disjoint
// in the index space so the JSON can mix them under separate prefixes.
constexpr int k_bar_index_base = 100000;

// ---- module-static state ----------------------------------------------------
//
// Hidden by default (dev affordance — shouldn't hijack input on boot). F9
// reveals it; the first reveal triggers the lazy scan+load.
bool             g_visible      = false;
bool             g_loaded       = false;   // lazy scan done?
bool             g_scan_ok      = false;   // did we find the dir + any clips?
std::vector<Clip> g_clips;
VoiceId          g_last_voice   = 0;       // cut the previous one-shot on replay
float            g_play_gain    = 0.8f;    // audition gain (slider)
bool             g_auto_advance = false;   // Enter -> focus+play next row
int              g_focus_row    = -1;      // row to grab keyboard focus next frame
int              g_dirty_saves  = 0;       // edits since last save (HUD only)

// ---- reverse lookup: extracted index -> CURRENT engine binding --------------
//
// The engine doesn't bind speech yet, so EVERY row currently reports
// "(unused)". This struct is in place so wiring a binding later is a
// one-line edit (no header / sort / persistence reshuffling).
struct UseRow { int index; const char* use; };
constexpr UseRow k_current_use[] = {
    // { <NN>, "<event or call-site>" },  // uncomment + fill when wired up
};

const char* lookup_use(int index) {
    for (const UseRow& r : k_current_use)
        if (r.index == index) return r.use;
    return "(unused)";
}

// ---- tiny WAV header peek for duration --------------------------------------
//
// audio.h doesn't expose a sample's frame count, so read it straight off the
// RIFF header here. Same helper shape as the F7 / F8 tools. Returns 0.0 if
// we can't make sense of it — a missing duration is cosmetic, never fatal.
float wav_duration_seconds(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0.0f;
    char hdr[12] = {0};
    f.read(hdr, 12);
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
        return 0.0f;

    uint32_t sample_rate = 0;
    uint16_t channels = 0, bits = 0;
    uint32_t data_bytes = 0;

    char id[4];
    while (f.read(id, 4)) {
        uint32_t sz = 0;
        if (!f.read(reinterpret_cast<char*>(&sz), 4)) break;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            char fmt[16] = {0};
            const uint32_t want = sz < 16 ? sz : 16;
            f.read(fmt, want);
            std::memcpy(&channels,    fmt + 2, 2);
            std::memcpy(&sample_rate, fmt + 4, 4);
            std::memcpy(&bits,        fmt + 14, 2);
            if (sz > want) f.seekg(sz - want, std::ios::cur);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data_bytes = sz;
            break;
        } else {
            f.seekg(sz, std::ios::cur);
        }
        if (sz & 1) f.seekg(1, std::ios::cur);
    }

    if (sample_rate == 0 || channels == 0 || bits == 0 || data_bytes == 0)
        return 0.0f;
    const uint32_t frame_bytes = channels * (bits / 8);
    if (frame_bytes == 0) return 0.0f;
    const double frames = double(data_bytes) / double(frame_bytes);
    return float(frames / double(sample_rate));
}

// ---- persistence ------------------------------------------------------------
//
// Read docs/speech_labels.json (if present) and copy any saved labels into
// the matching clips, so a labeling session resumes where it left off.
// Tolerant of a missing/partial file — that's the normal first-run case.
void load_existing_labels() {
    json::Value root = json::parse_file(k_labels_path);
    if (!root.is_object()) return;
    int restored = 0;
    for (Clip& c : g_clips) {
        char key[64];
        if (c.is_bar)
            std::snprintf(key, sizeof key, "bar/%s", c.filename.c_str());
        else
            std::snprintf(key, sizeof key, "speech_%04d", c.index);
        const json::Value* entry = root.find(key);
        if (!entry || !entry->is_object()) continue;
        if (const json::Value* lbl = entry->find("label"); lbl && lbl->is_string()) {
            std::snprintf(c.label, sizeof c.label, "%s", lbl->as_string().c_str());
            if (c.label[0]) ++restored;
        }
    }
    if (restored)
        std::printf("[speech_labeler] restored %d saved label(s) from %s\n",
                    restored, k_labels_path);
}

std::string json_escape(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 2);
    for (char ch : s) {
        switch (ch) {
            case '"':  r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n";  break;
            case '\t': r += "\\t";  break;
            case '\r': r += "\\r";  break;
            default:   r += ch;     break;
        }
    }
    return r;
}

// Hand-emit the committable labels JSON. Flat object keyed by "speech_NNNN",
// each value { "label", "current_use" }. We write EVERY clip (even unlabeled
// ones, label="") so the file doubles as a checklist of what's left to do
// and the key set is stable across saves for a clean git diff.
bool save_labels() {
    std::error_code ec;
    fs::create_directories(fs::path(k_labels_path).parent_path(), ec);

    std::string out;
    out += "{\n";
    out += "  \"_comment\": \"Human-labeled SPEECH.PAK clips (ground truth). "
           "Generated by the F9 in-game speech labeler. 'current_use' is the "
           "engine's CURRENT binding for context (currently always '(unused)'"
           " — speech isn't wired into any event/gun yet).\",\n";
    for (size_t i = 0; i < g_clips.size(); ++i) {
        const Clip& c = g_clips[i];
        char key[64];
        if (c.is_bar)
            std::snprintf(key, sizeof key, "bar/%s", c.filename.c_str());
        else
            std::snprintf(key, sizeof key, "speech_%04d", c.index);
        out += "  \"";
        out += key;
        out += "\": { \"label\": \"";
        out += json_escape(c.label);
        out += "\", \"current_use\": \"";
        out += json_escape(c.current_use);
        out += "\" }";
        out += (i + 1 < g_clips.size()) ? ",\n" : "\n";
    }
    out += "}\n";

    const std::string tmp = std::string(k_labels_path) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            std::fprintf(stderr, "[speech_labeler] could not open %s for write\n",
                         tmp.c_str());
            return false;
        }
        f << out;
    }
    fs::rename(tmp, k_labels_path, ec);
    if (ec) {
        std::fprintf(stderr, "[speech_labeler] rename %s -> %s failed: %s\n",
                     tmp.c_str(), k_labels_path, ec.message().c_str());
        return false;
    }

    int labeled = 0;
    for (const Clip& c : g_clips) if (c.label[0]) ++labeled;
    std::printf("[speech_labeler] saved %d labels to %s (%d of %zu named)\n",
                (int)g_clips.size(), k_labels_path, labeled, g_clips.size());
    g_dirty_saves = 0;
    return true;
}

// ---- lazy clip scan + load --------------------------------------------------
//
// First-open only. Scans k_speech_dir for speech_NNNN.wav, sorts by the
// NNNN index (NOT lexically — speech_2 vs speech_0010), loads each into a
// SampleId, peeks its duration, and resolves its current-use tag. Missing
// dir / no clips leaves g_scan_ok false so build() shows a friendly "run
// the extraction first" note instead of crashing.
void ensure_loaded() {
    if (g_loaded) return;
    g_loaded = true;

    std::error_code ec;
    if (!fs::is_directory(k_speech_dir, ec)) {
        std::printf("[speech_labeler] no speech WAVs at %s/ — "
                    "run `python3 tools/extract_speech_pak.py %s "
                    "--convert-wav %s` first\n",
                    k_speech_dir, "assets/speech/SPEECH.PAK", k_speech_dir);
        return;
    }

    for (const auto& entry : fs::directory_iterator(k_speech_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        // Match speech_<digits>.wav and pull the index.
        if (name.rfind("speech_", 0) != 0) continue;
        if (name.size() < 5 || entry.path().extension() != ".wav") continue;
        int idx = -1;
        if (std::sscanf(name.c_str(), "speech_%d.wav", &idx) != 1 || idx < 0) continue;

        Clip c;
        c.index       = idx;
        c.filename    = name;
        c.path        = entry.path().string();
        c.sample      = audio::load(c.path);
        c.duration_s  = wav_duration_seconds(c.path);
        c.current_use = lookup_use(idx);
        c.is_bar      = false;
        g_clips.push_back(std::move(c));
    }

    // Bar/fixer conversations from assets/speech/bar/. Each entry's filename
    // is its own key in the JSON (avoids collisions across PAK files); the
    // int index is an in-loadtable sequence number for sorting/display.
    int bar_seq = 0;
    if (fs::is_directory(k_speech_dir_bar, ec)) {
        for (const auto& entry : fs::directory_iterator(k_speech_dir_bar, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() != ".wav") continue;
            if (name.size() < 1) continue;

            Clip c;
            c.index       = k_bar_index_base + bar_seq++;
            c.filename    = name;
            c.path        = entry.path().string();
            c.sample      = audio::load(c.path);
            c.duration_s  = wav_duration_seconds(c.path);
            c.current_use = "(unused)";
            c.is_bar      = true;
            g_clips.push_back(std::move(c));
        }
    }

    std::sort(g_clips.begin(), g_clips.end(),
              [](const Clip& a, const Clip& b) { return a.index < b.index; });

    g_scan_ok = !g_clips.empty();
    int ok = 0;
    for (const Clip& c : g_clips) if (c.sample != 0) ++ok;
    std::printf("[speech_labeler] loaded %d/%zu total clips (%s + bar/conv)\n",
                ok, g_clips.size(), k_speech_dir);

    load_existing_labels();
}

// Play one clip, cutting whatever audition was already going (we only ever
// want to hear one at a time while labeling).
void audition(const Clip& c) {
    if (c.sample == 0) return;
    if (g_last_voice) audio::stop(g_last_voice);
    g_last_voice = audio::play(c.sample, g_play_gain);
}

void stop_all() {
    if (g_last_voice) audio::stop(g_last_voice);
    g_last_voice = 0;
}

} // namespace

// ---- lifecycle --------------------------------------------------------------

void init() {
    std::printf("[speech_labeler] press F9 to open the speech labeler "
                "(labels -> %s)\n", k_labels_path);
}

void shutdown() {}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_F9) {
        g_visible = !g_visible;
        if (g_visible) ensure_loaded();
        return true;
    }
    return false;
}

// ---- build ------------------------------------------------------------------

void build() {
    if (!g_visible) return;

    ImGui::SetNextWindowSize(ImVec2(720.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Speech Labeler (F9)", &g_visible)) {
        ImGui::End();
        return;
    }

    if (!g_scan_ok) {
        ImGui::TextWrapped("No speech WAVs found at %s/.", k_speech_dir);
        ImGui::Spacing();
        ImGui::TextWrapped("Extract + convert first:");
        ImGui::TextDisabled("  python3 tools/extract_speech_pak.py "
                            "assets/speech/SPEECH.PAK --convert-wav %s",
                            k_speech_dir);
        ImGui::End();
        return;
    }

    // ---- toolbar -------------------------------------------------------------
    if (ImGui::Button("Save")) save_labels();
    ImGui::SameLine();
    if (ImGui::Button("Stop all")) stop_all();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("gain", &g_play_gain, 0.0f, 1.5f, "%.2f");
    ImGui::SameLine();
    ImGui::Checkbox("auto-advance", &g_auto_advance);

    int labeled = 0;
    for (const Clip& c : g_clips) if (c.label[0]) ++labeled;
    ImGui::Text("%d / %zu labeled%s", labeled, g_clips.size(),
                g_dirty_saves ? "   (unsaved edits)" : "");
    ImGui::TextDisabled("labels save to %s (committable)", k_labels_path);
    ImGui::Separator();

    // ---- the list ------------------------------------------------------------
    ImGui::BeginChild("clip_list", ImVec2(0, 0), false);
    for (int i = 0; i < (int)g_clips.size(); ++i) {
        Clip& c = g_clips[i];
        ImGui::PushID(i);

        // index + filename, fixed-width so the columns line up.
        ImGui::Text("%-22s", c.filename.c_str());
        ImGui::SameLine(140.0f);

        const bool dead = (c.sample == 0);
        if (dead) ImGui::BeginDisabled();
        if (ImGui::Button("Play")) audition(c);
        if (dead) ImGui::EndDisabled();

        ImGui::SameLine();
        if (c.duration_s > 0.0f) ImGui::Text("%5.2fs", c.duration_s);
        else                     ImGui::TextDisabled("  ?  ");

        // The label box. Enter commits + (optionally) advances to the next row.
        ImGui::SameLine(270.0f);
        ImGui::SetNextItemWidth(240.0f);
        if (g_focus_row == i) {
            ImGui::SetKeyboardFocusHere();
            g_focus_row = -1;
        }
        const bool entered = ImGui::InputText(
            "##label", c.label, sizeof c.label, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemDeactivatedAfterEdit() || entered) ++g_dirty_saves;
        if (entered && g_auto_advance && i + 1 < (int)g_clips.size()) {
            g_focus_row = i + 1;
            audition(g_clips[i + 1]);
        }

        // current-engine-binding tag — currently always (unused); see k_current_use.
        ImGui::SameLine(520.0f);
        if (std::strcmp(c.current_use, "(unused)") == 0)
            ImGui::TextDisabled("(unused)");
        else
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                               "used by: %s", c.current_use);

        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace speech_labeler
